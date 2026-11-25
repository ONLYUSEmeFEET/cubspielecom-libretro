#ifndef CUBSPIELE_LCD_H
#define CUBSPIELE_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "sm8521.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LCD_PALETTE_OLIVE = 0,      /* Calibrated reflective olive-green (#b9c48a) */
    LCD_PALETTE_GRAYSCALE = 1,  /* Neutral monochrome grayscale */
    LCD_PALETTE_POCKET = 2,     /* Game Boy Pocket crisp silver-gray */
    LCD_PALETTE_HIGH_CONTRAST = 3
} GameComPaletteId;

typedef struct {
    GameComPaletteId palette;
    float ghosting; /* 0.0 (off) .. 0.75 (heavy STN persistence) */
    float shade_buf[SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT];
    uint32_t fb_xrgb8888[SM8521_SCREEN_WIDTH * SM8521_SCREEN_HEIGHT];
    bool initialized;
} GameComLCD;

void gamecom_lcd_init(GameComLCD *lcd);
void gamecom_lcd_reset(GameComLCD *lcd);
void gamecom_lcd_render(GameComLCD *lcd, const uint8_t *display_buffer,
                        bool draw_cursor, int16_t cursor_x, int16_t cursor_y);

#ifdef __cplusplus
}
#endif

#endif /* CUBSPIELE_LCD_H */
