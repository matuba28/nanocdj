#include "gfx.h"
#include "font5x7.h"
#include "util.h"
#include <string.h>

#ifdef HB_DEVICE
pixel_t *fb;                              /* set by the port to the OS framebuffer */
#else
static pixel_t fb_mem[SCREEN_W * SCREEN_H];
pixel_t *fb = fb_mem;
#endif

void gfx_clear(pixel_t c) { for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = c; }

void gfx_pixel(int x, int y, pixel_t c)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) fb[y * SCREEN_W + x] = c;
}

void gfx_rect(int x, int y, int w, int h, pixel_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (int j = 0; j < h; j++) {
        pixel_t *p = &fb[(y + j) * SCREEN_W + x];
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void gfx_frame(int x, int y, int w, int h, pixel_t c)
{
    gfx_rect(x, y, w, 1, c); gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c); gfx_rect(x + w - 1, y, 1, h, c);
}

void gfx_vline(int x, int y0, int y1, pixel_t c) { if (y1 < y0) { int t = y0; y0 = y1; y1 = t; } gfx_rect(x, y0, 1, y1 - y0 + 1, c); }
void gfx_hline(int x0, int x1, int y, pixel_t c) { if (x1 < x0) { int t = x0; x0 = x1; x1 = t; } gfx_rect(x0, y, x1 - x0 + 1, 1, c); }

void gfx_disc(int cx, int cy, int r, pixel_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) gfx_pixel(cx + dx, cy + dy, c);
}

void gfx_ring(int cx, int cy, int r0, int r1, pixel_t c)
{
    for (int dy = -r1; dy <= r1; dy++)
        for (int dx = -r1; dx <= r1; dx++) {
            int q = dx * dx + dy * dy;
            if (q <= r1 * r1 && q >= r0 * r0) gfx_pixel(cx + dx, cy + dy, c);
        }
}

void gfx_tri_right(int x, int y, int h, pixel_t c)
{
    for (int j = 0; j < h; j++) {
        int w = (j < h / 2 ? j : h - 1 - j) + 1;
        gfx_rect(x, y + j, w, 1, c);
    }
}

/* fold Latin-1 accented letters to ASCII so titles stay readable */
static int is_combining(uint32_t cp) { return cp >= 0x300 && cp < 0x370; }

static int fold(uint32_t cp)
{
    static const char *l1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp >= 32 && cp < 127) return (int)cp;
    if (cp >= 0xc0 && cp <= 0xff) return l1[cp - 0xc0];
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201c || cp == 0x201d) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    return '?';
}

static void glyph(int x, int y, int ch, pixel_t c, int s)
{
    const uint8_t *g = font5x7[ch - 32];
    for (int r = 0; r < 8; r++)
        for (int col = 0; col < 5; col++)
            if (g[r] >> (4 - col) & 1) gfx_rect(x + col * s, y + r * s, s, s, c);
}

int gfx_text_w(const char *s, int scale)
{
    int n = 0;
    uint32_t cp;
    while ((cp = utf8_next(&s))) n += !is_combining(cp);
    return n * 6 * scale;
}

int gfx_text(int x, int y, const char *s, pixel_t c, int scale, int max_w)
{
    int adv = 6 * scale;
    int clip = max_w > 0 && gfx_text_w(s, scale) > max_w;
    int limit = clip ? x + max_w - 2 * adv : 1 << 30;
    uint32_t cp;
    while ((cp = utf8_next(&s))) {
        if (is_combining(cp)) continue;   /* accents stored as separate marks: draw the base letter only */
        if (x + adv > limit) { glyph(x, y, '.', c, scale); glyph(x + adv / 2, y, '.', c, scale); return x + adv; }
        glyph(x, y, fold(cp), c, scale);
        x += adv;
    }
    return x;
}

void gfx_text_center(int cx, int y, const char *s, pixel_t c, int scale)
{
    gfx_text(cx - gfx_text_w(s, scale) / 2 + scale / 2, y, s, c, scale, 0);
}
