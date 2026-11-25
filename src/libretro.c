#include "libretro.h"
#include "sm8521.h"
#include "lcd.h"
#include "audio.h"
#include "zip_bios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static retro_environment_t       environ_cb     = NULL;
static retro_video_refresh_t     video_cb       = NULL;
static retro_audio_sample_t      audio_cb       = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t        input_poll_cb  = NULL;
static retro_input_state_t       input_state_cb = NULL;
static retro_log_printf_t        log_cb         = NULL;

#define TAP_QUEUE_CAP 16
#define TAP_PRESS_FRAMES   3
#define TAP_RELEASE_FRAMES 2

typedef struct {
    int16_t x;
    int16_t y;
} TouchTap;

static SM8521       g_cpu;
static GameComLCD   g_lcd;
static GameComAudio g_audio;
static bool         g_bios_loaded = false;
static GameComTouchOverlay g_touch_overlay = TOUCH_OVERLAY_CELL_HIGHLIGHT;
static bool         g_touch_single_click = true;
static bool         g_native_74hz_timing = false;
static bool         g_prev_pointer_down = false;
static int          g_click_pulse_frames = 0;
static int          g_click_release_frames = 0;
static bool         g_pointer_active = false;
static int16_t      g_last_cursor_x = SM8521_SCREEN_WIDTH / 2;
static int16_t      g_last_cursor_y = SM8521_SCREEN_HEIGHT / 2;
static int16_t      g_locked_click_x = 0;
static int16_t      g_locked_click_y = 0;
static TouchTap     g_tap_queue[TAP_QUEUE_CAP];
static int          g_tap_head = 0;
static int          g_tap_tail = 0;

#define RETRO_SUBSYSTEM_DUAL_CART 1u

static const struct retro_subsystem_rom_info g_dual_cart_roms[] = {
    { "Cartridge Slot 1", "tgc|bin|rom", false, false, true,  NULL, 0 },
    { "Cartridge Slot 2", "tgc|bin|rom", false, false, false, NULL, 0 }
};

static const struct retro_subsystem_info g_subsystems[] = {
    { "Dual Cartridge (Slot 1 + Slot 2)", "gamecom_dual_cart", g_dual_cart_roms, 2, RETRO_SUBSYSTEM_DUAL_CART },
    { NULL, NULL, NULL, 0, 0 }
};

static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static bool load_file_into(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return false;
    }
    *out_buf = buf;
    *out_size = rd;
    return true;
}

