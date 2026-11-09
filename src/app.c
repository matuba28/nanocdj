/* The CDJ app: browser screen + deck screen, all touch driven. 240x432 portrait. */
#include "app.h"
#include "deck.h"
#include "gfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- colours ---------- */
#define C_BG      RGB(0, 0, 0)
#define C_PANEL   RGB(24, 24, 28)
#define C_EDGE    RGB(60, 60, 68)
#define C_TEXT    RGB(235, 235, 235)
#define C_DIM     RGB(130, 130, 140)
#define C_ORANGE  RGB(255, 140, 0)
#define C_RED     RGB(230, 40, 40)
#define C_GREEN   RGB(40, 210, 90)
#define C_BLUE    RGB(40, 120, 255)
#define C_LOOP    RGB(70, 45, 0)

/* ---------- layout ---------- */
#define HDR_H 28
#define OV_X 4
#define OV_Y 31
#define OV_W 232
#define OV_H 30
#define DT_Y 64
#define DT_H 80
#define INFO_Y 147
#define JOG_X 4
#define JOG_Y 192
#define JOG_W 168
#define JOG_H 140
#define JOG_CX (JOG_X + JOG_W / 2)
#define JOG_CY (JOG_Y + JOG_H / 2)
#define JOG_R 66
#define SL_X 178
#define SL_W 58
#define SL_Y 192
#define SL_FINE_H 26
#define SL_TRACK_Y (SL_Y + SL_FINE_H + 4)
#define SL_TRACK_H 176
#define BTN_Y 336
#define BTN_W 53
#define BTN_H 44
#define ROW_H 48      /* browser row */
#define BR_HDR_H 40   /* browser header */

enum { CTL_NONE, CTL_BACK, CTL_OVERVIEW, CTL_DETAIL, CTL_RANGE, CTL_QUANT, CTL_JOGMODE, CTL_JOG,
       CTL_SLIDER, CTL_FINE_MINUS, CTL_FINE_PLUS,
       CTL_CUE, CTL_IN, CTL_OUT, CTL_PLAY, CTL_BEAT, CTL_EXIT,
       CTL_LIST, CTL_TO_DECK };

typedef enum { SCR_BROWSE, SCR_DECK } screen_t;

typedef struct { int is_folder, is_track; uint32_t id; char label[128]; char sub[96]; char right[16];
                 uint32_t bpm100; int order; } item_t;

/* track-list sorting */
enum { SORT_ORDER, SORT_BPM, SORT_ARTIST, SORT_NAME, SORT_COUNT };
static const char *const sort_label[SORT_COUNT] = { "#", "BPM", "ARTIST", "NAME" };
static const int sort_w[SORT_COUNT] = { 36, 60, 84, 60 };   /* segment widths, sum = SCREEN_W */
#define SORT_H 34

typedef struct { int active, ctl, x0, y0, lx, ly, moved; double ang; uint32_t t0, rep; } touch_t;
#define NUDGE_DELAY_MS 400   /* holding a pitch nudge button repeats after this... */
#define NUDGE_RATE_MS  60    /* ...every this often (~0.33%/s in 0.02% steps) */

static struct {
    library *L;
    deck_t deck;
    screen_t scr;

    /* browser */
    uint32_t folder_stack[32]; int fsp;   /* stack of folder ids; top is current */
    int in_tracks; uint32_t playlist;     /* showing tracks of `playlist` (0xffffffff = all) */
    char title[96];
    item_t *items; int nitems;
    int scroll, list_save_scroll[32];

    const lib_track *cur;
    int zoom;
    touch_t touch[8];
    char status[64]; uint32_t status_until;
    uint32_t fader_tap_ms; int fader_tap_y;   /* last tap (no drag) on the pitch fader */
    int sort_mode, sort_desc;                  /* kept while moving between playlists */
    int quit;
} ui;

#define DOUBLE_TAP_MS 350
#define CTL_SORT_BASE 100   /* touch ctl ids for the sort bar segments: CTL_SORT_BASE + mode */

#define ALL_TRACKS 0xffffffffu

static void status(const char *s) { snprintf(ui.status, sizeof ui.status, "%s", s); ui.status_until = hal_ticks_ms() + 1500; }

/* ================= browser ================= */

static item_t *add_item(void)
{
    if ((ui.nitems & 63) == 0) ui.items = realloc(ui.items, sizeof(item_t) * (ui.nitems + 64));
    item_t *it = &ui.items[ui.nitems++];
    memset(it, 0, sizeof *it);
    return it;
}

static void add_track_item(const lib_track *t)
{
    item_t *it = add_item();
    it->is_track = 1; it->id = t->id;
    it->bpm100 = t->bpm100;
    it->order = ui.nitems - 1;
    snprintf(it->label, sizeof it->label, "%s", t->title);
    snprintf(it->sub, sizeof it->sub, "%s", t->artist);
    if (t->bpm100) snprintf(it->right, sizeof it->right, "%.1f", t->bpm100 / 100.0);
}

/* case-insensitive ASCII compare */
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

