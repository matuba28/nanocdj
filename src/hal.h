/* Hardware abstraction layer.
 * Everything the CDJ app needs from the device goes through these functions.
 * port/nanoapps/hal_nanoapps.c implements them on the iPod; tests/ has host
 * versions for the unit tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SCREEN_W 240
#define SCREEN_H 432
#define AUDIO_RATE 44100

/* ---- input ---- */
enum { BTN_HOME = 1, BTN_PLAYPAUSE, BTN_VOLUP, BTN_VOLDOWN };

typedef enum { EV_TOUCH_DOWN, EV_TOUCH_MOVE, EV_TOUCH_UP, EV_BUTTON_DOWN, EV_BUTTON_UP, EV_QUIT } hal_ev_type;

typedef struct {
    hal_ev_type type;
    int id;      /* touch finger id */
    int x, y;    /* touch position in screen pixels */
    int button;  /* BTN_* */
} hal_event;

int  hal_poll(hal_event *ev);          /* returns 1 if an event was filled */
uint32_t hal_ticks_ms(void);

/* ---- audio ----
 * The callback must fill `frames` stereo float frames. On the iPod it is called from
 * the frame callback (one thread); the lock is for platforms with an audio thread. */
typedef void (*hal_audio_cb)(float *out, int frames, void *user);
int  hal_audio_start(hal_audio_cb cb, void *user);
void hal_audio_lock(void);
void hal_audio_unlock(void);
/* Platforms that render audio ahead of time from the UI loop (the iPod):
 * hal_audio_rewind() takes back what is rendered but not yet heard and returns how
 * many frames that was (the app passes it to deck_unrender before changing the
 * transport, so the change is heard at once instead of a buffer later);
 * hal_audio_pump() renders now (the app calls it straight after handling input).
 * Platforms with a pull callback return 0 / do nothing. */
unsigned hal_audio_rewind(void);
void hal_audio_pump(void);

/* ---- files ---- */
void *hal_read_file(const char *path, size_t *size); /* malloc'd, NULL on failure */

/* Forward-only streaming reads (the iPod's file API can't seek). One file open
 * at a time is enough. read returns bytes read, 0 at end of file, <0 on error. */
void   *hal_file_open(const char *path);
int32_t hal_file_read(void *f, void *buf, uint32_t n);
void    hal_file_close(void *f);

uint32_t hal_time_us(void);   /* free-running microseconds, for budgeting work */
/* Seconds of playback left before the device's audio needs a restart (the iPod's
 * sound player allows only so many buffer starts per boot); -1 = no such limit. */
int hal_audio_time_left_s(void);
size_t   hal_mem_avail(void); /* the most the app may still allocate in one block */

/* Whole-file decode for formats other than MP3 (MP3s are streamed by
 * src/mp3stream.c). Interleaved stereo float at AUDIO_RATE. A host platform without
 * one returns HAL_NO_DECODER: the deck then plays a click on every beat of the
 * Rekordbox grid, with the length taken from the analysis (the tests use this). The
 * iPod build never calls it: a track that can't be streamed is refused with the
 * reason (deck_stream_error). */
#define HAL_NO_DECODER 1
typedef struct { float *data; int64_t frames; } hal_pcm;
int  hal_decode_audio(const char *path, hal_pcm *out);
void hal_free_pcm(hal_pcm *pcm);
