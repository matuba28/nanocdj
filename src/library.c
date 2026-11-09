#include "library.h"
#include <stdlib.h>
#include <string.h>

lib_track *lib_add_track(library *L)
{
    if ((L->nt & 255) == 0) L->t = realloc(L->t, sizeof(lib_track) * (L->nt + 256));
    lib_track *t = &L->t[L->nt++];
    memset(t, 0, sizeof *t);
    return t;
}

lib_playlist *lib_add_playlist(library *L)
{
    if ((L->np & 63) == 0) L->p = realloc(L->p, sizeof(lib_playlist) * (L->np + 64));
    lib_playlist *p = &L->p[L->np++];
    memset(p, 0, sizeof *p);
    return p;
}

void lib_add_entry(library *L, uint32_t playlist, uint32_t track, uint32_t index)
{
    if ((L->ne & 1023) == 0) L->e = realloc(L->e, sizeof(lib_entry) * (L->ne + 1024));
    L->e[L->ne++] = (lib_entry){ playlist, track, index };
}

const lib_track *lib_find(const library *L, uint32_t id)
{
    for (int i = 0; i < L->nt; i++) if (L->t[i].id == id) return &L->t[i];
    return NULL;
}

static int cmp_entry(const void *a, const void *b)
{
    const lib_entry *x = a, *y = b;
    if (x->playlist != y->playlist) return x->playlist < y->playlist ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static int cmp_pl(const void *a, const void *b)
{
    const lib_playlist *x = a, *y = b;
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    return x->sort < y->sort ? -1 : x->sort > y->sort;
}

void lib_sort_entries(library *L)
{
    if (L->ne) qsort(L->e, (size_t)L->ne, sizeof(lib_entry), cmp_entry);
    if (L->np) qsort(L->p, (size_t)L->np, sizeof(lib_playlist), cmp_pl);
}
