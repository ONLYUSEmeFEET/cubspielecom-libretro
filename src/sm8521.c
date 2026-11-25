#include "sm8521.h"
#include <string.h>
#include <time.h>

#define FLAG_C 0x80u
#define FLAG_Z 0x40u
#define FLAG_S 0x20u
#define FLAG_V 0x10u
#define FLAG_D 0x08u
#define FLAG_H 0x04u
#define FLAG_B 0x02u
#define FLAG_I 0x01u

static const uint8_t sm8500_b2w[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };
static const uint32_t gamecom_timer_limit[8] = { 1, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

static inline uint8_t *get_active_cart(SM8521 *cpu) {
    uint8_t p3 = cpu->m_p_ram[SM8521_P3] & 0xC0u;
    if (p3 == 0x40u) {
        return cpu->has_cart1 ? cpu->cart1_rom : NULL;
    } else if (p3 == 0x80u) {
        return cpu->has_cart2 ? cpu->cart2_rom : NULL;
    } else {
        if (cpu->latched_cart_slot == 2) {
            return cpu->has_cart2 ? cpu->cart2_rom : NULL;
        }
        return cpu->has_cart1 ? cpu->cart1_rom : NULL;
    }
}

static inline void get_sp(SM8521 *cpu) {
    cpu->m_SP = cpu->m_p_ram[0x1D];
    if (cpu->m_SYS & 0x40u) {
        cpu->m_SP |= (uint16_t)(cpu->m_p_ram[0x1C] << 8);
    }
}

static void set_input_line(SM8521 *cpu, int inptnum, bool assert_line) {
    cpu->m_IR0 = cpu->m_p_ram[0x12];
    cpu->m_IR1 = cpu->m_p_ram[0x13];
    if (assert_line) {
        cpu->m_IFLAGS |= (uint16_t)(1u << inptnum);
        cpu->m_CheckInterrupts = 1;
        switch (inptnum) {
            case SM8521_DMA_INT:  cpu->m_IR0 |= 0x80u; break;
            case SM8521_TIM0_INT: cpu->m_IR0 |= 0x40u; break;
            case SM8521_EXT_INT:  cpu->m_IR0 |= 0x10u; break;
            case SM8521_UART_INT: cpu->m_IR0 |= 0x08u; break;
            case SM8521_LCDC_INT: cpu->m_IR0 |= 0x01u; break;
            case SM8521_TIM1_INT: cpu->m_IR1 |= 0x40u; break;
            case SM8521_CK_INT:   cpu->m_IR1 |= 0x10u; break;
            case SM8521_PIO_INT:  cpu->m_IR1 |= 0x04u; break;
        }
    } else {
        cpu->m_IFLAGS &= (uint16_t)~(1u << inptnum);
        switch (inptnum) {
            case SM8521_DMA_INT:  cpu->m_IR0 &= (uint8_t)~0x80u; break;
            case SM8521_TIM0_INT: cpu->m_IR0 &= (uint8_t)~0x40u; break;
            case SM8521_EXT_INT:  cpu->m_IR0 &= (uint8_t)~0x10u; break;
            case SM8521_UART_INT: cpu->m_IR0 &= (uint8_t)~0x08u; break;
            case SM8521_LCDC_INT: cpu->m_IR0 &= (uint8_t)~0x01u; break;
            case SM8521_TIM1_INT: cpu->m_IR1 &= (uint8_t)~0x40u; break;
            case SM8521_CK_INT:   cpu->m_IR1 &= (uint8_t)~0x10u; break;
            case SM8521_PIO_INT:  cpu->m_IR1 &= (uint8_t)~0x04u; break;
        }
        if (cpu->m_IFLAGS == 0) {
            cpu->m_CheckInterrupts = 0;
        }
    }
    cpu->m_p_ram[0x12] = cpu->m_IR0;
    cpu->m_p_ram[0x13] = cpu->m_IR1;
}

static void push_byte(SM8521 *cpu, uint8_t val) {
    cpu->m_SP = (uint16_t)(cpu->m_SP - 1u);
    if ((cpu->m_SYS & 0x40u) == 0) {
        cpu->m_SP &= 0xFFu;
    }
    sm8521_mem_writebyte(cpu, cpu->m_SP, val);
}

static void take_interrupt(SM8521 *cpu, uint16_t vector) {
    cpu->m_halted = 0;
    get_sp(cpu);
    cpu->m_SYS = cpu->m_p_ram[0x19];
    cpu->m_PS1 = cpu->m_p_ram[0x1F];

    push_byte(cpu, (uint8_t)(cpu->m_PC & 0xFFu));
    push_byte(cpu, (uint8_t)((cpu->m_PC >> 8) & 0xFFu));
    push_byte(cpu, cpu->m_PS1);
    cpu->m_PS1 &= (uint8_t)~0x01u;

    cpu->m_p_ram[0x1F] = cpu->m_PS1;
    cpu->m_p_ram[0x1D] = (uint8_t)(cpu->m_SP & 0xFFu);
    if (cpu->m_SYS & 0x40u) {
        cpu->m_p_ram[0x1C] = (uint8_t)((cpu->m_SP >> 8) & 0xFFu);
    }

    cpu->m_PC = sm8521_mem_readword(cpu, vector);
}

static void process_interrupts(SM8521 *cpu) {
    if (!cpu->m_CheckInterrupts) return;

    cpu->m_IE0 = cpu->m_p_ram[0x10];
    cpu->m_IE1 = cpu->m_p_ram[0x11];
    cpu->m_IR0 = cpu->m_p_ram[0x12];
    cpu->m_IR1 = cpu->m_p_ram[0x13];
    cpu->m_PS0 = cpu->m_p_ram[0x1E];
    cpu->m_PS1 = cpu->m_p_ram[0x1F];

    uint16_t nmi_mask = (uint16_t)((1u << SM8521_WDT_INT) | (1u << SM8521_ILL_INT) | (1u << SM8521_NMI_INT));
    bool dma_armed = (cpu->m_p_ram[SM8521_DMC] & 0x80u) != 0;
    if (!dma_armed &&
        ((cpu->m_IFLAGS & nmi_mask) != 0 ||
         (cpu->m_IR0 & cpu->m_IE0) != 0 ||
         (cpu->m_IR1 & cpu->m_IE1) != 0)) {
        cpu->m_halted = 0;
    }

    if ((cpu->m_PS1 & 0x01u) == 0) return;

    uint8_t prio = cpu->m_PS0 & 0x07u;

    if (cpu->m_IFLAGS & (1u << SM8521_WDT_INT)) {
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_WDT_INT);
        take_interrupt(cpu, 0x101C);
        return;
    }
    if (cpu->m_IFLAGS & ((1u << SM8521_ILL_INT) | (1u << SM8521_NMI_INT))) {
        cpu->m_IFLAGS &= (uint16_t)~((1u << SM8521_ILL_INT) | (1u << SM8521_NMI_INT));
        take_interrupt(cpu, 0x101E);
        return;
    }
    if ((cpu->m_IR0 & 0x80u) && (cpu->m_IE0 & 0x80u)) {
        cpu->m_IR0 &= (uint8_t)~0x80u;
        cpu->m_p_ram[0x12] = cpu->m_IR0;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_DMA_INT);
        take_interrupt(cpu, 0x1000);
        return;
    }
    if ((cpu->m_IR0 & 0x40u) && (cpu->m_IE0 & 0x40u)) {
        cpu->m_IR0 &= (uint8_t)~0x40u;
        cpu->m_p_ram[0x12] = cpu->m_IR0;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_TIM0_INT);
        take_interrupt(cpu, 0x1002);
        return;
    }
    if ((cpu->m_IR0 & 0x10u) && (cpu->m_IE0 & 0x10u) && prio < 7) {
        cpu->m_IR0 &= (uint8_t)~0x10u;
        cpu->m_p_ram[0x12] = cpu->m_IR0;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_EXT_INT);
        take_interrupt(cpu, 0x1006);
        return;
    }
    if ((cpu->m_IR0 & 0x08u) && (cpu->m_IE0 & 0x08u) && prio < 6) {
        cpu->m_IR0 &= (uint8_t)~0x08u;
        cpu->m_p_ram[0x12] = cpu->m_IR0;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_UART_INT);
        take_interrupt(cpu, 0x1008);
        return;
    }
    if ((cpu->m_IR0 & 0x01u) && (cpu->m_IE0 & 0x01u) && prio < 5) {
        cpu->m_IR0 &= (uint8_t)~0x01u;
        cpu->m_p_ram[0x12] = cpu->m_IR0;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_LCDC_INT);
        take_interrupt(cpu, 0x100E);
        return;
    }
    if ((cpu->m_IR1 & 0x40u) && (cpu->m_IE1 & 0x40u) && prio < 4) {
        cpu->m_IR1 &= (uint8_t)~0x40u;
        cpu->m_p_ram[0x13] = cpu->m_IR1;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_TIM1_INT);
        take_interrupt(cpu, 0x1012);
        return;
    }
    if ((cpu->m_IR1 & 0x10u) && (cpu->m_IE1 & 0x10u) && prio < 3) {
        cpu->m_IR1 &= (uint8_t)~0x10u;
        cpu->m_p_ram[0x13] = cpu->m_IR1;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_CK_INT);
        take_interrupt(cpu, 0x1016);
        return;
    }
    if ((cpu->m_IR1 & 0x04u) && (cpu->m_IE1 & 0x04u) && prio < 2) {
        cpu->m_IR1 &= (uint8_t)~0x04u;
        cpu->m_p_ram[0x13] = cpu->m_IR1;
        cpu->m_IFLAGS &= (uint16_t)~(1u << SM8521_PIO_INT);
        take_interrupt(cpu, 0x101A);
        return;
    }
}