static bool load_bios_from_system_dir(void) {
    const char *sys_dir = NULL;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) || !sys_dir) {
        sys_dir = ".";
    }

    /* 1. Try loose BIOS files (internal.bin / external.bin or No-Intro .tgc names) */
    char int_paths[4][1024];
    char ext_paths[4][1024];
    snprintf(int_paths[0], sizeof(int_paths[0]), "%s/gamecom/internal.bin", sys_dir);
    snprintf(int_paths[1], sizeof(int_paths[1]), "%s/internal.bin", sys_dir);
    snprintf(int_paths[2], sizeof(int_paths[2]), "%s/gamecom/[BIOS] Tiger Game.com (USA) (Internal).tgc", sys_dir);
    snprintf(int_paths[3], sizeof(int_paths[3]), "%s/[BIOS] Tiger Game.com (USA) (Internal).tgc", sys_dir);

    snprintf(ext_paths[0], sizeof(ext_paths[0]), "%s/gamecom/external.bin", sys_dir);
    snprintf(ext_paths[1], sizeof(ext_paths[1]), "%s/external.bin", sys_dir);
    snprintf(ext_paths[2], sizeof(ext_paths[2]), "%s/gamecom/[BIOS] Tiger Game.com (USA) (External).tgc", sys_dir);
    snprintf(ext_paths[3], sizeof(ext_paths[3]), "%s/[BIOS] Tiger Game.com (USA) (External).tgc", sys_dir);

    uint8_t *int_buf = NULL, *ext_buf = NULL;
    size_t int_sz = 0, ext_sz = 0;

    for (int i = 0; i < 4 && !int_buf; i++) {
        load_file_into(int_paths[i], &int_buf, &int_sz);
    }
    for (int i = 0; i < 4 && !ext_buf; i++) {
        load_file_into(ext_paths[i], &ext_buf, &ext_sz);
    }

    if (int_buf && ext_buf) {
        sm8521_load_internal_bios(&g_cpu, int_buf, int_sz);
        sm8521_load_external_bios(&g_cpu, ext_buf, ext_sz);
        free(int_buf);
        free(ext_buf);
        g_bios_loaded = true;
        return true;
    }
    free(int_buf);
    free(ext_buf);

    /* 2. Try standard RetroArch/MAME gamecom.zip archive */
    char zip_paths[2][1024];
    snprintf(zip_paths[0], sizeof(zip_paths[0]), "%s/gamecom.zip", sys_dir);
    snprintf(zip_paths[1], sizeof(zip_paths[1]), "%s/gamecom/gamecom.zip", sys_dir);

    uint8_t *z_int = (uint8_t *)malloc(0x1000u);
    uint8_t *z_ext = (uint8_t *)malloc(0x40000u);
    if (z_int && z_ext) {
        for (int i = 0; i < 2; i++) {
            if (zip_load_gamecom_bios(zip_paths[i], z_int, z_ext)) {
                sm8521_load_internal_bios(&g_cpu, z_int, 0x1000u);
                sm8521_load_external_bios(&g_cpu, z_ext, 0x40000u);
                if (log_cb) {
                    log_cb(RETRO_LOG_INFO, "[Cub Spiele.Com] Loaded Game.com BIOS from %s\n", zip_paths[i]);
                }
                free(z_int);
                free(z_ext);
                g_bios_loaded = true;
                return true;
            }
        }
    }
    free(z_int);
    free(z_ext);

    if (log_cb) {
        log_cb(RETRO_LOG_ERROR,
               "[Cub Spiele.Com] Missing BIOS! Place gamecom.zip or internal.bin + external.bin in %s/gamecom/ or %s/\n",
               sys_dir, sys_dir);
    }
    if (environ_cb) {
        struct retro_message msg;
        msg.msg = "Cub Spiele.Com: Missing BIOS (gamecom.zip or internal.bin + external.bin in system/)";
        msg.frames = 300;
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
    g_bios_loaded = false;
    return false;
}

