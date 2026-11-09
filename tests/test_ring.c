/* Host test for port/nanoapps/audio_ring.c against a simulated OS voice.
 * The render callback writes the source's frame counter into the PCM (skips move
 * it on too), so what the "speaker" plays can be checked sample by sample for
 * dropouts, repeats and phase: a source that stays on the wall clock keeps
 * (value heard - time heard) constant. Run: make test-ring */
#define RING_HOST_TEST
#include "../port/nanoapps/audio_ring.c"
#include <stdio.h>

uint32_t sim_now_us;
sim_voice_t sim_voice;
int sim_voices_started, sim_double_voice;
static uint8_t *cur;              /* descriptor the voice is reading, NULL = idle */
static uint32_t pos;              /* frame within it */
static uint64_t short_blocks;

static uint32_t factor = 2;      /* device: the length counter runs at half speed */
static void take(uint8_t *d)
{
    cur = d; pos = 0;
    sim_voice.counter = U32(d, D_DURATION) * sim_voice.out_rate / 1000u;   /* setSource */
    U8(d, D_PLAYING) = 1;
    U32(d, D_VOICE) = PTR_TO_U32(&sim_voice);
    if (U32(d, D_BUF_LEN) / 4u < SIM_PASS) short_blocks++;
}
void sim_play(uint8_t *d) { sim_voices_started++; if (cur) sim_double_voice++; take(d); }

/* ---- mixer + DAC ---- */
static uint32_t fifo[4 * SIM_PASS]; static uint32_t fifo_n, fifo_h;   /* fetched, not yet heard */
static uint8_t fifo_real[4 * SIM_PASS];

static void mixer_pass(void)     /* pull one pass from the chain */
{
    for (uint32_t i = 0; i < SIM_PASS; i++) {
        uint32_t v = 0; int real = 0;
        if (cur) {
            int16_t *pcm = (int16_t *)U32_TO_PTR(U32(cur, D_BUF_A));
            uint32_t len = U32(cur, D_BUF_LEN) / 4u;
            /* past the PCM the voice pads silence until its counter runs out */
            v = pos < len ? ((uint16_t)pcm[pos * 2] | (uint32_t)(uint16_t)pcm[pos * 2 + 1] << 16) : 0;
            real = 1;
            uint32_t end = sim_voice.counter ? sim_voice.counter * factor : len;
            if (++pos >= end) {                                  /* completion handler */
                uint8_t *next = (uint8_t *)U32_TO_PTR(U32(cur, D_NEXT));
                U8(cur, D_PLAYING) = 0;
                cur = NULL;
                if (next) take(next);
            }
        }
        uint32_t at = (fifo_h + fifo_n) % (4 * SIM_PASS);
        fifo[at] = v; fifo_real[at] = (uint8_t)real; fifo_n++;
    }
}

/* ---- the "speaker" ---- */
static uint64_t heard, dropouts, repeats, jumps, silent;
static int64_t phase_min, phase_max, last_phase; static int have_phase;
static uint64_t dac_frames;      /* DAC clock: frames output so far */
static uint32_t last_v;

static void hear(uint32_t v, int real)
{
    dac_frames++;
    if (!real) { silent++; return; }
    if (v == 0) { dropouts++; return; }                      /* zeroed buffer: written too late */
    if (heard && v != last_v + 1) { if (v <= last_v) repeats++; else jumps++; }
    last_v = v; heard++;
    int64_t ph = (int64_t)v - (int64_t)dac_frames;
    if (!have_phase) { phase_min = phase_max = ph; have_phase = 1; }
    if (ph < phase_min) phase_min = ph;
    if (ph > phase_max) phase_max = ph;
    last_phase = ph;
}
static void reset_ears(void) { dropouts = repeats = jumps = silent = 0; have_phase = 0; }

