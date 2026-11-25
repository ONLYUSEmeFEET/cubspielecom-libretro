#include "libretro.h"
#include "sm8521.h"
#include "lcd.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

static void test_ps0_mirror_and_vram(void) {
    SM8521 *cpu = (SM8521 *)calloc(1, sizeof(SM8521));
    sm8521_init(cpu);

    sm8521_mem_writebyte(cpu, SM8521_PS0, 0x28);
    EXPECT(cpu->m_PS0 == 0x28, "PS0 updated via $001B");
    EXPECT(cpu->m_p_ram[SM8521_PS0] == 0x28, "$001B synced");
    EXPECT(cpu->m_p_ram[SM8521_PS0_MIRROR] == 0x28, "$001E mirror synced");

    sm8521_mem_writebyte(cpu, SM8521_PS0_MIRROR, 0x38);
    EXPECT(cpu->m_PS0 == 0x38, "PS0 updated via $001E");
    EXPECT(cpu->m_p_ram[SM8521_PS0] == 0x38, "$001B synced from $001E");

    /* VRAM is write-only on SM8521 hardware (reads return 0x00) */
    sm8521_mem_writebyte(cpu, 0xA123, 0xAB);
    EXPECT(cpu->videoram[0x0123] == 0xAB, "VRAM write stored");
    EXPECT(sm8521_mem_readbyte(cpu, 0xA123) == 0x00, "VRAM read returns 0x00");

    /* NVRAM read/write */
    sm8521_mem_writebyte(cpu, 0xFC60, 0x5A);
    EXPECT(sm8521_mem_readbyte(cpu, 0xFC60) == 0x5A, "NVRAM read/write works");

    free(cpu);
}

static void test_sm8521_alu_and_wr_invariants(void) {
    SM8521 *cpu = (SM8521 *)calloc(1, sizeof(SM8521));
    sm8521_init(cpu);

    /* Place tiny test program in internal BIOS ROM ($1020) */
    uint8_t rom[0x1000];
    memset(rom, 0xFF, sizeof(rom)); /* NOP */
    /* 1) Test 8-bit ADC signed overflow: 0x7F + 0x01 + C(0) = 0x80 -> V=1, S=1 */
    /* C0 7F : MOV r0, #$7F; 53 01 00 : ADC R$00, #$01 */
    rom[0x20] = 0xC0; rom[0x21] = 0x7F;
    rom[0x22] = 0x53; rom[0x23] = 0x01; rom[0x24] = 0x00;

    /* 2) Test w(r) 16-bit internal RAM addressing (5A 80 FF 42):
     *    r0 = 0x80, w = 0xFF -> effective addr = 0x017F (NOT wrapped to $007F) */
    rom[0x25] = 0x5A; rom[0x26] = 0x80; rom[0x27] = 0xFF; rom[0x28] = 0x42;

    sm8521_load_internal_bios(cpu, rom, sizeof(rom));
    sm8521_reset(cpu);

    cpu->m_p_ram[0x017F] = 0x42;
    cpu->m_p_ram[0x007F] = 0x00;

    sm8521_step(cpu); /* MOV r0, #$7F */
    sm8521_step(cpu); /* ADC R$00, #$01 */
    EXPECT(sm8521_mem_readbyte(cpu, 0x00) == 0x80, "ADC 0x7F+1 = 0x80");
    EXPECT((cpu->m_PS1 & 0x10) != 0, "ADC sets FLAG_V on 0x7F+1");
    EXPECT((cpu->m_PS1 & 0x20) != 0, "ADC sets FLAG_S on 0x80");

    sm8521_step(cpu); /* 5A 80 FF 42: CMP (0xFF + r0 = 0x017F), #$42 */
    EXPECT((cpu->m_PS1 & 0x40) != 0, "w(r) 5A case 0x80 reads $017F without wrapping to $007F");

    free(cpu);
}

static void test_portable_savestate(void) {
    SM8521 *cpu = (SM8521 *)calloc(1, sizeof(SM8521));
    sm8521_init(cpu);
    cpu->m_PC = 0x3456;
    cpu->cycles = 12345678ULL;
    cpu->scanline_cycle_accum = 199;
    cpu->nvram[0x123] = 0x7E;

    size_t sz = sm8521_serialize_size();
    EXPECT(sz > 60000 && sz < 100000, "Savestate size in expected range");
    void *blob = malloc(sz);
    EXPECT(sm8521_serialize(cpu, blob, sz), "Serialize succeeds");

    cpu->m_PC = 0x0000;
    cpu->cycles = 0;
    cpu->scanline_cycle_accum = 0;
    cpu->nvram[0x123] = 0x00;

    EXPECT(sm8521_unserialize(cpu, blob, sz), "Unserialize succeeds");
    EXPECT(cpu->m_PC == 0x3456, "PC restored");
    EXPECT(cpu->cycles == 12345678ULL, "Cycles restored");
    EXPECT(cpu->scanline_cycle_accum == 199, "scanline_cycle_accum restored");
    EXPECT(cpu->nvram[0x123] == 0x7E, "NVRAM restored");

    free(blob);
    free(cpu);
}

static void test_libretro_abi(void) {
    EXPECT(retro_api_version() == RETRO_API_VERSION, "API version == 1");

    struct retro_system_info si;
    retro_get_system_info(&si);
    EXPECT(si.library_name && strcmp(si.library_name, "Cub Spiele.Com") == 0, "library_name matches");
    EXPECT(si.valid_extensions && strstr(si.valid_extensions, "tgc") != NULL, "supports .tgc");

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    EXPECT(av.geometry.base_width == 200 && av.geometry.base_height == 160, "200x160 geometry");
    EXPECT(av.timing.fps == 60.0, "Default 60Hz real-time pacing");
    EXPECT(av.timing.sample_rate == 44100.0, "44100 Hz sample rate");

    retro_init();
    EXPECT(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000, "8KB NVRAM save RAM");
    EXPECT(retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x0400, "1KB internal system RAM");
    EXPECT(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) != NULL, "SAVE_RAM pointer valid");
    EXPECT(retro_serialize_size() == sm8521_serialize_size(), "retro_serialize_size matches");
    retro_deinit();
}

int main(void) {
    test_ps0_mirror_and_vram();
    test_sm8521_alu_and_wr_invariants();
    test_portable_savestate();
    test_libretro_abi();

    if (g_failures == 0) {
        printf("PASS: All Cub Spiele.Com libretro unit & ABI tests succeeded!\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d unit test(s) failed.\n", g_failures);
    return 1;
}
