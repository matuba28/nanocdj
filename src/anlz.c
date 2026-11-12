#include "anlz.h"
#include "hal.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint16_t b16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void add_cue(anlz_t *a, int hot, int type, uint32_t t, uint32_t lt)
{
    if (a->ncues >= ANLZ_MAX_CUES) return;
    anlz_cue *c = &a->cues[a->ncues++];
    c->hot = hot;
    c->is_loop = type == 2 && lt != 0xffffffff;
    c->time_ms = t;
    c->loop_ms = c->is_loop ? lt : 0;
}

/* small tags, handed over whole: preview and cue lists */
static void on_tag(anlz_t *a, const uint8_t *t, uint32_t hl, uint32_t tl, int *have_ext_cues)
{
    const uint8_t *body = t + hl, *end = t + tl;
    if (!memcmp(t, "PWAV", 4) && hl >= 20) {
        uint32_t n = b32(t + 12);
        if (n > 400) n = 400;
        if (body + n <= end) { memcpy(a->preview, body, n); a->has_preview = 1; }
    } else if (!memcmp(t, "PCO2", 4) && hl >= 20) {
        /* extended cue list (EXT file), preferred when present */
        int n = b16(t + 16);
        const uint8_t *e = body;
        for (int i = 0; i < n && e + 0x1c <= end; i++) {
            if (memcmp(e, "PCP2", 4)) break;
            uint32_t el = b32(e + 8);
            if (el < 0x1c || e + el > end) break;
            add_cue(a, (int)b32(e + 0x0c), e[0x10], b32(e + 0x14), b32(e + 0x18));
            *have_ext_cues = 1;
            e += el;
        }
    } else if (!memcmp(t, "PCOB", 4) && hl >= 24 && !*have_ext_cues) {
        int n = b16(t + 18);
        const uint8_t *e = body;
        for (int i = 0; i < n && e + 0x28 <= end; i++) {
            if (memcmp(e, "PCPT", 4)) break;
            uint32_t el = b32(e + 8);
            if (el < 0x28 || e + el > end) break;
            add_cue(a, (int)b32(e + 0x0c), e[0x1c], b32(e + 0x20), b32(e + 0x24));
            e += el;
        }
    }
}

/* ---- reading a file forward, tag by tag (hal_file_*: no seek on the iPod) ---- */

typedef struct { void *f; int eof; } rd_t;
static uint8_t chunk[4096];

static int rd(rd_t *r, void *buf, uint32_t n)       /* exactly n bytes, or -1 */
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        int32_t got = hal_file_read(r->f, p, n);
        if (got <= 0) { r->eof = 1; return -1; }
        p += got; n -= (uint32_t)got;
    }
    return 0;
}

static int skip(rd_t *r, uint32_t n)
{
    while (n) {
        uint32_t m = n > sizeof chunk ? sizeof chunk : n;
        if (rd(r, chunk, m)) return -1;
        n -= m;
    }
    return 0;
}

/* how many detail entries to fold into one so at most ANLZ_WAVE_MAX are kept */
static int wave_div_for(uint32_t n) { return n > ANLZ_WAVE_MAX ? (int)((n + ANLZ_WAVE_MAX - 1) / ANLZ_WAVE_MAX) : 1; }

/* Detail waveform (PWV3: 1 byte, PWV5: 2 bytes big-endian per entry), folded
 * `div` to one by keeping the tallest entry of each group. */
static int read_wave(rd_t *r, anlz_t *a, uint32_t n, uint32_t body, int two)
{
    uint32_t es = two ? 2u : 1u;
    if ((uint64_t)n * es > body) n = body / es;
    int div = a->wave_div ? a->wave_div : wave_div_for(n);
    uint32_t out_n = (n + (uint32_t)div - 1) / (uint32_t)div;
    void *dst = out_n ? malloc((size_t)out_n * es) : NULL;   /* refused: no waveform, carry on */
    uint32_t done = 0, best = 0, bh = 0, k = 0;
    while (done < n) {
        uint32_t m = (n - done) * es > sizeof chunk ? sizeof chunk / es : n - done;
        if (rd(r, chunk, m * es)) return -1;
        for (uint32_t i = 0; dst && i < m; i++) {
            uint32_t v = two ? b16(chunk + i * 2) : chunk[i];
            uint32_t h = two ? (v >> 2) & 31 : v & 31;
            if ((done + i) % (uint32_t)div == 0) { best = v; bh = h; }
            else if (h > bh) { best = v; bh = h; }
            if ((done + i) % (uint32_t)div == (uint32_t)div - 1 || done + i == n - 1) {
                if (two) ((uint16_t *)dst)[k++] = (uint16_t)best; else ((uint8_t *)dst)[k++] = (uint8_t)best;
            }
        }
        done += m;
    }
    if (body > n * es && skip(r, body - n * es)) return -1;
    if (!dst) return 0;
    a->wave_div = div;
    if (two) { a->cwave = dst; a->ncwave = (int)k; } else { a->wave = dst; a->nwave = (int)k; }
    return 0;
}

