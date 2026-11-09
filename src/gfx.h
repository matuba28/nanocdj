#pragma once
#include <stdint.h>
#include "hal.h"

/* Pixel format: XRGB8888 on the iPod (NanoApps raw surface), RGB565 in host builds. */
#ifdef HB_DEVICE
typedef uint32_t pixel_t;
#define RGB(r, g, b) ((pixel_t)(0xff000000u | ((uint32_t)((r) & 0xff) << 16) | ((uint32_t)((g) & 0xff) << 8) | ((b) & 0xff)))
#else
typedef uint16_t pixel_t;
#define RGB(r, g, b) ((pixel_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xff) >> 3)))
#endif

extern pixel_t *fb;   /* SCREEN_W * SCREEN_H, row-major */

void gfx_clear(pixel_t c);
void gfx_rect(int x, int y, int w, int h, pixel_t c);
void gfx_frame(int x, int y, int w, int h, pixel_t c);
void gfx_vline(int x, int y0, int y1, pixel_t c);
void gfx_hline(int x0, int x1, int y, pixel_t c);
void gfx_pixel(int x, int y, pixel_t c);
void gfx_disc(int cx, int cy, int r, pixel_t c);
void gfx_ring(int cx, int cy, int r0, int r1, pixel_t c);
void gfx_tri_right(int x, int y, int h, pixel_t c);   /* play symbol */

/* UTF-8 text, 5x7 glyphs on a 6px grid, scaled. Returns x after the text.
 * max_w > 0 clips (with "..") to that many pixels. */
int  gfx_text(int x, int y, const char *s, pixel_t c, int scale, int max_w);
int  gfx_text_w(const char *s, int scale);
void gfx_text_center(int cx, int y, const char *s, pixel_t c, int scale);
