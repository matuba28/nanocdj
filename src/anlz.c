#include "anlz.h"
#include "hal.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint16_t b16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

typedef void (*tag_fn)(anlz_t *a, const uint8_t *tag, uint32_t hlen, uint32_t tlen, int *have_ext_cues);

static int walk(const uint8_t *b, size_t n, anlz_t *a, tag_fn fn, int *flag)
{
    if (n < 12 || memcmp(b, "PMAI", 4) != 0) return -1;
    size_t o = b32(b + 4);
    while (o + 12 <= n) {
        uint32_t hl = b32(b + o + 4), tl = b32(b + o + 8);
        if (tl < 12 || hl > tl || o + tl > n) break;
        fn(a, b + o, hl, tl, flag);
        o += tl;
    }
    return 0;
}

static void add_cue(anlz_t *a, int hot, int type, uint32_t t, uint32_t lt)
{
    if (a->ncues >= ANLZ_MAX_CUES) return;
    anlz_cue *c = &a->cues[a->ncues++];
    c->hot = hot;
    c->is_loop = type == 2 && lt != 0xffffffff;
    c->time_ms = t;
    c->loop_ms = c->is_loop ? lt : 0;
}

static void on_tag(anlz_t *a, const uint8_t *t, uint32_t hl, uint32_t tl, int *have_ext_cues)
{
    const uint8_t *body = t + hl, *end = t + tl;
    if (!memcmp(t, "PQTZ", 4) && hl >= 24 && !a->beats) {
        uint32_t n = b32(t + 20);
        if (body + (size_t)n * 8 > end) n = (uint32_t)((end - body) / 8);
        a->beats = malloc(sizeof(anlz_beat) * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = body + i * 8;
            a->beats[i] = (anlz_beat){ b32(e + 4), b16(e + 2), (uint8_t)b16(e) };
        }
        a->nbeats = (int)n;
    } else if (!memcmp(t, "PWAV", 4) && hl >= 20) {
        uint32_t n = b32(t + 12);
        if (n > 400) n = 400;
        if (body + n <= end) { memcpy(a->preview, body, n); a->has_preview = 1; }
    } else if (!memcmp(t, "PWV3", 4) && hl >= 24 && !a->wave) {
        uint32_t n = b32(t + 16);
        if (body + n > end) n = (uint32_t)(end - body);
        a->wave = malloc(n ? n : 1); memcpy(a->wave, body, n); a->nwave = (int)n;
    } else if (!memcmp(t, "PWV5", 4) && hl >= 24 && !a->cwave) {
        uint32_t n = b32(t + 16);
        if (body + (size_t)n * 2 > end) n = (uint32_t)((end - body) / 2);
        a->cwave = malloc(sizeof(uint16_t) * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++) a->cwave[i] = b16(body + i * 2);
        a->ncwave = (int)n;
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
    size_t n;
    /* EXT first so its richer cue list (PCO2) wins over the DAT's PCOB */
    uint8_t *b = hal_read_file(ext, &n);
    if (b) { ok |= walk(b, n, a, on_tag, &have_ext_cues) == 0; free(b); }
    b = hal_read_file(dat_path, &n);
    if (b) { ok |= walk(b, n, a, on_tag, &have_ext_cues) == 0; free(b); }
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
