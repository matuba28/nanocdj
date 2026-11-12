#pragma once
#include "library.h"
#include "mp3stream.h"

/* The platform layer has already been initialised. */
void app_init(library *L);
int  app_tick(void);          /* handle input + draw one frame into fb; 0 = quit */

/* audio output missed `frames` of time (platform calls this from its audio pump) */
void app_audio_skip(unsigned frames);

/* nothing is sounding (no track, or paused): a good moment for slow housekeeping */
int app_deck_silent(void);

/* streaming stats of the loaded track; 0 if it isn't streamed */
int app_stream_stats(mp3s_stats_t *st);