static void handle_stylus_press(SM8521 *cpu, int column) {
    uint16_t data = 0;
    if (cpu->stylus_pressed) {
        int col = cpu->stylus_x / 16;
        int row = cpu->stylus_y / 16;
        if (col < 0) col = 0; else if (col > 12) col = 12;
        if (row < 0) row = 0; else if (row > 9) row = 9;
        if (col == column) {
            data = (uint16_t)(1u << row);
        }
    }
    if (data) {
        uint16_t stylus_y = data ^ 0x3FFu;
        cpu->m_p_ram[SM8521_P0] = (uint8_t)(stylus_y & 0xFFu);
        cpu->m_p_ram[SM8521_P1] = (uint8_t)((cpu->m_p_ram[SM8521_P1] & 0xFCu) | ((stylus_y >> 8) & 0x03u));
    } else {
        cpu->m_p_ram[SM8521_P0] = 0xFFu;
        cpu->m_p_ram[SM8521_P1] |= 0x03u;
    }
}

static uint8_t read_in0(const SM8521 *cpu) {
    uint8_t val = 0xFFu;
    uint32_t b = cpu->buttons_mask;
    if (b & GAMECOM_BTN_UP)    val &= (uint8_t)~0x01u;
    if (b & GAMECOM_BTN_DOWN)  val &= (uint8_t)~0x02u;
    if (b & GAMECOM_BTN_LEFT)  val &= (uint8_t)~0x04u;
    if (b & GAMECOM_BTN_RIGHT) val &= (uint8_t)~0x08u;
    if (b & GAMECOM_BTN_MENU)  val &= (uint8_t)~0x10u;
    if (b & GAMECOM_BTN_PAUSE) val &= (uint8_t)~0x20u;
    if (b & GAMECOM_BTN_SOUND) val &= (uint8_t)~0x40u;
    if (b & GAMECOM_BTN_A)     val &= (uint8_t)~0x80u;
    return val;
}

static uint8_t read_in1(const SM8521 *cpu) {
    uint8_t val = 0xFFu;
    uint32_t b = cpu->buttons_mask;
    if (b & GAMECOM_BTN_B) val &= (uint8_t)~0x01u;
    if (b & GAMECOM_BTN_C) val &= (uint8_t)~0x02u;
    return val;
}

static uint8_t read_in2(const SM8521 *cpu) {
    uint8_t val = 0xFFu;
    uint32_t b = cpu->buttons_mask;
    if (b & (GAMECOM_BTN_RESET | GAMECOM_BTN_POWER)) val &= (uint8_t)~0x01u;
    if (b & GAMECOM_BTN_D) val &= (uint8_t)~0x02u;
    if (cpu->stylus_pressed) val &= (uint8_t)~0x04u;
    return val;
}

static void handle_input_press(SM8521 *cpu, uint16_t mux_data) {
    switch (mux_data) {
        case 0xFFFB: handle_stylus_press(cpu, 0); break;
        case 0xFFF7: handle_stylus_press(cpu, 1); break;
        case 0xFFEF: handle_stylus_press(cpu, 2); break;
        case 0xFFDF: handle_stylus_press(cpu, 3); break;
        case 0xFFBF: handle_stylus_press(cpu, 4); break;
        case 0xFF7F: handle_stylus_press(cpu, 5); break;
        case 0xFEFF: handle_stylus_press(cpu, 6); break;
        case 0xFDFF: handle_stylus_press(cpu, 7); break;
        case 0xFBFF: handle_stylus_press(cpu, 8); break;
        case 0xF7FF: handle_stylus_press(cpu, 9); break;
        case 0xEFFF: handle_stylus_press(cpu, 10); break;
        case 0xDFFF: handle_stylus_press(cpu, 11); break;
        case 0xBFFF: handle_stylus_press(cpu, 12); break;
        case 0x7FFF:
            cpu->m_p_ram[SM8521_P0] = read_in0(cpu);
            cpu->m_p_ram[SM8521_P1] = (uint8_t)((cpu->m_p_ram[SM8521_P1] & 0xFCu) | (read_in1(cpu) & 0x03u));
            break;
        case 0xFFFF:
            cpu->m_p_ram[SM8521_P0] = (uint8_t)((cpu->m_p_ram[SM8521_P0] & 0xFCu) | (read_in2(cpu) & 0x03u));
            cpu->m_p_ram[SM8521_P1] = 0xFFu;
            break;
    }
}

static inline uint8_t read_mmu_bank(SM8521 *cpu, int bank_index, uint16_t offset_in_bank) {
    uint8_t mmu_val = cpu->mmu[bank_index];
    uint32_t offset = offset_in_bank & 0x1FFFu;
    if (mmu_val < 0x20u) {
        uint32_t addr = (((uint32_t)mmu_val << 13) + offset) & 0x3FFFFu;
        return cpu->external_rom[addr];
    } else {
        uint8_t *cart = get_active_cart(cpu);
        if (cart) {
            uint32_t addr = (((uint32_t)mmu_val << 13) + offset) & 0x1FFFFFu;
            return cart[addr];
        }
        return 0xFFu;
    }
}

static uint8_t program_read_byte(SM8521 *cpu, uint16_t addr) {
    if (addr < 0x0400u) {
        if (addr == SM8521_P3) {
            return (uint8_t)(cpu->m_p_ram[SM8521_P3] & ~0x0Cu);
        }
        if (addr == SM8521_URTR) {
            cpu->m_p_ram[SM8521_URTS] &= (uint8_t)~0x1Du;
            return cpu->m_p_ram[SM8521_URTR];
        }
        return cpu->m_p_ram[addr];
    }
    if (addr < 0x1000u) return 0xFFu;
    if (addr < 0x2000u) return cpu->internal_rom[addr - 0x1000u];
    if (addr < 0x4000u) return read_mmu_bank(cpu, 1, (uint16_t)(addr - 0x2000u));
    if (addr < 0x6000u) return read_mmu_bank(cpu, 2, (uint16_t)(addr - 0x4000u));
    if (addr < 0x8000u) return read_mmu_bank(cpu, 3, (uint16_t)(addr - 0x6000u));
    if (addr < 0xA000u) return read_mmu_bank(cpu, 4, (uint16_t)(addr - 0x8000u));
    if (addr < 0xE000u) return 0x00u; /* VRAM is write-only on SM8521 */
    return cpu->nvram[addr - 0xE000u];
}

