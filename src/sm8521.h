#ifndef CUBSPIELE_SM8521_H
#define CUBSPIELE_SM8521_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM8521_SCREEN_WIDTH   200
#define SM8521_SCREEN_HEIGHT  160
#define SM8521_CPU_CLOCK_HZ   4915200

/* SM8521 SFR addresses ($0010-$007F) */
#define SM8521_IE0        0x10
#define SM8521_IE1        0x11
#define SM8521_IR0        0x12
#define SM8521_IR1        0x13
#define SM8521_P0         0x14
#define SM8521_P1         0x15
#define SM8521_P2         0x16
#define SM8521_P3         0x17
#define SM8521_SYS        0x19
#define SM8521_CKC        0x1A
#define SM8521_PS0        0x1B
#define SM8521_SPH        0x1C
#define SM8521_SPL        0x1D
#define SM8521_PS0_MIRROR 0x1E
#define SM8521_PS1        0x1F

#define SM8521_MMU0       0x24
#define SM8521_MMU1       0x25
#define SM8521_MMU2       0x26
#define SM8521_MMU3       0x27
#define SM8521_MMU4       0x28

#define SM8521_URTT       0x2B
#define SM8521_URTR       0x2C
#define SM8521_URTS       0x2D
#define SM8521_URTC       0x2E
#define SM8521_URTB       0x2F

#define SM8521_LCDC       0x30
#define SM8521_LCH        0x31
#define SM8521_LCV        0x32
#define SM8521_DMC        0x34
#define SM8521_DMX1       0x35
#define SM8521_DMY1       0x36
#define SM8521_DMDX       0x37
#define SM8521_DMDY       0x38
#define SM8521_DMX2       0x39
#define SM8521_DMY2       0x3A
#define SM8521_DMPL       0x3B
#define SM8521_DMBR       0x3C
#define SM8521_DMVP       0x3D

#define SM8521_SGC        0x40
#define SM8521_SG0L       0x42
#define SM8521_SG1L       0x44
#define SM8521_SG0TH      0x46
#define SM8521_SG0TL      0x47
#define SM8521_SG1TH      0x48
#define SM8521_SG1TL      0x49
#define SM8521_SG2L       0x4A
#define SM8521_SG2TH      0x4C
#define SM8521_SG2TL      0x4D
#define SM8521_SGDA       0x4E
#define SM8521_SG0W0      0x60
#define SM8521_SG1W0      0x70

#define SM8521_TM0C       0x50
#define SM8521_TM0D       0x51
#define SM8521_TM1C       0x52
#define SM8521_TM1D       0x53

/* Interrupt lines */
#define SM8521_ILL_INT    0
#define SM8521_WDT_INT    1
#define SM8521_NMI_INT    2
#define SM8521_DMA_INT    3
#define SM8521_TIM0_INT   4
#define SM8521_EXT_INT    5
#define SM8521_UART_INT   6
#define SM8521_LCDC_INT   7
#define SM8521_TIM1_INT   8
#define SM8521_CK_INT     9
#define SM8521_PIO_INT    10

/* Button bitmask */
#define GAMECOM_BTN_UP     (1u << 0)
#define GAMECOM_BTN_DOWN   (1u << 1)
#define GAMECOM_BTN_LEFT   (1u << 2)
#define GAMECOM_BTN_RIGHT  (1u << 3)
#define GAMECOM_BTN_A      (1u << 4)
#define GAMECOM_BTN_B      (1u << 5)
#define GAMECOM_BTN_C      (1u << 6)
#define GAMECOM_BTN_D      (1u << 7)
#define GAMECOM_BTN_MENU   (1u << 8)
#define GAMECOM_BTN_PAUSE  (1u << 9)
#define GAMECOM_BTN_SOUND  (1u << 10)
#define GAMECOM_BTN_POWER  (1u << 11)
#define GAMECOM_BTN_RESET  (1u << 12)

typedef struct {
    bool     enabled;
    uint16_t upcounter_max;
    uint32_t prescale_max;
    uint32_t prescale_count;
} GameComTimer;

typedef struct {
    uint8_t  sgc;
    uint8_t  sg0l;
    uint8_t  sg1l;
    uint8_t  sg2l;
    uint16_t sg0t;
    uint16_t sg1t;
    uint16_t sg2t;
    uint8_t  sgda;
    uint8_t  sg0w[16];
    uint8_t  sg1w[16];
} GameComAudioRegs;

typedef struct SM8521 SM8521;

