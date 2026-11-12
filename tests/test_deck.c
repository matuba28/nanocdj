/* Engine tests with a synthetic track: 120 BPM grid, 60 s of audio. Run: make test */
#include "../src/deck.h"
#include "../src/library.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stub HAL */
void hal_audio_lock(void) {}
void hal_audio_unlock(void) {}
void *hal_read_file(const char *p, size_t *s) { return NULL; }
int hal_decode_audio(const char *p, hal_pcm *o) { return -1; }
void hal_free_pcm(hal_pcm *p) { free(p->data); }
#include "hal_files_host.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(deck_t *d, int frames)
{
    float buf[2 * 512];
    while (frames > 0) { int n = frames > 512 ? 512 : frames; deck_render(d, buf, n); frames -= n; }
}

static void setup(deck_t *d)
{
    deck_init(d);
    d->pcm.frames = AUDIO_RATE * 60;
    d->pcm.data = calloc((size_t)d->pcm.frames * 2, sizeof(float));
    d->an.nbeats = 120;
    d->an.beats = calloc(120, sizeof(anlz_beat));
    for (int i = 0; i < 120; i++) d->an.beats[i] = (anlz_beat){ (uint32_t)(100 + i * 500), 12000, (uint8_t)(i % 4 + 1) };
    d->loaded = 1;
    d->cue = AUDIO_RATE * 0.1;
    d->pos = d->cue;
}

/* ---- synthetic Rekordbox analysis files, for the streaming reader ---- */
static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f); }
static void put16(FILE *f, uint32_t v) { fputc(v >> 8, f); fputc(v, f); }
static uint32_t wv(uint32_t i) { return ((i * 7u) % 31u) << 2 | (i % 7u) << 13; }   /* PWV5 entry: height (i*7)%31 */

/* EXT: PWV3 + PWV5 with n entries, a PCO2 cue list; DAT: PQTZ with nb beats, PWAV */
static void write_anlz(const char *dat, const char *ext, uint32_t n, uint32_t nb)
{
    FILE *f = fopen(ext, "wb");
    fwrite("PMAI", 1, 4, f); put32(f, 28); put32(f, 0); for (int i = 0; i < 16; i++) fputc(0, f);
    fwrite("PWV3", 1, 4, f); put32(f, 24); put32(f, 24 + n); put32(f, 1); put32(f, n); put32(f, 0x960000);
    for (uint32_t i = 0; i < n; i++) fputc((int)((i * 7u) % 31u), f);
    fwrite("PWV5", 1, 4, f); put32(f, 24); put32(f, 24 + n * 2); put32(f, 2); put32(f, n); put32(f, 0x960305);
    for (uint32_t i = 0; i < n; i++) put16(f, wv(i));
    fwrite("PCO2", 1, 4, f); put32(f, 20); put32(f, 20 + 0x1c); put32(f, 0); put16(f, 1); put16(f, 0);
    fwrite("PCP2", 1, 4, f); put32(f, 0x10); put32(f, 0x1c); put32(f, 1); fputc(1, f); fputc(0, f); put16(f, 0); put32(f, 61234); put32(f, 0xffffffff);
    fclose(f);
    f = fopen(dat, "wb");
    fwrite("PMAI", 1, 4, f); put32(f, 28); put32(f, 0); for (int i = 0; i < 16; i++) fputc(0, f);
    fwrite("PQTZ", 1, 4, f); put32(f, 24); put32(f, 24 + nb * 8); put32(f, 0); put32(f, 0x80000); put32(f, nb);
    for (uint32_t i = 0; i < nb; i++) { put16(f, i % 4 + 1); put16(f, 12400); put32(f, 100 + i * 484); }
    fwrite("PWAV", 1, 4, f); put32(f, 20); put32(f, 20 + 400); put32(f, 400); put32(f, 0x10000);
    for (int i = 0; i < 400; i++) fputc(i % 32, f);
    fclose(f);
}