static void check_variables(void) {
    if (!environ_cb) return;
    struct retro_variable var;

    var.key = "cubspielecom_palette";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "grayscale") == 0) g_lcd.palette = LCD_PALETTE_GRAYSCALE;
        else if (strcmp(var.value, "pocket") == 0) g_lcd.palette = LCD_PALETTE_POCKET;
        else if (strcmp(var.value, "high_contrast") == 0) g_lcd.palette = LCD_PALETTE_HIGH_CONTRAST;
        else g_lcd.palette = LCD_PALETTE_OLIVE;
    }

    var.key = "cubspielecom_ghosting";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "light") == 0) g_lcd.ghosting = 0.25f;
        else if (strcmp(var.value, "medium") == 0) g_lcd.ghosting = 0.45f;
        else if (strcmp(var.value, "heavy") == 0) g_lcd.ghosting = 0.65f;
        else g_lcd.ghosting = 0.0f;
    }

    var.key = "cubspielecom_speaker_filter";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        g_audio.speaker_filter_enabled = (strcmp(var.value, "enabled") == 0);
    }

    var.key = "cubspielecom_vsync";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        g_cpu.vsync_enabled = (strcmp(var.value, "enabled") == 0);
    }

    var.key = "cubspielecom_touch_cursor";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "full_grid") == 0) g_touch_overlay = TOUCH_OVERLAY_FULL_GRID;
        else if (strcmp(var.value, "crosshair") == 0) g_touch_overlay = TOUCH_OVERLAY_CROSSHAIR;
        else if (strcmp(var.value, "disabled") == 0) g_touch_overlay = TOUCH_OVERLAY_OFF;
        else g_touch_overlay = TOUCH_OVERLAY_CELL_HIGHLIGHT;
    }

    var.key = "cubspielecom_touch_mode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        g_touch_single_click = (strcmp(var.value, "hold") != 0);
    }

    var.key = "cubspielecom_timing";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        bool want_native = (strcmp(var.value, "native_74hz") == 0);
        if (want_native != g_native_74hz_timing) {
            g_native_74hz_timing = want_native;
            struct retro_system_av_info av;
            retro_get_system_av_info(&av);
            environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        }
    }
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    struct retro_log_callback logging;
    if (cb && cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    } else {
        log_cb = fallback_log;
    }

    bool no_game = true;
    if (cb) {
        cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

        static const struct retro_variable vars[] = {
            { "cubspielecom_palette", "LCD Color Palette; olive|grayscale|pocket|high_contrast" },
            { "cubspielecom_ghosting", "STN LCD Ghosting; off|light|medium|heavy" },
            { "cubspielecom_speaker_filter", "28mm Internal Speaker Filter; enabled|disabled" },
            { "cubspielecom_vsync", "Tear-Free V-Sync Latch; enabled|disabled" },
            { "cubspielecom_touch_cursor", "Touch Grid & Cursor Overlay; cell_highlight|full_grid|crosshair|disabled" },
            { "cubspielecom_touch_mode", "Stylus Click Mode; single_click|hold" },
            { "cubspielecom_timing", "Frame Pacing (Restart/Live); 60hz_realtime|native_74hz" },
            { NULL, NULL }
        };
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
        cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void *)g_subsystems);
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Cub Spiele.Com";
    info->library_version  = "0.1.0";
    info->valid_extensions = "tgc|bin|rom";
    info->need_fullpath    = false;
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = SM8521_SCREEN_WIDTH;
    info->geometry.base_height  = SM8521_SCREEN_HEIGHT;
    info->geometry.max_width    = SM8521_SCREEN_WIDTH;
    info->geometry.max_height   = SM8521_SCREEN_HEIGHT;
    info->geometry.aspect_ratio = (float)SM8521_SCREEN_WIDTH / (float)SM8521_SCREEN_HEIGHT;
    if (g_native_74hz_timing) {
        /* Native SM8521 LCH=$07 / LCV=$27 frame rate: 4,915,200 / (320 * 207) = 74.20289855 Hz */
        info->timing.fps = (double)SM8521_CPU_CLOCK_HZ / (320.0 * 207.0);
    } else {
        /* 60 Hz real-time pacing: 81,920 SM8521 cycles/frame = 100.0% real-time on all displays */
        info->timing.fps = 60.0;
    }
    info->timing.sample_rate    = (double)GAMECOM_AUDIO_SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

RETRO_API void retro_init(void) {
    sm8521_init(&g_cpu);
    gamecom_lcd_init(&g_lcd);
    gamecom_audio_init(&g_audio);

    g_cpu.vsync_enabled = true;
    g_cpu.cb_userdata = &g_audio;
    g_cpu.on_dac_sample = gamecom_audio_on_dac_sample;
    g_cpu.on_sound_reg_write = gamecom_audio_on_reg_write;
    g_cpu.on_audio_frame_end = gamecom_audio_end_frame;
    g_cpu.on_bios_audio_clip = gamecom_audio_on_bios_clip;
}

RETRO_API void retro_deinit(void) {
    g_bios_loaded = false;
}