static int cmp_items(const void *pa, const void *pb)
{
    const item_t *a = pa, *b = pb;
    int r = 0, missing_a = 0, missing_b = 0;
    switch (ui.sort_mode) {
    case SORT_BPM:    missing_a = !a->bpm100; missing_b = !b->bpm100; break;
    case SORT_ARTIST: missing_a = !a->sub[0]; missing_b = !b->sub[0]; break;
    case SORT_NAME:   missing_a = !a->label[0]; missing_b = !b->label[0]; break;
    default: break;
    }
    if (missing_a != missing_b) return missing_a - missing_b;   /* missing values always last */
    if (!missing_a) {
        switch (ui.sort_mode) {
        case SORT_BPM:    r = (a->bpm100 > b->bpm100) - (a->bpm100 < b->bpm100); break;
        case SORT_ARTIST: r = ci_cmp(a->sub, b->sub); if (!r) r = ci_cmp(a->label, b->label); break;
        case SORT_NAME:   r = ci_cmp(a->label, b->label); break;
        default: break;
        }
    }
    if (ui.sort_desc) r = -r;
    return r ? r : a->order - b->order;   /* stable: ties keep playlist order */
}

static void sort_items(void)
{
    if (ui.nitems > 1) qsort(ui.items, (size_t)ui.nitems, sizeof(item_t), cmp_items);
}

static int list_top(void) { return BR_HDR_H + (ui.in_tracks ? SORT_H : 0); }

static void build_list(void)
{
    library *L = ui.L;
    ui.nitems = 0;
    if (ui.in_tracks) {
        if (ui.playlist == ALL_TRACKS) {
            for (int i = 0; i < L->nt; i++) add_track_item(&L->t[i]);
        } else {
            for (int i = 0; i < L->ne; i++)
                if (L->e[i].playlist == ui.playlist) {
                    const lib_track *t = lib_find(L, L->e[i].track);
                    if (t) add_track_item(t);
                }
        }
        sort_items();
        return;
    }
    uint32_t folder = ui.folder_stack[ui.fsp];
    if (ui.fsp == 0) {
        item_t *it = add_item();
        it->id = ALL_TRACKS;
        snprintf(it->label, sizeof it->label, "ALL TRACKS");
        snprintf(it->sub, sizeof it->sub, "%d tracks", L->nt);
    }
    for (int i = 0; i < L->np; i++) {
        const lib_playlist *p = &L->p[i];
        if (p->parent != folder) continue;
        item_t *it = add_item();
        it->is_folder = p->is_folder; it->id = p->id;
        snprintf(it->label, sizeof it->label, "%s", p->name);
        snprintf(it->sub, sizeof it->sub, "%s", p->is_folder ? "folder" : "playlist");
    }
}

static const char *playlist_name(uint32_t id)
{
    if (id == ALL_TRACKS) return "ALL TRACKS";
    for (int i = 0; i < ui.L->np; i++) if (ui.L->p[i].id == id) return ui.L->p[i].name;
    return "?";
}

static void browser_back(void)
{
    if (ui.in_tracks) { ui.in_tracks = 0; ui.scroll = ui.list_save_scroll[ui.fsp]; }
    else if (ui.fsp > 0) { ui.fsp--; ui.scroll = ui.list_save_scroll[ui.fsp]; }
    else if (ui.cur) { ui.scr = SCR_DECK; return; }
    snprintf(ui.title, sizeof ui.title, "%s", ui.fsp ? playlist_name(ui.folder_stack[ui.fsp]) : ui.L->source);
    build_list();
}

static void draw_browser(void);

static void load_track(const lib_track *t)
{
    /* show a loading frame, decoding blocks */
    draw_browser();
    gfx_rect(30, 186, 180, 48, C_PANEL); gfx_frame(30, 186, 180, 48, C_ORANGE);
    gfx_text_center(120, 202, "LOADING...", C_ORANGE, 2);
    if (deck_load(&ui.deck, t->path, t->anlz) != 0) { status("CAN'T OPEN FILE"); return; }
    ui.cur = t;
    ui.scr = SCR_DECK;
    if (!ui.deck.an.nbeats) status("NO BEAT GRID");
}

static void browser_select(int idx)
{
    if (idx < 0 || idx >= ui.nitems) return;
    item_t *it = &ui.items[idx];
    if (it->is_track) { const lib_track *t = lib_find(ui.L, it->id); if (t) load_track(t); return; }
    ui.list_save_scroll[ui.fsp] = ui.scroll;
    if (it->is_folder) {
        if (ui.fsp < 31) ui.folder_stack[++ui.fsp] = it->id;
    } else {
        ui.in_tracks = 1; ui.playlist = it->id;
    }
    snprintf(ui.title, sizeof ui.title, "%s", it->label);
    ui.scroll = 0;
    build_list();
}

static void clamp_scroll(void)
{
    int max = ui.nitems * ROW_H - (SCREEN_H - list_top());
    if (ui.scroll > max) ui.scroll = max;
    if (ui.scroll < 0) ui.scroll = 0;
}

