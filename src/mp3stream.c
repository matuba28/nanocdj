/* See mp3stream.h. */
#include "mp3stream.h"
#include "hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "../third_party/minimp3/minimp3.h"

#define WIN_MAX    (1024u * 1024u)    /* compressed window: ~26 s at 320 kbps (~6 behind, ~20 ahead) */
#define WIN_MIN    (192u * 1024u)
/* The cue buffer has to outlast the re-read a far CUE needs: no seek, so jumping
 * back means reading the file again from the start, at ~0.7 MB/s on the iPod
 * (measured): ~7 s for a cue two minutes into a 320 kbps track. */
#define CUE_BYTES  (768u * 1024u)     /* ~19 s at 320 kbps */
#define CUE_MIN    (64u * 1024u)
#define NSLOTS     96                 /* decoded frames cached: ~2.5 s */
#define CHUNK      (32u * 1024u)      /* largest read call; slide granularity */
#define READ_MIN   (4u * 1024u)       /* smallest read call (the OS file cache is 4 KB) */
#define LOW_AHEAD_S 4                 /* less than this read ahead: read harder */
#define URGENT_US  12000u             /* per-frame read budget while catching up */
#define CUE_LEAD   5                  /* cue buffer starts this many frames early (decoder priming) */
#define NONE       0xffffffffu

struct mp3s {
    void    *fh;
    char     path[640];
    uint32_t fpos;            /* file offset of the next read */
    int      file_eof;

    uint8_t *win;             /* compressed window: file bytes [base, base+len) */
    uint32_t cap, base, len;

    uint32_t scan;            /* file offset of the next header to parse */
    uint32_t *idx;            /* byte offset of every frame, in file order */
    uint32_t nidx, idx_cap;
    int      ended;           /* every frame is indexed */
    uint32_t end_off;

    int      have_fmt, ver, sr_idx, hz, spf, first;   /* first = index of the first audio frame */
    int      bitrate, lame_delay, lame_pad, has_xing;

    uint8_t *cb;              /* cue buffer: file bytes [cbase, cbase+clen) */
    uint32_t ccap, cbase, clen;
    int64_t  cue_frame;

    mp3dec_t dec;
    int64_t  dec_last;        /* frame index the decoder state follows, -1 = none */
    int16_t *slots;           /* NSLOTS decoded frames, stereo */
    int64_t  tags[NSLOTS];    /* audio frame number held by each slot, -1 = empty */
    int16_t  tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];

    uint32_t bytes_read, reopens, misses, decodes, read_us;
    uint32_t us_per_kb;       /* measured read cost (EWMA) */
    uint32_t ahead_ms;        /* audio read ahead of the playhead, at the last service */
};

static uint8_t skipbuf[CHUNK];   /* bytes read only to move forward (no seek) */

/* ---------------- MPEG audio Layer III headers ---------------- */

static const uint16_t br_kbps[2][15] = {
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },          /* MPEG 2 / 2.5 */
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },      /* MPEG 1 */
};
static const uint32_t sr_hz[3] = { 44100, 48000, 32000 };

typedef struct { int ver, sr_idx, hz, spf, len, mono, kbps; } hdr_t;

/* ver: 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
static int parse_hdr(const uint8_t *h, hdr_t *o)
{
    if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return 0;
    int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    int bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return 0;
    int m1 = ver == 3;
    o->ver = ver; o->sr_idx = sri;
    o->hz = (int)(sr_hz[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2));
    o->kbps = br_kbps[m1][bri];
    o->spf = m1 ? 1152 : 576;
    o->len = (m1 ? 144 : 72) * o->kbps * 1000 / o->hz + pad;
    o->mono = (h[3] >> 6) == 3;
    return o->len > 4;
}

/* Xing/Info header in the first frame (an encoder's silent metadata frame).
 * *lame is set when the encoder tag says LAME. */