typedef void (*sm8521_dac_cb_t)(void *userdata, uint8_t val, uint32_t hw_rate);
typedef void (*sm8521_sound_reg_cb_t)(void *userdata, const GameComAudioRegs *regs, uint64_t cycles);
typedef void (*sm8521_audio_frame_cb_t)(void *userdata, uint64_t cycles);
typedef void (*sm8521_bios_clip_cb_t)(void *userdata, const uint8_t *samples, size_t len, uint8_t bank);

struct SM8521 {
    /* CPU registers */
    uint16_t m_PC;
    uint16_t m_SP;
    uint8_t  m_PS0;
    uint8_t  m_PS1;
    uint8_t  m_SYS;
    uint8_t  m_IE0;
    uint8_t  m_IE1;
    uint8_t  m_IR0;
    uint8_t  m_IR1;
    uint8_t  m_CKC;
    uint8_t  m_clock_changed;
    uint8_t  m_halted;
    uint16_t m_IFLAGS;
    uint8_t  m_CheckInterrupts;
    uint16_t m_oldpc;
    uint64_t cycles;
    uint64_t last_dac_cycle;
    uint32_t dma_count;

    /* Memory regions */
    uint8_t  m_register_ram[0x108];
    uint8_t  m_p_ram[0x0400];
    uint8_t  videoram[0x4000];
    uint8_t  nvram[0x2000];
    uint8_t  internal_rom[0x1000];
    uint8_t  external_rom[0x40000];

    /* Cartridge ROMs (2MB mirrored buffers; owned by struct, excluded from savestates) */
    uint8_t  cart1_rom[0x200000];
    uint8_t  cart2_rom[0x200000];
    bool     has_cart1;
    bool     has_cart2;
    uint8_t  latched_cart_slot;
    bool     dual_cart_slot_fix;

    /* MMU banks 0..4 */
    uint8_t  mmu[5];

    /* Timers & 1Hz clock */
    GameComTimer m_timer[2];
    uint32_t clock_timer_accum_cycles;

    /* Sound registers */
    GameComAudioRegs m_sound;

    /* LCD scanline & framebuffer (200x160 2-bpp shade values 0..3) */
    uint16_t m_scanline;
    uint8_t  display_buffer[SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT];
    uint8_t  committed_vram_page[0x2000];
    bool     committed_vram_valid;
    uint8_t  last_lcdc_page_bit;
    bool     vsync_enabled;
    uint32_t last_frame_cycles;
    uint32_t last_hw_base_frame_cycles;
    bool     cartridge_executed_this_frame;
    bool     allow_data_found_prompt;

    /* Input state */
    uint32_t buttons_mask;
    bool     stylus_pressed;
    int16_t  stylus_x;
    int16_t  stylus_y;

    /* Callbacks (not serialized) */
    void                   *cb_userdata;
    sm8521_dac_cb_t         on_dac_sample;
    sm8521_sound_reg_cb_t   on_sound_reg_write;
    sm8521_audio_frame_cb_t on_audio_frame_end;
    sm8521_bios_clip_cb_t   on_bios_audio_clip;
};

void sm8521_init(SM8521 *cpu);
void sm8521_reset(SM8521 *cpu);
void sm8521_sync_datetime(SM8521 *cpu);

void sm8521_load_internal_bios(SM8521 *cpu, const uint8_t *data, size_t size);
void sm8521_load_external_bios(SM8521 *cpu, const uint8_t *data, size_t size);
void sm8521_load_cartridge(SM8521 *cpu, const uint8_t *data, size_t size, int slot);
void sm8521_unload_cartridge(SM8521 *cpu, int slot);

void sm8521_set_input(SM8521 *cpu, uint32_t buttons_mask, bool stylus_pressed, int16_t stylus_x, int16_t stylus_y);
uint32_t sm8521_step(SM8521 *cpu);
void sm8521_run_frame(SM8521 *cpu);

uint8_t  sm8521_mem_readbyte(SM8521 *cpu, uint16_t offset);
void     sm8521_mem_writebyte(SM8521 *cpu, uint16_t offset, uint8_t data);
uint16_t sm8521_mem_readword(SM8521 *cpu, uint16_t offset);
void     sm8521_mem_writeword(SM8521 *cpu, uint16_t offset, uint16_t value);

/* Serialization (deterministic savestate without ROMs or function pointers) */
size_t sm8521_serialize_size(void);
bool   sm8521_serialize(const SM8521 *cpu, void *dst, size_t size);
bool   sm8521_unserialize(SM8521 *cpu, const void *src, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CUBSPIELE_SM8521_H */
