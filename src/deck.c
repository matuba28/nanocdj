#include "deck.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define MS2F(ms) ((double)(ms) * AUDIO_RATE / 1000.0)
#define AT_CUE_TOLERANCE (AUDIO_RATE / 100)   /* 10 ms */

void deck_init(deck_t *d)
{
    memset(d, 0, sizeof *d);
    d->pitch_range = 0.10;
    d->quantize = 1;
    d->jog_mode = JOG_BEND;
}

static int is_mp3(const char *p)
{
    size_t n = strlen(p);
    return n > 4 && p[n - 4] == '.' && (p[n - 3] | 32) == 'm' && (p[n - 2] | 32) == 'p' && p[n - 1] == '3';
}

int deck_stream_error;
uint32_t deck_stream_detail;

void deck_unload(deck_t *d)
{
    hal_audio_lock();
    mp3s_t *st = d->stream;
    hal_pcm pcm = d->pcm;
    anlz_t an = d->an;
    d->stream = NULL; d->sc_n = 0; d->loaded = 0; d->hist_n = 0;
    memset(&d->pcm, 0, sizeof d->pcm);
    memset(&d->an, 0, sizeof d->an);
    hal_audio_unlock();
    mp3s_close(st);
    hal_free_pcm(&pcm);
    anlz_free(&an);
}

int deck_load(deck_t *d, const char *audio, const char *anlz)
{
    /* The old track goes first, all of it: on the iPod there's a file handle for one
     * stream, and loading the new analysis while the old one is still held leaves
     * holes in a heap of a few MB. */
    deck_unload(d);
    anlz_t an;
    anlz_load(&an, anlz); /* analysis is optional */
    int entries = an.ncwave ? an.ncwave : an.nwave;
    double est_s = entries ? (double)entries / anlz_wave_rate(&an) : 0;
    hal_pcm pcm = { 0 };
    mp3s_t *st = NULL;
    int r = 0;
    deck_stream_error = 0; deck_stream_detail = 0;
    if (!is_mp3(audio)) deck_stream_error = DECK_ERR_NOT_MP3;
    else if ((st = mp3s_open(audio, est_s)) == NULL) {
        deck_stream_error = mp3s_open_error;
        uint32_t hi = 0; for (const char *c = audio; *c; c++) if ((unsigned char)*c >= 0x80) hi++;
        deck_stream_detail = (uint32_t)strlen(audio) | hi << 16;      /* path length | non-ASCII bytes */
    } else {
        /* Find the first audio frame. A big ID3 tag (cover art) can be several
         * hundred KB before it, and the iPod reads ~0.7 MB/s, so keep reading
         * (load blocks anyway) for up to ~6 s worth: a 4 MB tag. */
        uint32_t t0 = hal_time_us();
        while (!mp3s_hz(st) && mp3s_searching(st) && hal_time_us() - t0 < 6000000u)
            mp3s_service(st, 0, -1, 100000);
        if (!mp3s_hz(st)) {
            mp3s_stats_t ss; mp3s_stats(st, &ss);
            deck_stream_error = DECK_ERR_NO_FRAMES; deck_stream_detail = ss.bytes_read;
            mp3s_close(st); st = NULL;
        }
    }
    if (st) {
        int64_t len = mp3s_length(st);                   /* known already for a short file */
        double ratio = mp3s_hz(st) / (double)AUDIO_RATE;
        pcm.frames = len > 0 ? (int64_t)(len / ratio) : est_s > 0 ? (int64_t)(est_s * AUDIO_RATE) : (int64_t)AUDIO_RATE * 600;
    }
#ifdef HB_DEVICE
    /* The iPod has no other decoder. Fail with the reason (deck_stream_error) rather
     * than load the track as beat clicks, the test signal from before there was one. */
    else { anlz_free(&an); return -1; }
#else
    else r = hal_decode_audio(audio, &pcm);
#endif
    if (r == HAL_NO_DECODER) {
        /* no decoder on this platform yet: run silently, length from the analysis */
        int entries = an.ncwave ? an.ncwave : an.nwave;
        pcm.data = NULL;
        pcm.frames = (int64_t)(entries * AUDIO_RATE / anlz_wave_rate(&an));
        if (!pcm.frames) { anlz_free(&an); return -1; }
    } else if (r != 0) {
        anlz_free(&an);
        return -1;
    }

    hal_audio_lock();
    double pitch = d->pitch, range = d->pitch_range;
    int q = d->quantize; jog_mode jm = d->jog_mode;
    memset(d, 0, sizeof *d);
    d->pitch = pitch; d->pitch_range = range; d->quantize = q; d->jog_mode = jm;
    d->pcm = pcm; d->an = an; d->loaded = 1;
    d->stream = st; d->ratio = st ? mp3s_hz(st) / (double)AUDIO_RATE : 1.0;

    /* auto-cue: first memory cue, else first beat, else start */
    d->cue = 0;
    for (int i = 0; i < an.ncues; i++)
        if (an.cues[i].hot == 0) { d->cue = MS2F(an.cues[i].time_ms); break; }
    if (d->cue == 0 && an.nbeats) d->cue = MS2F(an.beats[0].time_ms);
    d->pos = d->cue;
    hal_audio_unlock();
    return 0;
}

