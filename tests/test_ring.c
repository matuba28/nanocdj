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

/* Voices: each follows its own chain; up to three can sound at once and are mixed.
 * Like the device, a voice that has chained more than SIM_JOIN_LIMIT blocks dies,
 * and that "crashes the iPod" (sim_crashed). */
#ifndef SIM_JOIN_LIMIT
#define SIM_JOIN_LIMIT 1300u
#endif
typedef struct { uint8_t *cur; uint32_t pos, joins, seq; } voice_t;
static voice_t vo[3];
static uint32_t seq_counter, max_joins;
static int sim_crashed;
static uint64_t short_blocks, overlap_frames, overlap_max, overlap_run;
static uint32_t factor = 2;      /* device: the length counter runs at half speed */

static int active_voices(void) { int n = 0; for (int i = 0; i < 3; i++) if (vo[i].cur) n++; return n; }

/* the device's real limit: block starts are shared by every voice, per boot */
static uint32_t sim_total_starts, sim_total_limit = 0xffffffffu;
static void take(voice_t *v, uint8_t *d)
{
    if (++sim_total_starts > sim_total_limit) sim_crashed = 1;
    v->cur = d; v->pos = 0;
    sim_voice.counter = U32(d, D_DURATION) * sim_voice.out_rate / 1000u;   /* setSource */
    U8(d, D_PLAYING) = 1;
    U32(d, D_VOICE) = PTR_TO_U32(&sim_voice);
    if (U32(d, D_BUF_LEN) / 4u < SIM_PASS) short_blocks++;
    sim_voice.remain = U32(d, D_BUF_LEN) / 4u;
}
void sim_play(uint8_t *d)
{
    sim_voices_started++;
    if (active_voices()) sim_double_voice++;
    for (int i = 0; i < 3; i++) if (!vo[i].cur) { vo[i].joins = 0; vo[i].seq = ++seq_counter; take(&vo[i], d); return; }
    sim_crashed = 1;                                                /* more voices than the pool */
}

/* ---- mixer + DAC ---- */
static uint32_t fifo[4 * SIM_PASS]; static uint32_t fifo_n, fifo_h;   /* fetched, not yet heard */
static uint8_t fifo_real[4 * SIM_PASS];
static uint32_t fifo_seq[4 * SIM_PASS];

static void mixer_pass(void)     /* pull one pass from every voice */
{
    for (uint32_t i = 0; i < SIM_PASS; i++) {
        uint32_t v = 0; int real = 0; uint32_t best_seq = 0;
        int n_active = 0;
        for (int k = 0; k < 3; k++) {
            voice_t *x = &vo[k];
            if (!x->cur) continue;
            n_active++;
            int16_t *pcm = (int16_t *)U32_TO_PTR(U32(x->cur, D_BUF_A));
            uint32_t len = U32(x->cur, D_BUF_LEN) / 4u;
            /* past the PCM the voice pads silence until its counter runs out */
            uint32_t val = x->pos < len ? ((uint16_t)pcm[x->pos * 2] | (uint32_t)(uint16_t)pcm[x->pos * 2 + 1] << 16) : 0;
            /* the "speaker" hears the newest voice (in a handover it carries the stream on) */
            if (x->seq > best_seq) { best_seq = x->seq; v = val; real = 1; }
            uint32_t end = sim_voice.counter ? sim_voice.counter * factor : len;
            if (++x->pos >= end) {                               /* completion handler */
                uint8_t *next = (uint8_t *)U32_TO_PTR(U32(x->cur, D_NEXT));
                U8(x->cur, D_PLAYING) = 0;
                x->cur = NULL;
                if (next) {
                    if (++x->joins > max_joins) max_joins = x->joins;
                    if (x->joins > SIM_JOIN_LIMIT) sim_crashed = 1;
                    take(x, next);
                }
            }
        }
        if (n_active > 1) { overlap_frames++; if (++overlap_run > overlap_max) overlap_max = overlap_run; }
        else overlap_run = 0;
        uint32_t at = (fifo_h + fifo_n) % (4 * SIM_PASS);
        fifo[at] = v; fifo_real[at] = (uint8_t)real; fifo_seq[at] = best_seq; fifo_n++;
    }
    /* like the device: the voice's remaining count moves a pass at a time */
    for (int k = 0; k < 3; k++) if (vo[k].cur) {
        uint32_t len = U32(vo[k].cur, D_BUF_LEN) / 4u;
        sim_voice.remain = vo[k].pos < len ? len - vo[k].pos : 0;
    }
}