int main(void)
{
    deck_t d;

    /* pitch: +5% for one second advances 1.05 s */
    setup(&d);
    deck_set_pitch(&d, 0.05);
    deck_play_pressed(&d);
    double p0 = d.pos;
    run(&d, AUDIO_RATE);
    CHECK(fabs(d.pos - p0 - AUDIO_RATE * 1.05) < 1, "pitch advance %.1f", d.pos - p0);

    /* pitch is clamped to the range */
    deck_set_pitch(&d, 0.5);
    CHECK(fabs(d.pitch - 0.10) < 1e-9, "clamp %.3f", d.pitch);

    /* CUE while playing: back to cue, paused */
    deck_cue_down(&d); deck_cue_up(&d);
    CHECK(!d.playing && fabs(d.pos - d.cue) < 1, "cue return");

    /* CUE held at cue point previews, release returns */
    deck_set_pitch(&d, 0);
    deck_cue_down(&d);
    CHECK(d.playing && d.cue_previewing, "preview start");
    run(&d, AUDIO_RATE / 2);
    deck_cue_up(&d);
    CHECK(!d.playing && fabs(d.pos - d.cue) < 1, "preview release");

    /* CUE held + PLAY keeps playing after release */
    deck_cue_down(&d); deck_play_pressed(&d); deck_cue_up(&d);
    CHECK(d.playing, "cue+play continues");

    /* paused away from cue: CUE sets a new, quantized cue */
    d.playing = 0; d.pos = AUDIO_RATE * 10.37;   /* beats at 10.1 and 10.6: nearest is 10.6 */
    deck_cue_down(&d); deck_cue_up(&d);
    CHECK(fabs(d.cue / AUDIO_RATE - 10.6) < 0.001, "quantized cue %.4f", d.cue / AUDIO_RATE);

    /* 4-beat loop at 120 BPM = 2 s, playback stays inside */
    d.pos = AUDIO_RATE * 20.2;
    deck_beat_loop(&d, 4);
    CHECK(fabs((d.loop_out - d.loop_in) / AUDIO_RATE - 2.0) < 0.001, "loop len %.4f", (d.loop_out - d.loop_in) / AUDIO_RATE);
    CHECK(fabs(d.loop_in / AUDIO_RATE - 20.1) < 0.001, "loop start on beat %.4f", d.loop_in / AUDIO_RATE);
    deck_play_pressed(&d);
    if (!d.playing) deck_play_pressed(&d);
    double maxpos = 0;
    for (int i = 0; i < 400; i++) { run(&d, 512); if (d.pos > maxpos) maxpos = d.pos; CHECK(d.pos >= d.loop_in - 1, "loop below"); }
    CHECK(maxpos <= d.loop_out + 1, "loop overrun %.1f", maxpos - d.loop_out);

    deck_loop_scale(&d, 0.5);
    CHECK(fabs((d.loop_out - d.loop_in) / AUDIO_RATE - 1.0) < 0.001, "halve");
    deck_reloop_exit(&d);
    CHECK(!d.loop_active, "exit");
    run(&d, AUDIO_RATE * 3);
    CHECK(d.pos > d.loop_out, "plays past loop after exit");

    /* manual IN/OUT */
    d.pos = AUDIO_RATE * 30.0; deck_loop_in(&d);
    d.pos = AUDIO_RATE * 31.06; deck_loop_out(&d);
    CHECK(d.loop_active && fabs((d.loop_out - d.loop_in) / AUDIO_RATE - 1.0) < 0.001, "in/out quantized len %.4f", (d.loop_out - d.loop_in) / AUDIO_RATE);
    deck_reloop_exit(&d);

    /* jog bend: speeds up briefly then decays back to normal */
    d.playing = 1; deck_set_pitch(&d, 0);
    deck_jog_move(&d, 20);
    p0 = d.pos; run(&d, 512);
    CHECK(d.pos - p0 > 512 * 1.05, "bend faster %.1f", d.pos - p0);
    run(&d, AUDIO_RATE);
    p0 = d.pos; run(&d, 512);
    CHECK(fabs(d.pos - p0 - 512) < 1, "bend decayed %.2f", d.pos - p0);

    /* vinyl mode scratch: touching stops the platter, moving follows the finger, release resumes */
    d.jog_mode = JOG_VINYL;
    deck_jog_touch(&d, 1);
    CHECK(!d.playing && d.scrubbing, "vinyl touch stops");
    p0 = d.pos;
    deck_jog_move(&d, -100);                 /* backwards 0.4 s */
    run(&d, AUDIO_RATE / 4);
    CHECK(fabs(d.pos - (p0 - AUDIO_RATE * 0.4)) < AUDIO_RATE * 0.01, "scratch target %.3f s off", (d.pos - (p0 - AUDIO_RATE * 0.4)) / AUDIO_RATE);
    deck_jog_touch(&d, 0);
    CHECK(d.playing && !d.scrubbing, "vinyl release resumes");

    /* end of track stops */
    d.pos = d.pcm.frames - 100; run(&d, 1000);
    CHECK(!d.playing, "stops at end");

    /* ---- beat clicks (no decoded audio): onsets land exactly on beats ---- */
    {
        deck_t c; setup(&c);
        free(c.pcm.data); c.pcm.data = NULL;               /* silent deck: the click test signal plays */
        static float buf[2 * AUDIO_RATE * 4];
        /* onsets = samples where the output jumps from silence to sound */
#define ONSETS(nframes, list, nmax, count) do { count = 0; \
            for (int k = 1; k < (nframes); k++) \
                if (fabsf(buf[2 * k]) > 0.01f && fabsf(buf[2 * (k - 1)]) < 1e-6f && fabsf(buf[2 * k - 4 < 0 ? 0 : 2 * k - 4]) < 1e-6f && count < (nmax)) list[count++] = k; } while (0)
        int on[64], n;

        /* forward from 1.0 s for 2 s at 120 BPM: beats at 1.1, 1.6, 2.1, 2.6 s */
        c.pos = AUDIO_RATE * 1.0; c.playing = 1;
        deck_render(&c, buf, AUDIO_RATE * 2);
        ONSETS(AUDIO_RATE * 2, on, 64, n);
        CHECK(n == 4, "forward clicks: %d", n);
        for (int k = 0; k < n && k < 4; k++) {
            int expect = (int)(AUDIO_RATE * (0.1 + 0.5 * k));    /* relative to 1.0 s */
            CHECK(abs(on[k] - expect) <= 1, "click %d at %d, expected %d", k, on[k], expect);
        }
        /* downbeat (beat 1 of the bar) is the higher, louder click: 1.1 s is beat index 2 -> beat 3; 2.1 s is index 4 -> downbeat */
        float peak[4] = { 0 };
        for (int k = 0; k < n && k < 4; k++) for (int j = 0; j < 400; j++) if (fabsf(buf[2 * (on[k] + j)]) > peak[k]) peak[k] = fabsf(buf[2 * (on[k] + j)]);
        CHECK(peak[2] > peak[0] * 1.3f, "downbeat louder: %.2f vs %.2f", peak[2], peak[0]);

        /* a CUE jump between renders is a resync, not a click */
        c.pos = AUDIO_RATE * 10.30; c.click_env = 0;
        deck_render(&c, buf, AUDIO_RATE / 10);                   /* 10.30..10.40: no beat in there */
        ONSETS(AUDIO_RATE / 10, on, 64, n);
        CHECK(n == 0, "no click after a jump: %d", n);

        /* backwards (scratching back over beats) clicks too */
        c.playing = 0; c.scrubbing = 1; c.pos = AUDIO_RATE * 3.0; c.scrub_target = AUDIO_RATE * 1.5; c.scrub_rate = -1; c.click_env = 0;
        deck_render(&c, buf, AUDIO_RATE * 2);
        ONSETS(AUDIO_RATE * 2, on, 64, n);
        CHECK(n >= 3, "reverse clicks: %d", n);
        c.scrubbing = 0;

        /* a 1-beat loop clicks once per pass, at the loop start */
        c.pos = AUDIO_RATE * 20.2; c.playing = 1; c.click_env = 0;
        deck_beat_loop(&c, 1);                                   /* 20.1 .. 20.6 */
        deck_render(&c, buf, AUDIO_RATE * 2);                    /* 2 s = 4 passes, first starts mid-loop */
        ONSETS(AUDIO_RATE * 2, on, 64, n);
        CHECK(n == 4, "loop clicks: %d", n);
        for (int k = 1; k < n; k++) CHECK(abs((on[k] - on[k - 1]) - AUDIO_RATE / 2) <= 1, "loop click spacing %d", on[k] - on[k - 1]);
        c.loop_active = 0;

        /* paused: no clicks, and a sounding click's tail still finishes */
        c.playing = 0; c.click_env = 0.5f; c.click_x = 0; c.click_y = 1;
        deck_render(&c, buf, 2000);
        CHECK(c.click_env < 0.1f, "tail decays while paused");
        ONSETS(2000, on, 64, n);
        CHECK(n == 0, "no clicks while paused: %d", n);
    }

    /* skip (output stalled, e.g. screen lock): moves on at the current pitch */
    {
        deck_t k; setup(&k);
        deck_set_pitch(&k, 0.10); deck_play_pressed(&k);
        double p0 = k.pos;
        deck_skip(&k, AUDIO_RATE * 2);
        CHECK(fabs(k.pos - (p0 + AUDIO_RATE * 2 * 1.10)) < 1e-6, "skip at +10%%: %.1f", k.pos - p0);
        /* inside a loop it wraps like playback would */
        k.loop_in = AUDIO_RATE * 10; k.loop_out = AUDIO_RATE * 12; k.loop_active = 1; k.pos = AUDIO_RATE * 11;
        deck_set_pitch(&k, 0);
        deck_skip(&k, AUDIO_RATE * 5);              /* 11 + 5 = 16 -> 10 + (6 mod 2) = 10 */
        CHECK(fabs(k.pos - AUDIO_RATE * 10) < 1e-6 && k.playing, "skip wraps in loop: %.3f s", k.pos / AUDIO_RATE);
        k.loop_active = 0;
        /* past the end: stops at the end */
        deck_skip(&k, AUDIO_RATE * 600);
        CHECK(!k.playing && k.pos == k.pcm.frames - 1, "skip past end stops");
        /* paused: nothing moves */
        double p1 = k.pos; deck_skip(&k, AUDIO_RATE);
        CHECK(k.pos == p1, "paused skip doesn't move");
        /* clicks pick up on the beat after a skip */
        deck_t c; setup(&c); free(c.pcm.data); c.pcm.data = NULL; deck_play_pressed(&c);   /* no PCM = click mode */
        float buf[2 * 4410];
        deck_render(&c, buf, 4410);
        deck_skip(&c, AUDIO_RATE * 3 + 123);
        CHECK(c.click_env == 0, "skip silences a ringing click");
        deck_render(&c, buf, 4410);
        CHECK(c.click_sync == 1, "resynced to the grid");
    }

    /* deck_unrender: taking back rendered audio puts the deck where it was */
    {
        float buf[2 * 256];
        deck_t u; setup(&u); deck_play_pressed(&u);
        double p0 = u.pos;
        for (int i = 0; i < 10; i++) deck_render(&u, buf, 256);
        CHECK(fabs(u.pos - (p0 + 2560)) < 1e-6, "rendered 2560");
        deck_unrender(&u, 700);                                   /* two whole calls and part of a third */
        CHECK(fabs(u.pos - (p0 + 1860)) < 1e-6, "unrender 700 of 2560: %.1f", u.pos - p0);
        deck_render(&u, buf, 256); deck_unrender(&u, 256);
        CHECK(fabs(u.pos - (p0 + 1860)) < 1e-6, "render + unrender is a no-op");
        /* a pitch change part-way: each call is undone at the rate it was rendered at */
        deck_t v; setup(&v); deck_play_pressed(&v); p0 = v.pos;
        deck_render(&v, buf, 256); deck_render(&v, buf, 256);
        deck_set_pitch(&v, 0.10);
        deck_render(&v, buf, 256); deck_render(&v, buf, 256);
        deck_unrender(&v, 600);                                   /* the two fast calls and 88 frames at 1x */
        CHECK(fabs(v.pos - (p0 + 424)) < 1e-3, "across a pitch change: %.3f", v.pos - p0);
        /* over a loop wrap */
        deck_t w; setup(&w); deck_play_pressed(&w);
        w.loop_in = AUDIO_RATE * 10; w.loop_out = w.loop_in + 1000; w.loop_in_set = w.loop_active = 1; w.pos = w.loop_in + 900;
        deck_render(&w, buf, 256);
        CHECK(fabs(w.pos - (w.loop_in + 156)) < 1e-6, "wrapped");
        deck_unrender(&w, 100);                                   /* keep 156 frames: 900 + 156 wraps to 56 */
        CHECK(fabs(w.pos - (w.loop_in + 56)) < 1e-6, "partial call across the loop end: %.1f", w.pos - w.loop_in);
        deck_unrender(&w, 156);
        CHECK(fabs(w.pos - (w.loop_in + 900)) < 1e-6, "back before the wrap: %.1f", w.pos - w.loop_in);
        /* ran into the end of the track inside the audio taken back: playing again */
        deck_t e; setup(&e); deck_play_pressed(&e); e.pos = (double)e.pcm.frames - 100;
        deck_render(&e, buf, 256);
        CHECK(!e.playing, "stopped at the end");
        deck_unrender(&e, 256);
        CHECK(e.playing && fabs(e.pos - ((double)e.pcm.frames - 100)) < 1e-6, "end of track undone");
        deck_render(&e, buf, 256); deck_render(&e, buf, 256);
        deck_unrender(&e, 256);                                   /* only the silent call after the stop */
        CHECK(!e.playing && e.pos == e.pcm.frames - 1, "stays stopped when only silence is taken back");
        /* paused: nothing moves; more than is remembered: stops at the oldest */
        deck_t q; setup(&q); p0 = q.pos;
        deck_render(&q, buf, 256); deck_unrender(&q, 256);
        CHECK(q.pos == p0, "paused unrender");
        deck_play_pressed(&q);
        for (int i = 0; i < DECK_HIST + 20; i++) deck_render(&q, buf, 256);
        deck_unrender(&q, 256 * (DECK_HIST + 50));
        CHECK(fabs(q.pos - (p0 + 20 * 256)) < 1e-6, "unrender is bounded by the history: %.1f", q.pos - p0);
        /* the cue press itself: back to the cue from the audible position */
        deck_t c; setup(&c); deck_play_pressed(&c);
        for (int i = 0; i < 20; i++) deck_render(&c, buf, 256);
        deck_unrender(&c, 3000); deck_cue_down(&c);
        CHECK(!c.playing && c.pos == c.cue, "cue after unrender");
        free(u.pcm.data); free(v.pcm.data); free(w.pcm.data); free(e.pcm.data); free(q.pcm.data); free(c.pcm.data);
    }

    /* the library's string store */
    {
        library L; memset(&L, 0, sizeof L);
        lib_track *t = lib_add_track(&L);
        CHECK(t->path[0] == 0 && t->anlz[0] == 0, "paths start empty");
        const char *a = lib_str(&L, "/a/b.mp3"), *b = lib_str(&L, "/c/d.mp3");
        char big[40000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
        const char *c2 = lib_str(&L, big), *d2 = lib_str(&L, "after");
        CHECK(!strcmp(a, "/a/b.mp3") && !strcmp(b, "/c/d.mp3") && b == a + 9, "strings packed");
        CHECK(strlen(c2) == sizeof big - 1 && !strcmp(d2, "after") && !strcmp(a, "/a/b.mp3"), "long string, earlier ones intact");
        CHECK(lib_str(&L, "")[0] == 0, "empty string");
        CHECK(sizeof(lib_track) <= 264, "a track is small: %zu bytes", sizeof(lib_track));
    }

    /* streamed analysis reader: an hour-long track's waveform is thinned to fit, the
     * mono copy dropped once the color one is there, beats/cues/preview intact */
    {
        const char *dat = "tests/build/anlz_test.DAT", *ext = "tests/build/anlz_test.EXT";
        uint32_t n = 150u * 3600u, nb = 7440;                          /* 1 hour at 150/s, 124 BPM */
        write_anlz(dat, ext, n, nb);
        anlz_t a;
        CHECK(anlz_load(&a, dat) == 0, "loads");
        CHECK(a.wave_div == 3 && a.ncwave == (int)((n + 2) / 3), "1 h thinned 3:1 (div %d, %d entries)", a.wave_div, a.ncwave);
        CHECK(a.nwave == 0 && a.wave == NULL, "mono waveform dropped when color exists");
        int bad = 0;
        for (uint32_t g = 0; g < 1000 && g < (uint32_t)a.ncwave; g++) {           /* tallest of each 3 kept */
            uint32_t best = 0, bh = 0;
            for (uint32_t k = 0; k < 3; k++) { uint32_t v = wv(g * 3 + k), h = (v >> 2) & 31; if (k == 0 || h > bh) { best = v; bh = h; } }
            if (a.cwave[g] != best) bad++;
        }
        CHECK(bad == 0, "thinning keeps the tallest entry (%d wrong)", bad);
        CHECK(a.nbeats == (int)nb && a.beats[1].time_ms == 584 && a.beats[1].beat == 2 && a.beats[0].bpm100 == 12400, "beats");
        CHECK(a.ncues == 1 && a.cues[0].time_ms == 61234 && a.cues[0].hot == 1, "cue from PCO2");
        CHECK(a.has_preview && a.preview[33] == 1, "preview");
        CHECK(anlz_wave_rate(&a) == 50.0, "wave rate %f", anlz_wave_rate(&a));
        anlz_free(&a);
        write_anlz(dat, ext, 150u * 600u, 100);                             /* 10 min: kept as is */
        CHECK(anlz_load(&a, dat) == 0 && a.wave_div == 1 && a.ncwave == 90000, "short track not thinned");
        anlz_free(&a);
        CHECK(anlz_load(&a, "tests/build/missing.DAT") != 0 && a.nbeats == 0, "missing files: no analysis, no crash");
        anlz_free(&a);
    }

    printf(fails ? "%d FAILED\n" : "all deck tests passed\n", fails);
    return fails != 0;
}
