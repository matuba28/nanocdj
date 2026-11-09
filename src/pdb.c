/* Rekordbox export.pdb (DeviceSQL) reader.
 * Format reference: Deep Symmetry's crate-digger / djl-analysis documentation.
 * All integers in the .pdb are little-endian. */
#include "library.h"
#include "hal.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { T_TRACKS = 0, T_GENRES = 1, T_ARTISTS = 2, T_ALBUMS = 3, T_LABELS = 4, T_KEYS = 5,
       T_COLORS = 6, T_PLAYLIST_TREE = 7, T_PLAYLIST_ENTRIES = 8 };

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* DeviceSQL string -> UTF-8 */
static void dstr(const uint8_t *p, const uint8_t *end, char *out, size_t cap)
{
    out[0] = 0;
    if (!p || p >= end || cap < 2) return;
    uint8_t kind = p[0];
    if (kind & 1) {                       /* short ASCII: length in high 7 bits incl. header */
        int n = (kind >> 1) - 1;
        if (n < 0 || p + 1 + n > end) return;
        if ((size_t)n >= cap) n = (int)cap - 1;
        memcpy(out, p + 1, (size_t)n); out[n] = 0;
    } else if (kind == 0x40 || kind == 0x90) {
        if (p + 4 > end) return;
        int n = r16(p + 1) - 4;
        if (n < 0 || p + 4 + n > end) return;
        if (kind == 0x40) {
            if ((size_t)n >= cap) n = (int)cap - 1;
            memcpy(out, p + 4, (size_t)n); out[n] = 0;
        } else {
            utf16_to_utf8(p + 4, n / 2, 0 /*little endian*/, out, cap);
        }
    }
    /* strip trailing NULs some exports include */
    size_t l = strlen(out);
    while (l && out[l - 1] == 0) l--;
}

typedef void (*row_fn)(const uint8_t *row, const uint8_t *end, void *ctx);

static void walk_table(const uint8_t *f, size_t fsz, uint32_t plen, uint32_t first, uint32_t last,
                       row_fn fn, void *ctx)
{
    uint32_t idx = first;
    for (int guard = 0; guard < 1000000; guard++) {
        if ((uint64_t)(idx + 1) * plen > fsz) break;
        const uint8_t *pg = f + (size_t)idx * plen;
        const uint8_t *pend = pg + plen;
        uint32_t next = r32(pg + 0x0c);
        uint8_t flags = pg[0x1b];
        if ((flags & 0x40) == 0) {        /* data page */
            uint32_t nsmall = pg[0x18], nlarge = r16(pg + 0x22);
            uint32_t nrows = (nlarge > nsmall && nlarge != 0x1fff) ? nlarge : nsmall;
            uint32_t ngroups = nrows ? (nrows - 1) / 16 + 1 : 0;
            const uint8_t *heap = pg + 0x28;
            for (uint32_t g = 0; g < ngroups; g++) {
                const uint8_t *base = pend - g * 0x24;
                if (base - 0x24 < heap) break;
                uint16_t present = r16(base - 4);
                for (int i = 0; i < 16; i++) {
                    if (g * 16 + (uint32_t)i >= nrows) break;
                    if (!(present >> i & 1)) continue;
                    const uint8_t *row = heap + r16(base - 6 - 2 * i);
                    if (row < pend) fn(row, pend, ctx);
                }
            }
        }
        if (idx == last || next == idx) break;
        idx = next;
    }
}

/* ---- simple id -> name maps for artists and keys ---- */
typedef struct { uint32_t id; char name[96]; } named;
typedef struct { named *v; int n, cap; } named_list;

static void named_add(named_list *l, uint32_t id, const char *name)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, sizeof(named) * l->cap); }
    l->v[l->n].id = id;
    snprintf(l->v[l->n].name, sizeof l->v[l->n].name, "%s", name);
    l->n++;
}
static const char *named_get(const named_list *l, uint32_t id)
{
    for (int i = 0; i < l->n; i++) if (l->v[i].id == id) return l->v[i].name;
    return "";
}

typedef struct {
    library *L;
    const char *root;
    named_list artists, keys;
    uint32_t *track_artist, *track_key; /* parallel to L->t */
    int cap_extra;
    int dot_pioneer;   /* export lives in .PIONEER instead of PIONEER */
} pdb_ctx;

static void row_artist(const uint8_t *row, const uint8_t *end, void *vctx)
{
    pdb_ctx *c = vctx;
    if (row + 12 > end) return;
    uint16_t sub = r16(row);
    uint32_t id = r32(row + 4);
    uint32_t ofs = (sub == 0x64) ? r16(row + 0x0a) : row[9];
    char name[96];
    dstr(row + ofs, end, name, sizeof name);
    named_add(&c->artists, id, name);
}

static void row_key(const uint8_t *row, const uint8_t *end, void *vctx)
{
    pdb_ctx *c = vctx;
    if (row + 9 > end) return;
    char name[96];
    dstr(row + 8, end, name, sizeof name);
    named_add(&c->keys, r32(row), name);
}

