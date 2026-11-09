/* Rekordbox track analysis files (ANLZ0000.DAT / .EXT). Big-endian, tagged sections. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ANLZ_WAVE_PER_SEC 150   /* detail waveform entries per second */
#define ANLZ_MAX_CUES 64

typedef struct { uint32_t time_ms; uint16_t bpm100; uint8_t beat; } anlz_beat;

typedef struct {
    int hot;           /* 0 = memory cue, 1.. = hot cue A.. */
    int is_loop;
    uint32_t time_ms, loop_ms;
} anlz_cue;

typedef struct {
    anlz_beat *beats; int nbeats;
    uint8_t preview[400]; int has_preview;        /* PWAV: h = b & 31, white = b >> 5 */
    uint8_t *wave; int nwave;                     /* PWV3 mono detail, same encoding */
    uint16_t *cwave; int ncwave;                  /* PWV5 color detail: rrrgggbbbhhhhh-- */
    anlz_cue cues[ANLZ_MAX_CUES]; int ncues;
} anlz_t;

int  anlz_load(anlz_t *a, const char *dat_path); /* also loads the matching .EXT */
void anlz_free(anlz_t *a);

/* Read the PPTH (original audio path) tag from an ANLZ buffer into UTF-8. */
int anlz_read_path(const uint8_t *buf, size_t size, char *out, size_t cap);
