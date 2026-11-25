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
    lcd->ghosting = 0.0f;
    lcd->initialized = true;
}

void gamecom_lcd_reset(GameComLCD *lcd) {
    memset(lcd->shade_buf, 0, sizeof(lcd->shade_buf));
    memset(lcd->fb_xrgb8888, 0, sizeof(lcd->fb_xrgb8888));
}

void gamecom_lcd_render(GameComLCD *lcd, const uint8_t *display_buffer,
                        bool draw_cursor, int16_t cursor_x, int16_t cursor_y) {
    int pal_idx = (int)lcd->palette;
    if (pal_idx < 0 || pal_idx > 3) pal_idx = 0;
    const uint32_t *pal = PALETTE_TABLE[pal_idx];

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

    if (draw_cursor && cursor_x >= 0 && cursor_x < SM8521_SCREEN_WIDTH &&
        cursor_y >= 0 && cursor_y < SM8521_SCREEN_HEIGHT) {
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