RETRO_API void retro_reset(void) {
    sm8521_reset(&g_cpu);
    gamecom_lcd_reset(&g_lcd);
    gamecom_audio_reset(&g_audio);
    g_prev_pointer_down = false;
    g_click_pulse_frames = 0;
    g_click_release_frames = 0;
    g_tap_head = 0;
    g_tap_tail = 0;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb && !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Cub Spiele.Com] XRGB8888 pixel format not supported.\n");
        return false;
    }

    static const struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A Button" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B Button" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C Button" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "D Button" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Menu" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Pause" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Sound" },
        { 0, 0, 0, 0, NULL }
    };
    if (environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
        bool achievements = true;
        environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
    }

    if (!load_bios_from_system_dir()) {
        return false;
    }

    sm8521_unload_cartridge(&g_cpu, 1);
    sm8521_unload_cartridge(&g_cpu, 2);

    if (game) {
        if (game->data && game->size > 0) {
            sm8521_load_cartridge(&g_cpu, (const uint8_t *)game->data, game->size, 1);
        } else if (game->path) {
            uint8_t *cart_buf = NULL;
            size_t cart_sz = 0;
            if (load_file_into(game->path, &cart_buf, &cart_sz)) {
                sm8521_load_cartridge(&g_cpu, cart_buf, cart_sz, 1);
                free(cart_buf);
            }
        }
    }

    check_variables();
    retro_reset();
    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
    if (game_type == RETRO_SUBSYSTEM_DUAL_CART && info && num_info >= 1) {
        if (!retro_load_game(&info[0])) {
            return false;
        }
        if (num_info >= 2 && info[1].data && info[1].size > 0) {
            sm8521_load_cartridge(&g_cpu, (const uint8_t *)info[1].data, info[1].size, 2);
            if (log_cb) {
                log_cb(RETRO_LOG_INFO, "[Cub Spiele.Com] Dual Cartridge: loaded Slot 2 (%lu bytes)\n",
                       (unsigned long)info[1].size);
            }
        }
        retro_reset();
        return true;
    }
    if (num_info > 0 && info) {
        return retro_load_game(&info[0]);
    }
    return false;
}

RETRO_API void retro_unload_game(void) {
    sm8521_unload_cartridge(&g_cpu, 1);
    sm8521_unload_cartridge(&g_cpu, 2);
}

