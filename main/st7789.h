#pragma once

#include <stdint.h>

#define LCD_WIDTH 240
#define LCD_HEIGHT 240

void st7789_init(void);
void st7789_fill_rect(int y, int h, uint16_t color);
void st7789_fill_box(int x, int y, int w, int h, uint16_t color);
void st7789_draw_rgb565(int x, int y, int w, int h, const uint8_t *pixels);