static int read_beats(rd_t *r, anlz_t *a, uint32_t n, uint32_t body)
{
    if ((uint64_t)n * 8 > body) n = body / 8;
    anlz_beat *dst = n ? malloc(sizeof(anlz_beat) * n) : NULL;
    uint32_t done = 0;
    while (done < n) {
        uint32_t m = (n - done) * 8 > sizeof chunk ? sizeof chunk / 8 : n - done;
        if (rd(r, chunk, m * 8)) return -1;
        for (uint32_t i = 0; dst && i < m; i++) {
            const uint8_t *e = chunk + i * 8;
            dst[done + i] = (anlz_beat){ b32(e + 4), b16(e + 2), (uint8_t)b16(e) };
        }
        done += m;
    }
    if (body > n * 8 && skip(r, body - n * 8)) return -1;
    if (dst) { a->beats = dst; a->nbeats = (int)n; }
    return 0;
}

static int walk_file(const char *path, anlz_t *a, int *have_ext_cues)
{
    rd_t r = { hal_file_open(path), 0 };
    if (!r.f) return -1;
    uint8_t h[64];
    int ok = -1;
    if (rd(&r, h, 12) || memcmp(h, "PMAI", 4)) goto done;
    uint32_t first = b32(h + 4);
    if (first < 12 || skip(&r, first - 12)) goto done;
    ok = 0;
    for (;;) {
        if (rd(&r, h, 12)) break;                                  /* end of file */
        uint32_t hl = b32(h + 4), tl = b32(h + 8);
        if (tl < 12 || hl < 12 || hl > tl) break;
        uint32_t hrest = hl > sizeof h ? sizeof h - 12 : hl - 12;
        if (rd(&r, h + 12, hrest) || (hl > sizeof h && skip(&r, hl - (uint32_t)sizeof h))) break;
        uint32_t body = tl - hl;
        int e = 0;
        if (!memcmp(h, "PQTZ", 4) && hl >= 24 && !a->beats) e = read_beats(&r, a, b32(h + 20), body);
        else if (!memcmp(h, "PWV3", 4) && hl >= 24 && !a->wave && !a->cwave) e = read_wave(&r, a, b32(h + 16), body, 0);
        else if (!memcmp(h, "PWV5", 4) && hl >= 24 && !a->cwave) e = read_wave(&r, a, b32(h + 16), body, 1);
        else if ((!memcmp(h, "PWAV", 4) || !memcmp(h, "PCO2", 4) || !memcmp(h, "PCOB", 4)) && hl <= sizeof h && body <= 65536u) {
            uint8_t *t = malloc(tl);
            if (!t) e = skip(&r, body);
            else {
                memcpy(t, h, hl);
                e = rd(&r, t + hl, body);
                if (!e) on_tag(a, t, hl, tl, have_ext_cues);
                free(t);
            }
        } else e = skip(&r, body);
        if (e) break;
    }
done:
    hal_file_close(r.f);
    return ok;
}

static int cmp_cue(const void *x, const void *y)
{
    const anlz_cue *a = x, *b = y;
    return a->time_ms < b->time_ms ? -1 : a->time_ms > b->time_ms;
}

int anlz_load(anlz_t *a, const char *dat_path)
{
    memset(a, 0, sizeof *a);
    if (!dat_path || !dat_path[0]) return -1;
    char ext[700];
    snprintf(ext, sizeof ext, "%s", dat_path);
    size_t l = strlen(ext);
    if (l > 4) memcpy(ext + l - 4, ".EXT", 4);

    int have_ext_cues = 0, ok = 0;
    /* EXT first so its richer cue list (PCO2) wins over the DAT's PCOB */
    ok |= walk_file(ext, a, &have_ext_cues) == 0;
    ok |= walk_file(dat_path, a, &have_ext_cues) == 0;
    if (a->cwave && a->wave) { free(a->wave); a->wave = NULL; a->nwave = 0; }   /* color wins; mono unused */
    if (!a->wave_div) a->wave_div = 1;
    if (a->ncues > 1) qsort(a->cues, (size_t)a->ncues, sizeof(anlz_cue), cmp_cue);
    return ok ? 0 : -1;
}

void anlz_free(anlz_t *a)
{
    free(a->beats); free(a->wave); free(a->cwave);
    memset(a, 0, sizeof *a);
}

int anlz_read_path(const uint8_t *b, size_t n, char *out, size_t cap)
{
    out[0] = 0;
    if (n < 12 || memcmp(b, "PMAI", 4)) return -1;
    size_t o = b32(b + 4);
    while (o + 16 <= n) {
        uint32_t hl = b32(b + o + 4), tl = b32(b + o + 8);
        if (tl < 12 || o + tl > n) break;
        if (!memcmp(b + o, "PPTH", 4)) {
            uint32_t len = b32(b + o + 12);
            if (o + hl + len > n) return -1;
            utf16_to_utf8(b + o + hl, (int)(len / 2), 1, out, cap);
            return 0;
        }
        o += tl;
    }
    return -1;
}
