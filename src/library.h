#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t id;
    char title[128];
    char artist[96];
    char key[8];
    /* Paths live in the library's string store (lib_str): as two 640-byte arrays
     * they cost 1.3 KB a track, 1.3 MB of the iPod's few MB for 1,000 tracks. */
    const char *path; /* absolute path to the audio file ("" until set) */
    const char *anlz; /* absolute path to ANLZ0000.DAT ("" if none); .EXT is derived */
    uint32_t bpm100;  /* BPM * 100 */
    uint32_t duration_s;
} lib_track;

typedef struct {
    uint32_t id, parent, sort;
    int is_folder;
    char name[96];
} lib_playlist;

typedef struct { uint32_t playlist, track, index; } lib_entry;

typedef struct {
    lib_track *t;    int nt;
    lib_playlist *p; int np;
    lib_entry *e;    int ne;
    char source[96];
    char *str_at; size_t str_left;   /* string store: space left in the current chunk */
} library;

/* Load a Rekordbox USB export. `root` is the drive root (folder containing PIONEER/). */
int lib_load_pdb(library *L, const char *root);
const lib_track *lib_find(const library *L, uint32_t id);
void lib_sort_entries(library *L);

/* building a library (used by the export.pdb reader and the tests) */
lib_track *lib_add_track(library *L);
/* a copy of `s` that lives as long as the library ("" if there's no memory) */
const char *lib_str(library *L, const char *s);
lib_playlist *lib_add_playlist(library *L);
void lib_add_entry(library *L, uint32_t playlist, uint32_t track, uint32_t index);