static int is_xing(const uint8_t *f, const hdr_t *h, int *delay, int *pad, int *lame)
{
    int side = h->ver == 3 ? (h->mono ? 17 : 32) : (h->mono ? 9 : 17);
    const uint8_t *x = f + 4 + side;
    if (4 + side + 8 > h->len) return 0;
    if (memcmp(x, "Xing", 4) && memcmp(x, "Info", 4)) return 0;
    uint32_t flags = (uint32_t)x[4] << 24 | (uint32_t)x[5] << 16 | (uint32_t)x[6] << 8 | x[7];
    int p = 8 + (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
    if (4 + side + p + 24 <= h->len) {            /* LAME extension: encoder delay / padding */
        *lame = !memcmp(x + p, "LAME", 4);
        const uint8_t *l = x + p + 21;
        *delay = (l[0] << 4) | (l[1] >> 4);
        *pad = ((l[1] & 15) << 8) | l[2];
    }
    return 1;
}

/* ---------------- buffers ---------------- */

static const uint8_t *bytes_at(const mp3s_t *s, uint32_t off, uint32_t n)
{
    if (off >= s->base && off + n <= s->base + s->len) return s->win + (off - s->base);
    if (s->clen && off >= s->cbase && off + n <= s->cbase + s->clen) return s->cb + (off - s->cbase);
    return NULL;
}

static uint32_t frame_len(const mp3s_t *s, uint32_t f)
{
    if (f + 1 < s->nidx) return s->idx[f + 1] - s->idx[f];
    hdr_t h; const uint8_t *p = bytes_at(s, s->idx[f], 4);
    return p && parse_hdr(p, &h) ? (uint32_t)h.len : 0;
}

static int push_idx(mp3s_t *s, uint32_t off)
{
    if (s->nidx == s->idx_cap) {
        uint32_t nc = s->idx_cap + s->idx_cap / 2 + 1024;
        if ((size_t)(nc - s->idx_cap) * 4 > hal_mem_avail()) return 0;
        uint32_t *n = (uint32_t *)realloc(s->idx, (size_t)nc * 4);
        if (!n) return 0;
        s->idx = n; s->idx_cap = nc;
    }
    s->idx[s->nidx++] = off;
    return 1;
}

/* Parse headers from s->scan as far as the window allows. */
static void parse(mp3s_t *s)
{
    while (!s->ended) {
        uint32_t end = s->base + s->len;
        if (s->scan < s->base) s->scan = s->base;            /* (only after a skip past unparsed data) */
        if (s->scan + 10 > end) {
            if (s->file_eof && s->scan + 10 > s->fpos) { s->ended = 1; s->end_off = s->scan; }
            return;
        }
        const uint8_t *p = s->win + (s->scan - s->base);
        if (s->scan == 0 && !memcmp(p, "ID3", 3)) {          /* ID3v2 tag (artwork can be big) */
            uint32_t sz = (uint32_t)(p[6] & 127) << 21 | (uint32_t)(p[7] & 127) << 14 | (uint32_t)(p[8] & 127) << 7 | (p[9] & 127);
            s->scan = 10 + sz + ((p[5] & 0x10) ? 10 : 0);
            continue;
        }
        hdr_t h;
        if (!parse_hdr(p, &h) || (s->have_fmt && (h.ver != s->ver || h.sr_idx != s->sr_idx))) { s->scan++; continue; }
        if (!s->have_fmt) {
            /* lock on only where a second header follows, and look for a Xing frame */
            if (s->scan + (uint32_t)h.len + 4 > end) {
                if (s->file_eof && s->scan + 10 > s->fpos) { s->ended = 1; s->end_off = s->scan; }
                return;
            }
            hdr_t h2;
            if (!parse_hdr(p + h.len, &h2) || h2.ver != h.ver || h2.sr_idx != h.sr_idx) { s->scan++; continue; }
            s->have_fmt = 1; s->ver = h.ver; s->sr_idx = h.sr_idx; s->hz = h.hz; s->spf = h.spf; s->bitrate = h.kbps;
            /* Where Rekordbox's 0:00 is (measured on 80 tracks against their beat
             * grids): it plays a LAME-encoded file's info frame as 1152 samples of
             * silence, but skips the info frame ffmpeg ("Lavc") writes. Matching it
             * keeps the audio on the grid; getting it wrong puts every beat 26 ms off. */
            int lame = 0;
            int xing = is_xing(p, &h, &s->lame_delay, &s->lame_pad, &lame);
            s->has_xing = xing;
            s->first = xing && !lame ? 1 : 0;
        }
        if (!push_idx(s, s->scan)) { s->ended = 1; s->end_off = s->scan; return; }
        if (s->nidx > (uint32_t)s->first + 1u) s->bitrate = h.kbps;          /* (not the info frame's) */
        s->scan += (uint32_t)h.len;
    }
}

/* copy what the window holds of the cue region into the cue buffer */
static void fill_cue(mp3s_t *s)
{
    if (!s->cb || s->cbase == NONE || s->clen >= s->ccap) return;
    uint32_t want = s->cbase + s->clen;
    if (want < s->base || want >= s->base + s->len) return;
    uint32_t n = s->base + s->len - want;
    if (n > s->ccap - s->clen) n = s->ccap - s->clen;
    memcpy(s->cb + s->clen, s->win + (want - s->base), n);
    s->clen += n;
}

/* One read, timed: keeps a running cost per KB so later reads can be sized to
 * fit the frame's budget (a 32 KB read blocked the iPod for ~44 ms). */
static int32_t timed_read(mp3s_t *s, void *buf, uint32_t n)
{
    uint32_t t = hal_time_us();
    int32_t r = hal_file_read(s->fh, buf, n);
    if (r >= 1024) {
        uint32_t per = (uint32_t)((uint64_t)(hal_time_us() - t) * 1024u / (uint32_t)r);
        s->us_per_kb = (s->us_per_kb * 3u + per) / 4u;
        if (!s->us_per_kb) s->us_per_kb = 1;
    }
    return r;
}

static void reopen(mp3s_t *s, uint32_t new_base)
{
    if (s->fh) hal_file_close(s->fh);
    s->fh = hal_file_open(s->path);
    s->fpos = 0; s->file_eof = !s->fh;
    s->base = new_base; s->len = 0;
    s->reopens++;
}

/* ---------------- public ---------------- */

int mp3s_open_error;

mp3s_t *mp3s_open(const char *path, double est_seconds)
{
    mp3s_open_error = MP3S_ERR_MEMORY;
    if (hal_mem_avail() < sizeof(mp3s_t) + WIN_MIN + CUE_MIN + NSLOTS * 1152u * 4u + 65536u) return NULL;
    mp3s_t *s = (mp3s_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->fh = hal_file_open(path);
    if (!s->fh) { free(s); mp3s_open_error = MP3S_ERR_FILE; return NULL; }

    s->slots = (int16_t *)malloc(NSLOTS * 1152u * 4u);
    s->idx_cap = (uint32_t)((est_seconds > 0 ? est_seconds + 60 : 600) * 48000 / 1152) + 64;
    s->idx = (uint32_t *)malloc((size_t)s->idx_cap * 4);
    /* split what's free between the window and the cue buffer (separate blocks) */
    size_t avail = hal_mem_avail();
    size_t budget = avail / 10 * 8;
    uint32_t ccap = (uint32_t)(budget * 4 / 10 > CUE_BYTES ? CUE_BYTES : budget * 4 / 10);
    uint32_t cap = (uint32_t)(budget - ccap > WIN_MAX ? WIN_MAX : budget - ccap);
    if (ccap < CUE_MIN) ccap = 0;
    if (s->slots && s->idx && cap >= WIN_MIN) { s->win = (uint8_t *)malloc(cap); s->cap = cap; }
    if (s->win && ccap) { s->cb = (uint8_t *)malloc(ccap); s->ccap = s->cb ? ccap : 0; }
    s->us_per_kb = 1500;                               /* ~0.7 MB/s until measured */
    if (!s->slots || !s->idx || !s->win) { mp3s_close(s); return NULL; }
    mp3s_open_error = 0;

    for (int i = 0; i < NSLOTS; i++) s->tags[i] = -1;
    s->dec_last = -1;
    s->cue_frame = -1; s->cbase = NONE;
    mp3dec_init(&s->dec);
    return s;
}

void mp3s_close(mp3s_t *s)
{
    if (!s) return;
    if (s->fh) hal_file_close(s->fh);
    free(s->win); free(s->cb); free(s->slots); free(s->idx);
    free(s);
}

int mp3s_hz(const mp3s_t *s) { return s->have_fmt ? s->hz : 0; }
int mp3s_searching(const mp3s_t *s) { return !s->have_fmt && !s->file_eof && s->fh; }

int64_t mp3s_length(const mp3s_t *s)
{
    if (!s->ended || !s->have_fmt) return -1;
    return (int64_t)(s->nidx - (uint32_t)s->first) * s->spf;
}

/* audio frame number -> frame index; -1 if before the stream */
static int64_t frame_of(const mp3s_t *s, int64_t j) { return j < 0 ? -1 : j / s->spf + s->first; }

void mp3s_service(mp3s_t *s, int64_t play, int64_t cue, uint32_t budget_us)
{
    uint32_t t0 = hal_time_us();
    s->read_us = 0;

    /* cue buffer target */
    if (s->have_fmt && s->cb) {
        int64_t cf = cue >= 0 ? frame_of(s, cue) - CUE_LEAD : -1;
        if (cue >= 0 && cf < 0) cf = 0;
        if (cf != s->cue_frame) { s->cue_frame = cf; s->clen = 0; s->cbase = NONE; }
        if (cf >= 0 && s->cbase == NONE && cf < (int64_t)s->nidx) s->cbase = s->idx[cf];
    }

    uint32_t back = s->cap / 4;
    int64_t pf = s->have_fmt ? frame_of(s, play < 0 ? 0 : play) : 0;
    uint32_t play_off = NONE;
    if (pf >= 0 && pf < (int64_t)s->nidx) play_off = s->idx[pf];

    /* move the window to the playhead if it has left it */
    if (play_off != NONE) {
        uint32_t want_base = play_off > back ? play_off - back : 0;
        if (play_off < s->base) reopen(s, want_base);                       /* behind: start over */
        else if (want_base > s->base + s->len && want_base >= s->fpos) {   /* ahead (index known): skip to it */
            s->base = want_base; s->len = 0;
        }
    }

    /* read harder while catching up: skipping to a new spot, the playhead not
     * indexed yet, or little read ahead of it */
    s->ahead_ms = 0;
    if (play_off != NONE && s->base + s->len > play_off && s->bitrate)
        s->ahead_ms = (uint32_t)((uint64_t)(s->base + s->len - play_off) * 8u / (uint32_t)s->bitrate);
    int urgent = s->fpos < s->base || (s->have_fmt && play_off == NONE) ||
                 (!s->ended && !s->file_eof && s->ahead_ms < LOW_AHEAD_S * 1000u && s->len < s->cap);
    if (urgent && budget_us < URGENT_US) budget_us = URGENT_US;

    for (int reads = 0; s->fh && !s->file_eof; reads++) {
        /* size this read to what's left of the budget (at least one small read per call) */
        uint32_t used = hal_time_us() - t0;
        uint32_t left = used < budget_us ? budget_us - used : 0;
        uint32_t fit = (uint32_t)((uint64_t)left * 1024u / s->us_per_kb) & ~(READ_MIN - 1);
        if (fit < READ_MIN) { if (reads) break; fit = READ_MIN; }
        if (fit > CHUNK) fit = CHUNK;
        if (s->fpos < s->base) {                                            /* skip forward (no seek) */
            uint32_t n = s->base - s->fpos;
            if (n > fit) n = fit;
            int32_t r = timed_read(s, skipbuf, n);
            if (r <= 0) { s->file_eof = 1; break; }
            s->fpos += (uint32_t)r; s->bytes_read += (uint32_t)r;
            continue;
        }
        if (s->len == s->cap) {                                             /* full: slide if we can */
            uint32_t keep;
            if (play_off != NONE) keep = play_off > s->base + back ? play_off - back : s->base;
            else keep = s->base + s->len - back;                            /* playhead not indexed yet */
            if (keep > s->scan) keep = s->scan;                             /* never drop unparsed bytes */
            if (keep < s->base) keep = s->base;
            if (keep > s->base + s->len) keep = s->base + s->len;
            uint32_t shift = keep - s->base;
            if (shift < CHUNK) break;                                       /* full of what we want */
            memmove(s->win, s->win + shift, s->len - shift);
            s->base += shift; s->len -= shift;
        }
        uint32_t n = s->cap - s->len;
        if (n > fit) n = fit;
        int32_t r = timed_read(s, s->win + s->len, n);
        if (r <= 0) { s->file_eof = 1; break; }
        s->len += (uint32_t)r; s->fpos += (uint32_t)r; s->bytes_read += (uint32_t)r;
        parse(s);
        if (s->cb && s->have_fmt && s->cue_frame >= 0 && s->cbase == NONE && s->cue_frame < (int64_t)s->nidx)
            s->cbase = s->idx[s->cue_frame];
        fill_cue(s);
        if (!s->have_fmt) continue;
        if (play_off == NONE && pf >= 0 && pf < (int64_t)s->nidx) play_off = s->idx[pf];
    }
    parse(s);           /* notices the end of the file */
    fill_cue(s);
    s->read_us = hal_time_us() - t0;
}

/* decode frame f (file order); store its output if `keep` */
static int decode(mp3s_t *s, int64_t f, int keep)
{
    if (f < 0 || f >= (int64_t)s->nidx) return 0;
    /* hand minimp3 up to three whole frames: it needs the next header to lock on */
    uint32_t off = s->idx[f], n = 0;
    const uint8_t *p = NULL;
    for (int k = 3; k >= 1 && !p; k--) {
        n = 0;
        for (int i = 0; i < k && f + i < (int64_t)s->nidx; i++) n += frame_len(s, (uint32_t)(f + i));
        if (n) p = bytes_at(s, off, n);
    }
    if (!p) { s->dec_last = -1; return 0; }
    mp3dec_frame_info_t info;
    int ns = mp3dec_decode_frame(&s->dec, p, (int)n, s->tmp, &info);
    s->decodes++;
    if (ns <= 0 || info.frame_offset != 0) { s->dec_last = -1; return 0; }
    s->dec_last = f;
    if (!keep) return 1;
    int64_t a = f - s->first;
    int16_t *dst = s->slots + (size_t)(a % NSLOTS) * 1152u * 2u;
    if (info.channels == 1) for (int i = 0; i < ns; i++) dst[2 * i] = dst[2 * i + 1] = s->tmp[i];
    else memcpy(dst, s->tmp, (size_t)ns * 4);
    for (int i = ns; i < s->spf; i++) dst[2 * i] = dst[2 * i + 1] = 0;
    s->tags[a % NSLOTS] = a;
    return 1;
}

const int16_t *mp3s_samples(mp3s_t *s, int64_t j, int *n)
{
    if (!s->have_fmt || j < 0) return NULL;
    int64_t a = j / s->spf, f = a + s->first;
    int off = (int)(j - a * s->spf);
    int slot = (int)(a % NSLOTS);
    if (s->tags[slot] != a) {
        if (f >= (int64_t)s->nidx) { s->misses++; return NULL; }
        if (s->dec_last != f - 1) {
            /* Jumped: prime the decoder. Frame f-1 must decode cleanly (its second
             * half feeds f's overlap and filterbank), and its data can start up to
             * 511 bytes back (the bit reservoir), so go back until the frames
             * before f-1 hold that much: two frames at 320 kbps, three at 128. */
            int64_t g = f - 1;
            uint32_t acc = 0;
            while (g > 0 && (acc < 512 || g > f - 2)) { g--; acc += frame_len(s, (uint32_t)g); }
            if (g < 0) g = 0;
            for (; g < f; g++) decode(s, g, 0);
        }
        if (!decode(s, f, 1)) { s->misses++; return NULL; }
    }
    *n = s->spf - off;
    return s->slots + (size_t)slot * 1152u * 2u + (size_t)off * 2u;
}

void mp3s_stats(const mp3s_t *s, mp3s_stats_t *st)
{
    st->window_bytes = s->len; st->window_cap = s->cap; st->cue_bytes = s->clen;
    st->frames_indexed = s->nidx; st->bytes_read = s->bytes_read; st->reopens = s->reopens;
    st->misses = s->misses; st->decodes = s->decodes; st->read_us = s->read_us;
    st->eof = s->ended; st->hz = s->hz; st->bitrate_kbps = s->bitrate;
    st->xing = s->has_xing ? (s->first ? 1 : 2) : 0;
    st->read_kbps = s->us_per_kb ? 1000000u / s->us_per_kb : 0;
    st->ahead_ms = s->ahead_ms; st->lame_delay = s->lame_delay;
}
