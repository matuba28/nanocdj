/* Measure how Rekordbox's beat grid lines up with nanoCDJ's MP3 decode.
 * For each MP3, finds its analysis in the local Rekordbox share (by file name),
 * decodes the whole track with mp3stream, and scores sample offsets d by the
 * average energy rise across every beat (energy just after beat+d minus just
 * before). The best d is where the grid's beats sit on the attacks.
 * Build: make tools.  Usage: tools/.build/grid_offset a.mp3 ...  (macOS, with Rekordbox installed) */
#include "../src/mp3stream.h"
#include "../src/anlz.h"
#include "../src/hal.h"
#include "../tests/hal_files_host.h"
#include <dirent.h>
#include <string.h>
#include <math.h>

void *hal_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)n); fread(b, 1, (size_t)n, f); fclose(f);
    if (size) *size = (size_t)n; return b;
}

typedef struct { char name[256]; char dat[900]; } ref_t;
static ref_t *refs; static int nrefs;

static void index_share(void)
{
    char root[512]; snprintf(root, sizeof root, "%s/Library/Pioneer/rekordbox/share/PIONEER/USBANLZ", getenv("HOME"));
    DIR *d1 = opendir(root); if (!d1) return;
    struct dirent *a;
    while ((a = readdir(d1))) {
        if (a->d_name[0] == '.') continue;
        char p1[700]; snprintf(p1, sizeof p1, "%s/%s", root, a->d_name);
        DIR *d2 = opendir(p1); if (!d2) continue;
        struct dirent *b;
        while ((b = readdir(d2))) {
            if (b->d_name[0] == '.') continue;
            char dat[900]; snprintf(dat, sizeof dat, "%s/%s/ANLZ0000.DAT", p1, b->d_name);
            size_t n; uint8_t *buf = hal_read_file(dat, &n);
            if (!buf) continue;
            char path[700];
            if (anlz_read_path(buf, n, path, sizeof path) == 0) {
                const char *pb = strrchr(path, '/'); pb = pb ? pb + 1 : path;
                refs = realloc(refs, sizeof(ref_t) * (size_t)(nrefs + 1));
                snprintf(refs[nrefs].name, sizeof refs[nrefs].name, "%s", pb);
                snprintf(refs[nrefs].dat, sizeof refs[nrefs].dat, "%s", dat);
                nrefs++;
            }
            free(buf);
        }
        closedir(d2);
    }
    closedir(d1);
}

static int find_dat(const char *mp3, char *out, size_t cap)
{
    const char *bn = strrchr(mp3, '/'); bn = bn ? bn + 1 : mp3;
    for (int i = 0; i < nrefs; i++) if (!strcmp(refs[i].name, bn)) { snprintf(out, cap, "%s", refs[i].dat); return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    index_share();
    for (int k = 1; k < argc; k++) {
        char dat[900];
        if (find_dat(argv[k], dat, sizeof dat)) { printf("%s: no Rekordbox analysis\n", argv[k]); continue; }
        anlz_t an; anlz_load(&an, dat);
        if (an.nbeats < 32) { printf("%s: no beat grid\n", argv[k]); anlz_free(&an); continue; }
        mp3s_t *s = mp3s_open(argv[k], 0);
        if (!s) continue;
        /* decode in order, keeping the window on the playhead (like playback) */
        mp3s_service(s, 0, -1, 1000000);
        int hz = mp3s_hz(s);
        int64_t cap = (int64_t)(an.nbeats + 64) * hz;              /* generous: a beat a second */
        if (cap < (int64_t)hz * 1200) cap = (int64_t)hz * 1200;
        float *mono = malloc(sizeof(float) * (size_t)cap);
        int64_t n = 0;
        for (;;) {
            mp3s_service(s, n, -1, 1000000);
            int c; const int16_t *p = mp3s_samples(s, n, &c);
            if (!p) { if (mp3s_length(s) >= 0 && n >= mp3s_length(s)) break; if (mp3s_length(s) >= 0) { mono[n++] = 0; continue; } continue; }
            for (int i = 0; i < c && n < cap; i++, n++) mono[n] = (p[2 * i] + p[2 * i + 1]) * (0.5f / 32768.f);
            if (n >= cap) break;
        }
        /* energy prefix sums, then onset score per offset */
        const int W = 128;
        double *cs = malloc(sizeof(double) * (size_t)(n + 1)); cs[0] = 0;
        for (int64_t j = 0; j < n; j++) cs[j + 1] = cs[j] + (double)mono[j] * mono[j];
        int best = 0; double bestv = -1e9, v0 = 0;
        static double score[801];
        for (int di = 0; di <= 800; di++) {
            int d = (di - 400) * 8;                                   /* -3200..+3200 samples */
            double acc = 0; int cnt = 0;
            for (int b = 8; b < an.nbeats - 8; b++) {
                int64_t c = (int64_t)llround(an.beats[b].time_ms * (double)hz / 1000.0) + d;
                if (c - W < 0 || c + W >= n) continue;
                double e1 = cs[c + W] - cs[c], e0 = cs[c] - cs[c - W];
                acc += log((e1 + 1e-9) / (e0 + 1e-9)); cnt++;
            }
            score[di] = cnt ? acc / cnt : 0;
            if (score[di] > bestv) { bestv = score[di]; best = d; }
            if (d == 0) v0 = score[di];
        }
        mp3s_stats_t st; mp3s_stats(s, &st);
        const char *bn = strrchr(argv[k], '/');
        printf("%-40.40s %3d kbps xing %d lame %4d  best %+5d (%+6.1f ms)  score %.2f (at 0: %.2f)\n",
               bn ? bn + 1 : argv[k], st.bitrate_kbps, st.xing, st.lame_delay, best, best * 1000.0 / hz, bestv, v0);
        free(cs); free(mono); mp3s_close(s); anlz_free(&an);
    }
    return 0;
}