/* ---- the "speaker" ---- */
static uint64_t heard, dropouts, repeats, jumps, silent;
static int64_t phase_min, phase_max, last_phase; static int have_phase;
static uint64_t dac_frames;      /* DAC clock: frames output so far */
static uint32_t last_v;

static uint32_t last_seq; static uint64_t ho_switches; static int64_t ho_disc_max;
static uint32_t mark_v; static uint64_t mark_heard_at;   /* when a value >= mark_v was first heard */
static void hear(uint32_t v, int real, uint32_t seq)
{
    dac_frames++;
    if (real && mark_v && v >= mark_v && !mark_heard_at) mark_heard_at = dac_frames;
    if (!real) { silent++; return; }
    if (v == 0) { dropouts++; return; }                      /* zeroed buffer: written too late */
    int switched = heard && seq != last_seq;                  /* a handover to a new voice */
    last_seq = seq;
    if (switched) {
        int64_t d = (int64_t)v - (int64_t)(last_v + 1);       /* how far off the two voices were */
        if (d < 0) d = -d;
        if (d > ho_disc_max) ho_disc_max = d;
        ho_switches++;
    } else if (heard && v != last_v + 1) { if (v <= last_v) repeats++; else jumps++; }
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
            hear(fifo[fifo_h], fifo_real[fifo_h], fifo_seq[fifo_h]);
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

#ifdef HANDOVER_TEST
/* Voices die after SIM_JOIN_LIMIT joins; the ring must keep handing over to fresh
 * ones for as long as it plays, with the stream continuous across every handover. */
int main(void)
{
    sim_voice.out_rate = 44100;
    sim_now_us = 5000000;
    CHECK(ring_init(render, skipfn, NULL) == 0, "init");
    frames60(60 * 20); reset_ears();
    srand(3);
    for (int f = 0; f < 60 * 60 * 20; f++) {                  /* 20 minutes, uneven frames */
        ring_tick();
        sim_run(f % 7 == 0 ? 5 + (uint32_t)(rand() % 35) : (f % 3 == 2 ? 16 : 17));
    }
    ring_stats_t st; ring_stats(&st);
    printf("20 min: %u handovers, longest voice %u joins (limit %u), worst handover offset %.1f ms, "
           "longest overlap %.1f ms, %llu dropouts, phase spread %.1f ms\n",
           st.handovers, max_joins, SIM_JOIN_LIMIT, ho_disc_max / 44.1, overlap_max / 44.1,
           (unsigned long long)dropouts, (phase_max - phase_min) / 44.1);
    CHECK(!sim_crashed, "a voice went past the join limit (max %u)", max_joins);
    CHECK(st.handovers >= 20, "kept handing over (%u)", st.handovers);
    CHECK(ho_switches == st.handovers, "each handover switched voices once (%llu vs %u)", (unsigned long long)ho_switches, st.handovers);
    CHECK(ho_disc_max <= 44100 / 1000 * 20, "new voice within 20 ms of the old (%.1f ms)", ho_disc_max / 44.1);
    CHECK(overlap_max <= XF_FRAMES + 44100 / 50, "voices overlap only for the crossfade (%.1f ms)", overlap_max / 44.1);
    CHECK(dropouts == 0 && repeats == 0 && jumps == 0, "continuous apart from handovers: %llu dropouts %llu repeats %llu jumps",
          (unsigned long long)dropouts, (unsigned long long)repeats, (unsigned long long)jumps);
    /* each handover lands within a few ms either way, unbiased: over 32 of them that
     * random walk stays well under 0.1 s (in use there's one every 10 minutes) */
#ifndef HO_QUICKCHECK
    CHECK(phase_max - phase_min < 44100 / 10, "stays on the wall clock (spread %.1f ms)", (phase_max - phase_min) / 44.1);
#else
    /* 200 ms blocks (the quick device check only): HANDOVER_AIM is tuned for the real
     * 1 s blocks, here each handover lands ~4 ms early on average */
    CHECK(phase_max - phase_min < 44100 / 4, "stays near the wall clock (spread %.1f ms)", (phase_max - phase_min) / 44.1);
#endif
    CHECK(st.restarts == 1, "never had to restart from dry (%u)", st.restarts);
    printf(fails ? "%d FAILED\n" : "handover tests passed\n", fails);
    return fails != 0;
}
#else
int main(int argc, char **argv)
{
    if (argc > 1) factor = (uint32_t)atoi(argv[1]);
    printf("-- voice counter factor %u --\n", factor);
    sim_voice.out_rate = 44100;
    sim_now_us = 5000000;
    CHECK(ring_init(render, skipfn, NULL) == 0, "init");

    /* 0. calibration: within a few seconds the block timing is learnt */
    frames60(60 * 40);
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
    CHECK(st.out_rate == 44100 && st.block_frames == BLOCK_DEF && st.duration_ok == 0, "rate %u block %u dur %d", st.out_rate, st.block_frames, st.duration_ok);
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

    /* 3b. the lead that jitter pushed up comes back down once frames are punctual */
    { ring_stats_t ds; ring_stats(&ds);
      printf("lead after jitter %u ms, after 2 clean minutes %u ms\n", st.lead_ms, ds.lead_ms);
      CHECK(st.lead_ms <= LEAD_MS || ds.lead_ms < st.lead_ms, "lead decays (%u -> %u ms)", st.lead_ms, ds.lead_ms); }

    /* 3b'. gaps of ~90 ms push the lead up; an announced stall doesn't; and once frames
     *      are punctual again it comes back down */
    {
        ring_stats_t a0, a1, a2, a3; ring_stats(&a0);
        reset_ears();
        ring_cover(300); sim_run(250); ring_tick(); frames60(30);   /* e.g. a slow file write */
        ring_stats(&a1);
        CHECK(a1.lead_ms == a0.lead_ms, "an announced stall doesn't grow the lead (%u -> %u)", a0.lead_ms, a1.lead_ms);
        CHECK(dropouts == 0 && repeats == 0 && jumps == 0, "and the audio plays through it: %llu dropouts %llu repeats %llu jumps",
              (unsigned long long)dropouts, (unsigned long long)repeats, (unsigned long long)jumps);
        CHECK(llabs(last_phase - phase0) < 45, "in phase after it (%lld frames off)", (long long)(last_phase - phase0));
        for (int k = 0; k < 6; k++) { sim_run(75); ring_tick(); frames60(30); }   /* (+ the 16 ms before: 91 ms gaps) */
        ring_stats(&a2);
        CHECK(a2.lead_ms >= a0.lead_ms + 20, "late ticks grow the lead (%u -> %u)", a0.lead_ms, a2.lead_ms);
        frames60(60 * 40);
        ring_stats(&a3);
        CHECK(a3.lead_ms == a2.lead_ms, "not within the first clean minute (%u -> %u)", a2.lead_ms, a3.lead_ms);
        frames60(60 * 200);
        ring_stats(&a3);
        printf("lead: %u ms, %u after hitches, %u after 4 clean minutes\n", a0.lead_ms, a2.lead_ms, a3.lead_ms);
        CHECK(a3.lead_ms == LEAD_MS, "lead back to %u ms (%u)", LEAD_MS, a3.lead_ms);
        frames60(60); phase0 = last_phase;
    }

    /* 3c. a control change: without a rewind it is heard a whole lead later; with
     *     one, as soon as the mixer's next pass. The source jumps (a new "track
     *     position") at the change, and we time when the speaker first plays it. */
    {
        double lat[2];
        for (int rw = 0; rw < 2; rw++) {
            double worst = 0;
            for (int k = 0; k < 40; k++) {
                frames60(7 + k % 5); sim_run((uint32_t)(k % 16));     /* presses land anywhere in a frame / pass */
                uint32_t taken = rw ? ring_rewind() : 0;
                counter -= taken;                                     /* the source goes back by what was taken */
                counter += 500000u; mark_v = counter; mark_heard_at = 0;
                uint64_t t0 = dac_frames;
                frames60(20);
                CHECK(mark_heard_at, "the change was heard");
                double l = (double)(mark_heard_at - t0) / 44.1;
                if (l > worst) worst = l;
            }
            lat[rw] = worst;
        }
        mark_v = 0;
        ring_stats_t rs; ring_stats(&rs);
        printf("control change heard after (worst of 40): %.1f ms without rewind, %.1f ms with (%u rewinds, %u from the mixer position, %u ms taken back on average)\n",
               lat[0], lat[1], rs.rewinds, rs.rewinds_fetch, rs.rewind_ms);
        CHECK(lat[1] < 70, "rewind: change heard within 70 ms (%.1f)", lat[1]);
        CHECK(lat[1] + 30 < lat[0], "rewind is faster than waiting out the lead (%.1f vs %.1f)", lat[1], lat[0]);
        CHECK(rs.rewinds_fetch + 2 >= rs.rewinds && rs.rewinds >= 38, "rewinds used the mixer position, bar one at a block join (%u of %u)", rs.rewinds_fetch, rs.rewinds);
        ring_stats_t l0; ring_stats(&l0);
        reset_ears(); frames60(120);
        CHECK(dropouts == 0 && repeats == 0 && jumps == 0, "steady again after rewinds: %llu dropouts %llu repeats %llu jumps",
              (unsigned long long)dropouts, (unsigned long long)repeats, (unsigned long long)jumps);
        ring_stats_t l1; ring_stats(&l1);
        CHECK(l1.lead_ms <= l0.lead_ms, "rewinds don't grow the lead (%u -> %u)", l0.lead_ms, l1.lead_ms);
        frames60(60); phase0 = last_phase;
    }

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
    /* long enough for the queued tail (up to two blocks) to run out */
    uint32_t lock_ms = 2000u + 2u * RING_BLOCK_MS;
    sim_run(lock_ms);
    CHECK(active_voices() == 0, "lock: voice stopped");
    CHECK(heard - heard_before <= (uint64_t)BLOCK_DEF * 2u + 44100u, "lock: only the queued tail played (%llu frames)", (unsigned long long)(heard - heard_before));
    int v0 = sim_voices_started;
    frames60(120);
    printf("%u s lock: restarts %d, phase moved %.1f ms\n", lock_ms / 1000u, sim_voices_started - v0, (last_phase - phase0) / 44.1);
    CHECK(sim_voices_started == v0 + 1, "lock: one restart (%d)", sim_voices_started - v0);
    CHECK(repeats == 0, "lock: nothing replayed");
    CHECK(llabs(last_phase - phase0) < 662, "lock: within 15 ms of the wall clock (%.1f ms)", (last_phase - phase0) / 44.1);

    /* 6. 48 kHz mixer: blocks re-sized so the duration is exact */
    uint32_t div_was = g_ctr_div; int bad_was = g_duration_bad; g_ctr_div = 1; g_duration_bad = 0;
    g_out_rate = 48000; choose_block();
    CHECK(g_block % 48 == 0 && g_block * 10u > g_cap * 9u && duration_ms(g_block) * 48 == g_block && g_block <= g_cap, "48k block %u dur %u", g_block, duration_ms(g_block));
    g_out_rate = 44100; choose_block(); g_ctr_div = div_was; g_duration_bad = bad_was;

    CHECK(short_blocks == 0, "a block shorter than a mixer pass was played (%llu)", (unsigned long long)short_blocks);
    { ring_stats_t hs; ring_stats(&hs);
      CHECK((uint32_t)sim_double_voice == hs.handovers, "two voices only ever overlap in a handover (%d vs %u)", sim_double_voice, hs.handovers); }
    CHECK(!sim_crashed, "no voice went past the join limit (max %u joins)", max_joins);

    /* 7. unload: not safe while a voice holds a descriptor, safe once it ends, never loops */
    frames60(20);
    CHECK(ring_prepare_unload() == 0, "unload refused while playing");
    sim_run(RING_BLOCK_MS + 200u);                           /* the sounding block plays out (silenced) */
    CHECK(ring_prepare_unload() == 1, "unload allowed after the voice finished");
    CHECK(active_voices() == 0, "voice stopped");
    uint64_t before = heard;
    ring_tick(); sim_run(300);
    CHECK(heard == before, "nothing plays after unload");

    /* 8. release: buffers freed once the voice is done, and the ring can start again */
    ring_release();
    for (uint32_t i = 0; i < SLOTS; i++) CHECK(g_slot[i].pcm == NULL, "slot %u freed", i);
    CHECK(ring_init(render, skipfn, NULL) == 0, "re-init after release");

    /* 9. budget: the device crashes after ~1,300 starts per boot, shared by every
     *    voice. With a budget below that, the audio stops cleanly instead. */
    {
        uint32_t limit = sim_total_starts + 12u;               /* the "iPod" dies past this */
        sim_total_limit = limit;
        ring_stats_t b0; ring_stats(&b0);
        ring_set_budget(b0.starts + 8u);                       /* we stop well before */
        for (int f = 0; f < 60 * 60 * 3 && !sim_crashed; f++) { ring_tick(); sim_run(f % 3 == 2 ? 16 : 17); }
        ring_stats_t b1; ring_stats(&b1);
        printf("budget: stopped after %u starts (budget %u), device starts %u of %u, crashed %d\n",
               b1.starts - b0.starts, b1.budget - b0.starts, sim_total_starts, limit, sim_crashed);
        CHECK(!sim_crashed, "the budget kept the device under its limit");
        CHECK(b1.starts == b1.budget, "used exactly the budget (%u of %u)", b1.starts, b1.budget);
        CHECK(active_voices() == 0, "audio stopped once the budget ran out");
        uint64_t h = heard; for (int f = 0; f < 600; f++) { ring_tick(); sim_run(16); }
        CHECK(heard == h && !sim_crashed, "and stays stopped");
    }

    printf(fails ? "%d FAILED\n" : "all ring tests passed\n", fails);
    return fails != 0;
}
#endif
