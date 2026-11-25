#include "sm8521.h"
#include "lcd.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <internal.bin> <external.bin> [cart.tgc] [frames]\n", argv[0]);
        return 2;
    }

    size_t int_sz = 0, ext_sz = 0;
    uint8_t *int_rom = read_file(argv[1], &int_sz);
    uint8_t *ext_rom = read_file(argv[2], &ext_sz);
    if (!int_rom || !ext_rom) {
        fprintf(stderr, "FAIL: could not load BIOS files\n");
        return 1;
    }

    SM8521 *cpu = (SM8521 *)calloc(1, sizeof(SM8521));
    GameComLCD *lcd = (GameComLCD *)calloc(1, sizeof(GameComLCD));
    GameComAudio *audio = (GameComAudio *)calloc(1, sizeof(GameComAudio));

    sm8521_init(cpu);
    gamecom_lcd_init(lcd);
    gamecom_audio_init(audio);

    cpu->vsync_enabled = true;
    cpu->cb_userdata = audio;
    cpu->on_dac_sample = gamecom_audio_on_dac_sample;
    cpu->on_sound_reg_write = gamecom_audio_on_reg_write;
    cpu->on_audio_frame_end = gamecom_audio_end_frame;
    cpu->on_bios_audio_clip = gamecom_audio_on_bios_clip;

    sm8521_load_internal_bios(cpu, int_rom, int_sz);
    sm8521_load_external_bios(cpu, ext_rom, ext_sz);
    free(int_rom);
    free(ext_rom);

    if (argc >= 4 && argv[3][0] != '\0' && strcmp(argv[3], "none") != 0) {
        size_t cart_sz = 0;
        uint8_t *cart = read_file(argv[3], &cart_sz);
        if (!cart) {
            fprintf(stderr, "FAIL: could not load cart %s\n", argv[3]);
            return 1;
        }
        sm8521_load_cartridge(cpu, cart, cart_sz, 1);
        free(cart);
    }

    int frames = (argc >= 5) ? atoi(argv[4]) : 180;
    sm8521_reset(cpu);

    size_t total_audio_frames = 0;
    int non_zero_audio = 0;
    for (int f = 0; f < frames; f++) {
        audio->out_frames = 0;
        sm8521_run_frame(cpu);
        gamecom_lcd_render(lcd, cpu->display_buffer, false, 0, 0);
        total_audio_frames += audio->out_frames;
        for (size_t i = 0; i < audio->out_frames * 2; i++) {
            if (audio->out_stereo[i] != 0) non_zero_audio++;
        }
    }

    /* Count distinct shades on screen */
    int shade_counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT; i++) {
        shade_counts[cpu->display_buffer[i] & 3]++;
    }

    /* Savestate round-trip test */
    size_t st_sz = sm8521_serialize_size();
    void *st = malloc(st_sz);
    bool ser_ok = sm8521_serialize(cpu, st, st_sz);
    uint16_t pc_before = cpu->m_PC;
    uint64_t cyc_before = cpu->cycles;
    sm8521_run_frame(cpu);
    bool unser_ok = sm8521_unserialize(cpu, st, st_sz);
    bool match = (cpu->m_PC == pc_before && cpu->cycles == cyc_before);
    free(st);

    printf("Ran %d frames | cycles=%llu | PC=$%04X | DMAs=%u\n",
           frames, (unsigned long long)cpu->cycles, cpu->m_PC, cpu->dma_count);
    printf("Shades: [0]=%d [1]=%d [2]=%d [3]=%d\n",
           shade_counts[0], shade_counts[1], shade_counts[2], shade_counts[3]);
    printf("Audio frames=%lu (non-zero PCM samples=%d)\n",
           (unsigned long)total_audio_frames, non_zero_audio);
    printf("Savestate (%lu bytes): serialize=%d unserialize=%d exact_match=%d\n",
           (unsigned long)st_sz, ser_ok, unser_ok, match);

    free(cpu);
    free(lcd);
    free(audio);

    if (!ser_ok || !unser_ok || !match || (shade_counts[1] + shade_counts[2] + shade_counts[3]) == 0) {
        fprintf(stderr, "FAIL: smoke test assertions failed\n");
        return 1;
    }
    printf("PASS: Cub Spiele.Com libretro core smoke test succeeded!\n");
    return 0;
}
