/* Low-latency PCM output for nanoCDJ on NanoApps / RetailOS 1.1.2.
 *
 * Blocks are handed to the OS sound-effect voice as a CHAIN (descriptor+0x54 =
 * next): the voice re-arms itself from the next descriptor in its own completion
 * handler, so consecutive blocks join sample-exactly. That technique is NanoApps'
 * entrain app's (see its platform/audio_device.c).
 *
 * Version 1 chained 20 ms blocks. On the device that crackled at every join and
 * rebooted after ~30 s: a block that short is shorter than the OS mixer's pass,
 * and the voice's per-buffer accounting doesn't survive a pass that spans two
 * buffers. So blocks are now long (~200 ms, like the 320 ms+ entrain proved), and
 * latency comes from WHERE we write instead of how big the blocks are: each block
 * is zeroed when it is chained, and every tick renders only up to a short lead
 * ahead of the estimated play cursor, into blocks the voice is already playing or
 * about to. The voice reads our buffers live (entrain's pause relies on it).
 *
 * The play cursor is estimated from the wall clock and corrected every time the
 * OS's own "playing" flag moves to the next block, so it can't drift.
 *
 * If the frame callback stops (screen locked, another app in front), the voice
 * runs into zeroed buffer and then stops: silence, never stale audio. When ticks
 * come back the skip callback is asked to move the deck on by the time that was
 * missed, so the music stays on the wall clock (in phase with other decks).
 *
 * Integer-only and Thumb-safe: ring_prepare_unload() is called by the resident on
 * its own task, which has no FPU context. */
#pragma once
#include <stdint.h>

/* Fill `frames` interleaved stereo int16 frames. Called from ring_tick(). */
typedef void (*ring_render_fn)(int16_t *out, uint32_t frames, void *user);
/* Advance the source by `frames` without producing them (time the output missed). */
typedef void (*ring_skip_fn)(uint32_t frames, void *user);

int  ring_init(ring_render_fn fn, ring_skip_fn skip, void *user);   /* 0 on success */
void ring_tick(void);                            /* once per frame */
int  ring_prepare_unload(void);                  /* resident hook: 1 = safe to unload */
void ring_release(void);                         /* then: free the buffers (integer-only) */
/* RetailOS allows only ~1,300 block starts per boot (see audio_ring.c). The ring
 * stops queueing, so the audio stops cleanly, once it has made `starts` of them. */
void ring_set_budget(uint32_t starts);

typedef struct {
    uint32_t out_rate;      /* mixer rate read off the live voice (0 = unknown yet) */
    uint32_t block_frames;  /* current block length */
    uint32_t queued;        /* blocks handed to the OS, including the sounding one */
    uint32_t restarts;      /* times a voice had to be started */
    uint32_t lead_ms;       /* how far ahead of the cursor we write (adapts upward) */
    uint32_t late;          /* ticks that found the cursor past what was written */
    uint32_t skipped_ms;    /* total time handed to the skip callback */
    uint32_t voice_joins;   /* blocks the current voice has chained through */
    uint32_t joins_total;
    uint32_t handovers;     /* times the stream moved to a fresh voice */
    uint32_t starts;        /* block starts made (each chain join or play) */
    uint32_t budget;        /* starts allowed (ring_set_budget) */
    int      duration_ok;   /* exact per-block durations are being stated */
    uint32_t ctr_div;       /* voice counter = frames / this (0 = no counter, fallback) */
    uint32_t cal_ratio;     /* last measured block time / audio length, in % */
    uint32_t cal_steps;     /* measurements taken */
} ring_stats_t;
void ring_stats(ring_stats_t *s);