static void draw_browser(void)
{
    gfx_clear(C_BG);
    clamp_scroll();
    int first = ui.scroll / ROW_H;
    for (int i = first; i < ui.nitems; i++) {
        int y = list_top() + i * ROW_H - ui.scroll;
        if (y >= SCREEN_H) break;
        item_t *it = &ui.items[i];
        int loaded = it->is_track && ui.cur && ui.cur->id == it->id;
        pixel_t c = loaded ? C_ORANGE : C_TEXT;
        int ix = it->is_folder ? 22 : 6;
        if (it->is_folder) gfx_text(6, y + 7, ">", C_DIM, 2, 0);
        gfx_text(ix, y + 7, it->label, c, 2, SCREEN_W - ix - 4);            /* line 1: title, 2x */
        int rw = it->right[0] ? gfx_text_w(it->right, 2) + 6 : 0;
        if (rw) gfx_text(SCREEN_W - rw, y + 27, it->right, loaded ? C_ORANGE : C_DIM, 2, 0);
        gfx_text(ix, y + 32, it->sub, C_DIM, 1, SCREEN_W - ix - 8 - rw);   /* line 2: artist + BPM */
        gfx_hline(0, SCREEN_W - 1, y + ROW_H - 1, RGB(30, 30, 34));
    }
    if (!ui.nitems) gfx_text_center(120, 200, "EMPTY", C_DIM, 2);
    if (ui.in_tracks) {                                              /* sort bar */
        int x = 0;
        gfx_rect(0, BR_HDR_H, SCREEN_W, SORT_H, C_BG);
        for (int m = 0; m < SORT_COUNT; m++) {
            int on = m == ui.sort_mode, w = sort_w[m];
            gfx_rect(x + 2, BR_HDR_H + 3, w - 4, SORT_H - 6, on ? C_ORANGE : C_PANEL);
            gfx_text_center(x + w / 2, BR_HDR_H + 7, sort_label[m], on ? C_BG : C_TEXT, 2);
            if (on && m != SORT_ORDER) {                             /* direction: triangle under the label */
                int tx = x + w / 2, ty = BR_HDR_H + 25;
                for (int j = 0; j < 4; j++) {
                    int row = ui.sort_desc ? ty + 3 - j : ty + j;    /* tip up = ascending */
                    gfx_hline(tx - j * 2, tx + j * 2, row, C_BG);
                }
            }
            x += w;
        }
        gfx_hline(0, SCREEN_W - 1, BR_HDR_H + SORT_H - 1, C_EDGE);
    }
    int back = ui.in_tracks || ui.fsp > 0;
    gfx_rect(0, 0, SCREEN_W, BR_HDR_H, C_PANEL);
    gfx_hline(0, SCREEN_W - 1, BR_HDR_H - 1, C_EDGE);
    if (back) gfx_text(6, 9, "<", C_ORANGE, 3, 0);
    if (ui.cur) gfx_text(SCREEN_W - 22, 9, ">", C_ORANGE, 3, 0);   /* jump straight back to the deck */
    gfx_text(back ? 30 : 8, 13, ui.title, C_TEXT, 2, SCREEN_W - (back ? 30 : 8) - (ui.cur ? 30 : 6));
}

/* ================= deck drawing ================= */

static pixel_t wave_color_mono(uint8_t b, int dim)
{
    int w = b >> 5;          /* whiteness 0..7 */
    int r = 20 + w * 30, g = 80 + w * 22, bl = 255;
    if (dim) { r /= 3; g /= 3; bl /= 3; }
    return RGB(r, g, bl);
}

static pixel_t wave_color_rgb(uint16_t v, int dim)
{
    int r = (v >> 13) & 7, g = (v >> 10) & 7, b = (v >> 7) & 7;
    int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (!m) m = 1;
    /* normalise brightness so quiet passages stay visible, like the CDJ display */
    int k = dim ? 80 : 255;
    return RGB(r * k / m, g * k / m, b * k / m);
}

static double frames_to_x_ov(double f, double total) { return OV_X + f / total * OV_W; }

static void draw_overview(const deck_t *d)
{
    gfx_rect(OV_X, OV_Y, OV_W, OV_H, RGB(10, 10, 14));
    double total = (double)d->pcm.frames;
    if (total <= 0) return;
    int ph = (int)frames_to_x_ov(d->pos, total);
    int mid = OV_Y + OV_H - 2;
    if (d->loop_in_set && d->loop_out > d->loop_in) {
        int a = (int)frames_to_x_ov(d->loop_in, total), b = (int)frames_to_x_ov(d->loop_out, total);
        gfx_rect(a, OV_Y, b - a + 1, OV_H, d->loop_active ? C_LOOP : RGB(35, 30, 20));
    }
    for (int x = 0; x < OV_W; x++) {
        int dim = OV_X + x < ph;
        if (d->an.has_preview) {
            uint8_t b = d->an.preview[x * 400 / OV_W];
            int h = (b & 31) * (OV_H - 3) / 31;
            gfx_vline(OV_X + x, mid - h, mid, wave_color_mono(b, dim));
        } else {
            gfx_vline(OV_X + x, mid - 2, mid, dim ? C_DIM : C_EDGE);
        }
    }
    for (int i = 0; i < d->an.ncues; i++) {
        const anlz_cue *c = &d->an.cues[i];
        int x = (int)frames_to_x_ov(c->time_ms * (AUDIO_RATE / 1000.0), total);
        gfx_vline(x, OV_Y, OV_Y + 4, c->hot ? C_GREEN : C_RED);
    }
    int cx = (int)frames_to_x_ov(d->cue, total);
    gfx_rect(cx - 1, OV_Y + OV_H - 3, 3, 3, C_ORANGE);
    gfx_vline(ph, OV_Y, OV_Y + OV_H - 1, C_RED);
}

