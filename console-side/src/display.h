/* display.h — minimal ST7735 (T-Dongle-S3, 80x160) status screen. */
#pragma once
#include <stdint.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void display_init(void);
void display_fill(uint16_t color565);

/* 8x8 text. display_text draws left-to-right from (x,y). */
void display_char(int x, int y, char c, uint16_t fg, uint16_t bg);
void display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);

#define LCD_COLS 20   /* 160 / 8 */
#define LCD_ROWS 10   /*  80 / 8 */