/* ---------------- beat grid ---------------- */

double deck_beat_frame(const deck_t *d, int i)
{
    const anlz_t *a = &d->an;
    if (!a->nbeats) return 0;
    if (i < 0) return MS2F(a->beats[0].time_ms) + i * deck_beat_len(d, 0);
    if (i < a->nbeats) return MS2F(a->beats[i].time_ms);
    double last = MS2F(a->beats[a->nbeats - 1].time_ms);
    return last + (i - (a->nbeats - 1)) * deck_beat_len(d, last);
}

int deck_beat_at(const deck_t *d, double f)
{
    const anlz_t *a = &d->an;
    if (!a->nbeats) return -1;
    int lo = 0, hi = a->nbeats - 1;
    if (f < MS2F(a->beats[0].time_ms)) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (MS2F(a->beats[mid].time_ms) <= f) lo = mid; else hi = mid - 1;
    }
    return lo;
}

double deck_bpm_at(const deck_t *d, double f)
{
    const anlz_t *a = &d->an;
    if (!a->nbeats) return 0;
    int i = deck_beat_at(d, f);
    if (i < 0) i = 0;
    return a->beats[i].bpm100 / 100.0;
}

double deck_beat_len(const deck_t *d, double f)
{
    double bpm = deck_bpm_at(d, f);
    return bpm > 0 ? AUDIO_RATE * 60.0 / bpm : AUDIO_RATE * 0.5;
}

double deck_quantize(const deck_t *d, double f)
{
    if (!d->quantize || !d->an.nbeats) return f;
    int i = deck_beat_at(d, f);
    double a = deck_beat_frame(d, i), b = deck_beat_frame(d, i + 1);
    if (i < 0) { a = deck_beat_frame(d, 0) - deck_beat_len(d, 0); b = deck_beat_frame(d, 0); }
    return (f - a < b - f) ? a : b;
}

/* ---------------- transport ---------------- */

static int at_cue(const deck_t *d) { return fabs(d->pos - d->cue) < AT_CUE_TOLERANCE; }

void deck_play_pressed(deck_t *d)
{
    if (!d->loaded) return;
    if (d->cue_previewing) { d->cue_previewing = 0; d->playing = 1; return; } /* CUE+PLAY = keep going */
    d->playing = !d->playing;
}

void deck_cue_down(deck_t *d)
{
    if (!d->loaded) return;
    if (d->playing) {                      /* back to cue and pause */
        d->pos = d->cue; d->playing = 0;
    } else if (at_cue(d)) {                /* hold to preview from cue */
        d->cue_previewing = 1; d->playing = 1;
    } else {                               /* paused elsewhere: set a new cue here */
        d->cue = deck_quantize(d, d->pos);
        d->pos = d->cue;
    }
}

void deck_cue_up(deck_t *d)
{
    if (d->cue_previewing) { d->cue_previewing = 0; d->playing = 0; d->pos = d->cue; }
}

void deck_seek(deck_t *d, double f)
{
    if (!d->loaded || d->playing) return;
    if (f < 0) f = 0;
    if (f > d->pcm.frames - 1) f = (double)d->pcm.frames - 1;
    d->pos = f;
}

/* ---------------- loops ---------------- */

