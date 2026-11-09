/* Streaming MP3 test: src/mp3stream.c against a straight whole-file minimp3 decode.
 * Run: make test-mp3 MP3="path/to/track.mp3" (any number of files). */
#include "../src/mp3stream.h"
#include "../src/deck.h"
#include "../src/hal.h"
#include "hal_files_host.h"
#include "../third_party/minimp3/minimp3.h"
#include <string.h>
#include <math.h>

void hal_audio_lock(void) {}
void hal_audio_unlock(void) {}
void *hal_read_file(const char *p, size_t *sz) { (void)p; (void)sz; return NULL; }
int hal_decode_audio(const char *p, hal_pcm *o) { (void)p; (void)o; return -1; }
void hal_free_pcm(hal_pcm *p) { free(p->data); p->data = NULL; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int16_t *ref; static int64_t ref_n; static int ref_hz;

/* reference: decode everything in order, dropping a Xing/Info first frame like the stream does */
static int reference(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n); fread(b, 1, (size_t)n, f); fclose(f);
    size_t off = 0;
    if (n > 10 && !memcmp(b, "ID3", 3)) off = 10 + ((size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127)) + ((b[5] & 0x10) ? 10 : 0);
    static mp3dec_t dec; mp3dec_init(&dec);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = (size_t)n / 100 * 1152 * 2 + 1152 * 4; ref = malloc(cap * 2); ref_n = 0;
    int first = 1;
    while (off < (size_t)n) {
        mp3dec_frame_info_t info;
        int ns = mp3dec_decode_frame(&dec, b + off, (int)((size_t)n - off), pcm, &info);
        if (!info.frame_bytes) break;
        off += (size_t)info.frame_bytes;
        if (!ns) continue;
        if (first) {
            first = 0; ref_hz = info.hz;
            /* a Xing/Info frame decodes to silence: the stream skips it */
            const uint8_t *h = b + off - info.frame_bytes + info.frame_offset;
            int mono = (h[3] >> 6) == 3, m1 = ((h[1] >> 3) & 3) == 3;
            int side = m1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
            const uint8_t *x = h + 4 + side;
            if (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) {
                /* same rule as the stream: a LAME info frame plays (as Rekordbox does), others are skipped */
                uint32_t fl = (uint32_t)x[4] << 24 | (uint32_t)x[5] << 16 | (uint32_t)x[6] << 8 | x[7];
                int q = 8 + (fl & 1 ? 4 : 0) + (fl & 2 ? 4 : 0) + (fl & 4 ? 100 : 0) + (fl & 8 ? 4 : 0);
                if (memcmp(x + q, "LAME", 4)) continue;
            }
        }
        for (int i = 0; i < ns; i++) {
            ref[2 * (ref_n + i)] = pcm[i * info.channels];
            ref[2 * (ref_n + i) + 1] = pcm[i * info.channels + (info.channels - 1)];
        }
        ref_n += ns;
    }
    free(b);
    return 0;
}