static void program_write_byte(SM8521 *cpu, uint16_t addr, uint8_t data) {
    if (addr < 0x0400u) {
        if (addr >= 0x0014u && addr <= 0x0017u) {
            if (addr == SM8521_P3) {
                int pc_bank_idx = cpu->m_PC >> 13;
                bool in_bios_slot_sel = (cpu->mmu[1] == 2 && cpu->m_PC >= 0x3FC0u && cpu->m_PC <= 0x3FD5u);
                bool in_cart_space = !in_bios_slot_sel &&
                    ((cpu->m_PC >= 0x2000u && cpu->m_PC < 0xA000u && cpu->mmu[pc_bank_idx] >= 0x20u) ||
                     cpu->mmu[3] >= 0x20u || cpu->mmu[4] >= 0x20u);

                if (!in_cart_space) {
                    if ((data & 0xC0u) == 0x40u) cpu->latched_cart_slot = 1;
                    else if ((data & 0xC0u) == 0x80u) cpu->latched_cart_slot = 2;
                } else if (cpu->dual_cart_slot_fix) {
                    uint8_t slot_bits = (cpu->latched_cart_slot == 2) ? 0x80u : 0x40u;
                    data = (uint8_t)((data & 0x3Fu) | slot_bits);
                }
                data &= (uint8_t)~0x0Cu;
            }
            cpu->m_p_ram[addr] = data;
            if (addr == SM8521_P1 || addr == SM8521_P2) {
                uint16_t mux = (uint16_t)(cpu->m_p_ram[SM8521_P1] | (cpu->m_p_ram[SM8521_P2] << 8));
                handle_input_press(cpu, mux);
            }
            return;
        }

        if (addr >= 0x0020u && addr <= 0x007Fu) {
            switch (addr) {
                case SM8521_MMU1: cpu->mmu[1] = data; if (data >= 0x20u) cpu->cartridge_executed_this_frame = true; break;
                case SM8521_MMU2: cpu->mmu[2] = data; if (data >= 0x20u) cpu->cartridge_executed_this_frame = true; break;
                case SM8521_MMU3: cpu->mmu[3] = data; if (data >= 0x20u) cpu->cartridge_executed_this_frame = true; break;
                case SM8521_MMU4: cpu->mmu[4] = data; if (data >= 0x20u) cpu->cartridge_executed_this_frame = true; break;
                case SM8521_LCDC: {
                    uint8_t new_page_bit = data & 0x40u;
                    if (!cpu->vsync_enabled && new_page_bit != cpu->last_lcdc_page_bit) {
                        uint32_t page_off = new_page_bit ? 0x2000u : 0x0000u;
                        memcpy(cpu->committed_vram_page, &cpu->videoram[page_off], 0x2000u);
                        cpu->committed_vram_valid = true;
                        cpu->last_lcdc_page_bit = new_page_bit;
                    } else {
                        cpu->last_lcdc_page_bit = new_page_bit;
                    }
                    break;
                }
                case SM8521_URTT:
                    cpu->m_p_ram[SM8521_URTT] = data;
                    cpu->m_p_ram[SM8521_URTS] |= 0x42u;
                    return;
                case SM8521_URTS:
                    data = (uint8_t)((cpu->m_p_ram[SM8521_URTS] & 0x43u) | (data & (uint8_t)~0x5Fu));
                    break;
                case SM8521_DMBR: data &= 0x7Fu; break;
                case SM8521_TM0D:
                    cpu->m_timer[0].upcounter_max = data;
                    data = 0;
                    break;
                case SM8521_TM0C:
                    cpu->m_timer[0].enabled = (data & 0x80u) != 0;
                    cpu->m_timer[0].prescale_max = gamecom_timer_limit[data & 0x07u];
                    cpu->m_timer[0].prescale_count = 0;
                    cpu->m_p_ram[SM8521_TM0D] = 0;
                    break;
                case SM8521_TM1D:
                    cpu->m_timer[1].upcounter_max = data;
                    data = 0;
                    break;
                case SM8521_TM1C:
                    cpu->m_timer[1].enabled = (data & 0x80u) != 0;
                    if (data == 0x81u && cpu->m_p_ram[0x0102] == 0x5Fu && cpu->m_p_ram[0x0103] == 0x37u) {
                        cpu->m_timer[1].prescale_max = 16;
                    } else {
                        cpu->m_timer[1].prescale_max = gamecom_timer_limit[data & 0x07u];
                    }
                    cpu->m_timer[1].prescale_count = 0;
                    cpu->m_p_ram[SM8521_TM1D] = 0;
                    break;
                case SM8521_SGC:
                    cpu->m_sound.sgc = data;
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG0L:
                    cpu->m_sound.sg0l = data;
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG1L:
                    cpu->m_sound.sg1l = data;
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG0TH:
                    cpu->m_sound.sg0t = (uint16_t)((cpu->m_sound.sg0t & 0x00FFu) | ((uint16_t)data << 8));
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG0TL:
                    cpu->m_sound.sg0t = (uint16_t)((cpu->m_sound.sg0t & 0xFF00u) | data);
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG1TH:
                    cpu->m_sound.sg1t = (uint16_t)((cpu->m_sound.sg1t & 0x00FFu) | ((uint16_t)data << 8));
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG1TL:
                    cpu->m_sound.sg1t = (uint16_t)((cpu->m_sound.sg1t & 0xFF00u) | data);
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG2L:
                    cpu->m_sound.sg2l = data;
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG2TH:
                    cpu->m_sound.sg2t = (uint16_t)((cpu->m_sound.sg2t & 0x00FFu) | ((uint16_t)data << 8));
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SG2TL:
                    cpu->m_sound.sg2t = (uint16_t)((cpu->m_sound.sg2t & 0xFF00u) | data);
                    if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    break;
                case SM8521_SGDA: {
                    uint8_t prev_sgda = cpu->m_sound.sgda;
                    cpu->m_sound.sgda = data;
                    if ((cpu->m_sound.sgc & 0x8Fu) == 0x88u && cpu->on_dac_sample) {
                        uint64_t delta = cpu->cycles - cpu->last_dac_cycle;
                        cpu->last_dac_cycle = cpu->cycles;
                        if (delta >= 64) {
                            uint32_t tm1d = cpu->m_timer[1].upcounter_max ? cpu->m_timer[1].upcounter_max : 208u;
                            uint32_t prescale = cpu->m_timer[1].prescale_max ? cpu->m_timer[1].prescale_max : 1u;
                            uint32_t div = cpu->m_p_ram[0xAC] ? cpu->m_p_ram[0xAC] : 3u;
                            uint32_t period_cycles = tm1d * prescale * div;
                            if (period_cycles < 128u) period_cycles = 128u;
                            uint32_t hw_rate = (uint32_t)((SM8521_CPU_CLOCK_HZ + period_cycles / 2u) / period_cycles);
                            if (hw_rate < 4000u) hw_rate = 4000u;
                            if (hw_rate > 22050u) hw_rate = 22050u;
                            if (delta > (uint64_t)period_cycles * 3u / 2u && delta <= (uint64_t)period_cycles * 48u) {
                                uint32_t hold = (uint32_t)((delta + period_cycles / 2u) / period_cycles);
                                if (hold > 48u) hold = 48u;
                                for (uint32_t h = 1; h < hold; h++) {
                                    cpu->on_dac_sample(cpu->cb_userdata, prev_sgda, hw_rate);
                                }
                            }
                            cpu->on_dac_sample(cpu->cb_userdata, data, hw_rate);
                        }
                    }
                    break;
                }
                default:
                    if (addr >= SM8521_SG0W0 && addr < SM8521_SG0W0 + 16u) {
                        cpu->m_sound.sg0w[addr - SM8521_SG0W0] = data;
                        if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    } else if (addr >= SM8521_SG1W0 && addr < SM8521_SG1W0 + 16u) {
                        cpu->m_sound.sg1w[addr - SM8521_SG1W0] = data;
                        if (cpu->on_sound_reg_write) cpu->on_sound_reg_write(cpu->cb_userdata, &cpu->m_sound, cpu->cycles);
                    }
                    break;
            }
            cpu->m_p_ram[addr] = data;
            return;
        }
        cpu->m_p_ram[addr] = data;
        return;
    }

    if (addr >= 0xA000u && addr < 0xE000u) {
        cpu->videoram[addr - 0xA000u] = data;
        return;
    }
    if (addr >= 0xE000u) {
        cpu->nvram[addr - 0xE000u] = data;
    }
}