void deck_loop_in(deck_t *d)
{
    if (!d->loaded) return;
    d->loop_in = deck_quantize(d, d->pos);
    d->loop_in_set = 1;
    d->loop_active = 0;
    if (!d->playing) d->cue = d->loop_in;   /* CDJ behaviour: IN while paused also sets cue */
}

void deck_loop_out(deck_t *d)
{
    if (!d->loaded || !d->loop_in_set) return;
    double out = deck_quantize(d, d->pos);
    if (out <= d->loop_in + 64) out = d->loop_in + deck_beat_len(d, d->loop_in);
    d->loop_out = out;
    d->loop_active = 1;
    if (d->pos >= d->loop_out) d->pos = d->loop_in;
}

void deck_reloop_exit(deck_t *d)
{
    if (!d->loaded) return;
    if (d->loop_active) { d->loop_active = 0; return; }
    if (d->loop_in_set && d->loop_out > d->loop_in) { d->loop_active = 1; d->pos = d->loop_in; }
}

void deck_beat_loop(deck_t *d, int beats)
{
    if (!d->loaded) return;
    double start;
    if (d->an.nbeats) {
        int i = deck_beat_at(d, d->pos);
        start = d->quantize ? deck_beat_frame(d, i) : d->pos;
        if (i < 0) start = d->pos;
        d->loop_out = d->quantize && i >= 0 ? deck_beat_frame(d, i + beats) : start + beats * deck_beat_len(d, start);
    } else {
        start = d->pos;
        d->loop_out = start + beats * deck_beat_len(d, start);
    }
    d->loop_in = start;
    d->loop_in_set = 1;
    d->loop_active = 1;
}

void deck_loop_scale(deck_t *d, double factor)
{
    if (!d->loop_active) return;
    double len = (d->loop_out - d->loop_in) * factor;
    if (len < 256) return;
    d->loop_out = d->loop_in + len;
    if (d->pos >= d->loop_out) d->pos = d->loop_in + fmod(d->pos - d->loop_in, len);
}

/* ---------------- jog & pitch ---------------- */

#define SCRUB_FRAMES_PER_UNIT (AUDIO_RATE * 0.004)   /* 1 px of finger = 4 ms of audio */
#define BEND_PER_UNIT 0.006                            /* 1 px/event of finger = +0.6% speed */

void deck_jog_touch(deck_t *d, int down)
{
    if (!d->loaded) return;
    if (down) {
        if (d->jog_mode == JOG_VINYL || !d->playing) {
            d->resume_after_scrub = d->playing && d->jog_mode == JOG_VINYL;
            d->scrubbing = 1;
            d->scrub_target = d->pos;
            d->scrub_rate = 0;
            d->playing = 0;
        }
    } else if (d->scrubbing) {
        d->scrubbing = 0;
        if (d->resume_after_scrub) d->playing = 1;
        d->resume_after_scrub = 0;
    }
}

void deck_jog_move(deck_t *d, double delta)
{
    if (!d->loaded) return;
    if (d->scrubbing) {
        d->scrub_target += delta * SCRUB_FRAMES_PER_UNIT;
    } else if (d->playing) {
        d->bend += delta * BEND_PER_UNIT;
        if (d->bend > 0.5) d->bend = 0.5;
        if (d->bend < -0.5) d->bend = -0.5;
    }
}

void deck_set_pitch(deck_t *d, double p)
{
    if (p > d->pitch_range) p = d->pitch_range;
    if (p < -d->pitch_range) p = -d->pitch_range;
    d->pitch = p;
}

/* ---------------- audio ---------------- */

void deck_service(deck_t *d)
{
    if (!d->stream) return;
    mp3s_service(d->stream, (int64_t)(d->pos * d->ratio), (int64_t)(d->cue * d->ratio), 5000);
    int64_t len = mp3s_length(d->stream);
    if (len > 0) d->pcm.frames = (int64_t)(len / d->ratio);
}

/* one stereo source sample of the streamed track (0 if not decodable yet) */
static inline void src_sample(deck_t *d, int64_t j, float *l, float *r)
{
    if (j < d->sc_start || j >= d->sc_start + d->sc_n) {
        int n;
        const int16_t *p = j >= 0 ? mp3s_samples(d->stream, j, &n) : NULL;
        if (!p) { *l = *r = 0; return; }
        d->sc_start = j; d->sc_n = n; d->sc_p = p;
    }
    const int16_t *q = d->sc_p + (j - d->sc_start) * 2;
    *l = q[0] * (1.0f / 32768.0f); *r = q[1] * (1.0f / 32768.0f);
}