RETRO_API void retro_run(void) {
    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        check_variables();
    }

    if (input_poll_cb) {
        input_poll_cb();
    }

    uint32_t buttons = 0;
    bool stylus_pressed = false;
    int16_t stylus_x = 0;
    int16_t stylus_y = 0;

    if (input_state_cb) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     buttons |= GAMECOM_BTN_UP;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   buttons |= GAMECOM_BTN_DOWN;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   buttons |= GAMECOM_BTN_LEFT;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  buttons |= GAMECOM_BTN_RIGHT;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      buttons |= GAMECOM_BTN_A;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      buttons |= GAMECOM_BTN_B;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))      buttons |= GAMECOM_BTN_C;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))      buttons |= GAMECOM_BTN_D;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) buttons |= GAMECOM_BTN_MENU;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  buttons |= GAMECOM_BTN_PAUSE;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))      buttons |= GAMECOM_BTN_SOUND;

        /* Touchscreen / Mouse pointer (-0x7FFF..+0x7FFF -> 0..199, 0..159) */
        int16_t px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        int16_t py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        int16_t pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
        bool raw_down = (pressed != 0);

        if (px != 0 || py != 0 || raw_down) {
            g_pointer_active = true;
            int32_t sx = ((int32_t)px + 0x7FFF) * SM8521_SCREEN_WIDTH / 0xFFFF;
            int32_t sy = ((int32_t)py + 0x7FFF) * SM8521_SCREEN_HEIGHT / 0xFFFF;
            if (sx < 0) sx = 0; else if (sx >= SM8521_SCREEN_WIDTH) sx = SM8521_SCREEN_WIDTH - 1;
            if (sy < 0) sy = 0; else if (sy >= SM8521_SCREEN_HEIGHT) sy = SM8521_SCREEN_HEIGHT - 1;
            g_last_cursor_x = (int16_t)sx;
            g_last_cursor_y = (int16_t)sy;
        }
        stylus_x = g_last_cursor_x;
        stylus_y = g_last_cursor_y;

        if (g_touch_single_click) {
            if (raw_down && !g_prev_pointer_down) {
                /* Rising edge: enqueue tap so rapid clicks are never dropped */
                int next_tail = (g_tap_tail + 1) % TAP_QUEUE_CAP;
                if (next_tail != g_tap_head) {
                    g_tap_queue[g_tap_tail].x = g_last_cursor_x;
                    g_tap_queue[g_tap_tail].y = g_last_cursor_y;
                    g_tap_tail = next_tail;
                }
            }

            if (g_click_pulse_frames > 0) {
                stylus_pressed = true;
                stylus_x = g_locked_click_x;
                stylus_y = g_locked_click_y;
                g_click_pulse_frames--;
                if (g_click_pulse_frames == 0) {
                    /* Guarantee a clean release gap so hardware sees the next tap edge */
                    g_click_release_frames = TAP_RELEASE_FRAMES;
                }
            } else if (g_click_release_frames > 0) {
                stylus_pressed = false;
                g_click_release_frames--;
            } else if (g_tap_head != g_tap_tail) {
                /* Start next queued tap */
                g_locked_click_x = g_tap_queue[g_tap_head].x;
                g_locked_click_y = g_tap_queue[g_tap_head].y;
                g_tap_head = (g_tap_head + 1) % TAP_QUEUE_CAP;
                g_click_pulse_frames = TAP_PRESS_FRAMES - 1;
                stylus_pressed = true;
                stylus_x = g_locked_click_x;
                stylus_y = g_locked_click_y;
            } else {
                stylus_pressed = false;
            }
        } else {
            stylus_pressed = raw_down;
            g_click_pulse_frames = 0;
            g_click_release_frames = 0;
            g_tap_head = 0;
            g_tap_tail = 0;
        }
        g_prev_pointer_down = raw_down;
    }

    sm8521_set_input(&g_cpu, buttons, stylus_pressed, stylus_x, stylus_y);

    g_audio.out_frames = 0;
    if (g_native_74hz_timing) {
        sm8521_run_frame(&g_cpu);
    } else {
        /* Exact 1/60s of 4.9152 MHz SM8521 hardware time = 81,920 cycles (exact 735 PCM samples) */
        sm8521_run_cycles(&g_cpu, SM8521_CPU_CLOCK_HZ / 60u);
    }

    gamecom_lcd_render(&g_lcd, g_cpu.display_buffer,
                       g_touch_overlay, g_pointer_active, g_last_cursor_x, g_last_cursor_y);

    if (video_cb) {
        video_cb(g_lcd.fb_xrgb8888, SM8521_SCREEN_WIDTH, SM8521_SCREEN_HEIGHT,
                 SM8521_SCREEN_WIDTH * sizeof(uint32_t));
    }

    if (g_audio.out_frames > 0) {
        if (audio_batch_cb) {
            audio_batch_cb(g_audio.out_stereo, g_audio.out_frames);
        } else if (audio_cb) {
            for (size_t i = 0; i < g_audio.out_frames; i++) {
                audio_cb(g_audio.out_stereo[i * 2], g_audio.out_stereo[i * 2 + 1]);
            }
        }
    }
}

RETRO_API size_t retro_serialize_size(void) {
    return sm8521_serialize_size();
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    return sm8521_serialize(&g_cpu, data, size);
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    if (!sm8521_unserialize(&g_cpu, data, size)) return false;
    gamecom_audio_reset(&g_audio);
    g_audio.last_tone_cycle = g_cpu.cycles;
    return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index; (void)enabled; (void)code;
}

RETRO_API unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id) {
    switch (id & RETRO_MEMORY_MASK) {
        case RETRO_MEMORY_SAVE_RAM:   return g_cpu.nvram;
        case RETRO_MEMORY_SYSTEM_RAM: return g_cpu.m_p_ram;
        case RETRO_MEMORY_VIDEO_RAM:  return g_cpu.videoram;
        default: return NULL;
    }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    switch (id & RETRO_MEMORY_MASK) {
        case RETRO_MEMORY_SAVE_RAM:   return sizeof(g_cpu.nvram);
        case RETRO_MEMORY_SYSTEM_RAM: return sizeof(g_cpu.m_p_ram);
        case RETRO_MEMORY_VIDEO_RAM:  return sizeof(g_cpu.videoram);
        default: return 0;
    }
}