uint8_t sm8521_mem_readbyte(SM8521 *cpu, uint16_t offset) {
    if (offset < 0x10u) {
        return cpu->m_register_ram[offset + (cpu->m_PS0 & 0xF8u)];
    }
    return program_read_byte(cpu, offset);
}

void sm8521_mem_writebyte(SM8521 *cpu, uint16_t offset, uint8_t data) {
    if (offset < 0x10u) {
        cpu->m_register_ram[offset + (cpu->m_PS0 & 0xF8u)] = data;
    }
    program_write_byte(cpu, offset, data);
    switch (offset) {
        case 0x10: cpu->m_IE0 = data; break;
        case 0x11: cpu->m_IE1 = data; break;
        case 0x12: cpu->m_IR0 = data; break;
        case 0x13: cpu->m_IR1 = data; break;
        case 0x19: cpu->m_SYS = data; break;
        case 0x1A: cpu->m_CKC = data; break;
        case 0x1C:
        case 0x1D: get_sp(cpu); break;
        case 0x1B:
        case 0x1E:
            cpu->m_PS0 = data;
            cpu->m_p_ram[0x1B] = data;
            cpu->m_p_ram[0x1E] = data;
            for (uint16_t i = 0; i < 16u; i++) {
                program_write_byte(cpu, i, sm8521_mem_readbyte(cpu, i));
            }
            break;
        case 0x1F: cpu->m_PS1 = data; break;
    }
}