static void draw_detail(const deck_t *d)
{
    int mid = DT_Y + DT_H / 2;
    gfx_rect(0, DT_Y, SCREEN_W, DT_H, RGB(6, 6, 10));
    const anlz_t *a = &d->an;
    double per_px = ui.zoom;                    /* waveform entries per pixel */
    double fpe = AUDIO_RATE / (double)ANLZ_WAVE_PER_SEC; /* frames per entry */
    double center = d->pos / fpe;
    double f_left = (center - 120 * per_px) * fpe, f_right = (center + 120 * per_px) * fpe;
#define F2X(f) (120 + ((f) / fpe - center) / per_px)

    if (d->loop_in_set && d->loop_out > d->loop_in) {
        int x0 = (int)F2X(d->loop_in), x1 = (int)F2X(d->loop_out);
        gfx_rect(x0, DT_Y, x1 - x0, DT_H, d->loop_active ? C_LOOP : RGB(30, 26, 16));
    }

    int n = a->ncwave ? a->ncwave : a->nwave;
    if (n) {
        for (int x = 0; x < SCREEN_W; x++) {
            int i0 = (int)floor(center + (x - 120) * per_px);
            int best = -1, bh = -1;
            for (int k = 0; k < (int)per_px; k++) {
                int i = i0 + k;
                if (i < 0 || i >= n) continue;
                int h = a->ncwave ? (a->cwave[i] >> 2) & 31 : a->wave[i] & 31;
                if (h > bh) { bh = h; best = i; }
            }
            if (best < 0) continue;
            int h = bh * (DT_H / 2 - 4) / 31;
            pixel_t c = a->ncwave ? wave_color_rgb(a->cwave[best], x < 120) : wave_color_mono(a->wave[best], x < 120);
            gfx_vline(x, mid - h, mid + h, c);
        }
    } else {
        gfx_text_center(120, mid - 3, "NO WAVEFORM", C_DIM, 1);
    }

    /* beat grid ticks */
    if (a->nbeats) {
        int b0 = deck_beat_at(d, f_left), b1 = deck_beat_at(d, f_right) + 1;
        if (b0 < 0) b0 = 0;
        for (int i = b0; i <= b1 && i < a->nbeats; i++) {
            int x = (int)F2X(deck_beat_frame(d, i));
            int down = a->beats[i].beat == 1;
            pixel_t c = down ? C_RED : RGB(200, 200, 200);
            int len = down ? 8 : 4;
            gfx_vline(x, DT_Y, DT_Y + len, c);
            gfx_vline(x, DT_Y + DT_H - 1 - len, DT_Y + DT_H - 1, c);
        }
    }
    /* stored cues */
    for (int i = 0; i < a->ncues; i++) {
        int x = (int)F2X(a->cues[i].time_ms * (AUDIO_RATE / 1000.0));
        if (x < -4 || x > SCREEN_W + 4) continue;
        pixel_t c = a->cues[i].hot ? C_GREEN : C_RED;
        for (int j = 0; j < 5; j++) gfx_hline(x - (4 - j), x + (4 - j), DT_Y + DT_H - 5 + j - 4, c);
    }
    /* active cue */
    int cx = (int)F2X(d->cue);
    for (int j = 0; j < 6; j++) gfx_hline(cx - (5 - j), cx + (5 - j), DT_Y + j, C_ORANGE);

    gfx_rect(119, DT_Y, 2, DT_H, C_TEXT);   /* playhead */
    char z[8]; snprintf(z, sizeof z, "x%d", ui.zoom);
    gfx_text(SCREEN_W - 20, DT_Y + DT_H - 9, z, C_DIM, 1, 0);
#undef F2X
}

static void fmt_time(char *out, size_t n, double sec, int neg)
{
    if (sec < 0) sec = 0;
    int m = (int)(sec / 60);
    double s = sec - m * 60;
    snprintf(out, n, "%s%d:%04.1f", neg ? "-" : "", m, s);
}

static void chip(int x, int y, int w, const char *s, int on)
{
    gfx_rect(x, y, w, 15, on ? C_ORANGE : C_PANEL);
    gfx_frame(x, y, w, 15, on ? C_ORANGE : C_EDGE);
    gfx_text_center(x + w / 2, y + 4, s, on ? C_BG : C_TEXT, 1);
}

static double cur_bpm(const deck_t *d)
{
    double base = d->an.nbeats ? deck_bpm_at(d, d->pos) : (ui.cur ? ui.cur->bpm100 / 100.0 : 0);
    return base * (1.0 + d->pitch);
}