static inline float sample(const hal_pcm *p, int64_t i, int ch)
{
    if (i < 0 || i >= p->frames) return 0.f;
    return p->data[i * 2 + ch];
}

/* 4-point Hermite interpolation */
static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

/* ---------------- beat clicks (host builds without a decoder; the tests' signal) ---------------- */

static void click_trigger(deck_t *d, int beat)
{
    int down = beat >= 0 && beat < d->an.nbeats ? d->an.beats[beat].beat == 1 : ((beat % 4) + 4) % 4 == 0;
    double w = 2 * M_PI * (down ? 1568.0 : 1047.0) / AUDIO_RATE;    /* G6 on the downbeat, C6 otherwise */
    d->click_c = (float)cos(w); d->click_s = (float)sin(w);
    d->click_x = 0; d->click_y = 1;
    d->click_env = down ? 0.6f : 0.4f;
}

/* index of the beat just behind position f (works past both ends of the grid) */
static int click_beat_index(const deck_t *d, double f)
{
    int i = deck_beat_at(d, f);
    if (i >= 0 && i < d->an.nbeats - 1) return i;
    double len = deck_beat_len(d, f);
    double b0 = deck_beat_frame(d, 0);
    if (i < 0) return (int)floor((f - b0) / len);
    double last = deck_beat_frame(d, d->an.nbeats - 1);
    return d->an.nbeats - 1 + (int)floor((f - last) / len);
}

void deck_render(deck_t *d, float *out, int frames)
{
    if (!d->loaded) { memset(out, 0, sizeof(float) * 2 * frames); return; }

    /* per-block rate */
    double rate;
    if (d->scrubbing) {
        double want = (d->scrub_target - d->pos) / frames;   /* reach target within ~1 block */
        if (want > 4) want = 4;
        if (want < -4) want = -4;
        d->scrub_rate += (want - d->scrub_rate) * 0.35;      /* smooth so it sounds like a platter */
        rate = d->scrub_rate;
    } else if (d->playing) {
        rate = 1.0 + d->pitch + d->bend;
    } else {
        rate = 0;
    }
    /* jog bend decays with ~80 ms time constant */
    d->bend *= exp(-(double)frames / (AUDIO_RATE * 0.08));
    if (fabs(d->bend) < 1e-5) d->bend = 0;
    d->last_rate = rate;

    const hal_pcm *p = &d->pcm;
    double pos = d->pos;
    int was_playing = d->playing;
    unsigned hi = d->hist_w++ & (DECK_HIST - 1);
    d->hist[hi].pos = pos; d->hist[hi].rate = (float)rate;
    d->hist[hi].frames = (uint16_t)(frames > 65535 ? 65535 : frames); d->hist[hi].stopped = 0;
    if (d->hist_n < DECK_HIST) d->hist_n++;
    int clicks = !p->data && !d->stream && d->an.nbeats;
    double cur_f = 0, next_f = 0;
    if (clicks) {
        /* resync after a jump made outside rendering (cue, seek, load) */
        if (!d->click_sync || fabs(pos - d->click_end_pos) > AUDIO_RATE * 0.05) d->click_beat = click_beat_index(d, pos);
        d->click_sync = 1;
        cur_f = deck_beat_frame(d, d->click_beat);
        next_f = deck_beat_frame(d, d->click_beat + 1);
    }
    const float decay = 0.99811f;   /* ~12 ms */
    for (int n = 0; n < frames; n++) {
        int wrapped = 0;
        if (rate == 0 || (!p->data && !d->stream)) {
            out[2 * n] = out[2 * n + 1] = 0;
        } else if (d->stream) {
            double sp = pos * d->ratio;
            int64_t i = (int64_t)floor(sp);
            float t = (float)(sp - i), l[4], r[4];
            for (int k = 0; k < 4; k++) src_sample(d, i - 1 + k, &l[k], &r[k]);
            out[2 * n] = hermite(l[0], l[1], l[2], l[3], t);
            out[2 * n + 1] = hermite(r[0], r[1], r[2], r[3], t);
        } else {
            int64_t i = (int64_t)floor(pos);
            float t = (float)(pos - i);
            for (int ch = 0; ch < 2; ch++)
                out[2 * n + ch] = hermite(sample(p, i - 1, ch), sample(p, i, ch), sample(p, i + 1, ch), sample(p, i + 2, ch), t);
        }
        if (rate != 0) {
            pos += rate;
            if (d->loop_active && rate > 0 && pos >= d->loop_out) { pos -= d->loop_out - d->loop_in; wrapped = 1; }
            if (pos >= p->frames) { pos = (double)p->frames - 1; d->playing = 0; rate = 0; }
            if (pos < 0) { pos = 0; }
        }
        if (clicks) {
            if (wrapped) {   /* re-enter the loop just behind its start so the beat at loop_in clicks */
                d->click_beat = click_beat_index(d, pos - rate);
                cur_f = deck_beat_frame(d, d->click_beat); next_f = deck_beat_frame(d, d->click_beat + 1);
            }
            for (int guard = 0; rate > 0 && pos >= next_f && guard < 8; guard++) {
                click_trigger(d, ++d->click_beat);
                cur_f = next_f; next_f = deck_beat_frame(d, d->click_beat + 1);
            }
            for (int guard = 0; rate < 0 && pos < cur_f && guard < 8; guard++) {
                click_trigger(d, d->click_beat--);
                next_f = cur_f; cur_f = deck_beat_frame(d, d->click_beat);
            }
            if (d->click_env > 1e-4f) {
                float x = d->click_x * d->click_c - d->click_y * d->click_s;
                float y = d->click_x * d->click_s + d->click_y * d->click_c;
                d->click_x = x; d->click_y = y;
                float v = y * d->click_env;
                out[2 * n] += v; out[2 * n + 1] += v;
                d->click_env *= decay;
            }
        }
    }
    d->pos = pos;
    d->click_end_pos = pos;
    if (was_playing && !d->playing) { d->hist[hi].stopped = 1; d->last_rate = 0; }   /* ran into the end of the track */
}

