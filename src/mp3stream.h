/* Streaming MP3 source for the deck.
 *
 * The iPod has ~4-7 MB of free memory and its file API can only read forward
 * (no seek), while a 320 kbps track is 10-25 MB. So nothing is ever loaded
 * whole. Instead:
 *
 *  - The file is read forward in small steps (mp3s_service, once per UI frame,
 *    time-budgeted). Frame headers are parsed as bytes arrive, building an index
 *    of every frame's byte offset, so any position can be found exactly (CBR or
 *    VBR) once the reader has passed it.
 *  - A window of compressed audio is kept around the playhead (~9 s behind, the
 *    rest ahead). Jumping outside it reopens the file and reads forward to the
 *    new spot.
 *  - A few seconds of compressed audio at the cue point are copied into their
 *    own buffer, like a CDJ's cue buffer, so CUE is instant from anywhere while
 *    the window catches up behind it.
 *  - Frames are decoded on demand (minimp3) into a small cache. A jump primes
 *    the decoder with the two frames before the target, which makes the target
 *    frame bit-exact with a straight decode.
 *
 * Threads: mp3s_service (reads, moves the window) and mp3s_samples (decodes)
 * must not run at the same time. On the iPod everything is one thread; the
 * caller holds hal_audio_lock around mp3s_service on platforms with an audio thread. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct mp3s mp3s_t;

/* est_seconds: expected length (from Rekordbox), used to size the index.
 * Returns NULL if the file can't be opened or there's no memory for buffers. */
mp3s_t *mp3s_open(const char *path, double est_seconds);
enum { MP3S_ERR_FILE = 2, MP3S_ERR_MEMORY = 3 };
extern int mp3s_open_error;              /* why the last mp3s_open returned NULL */
void    mp3s_close(mp3s_t *s);

/* The stream's buffers (compressed window, cue buffer, decoded-frame cache, frame
 * index) are allocated once and kept between tracks: allocating ~2 MB afresh for
 * every load fragments a heap of a few MB until one load no longer fits. The first
 * mp3s_open allocates them if mp3s_reserve hasn't. Call mp3s_reserve early (before
 * other big allocations) to make sure of them: `leave` is how much memory to leave
 * for what comes after (0 = take the usual share). Returns 1 if the buffers exist.
 * mp3s_release frees them (not while a stream is open). */
int  mp3s_reserve(size_t leave);
void mp3s_release(void);

/* Read/parse/slide toward the source sample `play` (and keep the cue buffer at
 * source sample `cue`, -1 for none), spending at most ~budget_us. */
void mp3s_service(mp3s_t *s, int64_t play, int64_t cue, uint32_t budget_us);

/* Stereo int16 frames of decoded audio starting at source sample `j`:
 * returns a pointer to sample j and stores how many consecutive samples follow
 * it in *n, or NULL if that audio isn't available (yet). */
const int16_t *mp3s_samples(mp3s_t *s, int64_t j, int *n);

int     mp3s_hz(const mp3s_t *s);        /* source sample rate, 0 until the first frame is seen */
int     mp3s_searching(const mp3s_t *s); /* still reading toward the first audio frame */
int64_t mp3s_length(const mp3s_t *s);    /* exact length in source samples, -1 until the end is reached */

typedef struct {
    uint32_t window_bytes, window_cap, cue_bytes, cue_cap;
    uint32_t frames_indexed, bytes_read, reopens, misses, decodes;
    uint32_t read_us;                    /* time spent reading in the last service call */
    int      eof, hz, bitrate_kbps;
    uint32_t read_kbps;                  /* measured file read speed, KB/s */
    uint32_t ahead_ms;                   /* audio read ahead of the playhead */
    int      xing, lame_delay;   /* info frame: 0 none, 1 skipped (ffmpeg etc.), 2 played as silence (LAME) */
} mp3s_stats_t;
void mp3s_stats(const mp3s_t *s, mp3s_stats_t *st);