static void draw_info(const deck_t *d)
{
    char s[32];
    double bpm = cur_bpm(d);
    if (bpm > 0) snprintf(s, sizeof s, "%5.1f", bpm); else snprintf(s, sizeof s, "--.-");
    gfx_text(4, INFO_Y + 2, s, C_TEXT, 3, 0);
    gfx_text(4 + 5 * 18 + 2, INFO_Y + 16, "BPM", C_DIM, 1, 0);

    snprintf(s, sizeof s, "%+.2f%%", d->pitch * 100);
    int w = gfx_text_w(s, 2);
    gfx_text(SCREEN_W - 4 - w, INFO_Y + 2, s, fabs(d->pitch) < 1e-6 ? C_GREEN : C_TEXT, 2, 0);

    double el = d->pos / AUDIO_RATE, total = d->pcm.frames / (double)AUDIO_RATE;
    fmt_time(s, sizeof s, el, 0);
    gfx_text(4, INFO_Y + 28, s, C_DIM, 1, 0);
    fmt_time(s, sizeof s, total - el, 1);
    gfx_text(60, INFO_Y + 28, s, C_TEXT, 1, 0);

    char r[8];
    if (d->pitch_range >= 0.5) snprintf(r, sizeof r, "WIDE"); else snprintf(r, sizeof r, "%d%%", (int)lround(d->pitch_range * 100));
    chip(126, INFO_Y + 24, 46, r, 0);
    chip(176, INFO_Y + 24, 26, "Q", d->quantize);
    if (ui.cur && ui.cur->key[0]) gfx_text(208, INFO_Y + 28, ui.cur->key, C_DIM, 1, 0);
}

static void draw_jog(const deck_t *d, int touched)
{
    gfx_rect(JOG_X, JOG_Y, JOG_W, JOG_H, C_BG);
    pixel_t ring = touched ? C_ORANGE : (d->jog_mode == JOG_VINYL ? RGB(200, 200, 210) : RGB(90, 90, 100));
    gfx_disc(JOG_CX, JOG_CY, JOG_R, RGB(18, 18, 22));
    gfx_ring(JOG_CX, JOG_CY, JOG_R - 6, JOG_R, ring);
    /* platter marker: 33 1/3 rpm */
    double rev = d->pos / AUDIO_RATE * (100.0 / 3.0) / 60.0;
    double ang = (rev - floor(rev)) * 2 * M_PI - M_PI / 2;
    gfx_disc(JOG_CX + (int)lround(cos(ang) * (JOG_R - 14)), JOG_CY + (int)lround(sin(ang) * (JOG_R - 14)), 4, C_TEXT);
    /* cue position indicator around the ring: track progress */
    if (d->pcm.frames) {
        double p = d->pos / d->pcm.frames;
        for (int k = 0; k < 64; k++) {
            if (k / 64.0 > p) break;
            double a = k / 64.0 * 2 * M_PI - M_PI / 2;
            gfx_pixel(JOG_CX + (int)lround(cos(a) * (JOG_R + 3)), JOG_CY + (int)lround(sin(a) * (JOG_R + 3)), C_ORANGE);
        }
    }
    /* centre state */
    const char *st = d->cue_previewing ? "CUE" : d->playing ? "PLAY" : d->scrubbing ? "SCRATCH" : "PAUSE";
    gfx_text_center(JOG_CX, JOG_CY - 10, st, d->playing || d->cue_previewing ? C_GREEN : C_ORANGE, 1);
    if (d->loop_active) {
        double beats = (d->loop_out - d->loop_in) / deck_beat_len(d, d->loop_in);
        char s[24];
        if (beats >= 1) {
            int q = (int)lround(beats * 4);          /* quarter beats */
            if (q % 4 == 0) snprintf(s, sizeof s, "LOOP %d", q / 4);
            else snprintf(s, sizeof s, "LOOP %d.%02d", q / 4, (q % 4) * 25);
        } else {
            snprintf(s, sizeof s, "LOOP 1/%d", (int)lround(1 / beats));
        }
        gfx_text_center(JOG_CX, JOG_CY + 4, s, C_ORANGE, 1);
    }
    chip(JOG_X, JOG_Y, 44, d->jog_mode == JOG_VINYL ? "VINYL" : "BEND", d->jog_mode == JOG_VINYL);
}

static double fine_step(const deck_t *d) { return d->pitch_range >= 0.5 ? 0.001 : 0.0002; }

static int ctl_held(int ctl);
static void draw_slider(const deck_t *d)
{
    /* pitch nudge buttons (hold to repeat) */
    int hm = ctl_held(CTL_FINE_MINUS), hp = ctl_held(CTL_FINE_PLUS);
    gfx_rect(SL_X, SL_Y, SL_W, SL_FINE_H, hm ? RGB(60, 60, 66) : C_PANEL); gfx_frame(SL_X, SL_Y, SL_W, SL_FINE_H, hm ? C_ORANGE : C_EDGE);
    gfx_text_center(SL_X + SL_W / 2, SL_Y + 6, "-", hm ? C_ORANGE : C_TEXT, 2);
    int by = SL_TRACK_Y + SL_TRACK_H + 4;
    gfx_rect(SL_X, by, SL_W, SL_FINE_H, hp ? RGB(60, 60, 66) : C_PANEL); gfx_frame(SL_X, by, SL_W, SL_FINE_H, hp ? C_ORANGE : C_EDGE);
    gfx_text_center(SL_X + SL_W / 2, by + 6, "+", hp ? C_ORANGE : C_TEXT, 2);

    int cx = SL_X + SL_W / 2;
    gfx_rect(cx - 2, SL_TRACK_Y, 4, SL_TRACK_H, RGB(40, 40, 46));
    for (int k = 0; k <= 8; k++) {
        int y = SL_TRACK_Y + k * (SL_TRACK_H - 1) / 8;
        gfx_hline(cx - (k == 4 ? 16 : 8), cx - 6, y, k == 4 ? C_GREEN : C_EDGE);
        gfx_hline(cx + 6, cx + (k == 4 ? 16 : 8), y, k == 4 ? C_GREEN : C_EDGE);
    }
    /* CDJ convention: top = slower (-), bottom = faster (+) */
    double t = (d->pitch / d->pitch_range + 1) / 2;
    int y = SL_TRACK_Y + (int)lround(t * (SL_TRACK_H - 1));
    gfx_rect(SL_X + 6, y - 6, SL_W - 12, 13, fabs(d->pitch) < 1e-6 ? C_GREEN : C_TEXT);
    gfx_hline(SL_X + 8, SL_X + SL_W - 9, y, C_BG);
}