void deck_unrender(deck_t *d, unsigned frames)
{
    if (!d->loaded) return;
    while (frames && d->hist_n) {
        unsigned hi = (d->hist_w - 1) & (DECK_HIST - 1);
        unsigned n = d->hist[hi].frames;
        double pos = d->hist[hi].pos, rate = d->hist[hi].rate;
        if (d->hist[hi].stopped) d->playing = 1;     /* it stopped at the end inside this call: it will again */
        if (frames >= n) {                           /* the whole call */
            frames -= n;
            d->hist_w--; d->hist_n--;
        } else {                                     /* keep its first n - frames frames */
            unsigned keep = n - frames;
            double start = pos;
            pos += keep * rate;
            if (d->loop_active && rate > 0 && start < d->loop_out && pos >= d->loop_out) pos -= d->loop_out - d->loop_in;
            if (pos >= d->pcm.frames) pos = (double)d->pcm.frames - 1;
            if (pos < 0) pos = 0;
            d->hist[hi].frames = (uint16_t)keep; d->hist[hi].stopped = 0;
            frames = 0;
        }
        d->pos = pos;
    }
    d->click_sync = 0;      /* re-find the beat for the click signal */
    d->click_env = 0;
}

/* Advance as if `frames` had been played at the current pitch, without output.
 * Used when the output stalls (screen lock), so the track stays in time with the
 * outside world. Jog bend and scratching are momentary, so they're dropped. */
void deck_skip(deck_t *d, double frames)
{
    if (!d->loaded || !d->playing || d->scrubbing || frames <= 0) return;
    double rate = 1.0 + d->pitch;
    double pos = d->pos + frames * rate;
    if (d->loop_active && pos >= d->loop_out) {
        double len = d->loop_out - d->loop_in;
        pos = len > 0 ? d->loop_in + fmod(pos - d->loop_in, len) : d->loop_in;
    }
    if (pos >= d->pcm.frames) { pos = (double)d->pcm.frames - 1; d->playing = 0; }
    d->pos = pos;
    d->bend = 0;
    d->click_env = 0;       /* a click from before the gap doesn't ring on */
    d->click_sync = 0;      /* re-find the beat; beats inside the gap are not played */
}
