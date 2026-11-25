#include "lcd.h"
#include <string.h>

/* 4-shade XRGB8888 palettes (index 0 = lightest background .. 3 = darkest ink) */
static const uint32_t PALETTE_TABLE[4][4] = {
    /* LCD_PALETTE_OLIVE: Calibrated Tiger Game.com reflective olive-green (#b9c48a) */
    { 0x00B9C48Au, 0x008A9660u, 0x00525B36u, 0x001E2414u },
    /* LCD_PALETTE_GRAYSCALE */
    { 0x00DCE0DCu, 0x009BA09Bu, 0x00545854u, 0x00141614u },
    /* LCD_PALETTE_POCKET */
    { 0x00C8D0C0u, 0x008C9486u, 0x004E544Au, 0x00181C18u },
    /* LCD_PALETTE_HIGH_CONTRAST */
    { 0x00F4F6ECu, 0x00A4AA9Eu, 0x004C5048u, 0x00080A08u }
};

static inline uint32_t lerp_xrgb(uint32_t c0, uint32_t c1, float t) {
    int r0 = (int)((c0 >> 16) & 0xFF);
    int g0 = (int)((c0 >> 8) & 0xFF);
    int b0 = (int)(c0 & 0xFF);
    int r1 = (int)((c1 >> 16) & 0xFF);
    int g1 = (int)((c1 >> 8) & 0xFF);
    int b1 = (int)(c1 & 0xFF);

    int r = r0 + (int)((r1 - r0) * t + 0.5f);
    int g = g0 + (int)((g1 - g0) * t + 0.5f);
    int b = b0 + (int)((b1 - b0) * t + 0.5f);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void gamecom_lcd_init(GameComLCD *lcd) {
    memset(lcd, 0, sizeof(*lcd));
    lcd->palette = LCD_PALETTE_OLIVE;
    lcd->contrast = 1.0f;
    lcd->ghosting = 0.0f;
    lcd->initialized = true;
}

void gamecom_lcd_reset(GameComLCD *lcd) {
    memset(lcd->shade_buf, 0, sizeof(lcd->shade_buf));
    memset(lcd->fb_xrgb8888, 0, sizeof(lcd->fb_xrgb8888));
}

static inline uint32_t blend_dark(uint32_t px, float amount) {
    int r = (int)(((px >> 16) & 0xFF) * (1.0f - amount));
    int g = (int)(((px >> 8) & 0xFF) * (1.0f - amount));
    int b = (int)((px & 0xFF) * (1.0f - amount));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t apply_contrast_xrgb(uint32_t px, float contrast) {
    if (contrast >= 0.995f && contrast <= 1.005f) return px;
    int r = (int)((((int)((px >> 16) & 0xFF) - 128) * contrast) + 128.5f);
    int g = (int)((((int)((px >> 8) & 0xFF) - 128) * contrast) + 128.5f);
    int b = (int)((((int)(px & 0xFF) - 128) * contrast) + 128.5f);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void gamecom_lcd_render(GameComLCD *lcd, const uint8_t *display_buffer,
                        GameComTouchOverlay overlay, bool pointer_active,
                        int16_t cursor_x, int16_t cursor_y) {
    int pal_idx = (int)lcd->palette;
    if (pal_idx < 0 || pal_idx > 3) pal_idx = 0;
    uint32_t pal[4];
    for (int i = 0; i < 4; i++) {
        pal[i] = apply_contrast_xrgb(PALETTE_TABLE[pal_idx][i], lcd->contrast);
    }

    float ghost = lcd->ghosting;
    if (ghost < 0.0f) ghost = 0.0f;
    if (ghost > 0.75f) ghost = 0.75f;

    const int count = SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT;
    if (ghost <= 0.001f) {
        for (int i = 0; i < count; i++) {
            uint8_t s = display_buffer[i] & 3u;
            lcd->shade_buf[i] = (float)s;
            lcd->fb_xrgb8888[i] = pal[s];
        }
    } else {
        float alpha = 1.0f - ghost;
        for (int i = 0; i < count; i++) {
            float target = (float)(display_buffer[i] & 3u);
            float cur = lcd->shade_buf[i] + (target - lcd->shade_buf[i]) * alpha;
            if (cur < 0.0f) cur = 0.0f;
            if (cur > 3.0f) cur = 3.0f;
            lcd->shade_buf[i] = cur;

            int idx = (int)cur;
            if (idx >= 3) {
                lcd->fb_xrgb8888[i] = pal[3];
            } else {
                float frac = cur - (float)idx;
                lcd->fb_xrgb8888[i] = lerp_xrgb(pal[idx], pal[idx + 1], frac);
            }
        }
    }

    /* Optional full 13x10 hardware touch grid (16x16 px cells) */
    if (overlay == TOUCH_OVERLAY_FULL_GRID) {
        for (int x = 16; x < SM8521_SCREEN_WIDTH; x += 16) {
            for (int y = 0; y < SM8521_SCREEN_HEIGHT; y += 2) {
                lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x] =
                    blend_dark(lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x], 0.22f);
            }
        }
        for (int y = 16; y < SM8521_SCREEN_HEIGHT; y += 16) {
            for (int x = 0; x < SM8521_SCREEN_WIDTH; x += 2) {
                lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x] =
                    blend_dark(lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x], 0.22f);
            }
        }
    }

    if (pointer_active && overlay != TOUCH_OVERLAY_OFF &&
        cursor_x >= 0 && cursor_x < SM8521_SCREEN_WIDTH &&
        cursor_y >= 0 && cursor_y < SM8521_SCREEN_HEIGHT) {

        /* Draw active 16x16 hardware touch cell box so user sees exact target cell */
        if (overlay == TOUCH_OVERLAY_CELL_HIGHLIGHT || overlay == TOUCH_OVERLAY_FULL_GRID) {
            int x0 = (cursor_x / 16) * 16;
            int y0 = (cursor_y / 16) * 16;
            int x1 = x0 + 15;
            int y1 = y0 + 15;
            if (x1 >= SM8521_SCREEN_WIDTH) x1 = SM8521_SCREEN_WIDTH - 1;
            if (y1 >= SM8521_SCREEN_HEIGHT) y1 = SM8521_SCREEN_HEIGHT - 1;
            for (int x = x0; x <= x1; x++) {
                lcd->fb_xrgb8888[y0 * SM8521_SCREEN_WIDTH + x] ^= 0x00404040u;
                lcd->fb_xrgb8888[y1 * SM8521_SCREEN_WIDTH + x] ^= 0x00404040u;
            }
            for (int y = y0 + 1; y < y1; y++) {
                lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x0] ^= 0x00404040u;
                lcd->fb_xrgb8888[y * SM8521_SCREEN_WIDTH + x1] ^= 0x00404040u;
            }
        }

        /* Draw crisp crosshair at pointer position */
        for (int d = -3; d <= 3; d++) {
            int cx = cursor_x + d;
            int cy = cursor_y + d;
            if (cx >= 0 && cx < SM8521_SCREEN_WIDTH) {
                lcd->fb_xrgb8888[cursor_y * SM8521_SCREEN_WIDTH + cx] ^= 0x00FFFFFFu;
            }
            if (d != 0 && cy >= 0 && cy < SM8521_SCREEN_HEIGHT) {
                lcd->fb_xrgb8888[cy * SM8521_SCREEN_WIDTH + cursor_x] ^= 0x00FFFFFFu;
            }
        }
    }
}