static void button(int col, int row, const char *label, pixel_t lit, int held)
{
    int x = JOG_X + col * (BTN_W + 4), y = BTN_Y + row * (BTN_H + 4);
    gfx_rect(x, y, BTN_W, BTN_H, held ? RGB(60, 60, 66) : C_PANEL);
    gfx_frame(x, y, BTN_W, BTN_H, lit ? lit : C_EDGE);
    if (lit) gfx_frame(x + 1, y + 1, BTN_W - 2, BTN_H - 2, lit);
    if (!strcmp(label, ">")) {
        gfx_tri_right(x + BTN_W / 2 - 8, y + BTN_H / 2 - 9, 19, lit ? lit : C_TEXT);
        gfx_rect(x + BTN_W / 2 + 4, y + BTN_H / 2 - 8, 3, 17, lit ? lit : C_TEXT);
        gfx_rect(x + BTN_W / 2 + 9, y + BTN_H / 2 - 8, 3, 17, lit ? lit : C_TEXT);
    } else {
        gfx_text_center(x + BTN_W / 2, y + BTN_H / 2 - 4, label, lit ? lit : C_TEXT, 1);
    }
}

static int ctl_held(int ctl) { for (int i = 0; i < 8; i++) if (ui.touch[i].active && ui.touch[i].ctl == ctl) return 1; return 0; }

static void draw_deck(void)
{
    hal_audio_lock();
    deck_t d = ui.deck;          /* snapshot (pointers stay valid while we draw: only loads replace them, on this thread) */
    hal_audio_unlock();

    gfx_clear(C_BG);
    gfx_rect(0, 0, SCREEN_W, HDR_H, C_PANEL);
    gfx_hline(0, SCREEN_W - 1, HDR_H - 1, C_EDGE);
    gfx_text(8, 7, "<", C_ORANGE, 2, 0);
    gfx_text(28, 4, ui.cur ? ui.cur->title : "NO TRACK", C_TEXT, 1, SCREEN_W - 36);
    if (ui.cur) gfx_text(28, 15, ui.cur->artist, C_DIM, 1, SCREEN_W - 36);
    if (!d.loaded) { gfx_text_center(120, 200, "NO TRACK LOADED", C_DIM, 1); return; }
    draw_overview(&d);
    draw_detail(&d);
    draw_info(&d);
    draw_jog(&d, ctl_held(CTL_JOG));
    draw_slider(&d);

    int blink = (hal_ticks_ms() / 300) & 1;
    pixel_t cue_lit = (!d.playing && fabs(d.pos - d.cue) < AUDIO_RATE / 100) ? C_ORANGE : (!d.playing && blink ? C_ORANGE : 0);
    pixel_t play_lit = d.playing ? C_GREEN : (blink ? C_GREEN : 0);
    button(0, 0, "CUE", cue_lit, ctl_held(CTL_CUE));
    button(0, 1, ">", play_lit, ctl_held(CTL_PLAY));
    button(1, 0, d.loop_active ? "1/2 X" : "IN", d.loop_in_set && !d.loop_active ? C_ORANGE : 0, ctl_held(CTL_IN));
    button(2, 0, d.loop_active ? "2 X" : "OUT", 0, ctl_held(CTL_OUT));
    button(1, 1, "4 BEAT", 0, ctl_held(CTL_BEAT));
    button(2, 1, d.loop_active ? "EXIT" : "RELOOP", d.loop_active ? C_ORANGE : 0, ctl_held(CTL_EXIT));
}

/* ================= touch handling ================= */

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

static int hit_deck(int x, int y)
{
    if (y < HDR_H) return x < 60 ? CTL_BACK : CTL_NONE;
    if (in_rect(x, y, OV_X, OV_Y, OV_W, OV_H)) return CTL_OVERVIEW;
    if (in_rect(x, y, 0, DT_Y, SCREEN_W, DT_H)) return CTL_DETAIL;
    if (in_rect(x, y, 126, INFO_Y + 20, 46, 24)) return CTL_RANGE;
    if (in_rect(x, y, 176, INFO_Y + 20, 26, 24)) return CTL_QUANT;
    if (in_rect(x, y, JOG_X, JOG_Y, 48, 20)) return CTL_JOGMODE;
    if (in_rect(x, y, JOG_X, JOG_Y, JOG_W, JOG_H)) return CTL_JOG;
    if (in_rect(x, y, SL_X, SL_Y, SL_W, SL_FINE_H)) return CTL_FINE_MINUS;
    if (in_rect(x, y, SL_X, SL_TRACK_Y + SL_TRACK_H + 4, SL_W, SL_FINE_H)) return CTL_FINE_PLUS;
    if (in_rect(x, y, SL_X, SL_TRACK_Y - 4, SL_W, SL_TRACK_H + 8)) return CTL_SLIDER;
    if (y >= BTN_Y && x >= JOG_X && x < JOG_X + 3 * (BTN_W + 4)) {
        int col = (x - JOG_X) / (BTN_W + 4), row = (y - BTN_Y) / (BTN_H + 4);
        static const int map[2][3] = { { CTL_CUE, CTL_IN, CTL_OUT }, { CTL_PLAY, CTL_BEAT, CTL_EXIT } };
        if (row < 2) return map[row][col];
    }
    return CTL_NONE;
}