static void join_path(char *out, size_t cap, const char *root, const char *rel)
{
    size_t rl = strlen(root);
    int slash = rl && root[rl - 1] == '/';
    snprintf(out, cap, "%s%s%s", root, (slash || rel[0] == '/') ? "" : "/", (slash && rel[0] == '/') ? rel + 1 : rel);
}

static void row_track(const uint8_t *row, const uint8_t *end, void *vctx)
{
    pdb_ctx *c = vctx;
    if (row + 0x5e + 42 > end) return;
    library *L = c->L;
    int before = L->nt;
    lib_track *t = lib_add_track(L);
    if (L->nt > c->cap_extra) {
        c->cap_extra = L->nt * 2;
        c->track_artist = realloc(c->track_artist, sizeof(uint32_t) * c->cap_extra);
        c->track_key = realloc(c->track_key, sizeof(uint32_t) * c->cap_extra);
    }
    t->id = r32(row + 0x48);
    t->bpm100 = r32(row + 0x38);
    t->duration_s = r16(row + 0x54);
    c->track_artist[before] = r32(row + 0x44);
    c->track_key[before] = r32(row + 0x20);

    const uint8_t *so = row + 0x5e;
    char tmp[640];
    dstr(row + r16(so + 17 * 2), end, t->title, sizeof t->title);
    dstr(row + r16(so + 20 * 2), end, tmp, sizeof tmp);
    join_path(t->path, sizeof t->path, c->root, tmp);
    dstr(row + r16(so + 14 * 2), end, tmp, sizeof tmp);
    if (tmp[0]) {
        /* the database says /PIONEER/..., but Rekordbox on a Mac may write the folder as hidden .PIONEER */
        const char *rel = tmp;
        char fixed[660];
        if (c->dot_pioneer && !strncmp(tmp, "/PIONEER/", 9)) { snprintf(fixed, sizeof fixed, "/.%s", tmp + 1); rel = fixed; }
        join_path(t->anlz, sizeof t->anlz, c->root, rel);
    }
    if (!t->title[0]) dstr(row + r16(so + 19 * 2), end, t->title, sizeof t->title);
}

static void row_playlist(const uint8_t *row, const uint8_t *end, void *vctx)
{
    pdb_ctx *c = vctx;
    if (row + 0x15 > end) return;
    lib_playlist *p = lib_add_playlist(c->L);
    p->parent = r32(row);
    p->sort = r32(row + 8);
    p->id = r32(row + 0x0c);
    p->is_folder = r32(row + 0x10) != 0;
    dstr(row + 0x14, end, p->name, sizeof p->name);
}

static void row_entry(const uint8_t *row, const uint8_t *end, void *vctx)
{
    pdb_ctx *c = vctx;
    if (row + 12 > end) return;
    lib_add_entry(c->L, r32(row + 8), r32(row + 4), r32(row));
}

int lib_load_pdb(library *L, const char *root)
{
    char path[700];
    join_path(path, sizeof path, root, "PIONEER/rekordbox/export.pdb");
    size_t fsz;
    int dot = 0;
    uint8_t *f = hal_read_file(path, &fsz);
    if (!f) {
        join_path(path, sizeof path, root, ".PIONEER/rekordbox/export.pdb");
        f = hal_read_file(path, &fsz);
        dot = 1;
    }
    if (!f) return -1;
    if (fsz < 0x1c) { free(f); return -1; }
    uint32_t plen = r32(f + 4), ntables = r32(f + 8);
    if (plen < 0x100 || plen > 0x10000 || ntables > 64 || 0x1c + ntables * 16 > fsz) { free(f); return -1; }

    pdb_ctx c = { .L = L, .root = root, .dot_pioneer = dot };
    row_fn fns[9] = { [T_TRACKS] = row_track, [T_ARTISTS] = row_artist, [T_KEYS] = row_key,
                      [T_PLAYLIST_TREE] = row_playlist, [T_PLAYLIST_ENTRIES] = row_entry };
    /* artists & keys first so tracks can be resolved afterwards (order doesn't matter, but be explicit) */
    int order[] = { T_ARTISTS, T_KEYS, T_TRACKS, T_PLAYLIST_TREE, T_PLAYLIST_ENTRIES };
    for (int o = 0; o < 5; o++) {
        for (uint32_t i = 0; i < ntables; i++) {
            const uint8_t *td = f + 0x1c + i * 16;
            if (r32(td) != (uint32_t)order[o]) continue;
            walk_table(f, fsz, plen, r32(td + 8), r32(td + 12), fns[order[o]], &c);
        }
    }
    for (int i = 0; i < L->nt; i++) {
        snprintf(L->t[i].artist, sizeof L->t[i].artist, "%s", named_get(&c.artists, c.track_artist[i]));
        snprintf(L->t[i].key, sizeof L->t[i].key, "%s", named_get(&c.keys, c.track_key[i]));
    }
    free(c.artists.v); free(c.keys.v); free(c.track_artist); free(c.track_key);
    free(f);
    lib_sort_entries(L);
    snprintf(L->source, sizeof L->source, "REKORDBOX USB");
    return 0;
}