/* compare `count` samples from j; service as needed (up to `tries` calls per miss) */
static int64_t compare(mp3s_t *s, int64_t j, int64_t count, int tries, int64_t *missing)
{
    int64_t bad = 0;
    for (int64_t k = j; k < j + count && k < ref_n; ) {
        int n; const int16_t *p = mp3s_samples(s, k, &n);
        int t = 0;
        while (!p && t++ < tries) { mp3s_service(s, k, -1, 2000); p = mp3s_samples(s, k, &n); }
        if (!p) { if (missing) (*missing)++; k++; continue; }
        for (int i = 0; i < n && k < j + count && k < ref_n; i++, k++)
            if (p[2 * i] != ref[2 * k] || p[2 * i + 1] != ref[2 * k + 1]) bad++;
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_mp3s file.mp3...\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        const char *path = argv[a];
        if (reference(path) || !ref_n) { printf("%s: can't decode\n", path); fails++; continue; }
        double secs = ref_n / (double)ref_hz;
        printf("%s\n  %.1f s at %d Hz\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, secs, ref_hz);

        /* 1. play straight through with slow 8 KB reads and a small budget per frame */
        test_read_limit = 8192;
        mp3s_t *s = mp3s_open(path, secs);
        CHECK(s, "open");
        if (!s) continue;
        mp3s_service(s, 0, -1, 300000);
        CHECK(mp3s_hz(s) == ref_hz, "rate %d", mp3s_hz(s));
        int64_t bad = 0, missing = 0, starved = 0;
        uint32_t step = (uint32_t)(ref_hz / 60);                          /* one UI frame of audio */
        for (int64_t j = 0; j < ref_n; j += step) {
            mp3s_service(s, j, 0, 4000);
            int64_t m = 0;
            bad += compare(s, j, step, 0, &m);
            if (m) starved++;
            missing += m;
        }
        mp3s_stats_t st; mp3s_stats(s, &st);
        printf("  straight play: %lld wrong samples, %lld frames of UI starved, window %u KB, cue %u KB, reopens %u\n",
               (long long)bad, (long long)starved, st.window_cap / 1024, st.cue_bytes / 1024, st.reopens);
        CHECK(bad == 0, "straight play is bit-exact (%lld wrong)", (long long)bad);
        CHECK(missing == 0, "straight play never starves (%lld samples missing)", (long long)missing);
        /* minimp3's whole-buffer search drops a last frame that is followed by a tag;
         * the stream knows where it is and plays it, so it may be one frame longer */
        CHECK(mp3s_length(s) == ref_n || mp3s_length(s) == ref_n + 1152 || mp3s_length(s) == ref_n + 576,
              "length %lld vs %lld", (long long)mp3s_length(s), (long long)ref_n);
        CHECK(st.reopens == 0, "no reopen when just playing");
        test_read_limit = 0;

        /* 2. jumps anywhere: exact from the first sample after the decoder primes */
        srand(7);
        int64_t jbad = 0, jmiss = 0;
        for (int k = 0; k < 20; k++) {
            int64_t j = (int64_t)((double)rand() / RAND_MAX * (ref_n - ref_hz));
            jbad += compare(s, j, 4096, 400, &jmiss);
        }
        mp3s_stats(s, &st);
        printf("  20 random jumps: %lld wrong, %lld missing, reopens %u\n", (long long)jbad, (long long)jmiss, st.reopens);
        CHECK(jbad == 0 && jmiss == 0, "jumps exact");

        /* 3. cue buffer: playing far from the cue, CUE is still instant */
        if (secs > 120) {
            int64_t cue = (int64_t)(30.0 * ref_hz), far = (int64_t)(100.0 * ref_hz);
            for (int i = 0; i < 400; i++) mp3s_service(s, cue, cue, 4000);     /* (window passes the cue once) */
            for (int i = 0; i < 400; i++) mp3s_service(s, far, cue, 4000);     /* then the playhead is far away */
            int64_t m = 0;
            int64_t cb = compare(s, cue, ref_hz * 3, 0, &m);                    /* no service at all */
            printf("  cue from 70 s away: %lld wrong, %lld missing in the first 3 s\n", (long long)cb, (long long)m);
            CHECK(cb == 0 && m == 0, "cue buffer covers the first 3 s");
        }

        /* 4. scratch backwards 2 s from the middle: all from the window, exact */
        {
            int64_t mid = ref_n / 2;
            for (int i = 0; i < 400; i++) mp3s_service(s, mid, -1, 4000);
            int64_t sb = 0, sm = 0;
            for (int64_t j = mid; j > mid - 2 * ref_hz; j -= 37) {
                int n; const int16_t *p = mp3s_samples(s, j, &n);
                if (!p) { sm++; continue; }
                if (p[0] != ref[2 * j] || p[1] != ref[2 * j + 1]) sb++;
            }
            printf("  backwards 2 s: %lld wrong, %lld missing\n", (long long)sb, (long long)sm);
            CHECK(sb == 0 && sm == 0, "backwards exact");
        }

        mp3s_close(s);
        CHECK(test_open_files == 0, "file closed");

        /* 4b. iPod-speed flash (~0.7 MB/s): 60 s of play from load through the deck's
         * per-frame service; no misses, and no single read longer than the 12 ms catch-up budget */
        {
            test_read_us_per_kb = 1400; test_read_max_us = 0;
            mp3s_t *q = mp3s_open(path, secs);
            while (!mp3s_hz(q) && mp3s_searching(q)) mp3s_service(q, 0, -1, 100000);   /* load, as the deck does */
            test_read_max_us = 0; test_read_max_bytes = 0;
            int64_t m = 0;
            uint32_t worst = 0;
            for (int64_t j = 0; j < 60 * (int64_t)ref_hz && j < ref_n; j += step) {
                uint32_t t = hal_time_us();
                mp3s_service(q, j, 0, 5000);
                if (hal_time_us() - t > worst) worst = hal_time_us() - t;
                compare(q, j, step, 0, &m);
            }
            mp3s_stats_t qs; mp3s_stats(q, &qs);
            printf("  slow flash: %lld samples missing, longest read %.1f ms, longest service %.1f ms, measured %u KB/s, %u ms ahead\n",
                   (long long)m, test_read_max_us / 1000.0, worst / 1000.0, qs.read_kbps, qs.ahead_ms);
            CHECK(m == 0, "slow flash never starves");
            /* (sizes, not wall time: a busy host can stretch any read) 12 ms at 1.4 ms/KB = 8 KB */
            CHECK(test_read_max_bytes <= 8192, "reads sized to the 12 ms catch-up budget (largest %u bytes)", test_read_max_bytes);
            mp3s_close(q);
            test_read_us_per_kb = 0;
        }

        /* 5. the deck itself: 10 s of play from 20 s in at 0% pitch is the decode, exactly.
         * The load runs at iPod flash speed, so a big ID3 tag must not make it give up. */
        {
            test_read_us_per_kb = 1400;
            static deck_t d; deck_init(&d);
            uint32_t tl = hal_time_us();
            CHECK(deck_load(&d, path, "") == 0 && d.stream, "deck loads the mp3 as a stream (error %d, detail %u)", deck_stream_error, deck_stream_detail);
            printf("  deck load at iPod speed: %.0f ms\n", (hal_time_us() - tl) / 1000.0);
            test_read_us_per_kb = 0;
            if (!d.stream) continue;
            double ratio = d.ratio;
            d.pos = 20.0 * AUDIO_RATE; d.playing = 1;
            static float buf[2 * 735];
            int64_t dbad = 0;
            for (int f = 0; f < 600; f++) {                              /* 600 UI frames */
                deck_service(&d);
                double p0 = d.pos;
                deck_render(&d, buf, 735);
                if (ratio != 1.0) continue;                               /* (48k: resampled, not comparable) */
                for (int i = 0; i < 735; i++) {
                    int64_t j = (int64_t)p0 + i;
                    if (fabsf(buf[2 * i] - ref[2 * j] / 32768.f) > 1e-6f || fabsf(buf[2 * i + 1] - ref[2 * j + 1] / 32768.f) > 1e-6f) dbad++;
                }
            }
            printf("  deck play 10 s: %lld wrong samples%s\n", (long long)dbad, ratio != 1.0 ? " (48 kHz: resampled, not compared)" : "");
            CHECK(dbad == 0, "deck output is the decode");
            CHECK(fabs(d.pos - 30.0 * AUDIO_RATE) < 2, "deck moved 10 s (%.1f)", d.pos / AUDIO_RATE);
            deck_unload(&d);
            CHECK(test_open_files == 0, "deck closed its file");
        }
        free(ref);
    }
    printf(fails ? "%d FAILED\n" : "all mp3 stream tests passed\n", fails);
    return fails != 0;
}