static double jog_angle(int x, int y) { return atan2(y - JOG_CY, x - JOG_CX); }

static void seek_overview(int x)
{
    deck_t *d = &ui.deck;
    double f = (x - OV_X) / (double)OV_W * d->pcm.frames;
    deck_seek(d, f);
}

static void touch_down(touch_t *t, int x, int y)
{
    deck_t *d = &ui.deck;
    t->active = 1; t->x0 = t->lx = x; t->y0 = t->ly = y; t->moved = 0;
    t->t0 = t->rep = hal_ticks_ms();
    if (ui.scr == SCR_BROWSE) {
        if (y >= list_top()) t->ctl = CTL_LIST;
        else if (y >= BR_HDR_H) {                       /* sort bar */
            int x0 = 0, m = 0;
            while (m < SORT_COUNT - 1 && x >= x0 + sort_w[m]) x0 += sort_w[m++];
            t->ctl = CTL_SORT_BASE + m;
        }
        else if (ui.cur && x >= SCREEN_W - 48) t->ctl = CTL_TO_DECK;
        else if (x < 60) t->ctl = CTL_BACK;
        else t->ctl = CTL_NONE;
        return;
    }
    t->ctl = hit_deck(x, y);
    hal_audio_lock();
    switch (t->ctl) {
    case CTL_CUE: deck_cue_down(d); break;
    case CTL_PLAY: deck_play_pressed(d); break;
    case CTL_IN: if (d->loop_active) deck_loop_scale(d, 0.5); else deck_loop_in(d); break;
    case CTL_OUT: if (d->loop_active) deck_loop_scale(d, 2.0); else deck_loop_out(d); break;
    case CTL_EXIT: deck_reloop_exit(d); break;
    case CTL_BEAT: deck_beat_loop(d, 4); break;
    case CTL_FINE_MINUS: deck_set_pitch(d, d->pitch - fine_step(d)); break;
    case CTL_FINE_PLUS: deck_set_pitch(d, d->pitch + fine_step(d)); break;
    case CTL_JOG: t->ang = jog_angle(x, y); deck_jog_touch(d, 1); break;
    case CTL_SLIDER:
        /* double tap = reset to 0% (only taps count, never drags) */
        if (ui.fader_tap_ms && hal_ticks_ms() - ui.fader_tap_ms < DOUBLE_TAP_MS && abs(y - ui.fader_tap_y) < 40) {
            deck_set_pitch(d, 0);
            ui.fader_tap_ms = 0;
            t->ctl = CTL_NONE;          /* don't let this touch drag the fader */
        }
        break;
    case CTL_OVERVIEW: seek_overview(x); break;
    default: break;
    }
    hal_audio_unlock();
}

static void touch_move(touch_t *t, int x, int y)
{
    deck_t *d = &ui.deck;
    int dx = x - t->lx, dy = y - t->ly;
    if (abs(x - t->x0) > 6 || abs(y - t->y0) > 6) t->moved = 1;
    t->lx = x; t->ly = y;
    if (ui.scr == SCR_BROWSE) {
        if (t->ctl == CTL_LIST) ui.scroll -= dy;
        return;
    }
    hal_audio_lock();
    switch (t->ctl) {
    case CTL_SLIDER:
        /* relative: touching the fader never makes the tempo jump */
        deck_set_pitch(d, d->pitch + dy * (2 * d->pitch_range) / (SL_TRACK_H - 1));
        break;
    case CTL_JOG: {
        double a = jog_angle(x, y);
        double r = hypot(x - JOG_CX, y - JOG_CY);
        double delta;
        if (r < 18) delta = dx;                       /* near centre: plain horizontal drag */
        else {
            double da = a - t->ang;
            while (da > M_PI) da -= 2 * M_PI;
            while (da < -M_PI) da += 2 * M_PI;
            delta = da * r;                           /* clockwise = forward */
        }
        t->ang = a;
        deck_jog_move(d, delta);
        break;
    }
    case CTL_OVERVIEW: seek_overview(x); break;
    default: break;
    }
    hal_audio_unlock();
}

