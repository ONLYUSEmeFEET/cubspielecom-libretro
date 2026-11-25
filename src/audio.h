#ifndef CUBSPIELE_AUDIO_H
#define CUBSPIELE_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sm8521.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAMECOM_AUDIO_SAMPLE_RATE 44100
#define GAMECOM_AUDIO_MAX_FRAME_SAMPLES 2048
#define GAMECOM_DAC_RING_SIZE 8192
#define GAMECOM_CLIP_RING_SIZE 32768

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} BiquadFilter;

typedef struct {
    /* Hardware tone registers snapshot */
    GameComAudioRegs regs;

    /* SG0/SG1 32-step wavetable + SG2 15-bit LFSR state */
    bool     sg0_run;
    uint8_t  sg0_index;
    int32_t  sg0_div;
    int8_t   sg0_sample;

    bool     sg1_run;
    uint8_t  sg1_index;
    int32_t  sg1_div;
    int8_t   sg1_sample;

    bool     sg2_run;
    int32_t  sg2_div;
    uint16_t sg2_lfsr;
    int8_t   sg2_sample;

    uint64_t last_tone_cycle;
    double   tone_cycle_accum;
    float    tone_dc_prev_x;
    float    tone_dc_prev_y;

    /* Cycle-accurate SGDA ($004E) 8-bit DAC latch state */
    float    dac_level;
    float    dac_dc_prev_x;
    float    dac_dc_prev_y;
    bool     dac_active;

    /* BIOS digitized 4-bit clip ring buffer (resampled to 44.1 kHz) */
    float    clip_ring[GAMECOM_CLIP_RING_SIZE];
    uint32_t clip_read_pos;
    uint32_t clip_write_pos;

    /* 28mm internal speaker acoustic filter chain */
    bool         speaker_filter_enabled;
    BiquadFilter hpf_310;
    BiquadFilter bpf_1350;
    BiquadFilter peak_1220;
    BiquadFilter lpf_3450;

    /* Output interleaved stereo PCM buffer for current frame */
    int16_t  out_stereo[GAMECOM_AUDIO_MAX_FRAME_SAMPLES * 2];
    size_t   out_frames;
} GameComAudio;

void gamecom_audio_init(GameComAudio *audio);
void gamecom_audio_reset(GameComAudio *audio);

void gamecom_audio_on_reg_write(void *userdata, const GameComAudioRegs *regs, uint64_t cycles);
void gamecom_audio_on_dac_sample(void *userdata, uint8_t val, uint64_t cycles);
void gamecom_audio_on_bios_clip(void *userdata, const uint8_t *samples, size_t len, uint8_t bank);
void gamecom_audio_end_frame(void *userdata, uint64_t cycles);

#ifdef __cplusplus
}
#endif

#endif /* CUBSPIELE_AUDIO_H */