uint16_t sm8521_mem_readword(SM8521 *cpu, uint16_t offset) {
    uint8_t hi = sm8521_mem_readbyte(cpu, offset);
    uint8_t lo = sm8521_mem_readbyte(cpu, (uint16_t)(offset + 1u));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

void sm8521_mem_writeword(SM8521 *cpu, uint16_t offset, uint16_t value) {
    sm8521_mem_writebyte(cpu, offset, (uint8_t)((value >> 8) & 0xFFu));
    sm8521_mem_writebyte(cpu, (uint16_t)(offset + 1u), (uint8_t)(value & 0xFFu));
}

static void gamecom_handle_dma(SM8521 *cpu) {
    uint8_t dmc = cpu->m_p_ram[SM8521_DMC];
    if (!(dmc & 0x80u)) return;

    cpu->dma_count++;
    bool overwrite_mode = (dmc & 0x01u) != 0;
    uint8_t transfer_mode = dmc & 0x06u;
    int adjust_x = (dmc & 0x08u) ? -1 : 1;
    bool decrement_y = (dmc & 0x10u) != 0;
    uint8_t block_width = cpu->m_p_ram[SM8521_DMDX];
    uint8_t block_height = cpu->m_p_ram[SM8521_DMDY];
    uint8_t source_x = cpu->m_p_ram[SM8521_DMX1];
    int source_x_current = source_x & 3;
    uint8_t source_y = cpu->m_p_ram[SM8521_DMY1];
    uint32_t source_width = (cpu->m_p_ram[SM8521_LCH] & 0x20u) ? 50u : 40u;
    uint32_t dest_width = source_width;
    uint8_t dest_x = cpu->m_p_ram[SM8521_DMX2];
    int dest_x_current = dest_x & 3;
    uint8_t dest_y = cpu->m_p_ram[SM8521_DMY2];
    uint8_t palette = cpu->m_p_ram[SM8521_DMPL];
    uint32_t source_mask = 0x1FFFu;
    uint32_t dest_mask = 0x1FFFu;

    uint32_t src_vram_off = (cpu->m_p_ram[SM8521_DMVP] & 0x01u) ? 0x2000u : 0x0000u;
    uint32_t dst_vram_off = (cpu->m_p_ram[SM8521_DMVP] & 0x02u) ? 0x2000u : 0x0000u;

    const uint8_t *source_bank = cpu->videoram;
    uint32_t source_base = src_vram_off;
    uint8_t *dest_bank = cpu->videoram;
    uint32_t dest_base = dst_vram_off;

    switch (transfer_mode) {
        case 0x00: break;
        case 0x02: {
            source_width = 64u;
            source_mask = 0x3FFFu;
            uint8_t dmbr = cpu->m_p_ram[SM8521_DMBR];
            if (dmbr < 16u) {
                source_bank = cpu->external_rom;
                source_base = ((uint32_t)dmbr << 14) & 0x3FFFFu;
            } else {
                uint8_t *cart = get_active_cart(cpu);
                if (cart) {
                    source_bank = cart;
                    source_base = ((uint32_t)dmbr << 14) & 0x1FFFFFu;
                } else {
                    source_bank = cpu->external_rom;
                    source_base = 0;
                }
            }
            break;
        }
        case 0x04:
            source_width = 64u;
            source_bank = cpu->nvram;
            source_base = 0;
            break;
        case 0x06:
            dest_width = 64u;
            dest_bank = cpu->nvram;
            dest_base = 0;
            break;
    }

    uint16_t source_current = (uint16_t)(source_width * source_y + (source_x >> 2));
    uint16_t dest_current = (uint16_t)(dest_width * dest_y + (dest_x >> 2));
    uint16_t source_line = source_current;
    uint16_t dest_line = dest_current;

    for (int y_count = 0; y_count <= (int)block_height; y_count++) {
        uint8_t cur_src_y = (uint8_t)((source_y + (decrement_y ? -y_count : y_count)) & 0xFF);
        for (int x_count = 0; x_count <= (int)block_width; x_count++) {
            uint32_t src_addr;
            int src_adj;
            if (source_width == 64u) {
                uint8_t cur_src_x = (uint8_t)((source_x + x_count * adjust_x) & 0xFF);
                src_addr = source_base + ((((uint32_t)cur_src_y << 6) | (cur_src_x >> 2)) & source_mask);
                src_adj = ((cur_src_x & 3) ^ 3) << 1;
            } else {
                src_addr = source_base + (source_current & source_mask);
                src_adj = (source_x_current ^ 3) << 1;
            }
            uint32_t dst_addr = dest_base + (dest_current & dest_mask);
            int dst_adj = (dest_x_current ^ 3) << 1;

            uint8_t source_pixel = (uint8_t)((source_bank[src_addr] >> src_adj) & 3u);
            if (overwrite_mode || source_pixel != 0) {
                uint8_t other = (uint8_t)(dest_bank[dst_addr] & ~(3u << dst_adj));
                dest_bank[dst_addr] = (uint8_t)(other | (((palette >> (source_pixel << 1)) & 3u) << dst_adj));
            }

            source_x_current = (source_x_current + adjust_x) & 0xFF;
            if (source_x_current & 0x04) {
                source_current = (uint16_t)(source_current + adjust_x);
                source_x_current &= 3;
            }
            dest_x_current++;
            if (dest_x_current & 0x04) {
                dest_current = (uint16_t)(dest_current + 1u);
                dest_x_current &= 3;
            }
        }
        source_x_current = source_x & 3;
        dest_x_current = dest_x & 3;
        if (decrement_y) source_line = (uint16_t)(source_line - source_width);
        else source_line = (uint16_t)(source_line + source_width);
        source_current = source_line;
        dest_line = (uint16_t)(dest_line + dest_width);
        dest_current = dest_line;
    }

    cpu->m_p_ram[SM8521_DMC] &= 0x7Fu;
    cpu->m_halted = 0;
    set_input_line(cpu, SM8521_DMA_INT, true);
}

static void gamecom_update_timers(SM8521 *cpu, uint32_t cycles) {
    if (cpu->m_timer[0].enabled) {
        cpu->m_timer[0].prescale_count += cycles;
        uint32_t limit = cpu->m_timer[0].upcounter_max ? cpu->m_timer[0].upcounter_max : 256u;
        while (cpu->m_timer[0].prescale_count >= cpu->m_timer[0].prescale_max) {
            cpu->m_timer[0].prescale_count -= cpu->m_timer[0].prescale_max;
            uint32_t next_cnt = (uint32_t)cpu->m_p_ram[SM8521_TM0D] + 1u;
            if (next_cnt >= limit) {
                cpu->m_p_ram[SM8521_TM0D] = 0;
                if (cpu->m_p_ram[SM8521_IE0] & 0x40u) {
                    set_input_line(cpu, SM8521_TIM0_INT, true);
                }
            } else {
                cpu->m_p_ram[SM8521_TM0D] = (uint8_t)next_cnt;
            }
        }
    }

    if (cpu->m_timer[1].enabled) {
        cpu->m_timer[1].prescale_count += cycles;
        uint32_t limit = cpu->m_timer[1].upcounter_max ? cpu->m_timer[1].upcounter_max : 256u;
        while (cpu->m_timer[1].prescale_count >= cpu->m_timer[1].prescale_max) {
            cpu->m_timer[1].prescale_count -= cpu->m_timer[1].prescale_max;
            uint32_t next_cnt = (uint32_t)cpu->m_p_ram[SM8521_TM1D] + 1u;
            if (next_cnt >= limit) {
                cpu->m_p_ram[SM8521_TM1D] = 0;
                if (cpu->m_p_ram[SM8521_IE1] & 0x40u) {
                    set_input_line(cpu, SM8521_TIM1_INT, true);
                }
            } else {
                cpu->m_p_ram[SM8521_TM1D] = (uint8_t)next_cnt;
            }
        }
    }

    cpu->clock_timer_accum_cycles += cycles;
    while (cpu->clock_timer_accum_cycles >= SM8521_CPU_CLOCK_HZ) {
        cpu->clock_timer_accum_cycles -= SM8521_CPU_CLOCK_HZ;
        if (cpu->m_p_ram[SM8521_IE1] & 0x10u) {
            set_input_line(cpu, SM8521_CK_INT, true);
        }
    }
}

static void render_scanline(SM8521 *cpu) {
    uint16_t x = cpu->m_scanline;
    uint8_t lcdc = cpu->m_p_ram[SM8521_LCDC];
    uint32_t base_addr = (lcdc & 0x40u) ? 0x2000u : 0x0000u;
    bool normal_black = (lcdc & 0x01u) != 0;

    if (!(lcdc & 0x80u)) {
        uint8_t rest = normal_black ? 3u : 0u;
        for (int y = 0; y < SM8521_SCREEN_HEIGHT; y++) {
            cpu->display_buffer[y * SM8521_SCREEN_WIDTH + x] = rest;
        }
    } else {
        bool use_snap = cpu->committed_vram_valid && ((lcdc & 0x40u) == cpu->last_lcdc_page_bit);
        const uint8_t *vram_src = use_snap ? cpu->committed_vram_page : cpu->videoram;
        uint32_t line_off = (use_snap ? 0u : base_addr) + 40u * x;

        for (int i = 0; i < 40; i++) {
            uint8_t p = vram_src[line_off + i];
            int y_base = i * 4;
            uint8_t s0 = (p >> 6) & 3u;
            uint8_t s1 = (p >> 4) & 3u;
            uint8_t s2 = (p >> 2) & 3u;
            uint8_t s3 = p & 3u;
            if (normal_black) {
                s0 = (uint8_t)(3u - s0);
                s1 = (uint8_t)(3u - s1);
                s2 = (uint8_t)(3u - s2);
                s3 = (uint8_t)(3u - s3);
            }
            cpu->display_buffer[(y_base + 0) * SM8521_SCREEN_WIDTH + x] = s0;
            cpu->display_buffer[(y_base + 1) * SM8521_SCREEN_WIDTH + x] = s1;
            cpu->display_buffer[(y_base + 2) * SM8521_SCREEN_WIDTH + x] = s2;
            cpu->display_buffer[(y_base + 3) * SM8521_SCREEN_WIDTH + x] = s3;
        }
    }

    cpu->m_scanline = (uint16_t)((cpu->m_scanline + 1u) % SM8521_SCREEN_WIDTH);
}

void sm8521_sync_datetime(SM8521 *cpu) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return;
    int year = t->tm_year + 1900;
    int month = t->tm_mon + 1;

    cpu->m_p_ram[0x0120] = 0xA5u;
    cpu->m_p_ram[0x0121] = 0xF0u;
    cpu->m_p_ram[0x0122] = 0xAAu;
    cpu->m_p_ram[0x0123] = 0x55u;
    cpu->m_p_ram[0x0124] = (uint8_t)(year % 100);
    cpu->m_p_ram[0x0125] = (uint8_t)month;
    cpu->m_p_ram[0x0126] = (uint8_t)t->tm_mday;
    cpu->m_p_ram[0x0127] = (uint8_t)t->tm_hour;
    cpu->m_p_ram[0x0128] = (uint8_t)t->tm_min;
    cpu->m_p_ram[0x0129] = (uint8_t)t->tm_sec;
    cpu->m_p_ram[0x03FF] &= (uint8_t)~0x08u;

    cpu->nvram[0xFEE2 - 0xE000] = (uint8_t)((year / 1000) % 10);
    cpu->nvram[0xFEE3 - 0xE000] = (uint8_t)((year / 100) % 10);
    cpu->nvram[0xFEE4 - 0xE000] = (uint8_t)((year / 10) % 10);
    cpu->nvram[0xFEE5 - 0xE000] = (uint8_t)(year % 10);
    cpu->nvram[0xFEE6 - 0xE000] = (uint8_t)((month / 10) % 10);
    cpu->nvram[0xFEE7 - 0xE000] = (uint8_t)(month % 10);
}

void sm8521_init(SM8521 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    memset(cpu->internal_rom, 0xFF, sizeof(cpu->internal_rom));
    memset(cpu->external_rom, 0xFF, sizeof(cpu->external_rom));
    cpu->dual_cart_slot_fix = true;
    sm8521_reset(cpu);
}