static void touch_up(touch_t *t)
{
    deck_t *d = &ui.deck;
    t->active = 0;
    if (ui.scr == SCR_BROWSE) {
        if (t->ctl == CTL_BACK && !t->moved) browser_back();
        else if (t->ctl == CTL_TO_DECK && !t->moved) ui.scr = SCR_DECK;
        else if (t->ctl == CTL_LIST && !t->moved) browser_select((t->y0 - list_top() + ui.scroll) / ROW_H);
        else if (t->ctl >= CTL_SORT_BASE && !t->moved) {
            int m = t->ctl - CTL_SORT_BASE;
            if (m == ui.sort_mode) ui.sort_desc = m != SORT_ORDER && !ui.sort_desc;   /* tap again: flip */
            else { ui.sort_mode = m; ui.sort_desc = 0; }
            build_list();
            ui.scroll = 0;
        }
        return;
    }
    hal_audio_lock();
    switch (t->ctl) {
    case CTL_CUE: deck_cue_up(d); break;
    case CTL_JOG: deck_jog_touch(d, 0); break;
    case CTL_JOGMODE: if (!t->moved) d->jog_mode = d->jog_mode == JOG_BEND ? JOG_VINYL : JOG_BEND; break;
    case CTL_RANGE:
        if (!t->moved) {
            static const double r[] = { 0.06, 0.10, 0.16, 0.50 };
            int i = 0;
            while (i < 4 && r[i] <= d->pitch_range + 1e-9) i++;
            d->pitch_range = r[i % 4];
            deck_set_pitch(d, d->pitch);
        }
        break;
    case CTL_QUANT: if (!t->moved) d->quantize = !d->quantize; break;
    case CTL_SLIDER:
        if (!t->moved) { ui.fader_tap_ms = hal_ticks_ms(); ui.fader_tap_y = t->y0; if (!ui.fader_tap_ms) ui.fader_tap_ms = 1; }
        else ui.fader_tap_ms = 0;
        break;
    case CTL_DETAIL: if (!t->moved) ui.zoom = ui.zoom == 4 ? 1 : ui.zoom * 2; break;
    case CTL_BACK: ui.scr = SCR_BROWSE; break;
    default: break;
    }
    hal_audio_unlock();
}

/* Hardware buttons. Only Home is used (deck <-> browser). The side play/pause and
 * volume buttons are deliberately ignored: the iPod's own system also acts on them
 * (play/pause switches to the Music app, volume clicks) and NanoApps can't stop
 * that. Pitch nudges are on screen. */
static void on_button(int b, int down)
{
    if (b == BTN_HOME && down) {
        if (ui.scr == SCR_DECK) ui.scr = SCR_BROWSE; else if (ui.cur) ui.scr = SCR_DECK;
    }
}

static void audio_cb(float *out, int frames, void *user) { deck_render((deck_t *)user, out, frames); }
void app_audio_skip(unsigned frames) { deck_skip(&ui.deck, (double)frames); }
int app_stream_stats(mp3s_stats_t *st) { if (!ui.deck.stream) return 0; mp3s_stats(ui.deck.stream, st); return 1; }


void app_init(library *L)
{
    memset(&ui, 0, sizeof ui);
    ui.L = L;
    ui.zoom = 2;
    deck_init(&ui.deck);
    snprintf(ui.title, sizeof ui.title, "%s", L->source);
    build_list();
    if (hal_audio_start(audio_cb, &ui.deck) != 0) status("NO AUDIO OUTPUT");
}

/* Handle pending input and draw one frame into fb. Returns 0 once the app should quit. */
int app_tick(void)
{
    hal_event ev;
    while (hal_poll(&ev)) {
        touch_t *t = &ui.touch[(unsigned)ev.id & 7];
        switch (ev.type) {
        case EV_QUIT: ui.quit = 1; break;
        case EV_TOUCH_DOWN: touch_down(t, ev.x, ev.y); break;
        case EV_TOUCH_MOVE: if (t->active) touch_move(t, ev.x, ev.y); break;
        case EV_TOUCH_UP: if (t->active) touch_up(t); break;
        case EV_BUTTON_DOWN: on_button(ev.button, 1); break;
        case EV_BUTTON_UP: on_button(ev.button, 0); break;
        }
    }
    hal_audio_lock();
    deck_service(&ui.deck);          /* read the track ahead of the playhead */
    /* held pitch nudge buttons repeat */
    if (ui.scr == SCR_DECK) {
        uint32_t now = hal_ticks_ms();
        for (int i = 0; i < 8; i++) {
            touch_t *t = &ui.touch[i];
            if (!t->active || (t->ctl != CTL_FINE_MINUS && t->ctl != CTL_FINE_PLUS)) continue;
            if (now - t->t0 < NUDGE_DELAY_MS) continue;
            while (now - t->rep >= NUDGE_RATE_MS) {
                t->rep += NUDGE_RATE_MS;
                if (t->rep < t->t0 + NUDGE_DELAY_MS) continue;
                deck_set_pitch(&ui.deck, ui.deck.pitch + (t->ctl == CTL_FINE_PLUS ? 1 : -1) * fine_step(&ui.deck));
            }
        }
    }
    hal_audio_unlock();
    if (ui.scr == SCR_BROWSE) draw_browser(); else draw_deck();
    if (ui.status[0] && hal_ticks_ms() < ui.status_until) {
        int w = gfx_text_w(ui.status, 1) + 16;
        gfx_rect(120 - w / 2, 400, w, 20, C_PANEL); gfx_frame(120 - w / 2, 400, w, 20, C_ORANGE);
        gfx_text_center(120, 407, ui.status, C_ORANGE, 1);
    }
    return !ui.quit;
}