static double dac_rate = 44100.0, dac_frac;
static void sim_run(uint32_t ms)  /* advance real time; the DAC drains at dac_rate */
{
    for (uint32_t t = 0; t < ms * 10; t++) {                 /* 100 us steps */
        sim_now_us += 100;
        dac_frac += dac_rate / 10000.0;
        while (dac_frac >= 1.0) {
            dac_frac -= 1.0;
            if (fifo_n < SIM_PASS) mixer_pass();             /* double-buffered: prefetch a pass */
            hear(fifo[fifo_h], fifo_real[fifo_h]);
            fifo_h = (fifo_h + 1) % (4 * SIM_PASS); fifo_n--;
        }
    }
}

/* ---- source ---- */
static uint32_t counter = 1;      /* 0 is reserved for "never written" */
static uint64_t skipped;
static void render(int16_t *out, uint32_t frames, void *u)
{
    (void)u;
    for (uint32_t i = 0; i < frames; i++, counter++) {
        out[2 * i] = (int16_t)(counter & 0xffff);
        out[2 * i + 1] = (int16_t)(counter >> 16);
    }
}
static void skipfn(uint32_t frames, void *u) { (void)u; counter += frames; skipped += frames; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void frames60(int n) { for (int f = 0; f < n; f++) { ring_tick(); sim_run(f % 3 == 2 ? 16 : 17); } }

int main(int argc, char **argv)
{
    if (argc > 1) factor = (uint32_t)atoi(argv[1]);
    printf("-- voice counter factor %u --\n", factor);
    sim_voice.out_rate = 44100;
    sim_now_us = 5000000;
    CHECK(ring_init(render, skipfn, NULL) == 0, "init");

    /* 0. calibration: within a few seconds the block timing is learnt */
    frames60(60 * 10);
    ring_stats_t cs; ring_stats(&cs);
    printf("calibrated: counter divisor %u (0 = no counter), last ratio %u%%, %u measurements\n", cs.ctr_div, cs.cal_ratio, cs.cal_steps);
    CHECK(cs.ctr_div == 0, "starts and stays without a counter (what works on the device)");
    CHECK(cs.cal_ratio >= 90 && cs.cal_ratio <= 110, "blocks now take their own length (%u%%)", cs.cal_ratio);

    /* 1. steady 60 fps for 10 s */
    frames60(30);  reset_ears();                              /* let it settle */
    frames60(600);
    ring_stats_t st; ring_stats(&st);
    CHECK(dropouts == 0 && repeats == 0 && jumps == 0, "steady: dropouts %llu repeats %llu jumps %llu",
          (unsigned long long)dropouts, (unsigned long long)repeats, (unsigned long long)jumps);
    CHECK(sim_voices_started == 1, "steady: voices started %d", sim_voices_started);
    CHECK(st.out_rate == 44100 && st.block_frames == 8820 && st.duration_ok == 0, "rate %u block %u dur %d", st.out_rate, st.block_frames, st.duration_ok);
    double lat = (counter - last_v) / 44.1;
    printf("steady: latency %.1f ms (lead %u ms), %u blocks queued\n", lat, st.lead_ms, st.queued);
    CHECK(lat < 130, "latency %.1f ms", lat);
    int64_t phase0 = last_phase;

    /* 2. jittery UI: frame gaps 5..45 ms */
    reset_ears(); srand(1);
    for (int f = 0; f < 600; f++) { ring_tick(); sim_run(5 + (uint32_t)(rand() % 41)); }
    ring_stats(&st);
    printf("jitter 5-45 ms: %llu dropouts, lead now %u ms\n", (unsigned long long)dropouts, st.lead_ms);
    CHECK(dropouts == 0 && repeats == 0 && jumps == 0, "jitter: dropouts %llu repeats %llu jumps %llu",
          (unsigned long long)dropouts, (unsigned long long)repeats, (unsigned long long)jumps);

    /* 3. DAC clock 120 ppm fast, then slow, 60 s each: the cursor must not drift */
    reset_ears(); dac_rate = 44105.3; frames60(3600);
    CHECK(dropouts == 0 && repeats == 0, "fast DAC: dropouts %llu repeats %llu", (unsigned long long)dropouts, (unsigned long long)repeats);
    reset_ears(); dac_rate = 44094.7; frames60(3600);
    CHECK(dropouts == 0 && repeats == 0, "slow DAC: dropouts %llu repeats %llu", (unsigned long long)dropouts, (unsigned long long)repeats);
    dac_rate = 44100.0;
    CHECK(sim_voices_started == 1, "no restarts through drift (%d)", sim_voices_started);
    frames60(60); phase0 = last_phase;

    /* 4. a 150 ms hitch (no frames): goes quiet, then carries on in phase, same voice */
    reset_ears();
    sim_run(150); frames60(120);
    printf("150 ms hitch: %llu frames of silence, phase moved %lld frames\n",
           (unsigned long long)dropouts, (long long)(last_phase - phase0));
    CHECK(repeats == 0, "hitch: nothing replayed");
    CHECK(llabs(last_phase - phase0) < 45, "hitch: back in phase (%lld frames off)", (long long)(last_phase - phase0));
    CHECK(sim_voices_started == 1, "hitch: no restart");
    phase0 = last_phase;

    /* 5. screen locked for 5 s: the voice runs out (silence, never stale audio),
     *    restarts once, and the source has moved on by the time that passed */
    reset_ears();
    uint64_t heard_before = heard;
    sim_run(5000);
    CHECK(cur == NULL, "lock: voice stopped");
    CHECK(heard - heard_before < 44100 / 2, "lock: only the queued tail played (%llu frames)", (unsigned long long)(heard - heard_before));
    int v0 = sim_voices_started;
    frames60(120);
    printf("5 s lock: restarts %d, phase moved %.1f ms\n", sim_voices_started - v0, (last_phase - phase0) / 44.1);
    CHECK(sim_voices_started == v0 + 1, "lock: one restart (%d)", sim_voices_started - v0);
    CHECK(sim_double_voice == 0, "never two voices at once");
    CHECK(repeats == 0, "lock: nothing replayed");
    CHECK(llabs(last_phase - phase0) < 662, "lock: within 15 ms of the wall clock (%.1f ms)", (last_phase - phase0) / 44.1);

    /* 6. 48 kHz mixer: blocks re-sized so the duration is exact */
    uint32_t div_was = g_ctr_div; int bad_was = g_duration_bad; g_ctr_div = 1; g_duration_bad = 0;
    g_out_rate = 48000; choose_block();
    CHECK(g_block % 48 == 0 && g_block > 8000 && duration_ms(g_block) * 48 == g_block && g_block <= BLOCK_MAX, "48k block %u dur %u", g_block, duration_ms(g_block));
    g_out_rate = 44100; choose_block(); g_ctr_div = div_was; g_duration_bad = bad_was;

    CHECK(short_blocks == 0, "a block shorter than a mixer pass was played (%llu)", (unsigned long long)short_blocks);
    CHECK(sim_double_voice == 0, "two voices at once (%d)", sim_double_voice);

    /* 7. unload: not safe while a voice holds a descriptor, safe once it ends, never loops */
    frames60(20);
    CHECK(ring_prepare_unload() == 0, "unload refused while playing");
    sim_run(1000);
    CHECK(ring_prepare_unload() == 1, "unload allowed after the voice finished");
    CHECK(cur == NULL, "voice stopped");
    uint64_t before = heard;
    ring_tick(); sim_run(300);
    CHECK(heard == before, "nothing plays after unload");

    /* 8. release: buffers freed once the voice is done, and the ring can start again */
    ring_release();
    for (uint32_t i = 0; i < SLOTS; i++) CHECK(g_slot[i].pcm == NULL, "slot %u freed", i);
    CHECK(ring_init(render, skipfn, NULL) == 0, "re-init after release");

    printf(fails ? "%d FAILED\n" : "all ring tests passed\n", fails);
    return fails != 0;
}