void sm8521_reset(SM8521 *cpu) {
    memset(cpu->m_register_ram, 0, sizeof(cpu->m_register_ram));
    memset(cpu->m_p_ram, 0, sizeof(cpu->m_p_ram));
    memset(cpu->videoram, 0, sizeof(cpu->videoram));
    memset(cpu->display_buffer, 0, sizeof(cpu->display_buffer));
    memset(cpu->committed_vram_page, 0, sizeof(cpu->committed_vram_page));

    cpu->m_PC = 0x1020;
    cpu->m_SP = 0x0000;
    cpu->m_PS0 = 0x00;
    cpu->m_PS1 = 0x00;
    cpu->m_SYS = 0x00;
    cpu->m_IE0 = 0x00;
    cpu->m_IE1 = 0x00;
    cpu->m_IR0 = 0x00;
    cpu->m_IR1 = 0x00;
    cpu->m_CKC = 0x00;
    cpu->m_clock_changed = 0;
    cpu->m_halted = 0;
    cpu->m_IFLAGS = 0;
    cpu->m_CheckInterrupts = 0;
    cpu->m_oldpc = 0x1020;
    cpu->cycles = 0;
    cpu->last_dac_cycle = 0;
    cpu->dma_count = 0;
    cpu->m_scanline = 0;
    cpu->scanline_cycle_accum = 0;
    cpu->clock_timer_accum_cycles = 0;
    cpu->cartridge_executed_this_frame = false;
    cpu->committed_vram_valid = false;
    cpu->last_lcdc_page_bit = 0;
    cpu->last_frame_cycles = 66240;
    cpu->last_hw_base_frame_cycles = 66240;

    memset(cpu->mmu, 0, sizeof(cpu->mmu));
    cpu->latched_cart_slot = 1;

    memset(cpu->m_timer, 0, sizeof(cpu->m_timer));
    cpu->m_timer[0].prescale_max = 1;
    cpu->m_timer[1].prescale_max = 1;
    memset(&cpu->m_sound, 0, sizeof(cpu->m_sound));

    sm8521_mem_writeword(cpu, 0x10, 0x0000);
    sm8521_mem_writeword(cpu, 0x12, 0x0000);
    sm8521_mem_writeword(cpu, 0x14, 0xFFFF);
    sm8521_mem_writeword(cpu, 0x16, 0xFF00);
    sm8521_mem_writebyte(cpu, 0x19, 0x00);
    sm8521_mem_writebyte(cpu, 0x1A, 0x00);
    sm8521_mem_writebyte(cpu, 0x1F, 0x00);
    cpu->m_p_ram[SM8521_URTT] = 0xFF;
    cpu->m_p_ram[SM8521_URTR] = 0x00;
    cpu->m_p_ram[SM8521_URTS] = 0x42;
    cpu->m_p_ram[SM8521_URTC] = 0x00;
    cpu->m_p_ram[SM8521_URTB] = 0x00;
    sm8521_mem_writebyte(cpu, 0x5F, 0x38);

    cpu->m_p_ram[0x00A5] = 0x02;
    cpu->m_p_ram[0x0100] = 0x3D;
    cpu->m_p_ram[0x0101] = 0x0E;
    cpu->m_register_ram[0x28 + 2] = 0x3D;
    cpu->m_register_ram[0x28 + 3] = 0x0E;

    cpu->m_p_ram[SM8521_LCH] = 0x07;
    cpu->m_p_ram[SM8521_LCV] = 0x27;
    cpu->m_p_ram[SM8521_LCDC] = 0xB0;

    sm8521_sync_datetime(cpu);
}

void sm8521_load_internal_bios(SM8521 *cpu, const uint8_t *data, size_t size) {
    memset(cpu->internal_rom, 0xFF, sizeof(cpu->internal_rom));
    if (data && size > 0) {
        size_t n = size < sizeof(cpu->internal_rom) ? size : sizeof(cpu->internal_rom);
        memcpy(cpu->internal_rom, data, n);
    }
}

void sm8521_load_external_bios(SM8521 *cpu, const uint8_t *data, size_t size) {
    memset(cpu->external_rom, 0xFF, sizeof(cpu->external_rom));
    if (data && size > 0) {
        size_t n = size < sizeof(cpu->external_rom) ? size : sizeof(cpu->external_rom);
        memcpy(cpu->external_rom, data, n);
        /* BIOS Calendar Sunday shade fix (Bank 3 $5421, offset 0x07421) */
        if (n > 0x07426u &&
            cpu->external_rom[0x07421] == 0x58 &&
            cpu->external_rom[0x07422] == 0x02 &&
            cpu->external_rom[0x07423] == 0x08 &&
            cpu->external_rom[0x07424] == 0x49 &&
            cpu->external_rom[0x07425] == 0x31 &&
            cpu->external_rom[0x07426] == 0x61) {
            cpu->external_rom[0x07422] = 0x03;
        }
    }
}

static bool has_tiger_sig(const uint8_t *buf, size_t buf_len, size_t base) {
    static const uint8_t sig[9] = { 'T','i','g','e','r','D','M','G','C' };
    if (base + 0x1Cu > buf_len) return false;
    return memcmp(buf + base + 0x05, sig, 9) == 0;
}

