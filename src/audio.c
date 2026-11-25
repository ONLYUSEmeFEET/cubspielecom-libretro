#include "audio.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void biquad_reset(BiquadFilter *f) {
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

static void biquad_set_highpass(BiquadFilter *f, double fs, double fc, double q) {
    double w0 = 2.0 * M_PI * fc / fs;
    double c = cos(w0);
    double s = sin(w0);
    double alpha = s / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = (float)(((1.0 + c) * 0.5) / a0);
    f->b1 = (float)((-(1.0 + c)) / a0);
    f->b2 = (float)(((1.0 + c) * 0.5) / a0);
    f->a1 = (float)((-2.0 * c) / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
    biquad_reset(f);
}

static void biquad_set_lowpass(BiquadFilter *f, double fs, double fc, double q) {
    double w0 = 2.0 * M_PI * fc / fs;
    double c = cos(w0);
    double s = sin(w0);
    double alpha = s / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = (float)(((1.0 - c) * 0.5) / a0);
    f->b1 = (float)((1.0 - c) / a0);
    f->b2 = (float)(((1.0 - c) * 0.5) / a0);
    f->a1 = (float)((-2.0 * c) / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
    biquad_reset(f);
}

static void biquad_set_bandpass(BiquadFilter *f, double fs, double fc, double q) {
    double w0 = 2.0 * M_PI * fc / fs;
    double c = cos(w0);
    double s = sin(w0);
    double alpha = s / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = (float)(alpha / a0);
    f->b1 = 0.0f;
    f->b2 = (float)(-alpha / a0);
    f->a1 = (float)((-2.0 * c) / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
    biquad_reset(f);
}

static void biquad_set_peaking(BiquadFilter *f, double fs, double fc, double q, double gain_db) {
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * fc / fs;
    double c = cos(w0);
    double s = sin(w0);
    double alpha = s / (2.0 * q);
    double a0 = 1.0 + alpha / A;
    f->b0 = (float)((1.0 + alpha * A) / a0);
    f->b1 = (float)((-2.0 * c) / a0);
    f->b2 = (float)((1.0 - alpha * A) / a0);
    f->a1 = (float)((-2.0 * c) / a0);
    f->a2 = (float)((1.0 - alpha / A) / a0);
    biquad_reset(f);
}

static inline float biquad_process(BiquadFilter *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static inline float speaker_chain_process(GameComAudio *a, float x) {
    if (!a->speaker_filter_enabled) {
        if (x > 1.0f) x = 1.0f;
        else if (x < -1.0f) x = -1.0f;
        return x;
    }
    x *= 1.15f;
    float hp = biquad_process(&a->hpf_310, x);
    float bp = biquad_process(&a->bpf_1350, hp);
    float blended = bp * 0.65f + hp * 0.55f;
    float res = biquad_process(&a->peak_1220, blended);
    float biased = res + 0.035f * res * res;
    float shaped = tanhf(biased * 1.65f) / tanhf(1.65f);
    float lp = biquad_process(&a->lpf_3450, shaped);
    if (lp > 1.0f) lp = 1.0f;
    else if (lp < -1.0f) lp = -1.0f;
    return lp;
}

static inline int8_t read_signed_wave_sample(const uint8_t *wave, uint8_t step) {
    uint8_t byte = wave[(step >> 1) & 0x0F];
    int nib = (step & 1) == 0 ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
    return (int8_t)(nib >= 8 ? nib - 16 : nib);
}

static float integrate_wave_channel(GameComAudio *a, int ch, int32_t period, double cycles_window) {
    const uint8_t *wave = (ch == 0) ? a->regs.sg0w : a->regs.sg1w;
    int32_t div    = (ch == 0) ? a->sg0_div    : a->sg1_div;
    uint8_t index  = (ch == 0) ? a->sg0_index  : a->sg1_index;
    int8_t  sample = (ch == 0) ? a->sg0_sample : a->sg1_sample;
    double  sum    = 0.0;
    double  left   = cycles_window;

    while (left > 0.0) {
        double step_span = (double)(div + 1);
        if (step_span > left) {
            sum += (double)sample * left;
            div -= (int32_t)left;
            if (div < 0) div = 0;
            left = 0.0;
        } else {
            sum += (double)sample * step_span;
            left -= step_span;
            div = period;
            sample = read_signed_wave_sample(wave, index);
            index = (uint8_t)((index == 31) ? 0 : (index + 1));
        }
    }

    if (ch == 0) {
        a->sg0_div    = div;
        a->sg0_index  = index;
        a->sg0_sample = sample;
    } else {
        a->sg1_div    = div;
        a->sg1_index  = index;
        a->sg1_sample = sample;
    }
    return (float)(sum / cycles_window);
}

static float integrate_noise_channel(GameComAudio *a, int32_t period, double cycles_window) {
    int32_t  div    = a->sg2_div;
    uint16_t lfsr   = a->sg2_lfsr;
    int8_t   sample = a->sg2_sample;
    double   sum    = 0.0;
    double   left   = cycles_window;

    while (left > 0.0) {
        double step_span = (double)(div + 1);
        if (step_span > left) {
            sum += (double)sample * left;
            div -= (int32_t)left;
            if (div < 0) div = 0;
            left = 0.0;
        } else {
            sum += (double)sample * step_span;
            left -= step_span;
            div = period;
            uint16_t next_bit = (uint16_t)((lfsr ^ (lfsr >> 1)) & 1u);
            uint16_t shifted = (uint16_t)(((lfsr >> 1) | (next_bit << 14)) & 0x7FFFu);
            lfsr = (shifted == 0) ? 0x7FFFu : shifted;
            sample = (int8_t)(((lfsr & 1u) == 0) ? 7 : -8);
        }
    }

    a->sg2_div    = div;
    a->sg2_lfsr   = lfsr;
    a->sg2_sample = sample;
    return (float)(sum / cycles_window);
}

static inline void emit_stereo_sample(GameComAudio *a, float mono_sample) {
    if (a->out_frames >= GAMECOM_AUDIO_MAX_FRAME_SAMPLES) return;
    float filtered = speaker_chain_process(a, mono_sample);
    int32_t pcm = (int32_t)(filtered * 32767.0f);
    if (pcm > 32767) pcm = 32767;
    if (pcm < -32768) pcm = -32768;
    a->out_stereo[a->out_frames * 2 + 0] = (int16_t)pcm;
    a->out_stereo[a->out_frames * 2 + 1] = (int16_t)pcm;
    a->out_frames++;
}

static inline float pop_clip_sample(GameComAudio *a) {
    if (a->clip_read_pos == a->clip_write_pos) return 0.0f;
    float v = a->clip_ring[a->clip_read_pos];
    a->clip_read_pos = (a->clip_read_pos + 1u) % GAMECOM_CLIP_RING_SIZE;
    return v;
}

static void advance_audio_samples(GameComAudio *a, uint64_t target_cycles) {
    if (target_cycles < a->last_tone_cycle) {
        a->last_tone_cycle = target_cycles;
        a->tone_cycle_accum = 0.0;
        return;
    }
    uint64_t delta = target_cycles - a->last_tone_cycle;
    if (delta == 0) return;
    a->last_tone_cycle = target_cycles;

    const double cycles_per_sample = (double)SM8521_CPU_CLOCK_HZ / (double)GAMECOM_AUDIO_SAMPLE_RATE;
    a->tone_cycle_accum += (double)delta;

    const uint8_t sgc = a->regs.sgc;
    const int32_t p0 = a->regs.sg0t & 0x0FFF;
    const int32_t p1 = a->regs.sg1t & 0x0FFF;
    const int32_t p2 = a->regs.sg2t & 0x0FFF;
    const bool global_on = (sgc & 0x80) != 0;
    const bool ch0_active = global_on && (sgc & 0x01) != 0 && p0 > 1;
    const bool ch1_active = global_on && (sgc & 0x02) != 0 && p1 > 1;
    const bool ch2_active = global_on && (sgc & 0x04) != 0 && p2 > 1;

    const float l0 = (float)(a->regs.sg0l & 0x1F) / (32.0f * 16.0f);
    const float l1 = (float)(a->regs.sg1l & 0x1F) / (32.0f * 16.0f);
    const float l2 = (float)(a->regs.sg2l & 0x1F) / (32.0f * 16.0f);

    while (a->tone_cycle_accum >= cycles_per_sample) {
        a->tone_cycle_accum -= cycles_per_sample;

        float tone_mix = 0.0f;
        if (ch0_active) {
            tone_mix += integrate_wave_channel(a, 0, p0, cycles_per_sample) * l0;
        } else if (global_on && (sgc & 0x01) != 0) {
            tone_mix += (float)a->sg0_sample * l0;
        }

        if (ch1_active) {
            tone_mix += integrate_wave_channel(a, 1, p1, cycles_per_sample) * l1;
        } else if (global_on && (sgc & 0x02) != 0) {
            tone_mix += (float)a->sg1_sample * l1;
        }

        if (ch2_active) {
            tone_mix += integrate_noise_channel(a, p2, cycles_per_sample) * l2;
        } else if (global_on && (sgc & 0x04) != 0) {
            tone_mix += (float)a->sg2_sample * l2;
        }

        if (tone_mix > 1.0f) tone_mix = 1.0f;
        else if (tone_mix < -1.0f) tone_mix = -1.0f;

        float dc_free = tone_mix - a->tone_dc_prev_x + 0.996f * a->tone_dc_prev_y;
        a->tone_dc_prev_x = tone_mix;
        a->tone_dc_prev_y = dc_free;

        /* Cycle-accurate SGDA ($004E) 8-bit DAC with 44.1 kHz coupling capacitor DC blocker */
        float dac_in = ((sgc & 0x8Fu) == 0x88u && a->dac_active) ? a->dac_level : 0.0f;
        float dac_dc = dac_in - a->dac_dc_prev_x + 0.997f * a->dac_dc_prev_y;
        a->dac_dc_prev_x = dac_in;
        a->dac_dc_prev_y = dac_dc;
        float dac = dac_dc * 0.72f;

        float clip = pop_clip_sample(a) * 0.55f;
        float total = dc_free * 0.72f + dac + clip;
        emit_stereo_sample(a, total);
    }
}

void gamecom_audio_init(GameComAudio *audio) {
    memset(audio, 0, sizeof(*audio));
    audio->speaker_filter_enabled = true;
    audio->sg2_lfsr = 0x7FFFu;

    double fs = (double)GAMECOM_AUDIO_SAMPLE_RATE;
    biquad_set_highpass(&audio->hpf_310, fs, 310.0, 0.85);
    biquad_set_bandpass(&audio->bpf_1350, fs, 1350.0, 0.65);
    biquad_set_peaking(&audio->peak_1220, fs, 1220.0, 1.45, 3.8);
    biquad_set_lowpass(&audio->lpf_3450, fs, 3450.0, 0.80);
}

void gamecom_audio_reset(GameComAudio *audio) {
    bool filter = audio->speaker_filter_enabled;
    gamecom_audio_init(audio);
    audio->speaker_filter_enabled = filter;
}

void gamecom_audio_on_reg_write(void *userdata, const GameComAudioRegs *regs, uint64_t cycles) {
    GameComAudio *a = (GameComAudio *)userdata;
    advance_audio_samples(a, cycles);
    a->regs = *regs;

    int32_t p0 = a->regs.sg0t & 0x0FFF;
    int32_t p1 = a->regs.sg1t & 0x0FFF;
    int32_t p2 = a->regs.sg2t & 0x0FFF;
    bool global_on = (a->regs.sgc & 0x80) != 0;

    if (global_on && (a->regs.sgc & 0x01) != 0 && p0 > 1) {
        if (!a->sg0_run) {
            a->sg0_run = true;
            a->sg0_index = 1;
            a->sg0_div = p0;
            a->sg0_sample = read_signed_wave_sample(a->regs.sg0w, 0);
        }
    } else if (p0 <= 1) {
        a->sg0_run = false;
        a->sg0_index = 0;
        a->sg0_div = 0;
        a->sg0_sample = 0;
    }

    if (global_on && (a->regs.sgc & 0x02) != 0 && p1 > 1) {
        if (!a->sg1_run) {
            a->sg1_run = true;
            a->sg1_index = 1;
            a->sg1_div = p1;
            a->sg1_sample = read_signed_wave_sample(a->regs.sg1w, 0);
        }
    } else if (p1 <= 1) {
        a->sg1_run = false;
        a->sg1_index = 0;
        a->sg1_div = 0;
        a->sg1_sample = 0;
    }

    if (global_on && (a->regs.sgc & 0x04) != 0 && p2 > 1) {
        if (!a->sg2_run) {
            a->sg2_run = true;
            a->sg2_div = p2;
            a->sg2_lfsr = 0x7FFFu;
            a->sg2_sample = 0;
        }
    } else if (p2 <= 1) {
        a->sg2_run = false;
        a->sg2_div = 0;
        a->sg2_lfsr = 0x7FFFu;
        a->sg2_sample = 0;
    }
}

void gamecom_audio_on_dac_sample(void *userdata, uint8_t val, uint64_t cycles) {
    GameComAudio *a = (GameComAudio *)userdata;
    advance_audio_samples(a, cycles);
    a->dac_level = ((float)val - 128.0f) / 128.0f;
    a->dac_active = true;
}

void gamecom_audio_on_bios_clip(void *userdata, const uint8_t *samples, size_t len, uint8_t bank) {
    GameComAudio *a = (GameComAudio *)userdata;
    if (!samples || len == 0) return;

    /* Reset clip queue for new BIOS clip */
    a->clip_read_pos = 0;
    a->clip_write_pos = 0;

    double src_rate = (bank == 0x0E) ? 7877.0 : 8000.0;
    double ratio = (double)GAMECOM_AUDIO_SAMPLE_RATE / src_rate;
    double accum = 0.0;
    float prev_x = 0.0f;
    float prev_y = 0.0f;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = samples[i];
        int nibs[2];
        nibs[0] = (int)(b & 0x0F) - 8;
        nibs[1] = (int)((b >> 4) & 0x0F) - 8;

        for (int n = 0; n < 2; n++) {
            float x = (float)nibs[n] / 8.0f;
            prev_y = x - prev_x + 0.985f * prev_y;
            prev_x = x;
            float s = prev_y * 0.6f;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;

            accum += ratio;
            int count = (int)accum;
            accum -= (double)count;
            for (int k = 0; k < count; k++) {
                uint32_t next_w = (a->clip_write_pos + 1u) % GAMECOM_CLIP_RING_SIZE;
                if (next_w != a->clip_read_pos) {
                    a->clip_ring[a->clip_write_pos] = s;
                    a->clip_write_pos = next_w;
                }
            }
        }
    }
}

void gamecom_audio_end_frame(void *userdata, uint64_t cycles) {
    GameComAudio *a = (GameComAudio *)userdata;
    advance_audio_samples(a, cycles);
}
