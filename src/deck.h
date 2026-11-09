/* One CDJ-style deck. All positions are in frames at AUDIO_RATE.
 * Control functions are called from the UI thread with the audio lock held. */
#pragma once
#include <stdint.h>
#include "hal.h"
#include "anlz.h"
#include "mp3stream.h"

typedef enum { JOG_BEND, JOG_VINYL } jog_mode;

typedef struct {
    hal_pcm pcm;           /* whole decoded track (non-MP3, where a decoder exists), or just .frames when streaming */
    mp3s_t *stream;        /* MP3 streamed and decoded around the playhead */
    double ratio;          /* source samples per deck frame (48 kHz file: 1.088) */
    int64_t sc_start; int sc_n; const int16_t *sc_p;   /* last decoded run looked up */
    anlz_t an;
    int loaded;

    double pos;
    int playing;
    double pitch;          /* -range..+range as a fraction, e.g. 0.0312 = +3.12% */
    double pitch_range;    /* 0.06, 0.10, 0.16, 0.50 */
    int quantize;

    /* jog */
    jog_mode jog_mode;
    double bend;           /* extra playback rate added by the jog, decays to 0 */
    int scrubbing;         /* finger on the jog in vinyl mode, or paused search */
    double scrub_target;
    double scrub_rate;
    int resume_after_scrub;

    /* cue */
    double cue;
    int cue_previewing;    /* CUE held from the cue point */

    /* loop */
    double loop_in, loop_out;
    int loop_in_set, loop_active;

    double last_rate;      /* for display */

    /* beat clicks: the test signal played when there is no decoded audio */
    int click_beat, click_sync;
    double click_end_pos;
    float click_env, click_x, click_y, click_c, click_s;
} deck_t;

void deck_init(deck_t *d);
void deck_unload(deck_t *d);
/* Why the last loaded track isn't streaming (0 = it is): */
enum { DECK_ERR_NOT_MP3 = 1, /* MP3S_ERR_FILE = 2, MP3S_ERR_MEMORY = 3 */ DECK_ERR_NO_FRAMES = 4 };
extern int deck_stream_error;
extern uint32_t deck_stream_detail;   /* FILE: path length | non-ASCII bytes << 16; NO_FRAMES: bytes read */
int  deck_load(deck_t *d, const char *audio_path, const char *anlz_path); /* blocking */
void deck_render(deck_t *d, float *out, int frames);                     /* audio thread */
/* Keep the streamed track read ahead of the playhead. Once per UI frame, with the
 * audio lock held. */
void deck_service(deck_t *d);
/* move on by `frames` of output time without rendering (time the audio output missed) */
void deck_skip(deck_t *d, double frames);

void deck_play_pressed(deck_t *d);
void deck_cue_down(deck_t *d);
void deck_cue_up(deck_t *d);
void deck_loop_in(deck_t *d);
void deck_loop_out(deck_t *d);
void deck_reloop_exit(deck_t *d);
void deck_beat_loop(deck_t *d, int beats);
void deck_loop_scale(deck_t *d, double factor);   /* 0.5 halve, 2 double */
void deck_seek(deck_t *d, double frame);          /* only while paused, like a CDJ needle search */

void deck_jog_touch(deck_t *d, int down);
void deck_jog_move(deck_t *d, double delta);      /* delta in "jog units" (pixels) */
void deck_set_pitch(deck_t *d, double pitch);

/* beat-grid helpers */
double deck_beat_frame(const deck_t *d, int i);
int    deck_beat_at(const deck_t *d, double frame);   /* index of last beat <= frame, -1 if none */
double deck_bpm_at(const deck_t *d, double frame);    /* track BPM (unpitched) */
double deck_beat_len(const deck_t *d, double frame);  /* frames per beat */
double deck_quantize(const deck_t *d, double frame);