void sm8521_load_cartridge(SM8521 *cpu, const uint8_t *data, size_t size, int slot) {
    uint8_t *crt = (slot == 2) ? cpu->cart2_rom : cpu->cart1_rom;
    memset(crt, 0, 0x200000u);
    if (!data || size == 0) {
        if (slot == 2) cpu->has_cart2 = false;
        else cpu->has_cart1 = false;
        return;
    }

    size_t load_off = (size == 0x1C0000u) ? 0x40000u : 0u;
    size_t copy_len = size < (0x200000u - load_off) ? size : (0x200000u - load_off);
    memcpy(crt + load_off, data, copy_len);

    if (size != 0x1C0000u) {
        if (size <= 0x008000u) memcpy(crt + 0x008000u, crt, 0x008000u);
        if (size <= 0x010000u) memcpy(crt + 0x010000u, crt, 0x010000u);
        if (size <= 0x020000u) memcpy(crt + 0x020000u, crt, 0x020000u);
        if (size <= 0x040000u) memcpy(crt + 0x040000u, crt, 0x040000u);
        if (size <= 0x080000u) memcpy(crt + 0x080000u, crt, 0x080000u);
        if (size <= 0x100000u) memcpy(crt + 0x100000u, crt, 0x100000u);
    }

    static const size_t probes[4] = { 0x40000u, 0x80000u, 0x100000u, 0x00000u };
    bool any_valid = false;
    for (int i = 0; i < 4; i++) {
        if (has_tiger_sig(crt, 0x200000u, probes[i])) {
            any_valid = true;
            break;
        }
    }
    if (!any_valid) {
        for (size_t off = 0; off + 0x2000u <= size; off += 0x2000u) {
            if (has_tiger_sig(data, size, off)) {
                memcpy(crt + 0x40000u, data + off, 0x2000u);
                break;
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        if (has_tiger_sig(crt, 0x200000u, probes[i])) {
            crt[probes[i] + 0x04] |= 0x03u;
        }
    }

    if (slot == 2) cpu->has_cart2 = true;
    else cpu->has_cart1 = true;
}

void sm8521_unload_cartridge(SM8521 *cpu, int slot) {
    if (slot == 2) {
        cpu->has_cart2 = false;
        memset(cpu->cart2_rom, 0, sizeof(cpu->cart2_rom));
    } else {
        cpu->has_cart1 = false;
        memset(cpu->cart1_rom, 0, sizeof(cpu->cart1_rom));
    }
}

void sm8521_set_input(SM8521 *cpu, uint32_t buttons_mask, bool stylus_pressed, int16_t stylus_x, int16_t stylus_y) {
    cpu->buttons_mask = buttons_mask;
    cpu->stylus_pressed = stylus_pressed;
    cpu->stylus_x = stylus_x;
    cpu->stylus_y = stylus_y;
}

uint32_t sm8521_step(SM8521 *cpu) {
    uint32_t mycycles = 0;
    uint32_t r1 = 0, r2 = 0;
    uint32_t s1 = 0, s2 = 0;
    uint32_t d1 = 0, d2 = 0;
    uint32_t res = 0;

    process_interrupts(cpu);

    if (!cpu->m_halted) {
        if (cpu->m_PC >= 0x2000u && cpu->m_PC < 0xA000u && cpu->mmu[cpu->m_PC >> 13] >= 0x20u) {
            cpu->cartridge_executed_this_frame = true;
        }

        /* Suppress BIOS "DATA FOUND.. PRESS A TO KEEP / PRESS B TO CLEAR" ($5E11 in Bank 3) */
        if (cpu->m_PC == 0x5E11u && cpu->mmu[2] == 3) {
            if (cpu->allow_data_found_prompt) {
                cpu->allow_data_found_prompt = false;
            } else {
                get_sp(cpu);
                cpu->m_SYS = cpu->m_p_ram[0x19];
                cpu->m_PS1 = (uint8_t)(cpu->m_p_ram[0x1F] & ~(FLAG_I | FLAG_C));
                uint8_t hi = sm8521_mem_readbyte(cpu, cpu->m_SP);
                cpu->m_SP = (uint16_t)(cpu->m_SP + 1u);
                if ((cpu->m_SYS & 0x40u) == 0) cpu->m_SP &= 0xFFu;
                uint8_t lo = sm8521_mem_readbyte(cpu, cpu->m_SP);
                cpu->m_SP = (uint16_t)(cpu->m_SP + 1u);
                if ((cpu->m_SYS & 0x40u) == 0) cpu->m_SP &= 0xFFu;
                cpu->m_PC = (uint16_t)(((uint16_t)hi << 8) | lo);
                if (cpu->m_SYS & 0x40u) program_write_byte(cpu, 0x1C, (uint8_t)((cpu->m_SP >> 8) & 0xFFu));
                program_write_byte(cpu, 0x1D, (uint8_t)(cpu->m_SP & 0xFFu));
                program_write_byte(cpu, 0x1F, cpu->m_PS1);
            }
        }

        /* Intercept BIOS Digitized Waveform Stream start ($2354 in Bank 2) */
        if (cpu->m_PC == 0x2354u && cpu->mmu[1] == 2 && cpu->on_bios_audio_clip) {
            uint8_t clip_bank = cpu->m_register_ram[0x38 + 4];
            uint16_t clip_addr = (uint16_t)(((uint16_t)cpu->m_register_ram[0x38 + 2] << 8) | cpu->m_register_ram[0x38 + 3]);
            uint16_t clip_blocks = (uint16_t)(((uint16_t)cpu->m_register_ram[0x38 + 6] << 8) | cpu->m_register_ram[0x38 + 7]);
            size_t byte_len = (size_t)clip_blocks * 32u;
            if (byte_len > 0 && byte_len <= 0x10000u && clip_addr >= 0x6000u && clip_addr < 0x8000u) {
                const uint8_t *rom_src = (clip_bank < 0x20u) ? cpu->external_rom : get_active_cart(cpu);
                if (rom_src) {
                    static uint8_t clip_buf[0x10000];
                    uint32_t start_off = ((uint32_t)clip_bank << 13) + (clip_addr - 0x6000u);
                    uint32_t mask = (clip_bank < 0x20u) ? 0x3FFFFu : 0x1FFFFFu;
                    for (size_t i = 0; i < byte_len; i++) {
                        clip_buf[i] = rom_src[(start_off + i) & mask];
                    }
                    cpu->on_bios_audio_clip(cpu->cb_userdata, clip_buf, byte_len, clip_bank);
                }
            }
        }

        /* Sync RTC & Calendar once on cold boot right after BIOS init */
        if (cpu->m_PC == 0x20D0u && cpu->mmu[1] == 2) {
            sm8521_sync_datetime(cpu);
        }

        cpu->m_oldpc = cpu->m_PC;
        uint8_t op = sm8521_mem_readbyte(cpu, cpu->m_PC);
        cpu->m_PC = (uint16_t)(cpu->m_PC + 1u);
        cpu->m_SYS = cpu->m_p_ram[0x19];
        cpu->m_PS0 = cpu->m_p_ram[0x1E];
        cpu->m_PS1 = cpu->m_p_ram[0x1F];
        get_sp(cpu);

        switch (op) {
#include "sm8521_ops.inc"
        }

        if (cpu->m_SYS & 0x40u) {
            program_write_byte(cpu, 0x1C, (uint8_t)((cpu->m_SP >> 8) & 0xFFu));
        }
        program_write_byte(cpu, 0x1D, (uint8_t)(cpu->m_SP & 0xFFu));
        sm8521_mem_writebyte(cpu, 0x1E, cpu->m_PS0);
        program_write_byte(cpu, 0x1F, cpu->m_PS1);
    } else {
        uint8_t dmc = cpu->m_p_ram[SM8521_DMC];
        if (dmc & 0x80u) {
            uint32_t bw = (uint32_t)cpu->m_p_ram[SM8521_DMDX] + 1u;
            uint32_t bh = (uint32_t)cpu->m_p_ram[SM8521_DMDY] + 1u;
            uint32_t dma_cyc = (bw * bh) >> 1;
            if (dma_cyc < 4u) dma_cyc = 4u;
            if (dma_cyc > 256u) dma_cyc = 256u;
            mycycles = dma_cyc;
            gamecom_handle_dma(cpu);
        } else {
            mycycles = 4u;
        }
    }

    gamecom_update_timers(cpu, mycycles);
    cpu->cycles += mycycles;
    return mycycles;
}

void sm8521_run_frame(SM8521 *cpu) {
    cpu->cartridge_executed_this_frame =
        cpu->mmu[1] >= 0x20u || cpu->mmu[2] >= 0x20u ||
        cpu->mmu[3] >= 0x20u || cpu->mmu[4] >= 0x20u;

    bool lcd_enabled = (cpu->m_p_ram[SM8521_LCDC] & 0x80u) != 0;
    uint8_t lch = cpu->m_p_ram[SM8521_LCH];
    uint8_t lcv = cpu->m_p_ram[SM8521_LCV];

    uint32_t scanline_cycles = (lch == 0 && !lcd_enabled)
        ? 320u
        : (uint32_t)(((lch & 0x20u) ? 50u : 40u) * ((lch & 0x1Fu) + 1u));
    if (scanline_cycles < 200u) scanline_cycles = 200u;

    uint32_t vblank_lines = (lcv == 0 && !lcd_enabled) ? 7u : (uint32_t)(lcv & 0x0Fu);
    if (vblank_lines < 1u) vblank_lines = 1u;

    uint32_t line_cycles = 0;
    for (int line = 0; line < SM8521_SCREEN_WIDTH; line++) {
        while (line_cycles < scanline_cycles) {
            line_cycles += sm8521_step(cpu);
        }
        line_cycles -= scanline_cycles;
        if (!cpu->vsync_enabled) {
            render_scanline(cpu);
        }
    }
    cpu->committed_vram_valid = false;

    if (cpu->vsync_enabled) {
        cpu->m_scanline = 0;
        for (int line = 0; line < SM8521_SCREEN_WIDTH; line++) {
            render_scanline(cpu);
        }
    }

    if (lcd_enabled) {
        set_input_line(cpu, SM8521_LCDC_INT, true);
    }

    for (uint32_t vline = 0; vline < vblank_lines; vline++) {
        while (line_cycles < scanline_cycles) {
            line_cycles += sm8521_step(cpu);
        }
        line_cycles -= scanline_cycles;
    }

    uint32_t total_lines = SM8521_SCREEN_WIDTH + vblank_lines;
    cpu->last_hw_base_frame_cycles = total_lines * scanline_cycles;
    cpu->last_frame_cycles = cpu->last_hw_base_frame_cycles;

    if (cpu->on_audio_frame_end) {
        cpu->on_audio_frame_end(cpu->cb_userdata, cpu->cycles);
    }
}

void sm8521_run_cycles(SM8521 *cpu, uint32_t target_cycles) {
    cpu->cartridge_executed_this_frame =
        cpu->mmu[1] >= 0x20u || cpu->mmu[2] >= 0x20u ||
        cpu->mmu[3] >= 0x20u || cpu->mmu[4] >= 0x20u;

    uint64_t end_cycles = cpu->cycles + target_cycles;

    while (cpu->cycles < end_cycles) {
        uint32_t c = sm8521_step(cpu);
        cpu->scanline_cycle_accum += c;

        bool lcd_enabled = (cpu->m_p_ram[SM8521_LCDC] & 0x80u) != 0;
        uint8_t lch = cpu->m_p_ram[SM8521_LCH];
        uint8_t lcv = cpu->m_p_ram[SM8521_LCV];

        uint32_t scanline_cycles = (lch == 0 && !lcd_enabled)
            ? 320u
            : (uint32_t)(((lch & 0x20u) ? 50u : 40u) * ((lch & 0x1Fu) + 1u));
        if (scanline_cycles < 200u) scanline_cycles = 200u;

        uint32_t vblank_lines = (lcv == 0 && !lcd_enabled) ? 7u : (uint32_t)(lcv & 0x0Fu);
        if (vblank_lines < 1u) vblank_lines = 1u;
        uint32_t total_lines = SM8521_SCREEN_WIDTH + vblank_lines;

        while (cpu->scanline_cycle_accum >= scanline_cycles) {
            cpu->scanline_cycle_accum -= scanline_cycles;

            if (cpu->m_scanline < SM8521_SCREEN_WIDTH) {
                if (!cpu->vsync_enabled) {
                    uint16_t cur_col = cpu->m_scanline;
                    render_scanline(cpu);
                    cpu->m_scanline = (uint16_t)(cur_col + 1u);
                } else {
                    cpu->m_scanline++;
                }

                if (cpu->m_scanline == SM8521_SCREEN_WIDTH) {
                    cpu->committed_vram_valid = false;
                    if (cpu->vsync_enabled) {
                        cpu->m_scanline = 0;
                        for (int line = 0; line < SM8521_SCREEN_WIDTH; line++) {
                            render_scanline(cpu);
                        }
                        cpu->m_scanline = SM8521_SCREEN_WIDTH;
                    }
                    if (lcd_enabled) {
                        set_input_line(cpu, SM8521_LCDC_INT, true);
                    }
                }
            } else {
                cpu->m_scanline++;
                if (cpu->m_scanline >= total_lines) {
                    cpu->m_scanline = 0;
                }
            }
        }
    }

    cpu->last_frame_cycles = target_cycles;
    if (cpu->on_audio_frame_end) {
        cpu->on_audio_frame_end(cpu->cb_userdata, cpu->cycles);
    }
}

/* Deterministic savestate container (excludes BIOS/cart ROMs and callback pointers) */
#define SM8521_STATE_MAGIC 0x43535031u /* "CSP1" */

typedef struct {
    uint32_t magic;
    uint16_t m_PC, m_SP, m_IFLAGS, m_oldpc, m_scanline;
    uint8_t  m_PS0, m_PS1, m_SYS, m_IE0, m_IE1, m_IR0, m_IR1, m_CKC;
    uint8_t  m_clock_changed, m_halted, m_CheckInterrupts;
    uint8_t  latched_cart_slot, last_lcdc_page_bit, committed_vram_valid;
    uint8_t  mmu[5];
    uint64_t cycles;
    uint64_t last_dac_cycle;
    uint32_t dma_count;
    uint32_t clock_timer_accum_cycles;
    uint32_t last_frame_cycles;
    uint32_t last_hw_base_frame_cycles;
    GameComTimer     m_timer[2];
    GameComAudioRegs m_sound;
    uint8_t  m_register_ram[0x108];
    uint8_t  m_p_ram[0x0400];
    uint8_t  videoram[0x4000];
    uint8_t  nvram[0x2000];
    uint8_t  committed_vram_page[0x2000];
    uint8_t  display_buffer[SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT];
} SM8521StateBlob;

size_t sm8521_serialize_size(void) {
    return sizeof(SM8521StateBlob);
}

bool sm8521_serialize(const SM8521 *cpu, void *dst, size_t size) {
    if (!dst || size < sizeof(SM8521StateBlob)) return false;
    SM8521StateBlob b;
    memset(&b, 0, sizeof(b));
    b.magic = SM8521_STATE_MAGIC;
    b.m_PC = cpu->m_PC;
    b.m_SP = cpu->m_SP;
    b.m_IFLAGS = cpu->m_IFLAGS;
    b.m_oldpc = cpu->m_oldpc;
    b.m_scanline = cpu->m_scanline;
    b.m_PS0 = cpu->m_PS0;
    b.m_PS1 = cpu->m_PS1;
    b.m_SYS = cpu->m_SYS;
    b.m_IE0 = cpu->m_IE0;
    b.m_IE1 = cpu->m_IE1;
    b.m_IR0 = cpu->m_IR0;
    b.m_IR1 = cpu->m_IR1;
    b.m_CKC = cpu->m_CKC;
    b.m_clock_changed = cpu->m_clock_changed;
    b.m_halted = cpu->m_halted;
    b.m_CheckInterrupts = cpu->m_CheckInterrupts;
    b.latched_cart_slot = cpu->latched_cart_slot;
    b.last_lcdc_page_bit = cpu->last_lcdc_page_bit;
    b.committed_vram_valid = cpu->committed_vram_valid ? 1u : 0u;
    memcpy(b.mmu, cpu->mmu, sizeof(b.mmu));
    b.cycles = cpu->cycles;
    b.last_dac_cycle = cpu->last_dac_cycle;
    b.dma_count = cpu->dma_count;
    b.clock_timer_accum_cycles = cpu->clock_timer_accum_cycles;
    b.last_frame_cycles = cpu->last_frame_cycles;
    b.last_hw_base_frame_cycles = cpu->last_hw_base_frame_cycles;
    memcpy(b.m_timer, cpu->m_timer, sizeof(b.m_timer));
    b.m_sound = cpu->m_sound;
    memcpy(b.m_register_ram, cpu->m_register_ram, sizeof(b.m_register_ram));
    memcpy(b.m_p_ram, cpu->m_p_ram, sizeof(b.m_p_ram));
    memcpy(b.videoram, cpu->videoram, sizeof(b.videoram));
    memcpy(b.nvram, cpu->nvram, sizeof(b.nvram));
    memcpy(b.committed_vram_page, cpu->committed_vram_page, sizeof(b.committed_vram_page));
    memcpy(b.display_buffer, cpu->display_buffer, sizeof(b.display_buffer));
    memcpy(dst, &b, sizeof(b));
    return true;
}

bool sm8521_unserialize(SM8521 *cpu, const void *src, size_t size) {
    if (!src || size < sizeof(SM8521StateBlob)) return false;
    SM8521StateBlob b;
    memcpy(&b, src, sizeof(b));
    if (b.magic != SM8521_STATE_MAGIC) return false;
    cpu->m_PC = b.m_PC;
    cpu->m_SP = b.m_SP;
    cpu->m_IFLAGS = b.m_IFLAGS;
    cpu->m_oldpc = b.m_oldpc;
    cpu->m_scanline = b.m_scanline % SM8521_SCREEN_WIDTH;
    cpu->m_PS0 = b.m_PS0;
    cpu->m_PS1 = b.m_PS1;
    cpu->m_SYS = b.m_SYS;
    cpu->m_IE0 = b.m_IE0;
    cpu->m_IE1 = b.m_IE1;
    cpu->m_IR0 = b.m_IR0;
    cpu->m_IR1 = b.m_IR1;
    cpu->m_CKC = b.m_CKC;
    cpu->m_clock_changed = b.m_clock_changed;
    cpu->m_halted = b.m_halted;
    cpu->m_CheckInterrupts = b.m_CheckInterrupts;
    cpu->latched_cart_slot = b.latched_cart_slot;
    cpu->last_lcdc_page_bit = b.last_lcdc_page_bit;
    cpu->committed_vram_valid = b.committed_vram_valid != 0;
    memcpy(cpu->mmu, b.mmu, sizeof(b.mmu));
    cpu->cycles = b.cycles;
    cpu->last_dac_cycle = b.last_dac_cycle;
    cpu->dma_count = b.dma_count;
    cpu->clock_timer_accum_cycles = b.clock_timer_accum_cycles;
    cpu->last_frame_cycles = b.last_frame_cycles;
    cpu->last_hw_base_frame_cycles = b.last_hw_base_frame_cycles;
    memcpy(cpu->m_timer, b.m_timer, sizeof(b.m_timer));
    cpu->m_sound = b.m_sound;
    memcpy(cpu->m_register_ram, b.m_register_ram, sizeof(b.m_register_ram));
    memcpy(cpu->m_p_ram, b.m_p_ram, sizeof(b.m_p_ram));
    memcpy(cpu->videoram, b.videoram, sizeof(b.videoram));
    memcpy(cpu->nvram, b.nvram, sizeof(b.nvram));
    memcpy(cpu->committed_vram_page, b.committed_vram_page, sizeof(b.committed_vram_page));
    memcpy(cpu->display_buffer, b.display_buffer, sizeof(b.display_buffer));
    return true;
}
