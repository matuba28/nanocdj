/* See audio_ring.h. Descriptor layout and the chaining behaviour are RetailOS 1.1.2
 * facts documented (with addresses) in NanoApps apps/entrain/platform/audio_device.c. */
#include "audio_ring.h"
#include <stddef.h>

#ifndef RING_HOST_TEST
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_shared.h"
typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc, void *cb, void *cbdata);
#define OS_DESC_CTOR(d)  ((sfx_ctor_t)(0x08417efcu | 1u))(d)
#define OS_PLAY(d)       do { void *pl_ = ((sfx_player_inst_t)(0x08417eb8u | 1u))(); \
                              if (pl_) ((sfx_player_play_t)(0x0841828cu | 1u))(pl_, (d), 0, 0); } while (0)
#define OS_NOW_US()      hb_time_uptime_us()
#define OS_ALLOC(n)      hb_os_alloc(n)
#define OS_LARGEST()     hb_os_heap_largest()
#else
#include "ring_host_os.h"   /* simulated voice for host tests */
#endif

/* the OS keeps 32-bit pointers in descriptors (the host test maps them) */
#ifndef PTR_TO_U32
#define PTR_TO_U32(p) ((uint32_t)(uintptr_t)(p))
#define U32_TO_PTR(u) ((void *)(uintptr_t)(u))
#endif

/* descriptor fields (SoundEffectDescriptor, RetailOS 1.1.2) */
#define D_BUF_A    0x04
#define D_BUF_B    0x08
#define D_BUF_LEN  0x0C
#define D_TYPE     0x10
#define D_RATE     0x14
#define D_CHANNELS 0x18
#define D_BITS     0x1C
#define D_VOLUME   0x24
#define D_DURATION 0x34   /* ms; setSource turns it into the voice's sample counter */
#define D_TRIM_LO  0x38
#define D_TRIM_HI  0x3C
#define D_VOICE    0x48
#define D_PLAYMODE 0x51
#define D_FLAGS    0x52
#define D_NEXT     0x54   /* chain: followed at end of buffer */
#define D_PLAYING  0x64   /* byte: 1 while a voice holds it */

#define RATE        44100u
#define SLOTS       4u           /* slot array capacity */
#ifndef RING_SLOTS
#define RING_SLOTS  2u           /* slots in use: two long blocks alternate */
#endif
/* 1 s blocks. Latency doesn't depend on block length (we write just ahead of the
 * cursor), but the number of chained joins does: on the device the voice died
 * after ~1,300-1,500 joins (20 ms blocks: ~30 s; 200 ms blocks: ~4-5 min). */
/* RetailOS's sound-effect player allows only ~1,300 block starts per boot: every
 * chained join or play() uses one, whatever the voice, and only a restart of the
 * iPod gives them back (measured: crashes at ~1,230-1,300 with 20 ms, 200 ms and
 * 1 s blocks, with fresh voices, and across app sessions). Latency doesn't depend
 * on block length, so blocks are long: 6 s gives ~2 hours per boot. If there isn't
 * the memory, 3 s then 1 s. The budget itself is enforced by ring_set_budget(). */
#ifndef RING_BLOCK_MS
#define RING_BLOCK_MS 6000u
#endif
#define BLOCK_DEF   (441u * RING_BLOCK_MS / 10u)   /* frames, exact at 44.1k / 22.05k */
/* A voice can't chain forever: on the device one died after ~1,300 joins (whatever
 * the block length), taking the iPod with it (screen off, hard reset). So after
 * HANDOVER_JOINS the stream moves to a fresh voice: the new one starts XF_FRAMES
 * before the old one's last block ends, on a copy of that audio, and the two are
 * crossfaded. Start-time error is about a frame (~16 ms), well inside the fade. */
#ifndef RING_HANDOVER_JOINS
/* Off: moving to a fresh voice doesn't give starts back (the limit is shared by
 * every voice), but the mechanism is kept, tested, for firmware where it might. */
#define RING_HANDOVER_JOINS 0u
#endif
#define XF_FRAMES   2646u          /* 60 ms crossfade window */
/* A started voice is first read at the mixer's next 1024-frame pass (pass size
 * measured on the device). Against a simulated mixer with that pass size, aiming
 * the new voice 128 frames ahead of the cursor estimate made the handover offset
 * unbiased (mean 0.3 ms, sd 7.5 ms, worst 16 ms): without it every handover left
 * the music ~3 ms late, half a pass made it ~7 ms early, and those add up. */
#define HANDOVER_AIM 128u
#define LEAD_MS     80u          /* written ahead of the estimated cursor */
#define LEAD_MAX_MS 200u
#define JITTER_MS   100u         /* tick gaps up to this are cadence, longer ones are stalls */
#define NOSTART_MS  150u         /* a started voice that never shows up: give up on it */
/* ring_rewind(): how close to the mixer the write point is taken back. With the
 * mixer's own read position (voice+0x50) only a little slack is needed: it reads in
 * 1024-frame passes ~23 ms apart, and the re-render follows within a millisecond or
 * two. Without it, the cursor estimate can be a pass behind the mixer and a tick
 * out, so stay further ahead. */
#define REWIND_FETCH 256u        /* frames past the mixer's read position */
#define REWIND_EST   1536u       /* frames past the estimated cursor */
#define FETCH_SANE   2048u       /* a read position further than this from the estimate isn't believed */
/* The lead grows when ticks come late, and comes back down once they have been
 * punctual for a while: after DECAY_FIRST_MS without a late tick and with every
 * gap well under the one that last made it grow, it drops DECAY_STEP_MS, and again
 * every DECAY_NEXT_MS. A hitch that recurs more often than that keeps its lead. */
#define DECAY_FIRST_MS 60000u
#define DECAY_NEXT_MS  30000u
#define DECAY_STEP_MS  20u
#define DECAY_ROOM_MS  15u

#define U32(d, off) (*(volatile uint32_t *)((d) + (off)))
#define U8(d, off)  (*(volatile uint8_t *)((d) + (off)))

typedef struct {
    uint8_t  desc[0x80] __attribute__((aligned(8)));
    int16_t *pcm;
    uint32_t start;              /* stream frame of pcm[0] */
    uint32_t frames;
    uint8_t  queued, seen;
} slot_t;

static slot_t   g_slot[SLOTS];
static uint8_t  g_q[SLOTS];       /* FIFO of queued slot indices, [0] = oldest */
static uint32_t g_qn;
static ring_render_fn g_render;
static ring_skip_fn   g_skip;
static void    *g_user;
static uint32_t g_out_rate, g_block = BLOCK_DEF;
static uint32_t g_cap = BLOCK_DEF;          /* frames each slot buffer holds */
static uint32_t g_nslots = RING_SLOTS;
static uint32_t g_starts;                   /* block starts committed (chains + plays) */
static uint32_t g_budget = 0xffffffffu;     /* starts allowed before stopping */
/* g_duration_bad starts set: on the device (RetailOS 1.1.2, nano 7G 2012) neither
 * counter = frames nor frames / 2 timed right (blocks ran ~2x long), while no
 * counter at all measured 98-101% for minutes. Starting there avoids ~10 s of
 * calibration glitches; blocks are still timed and reported (NC_CAL). */
#ifndef RING_FORCE_DIV
static int      g_duration_bad = 1, g_stopping, g_ready, g_running;
#else
/* experiment: state the voice counter as frames / RING_FORCE_DIV and never change it */
static int      g_duration_bad = 0, g_stopping, g_ready, g_running;
#endif
static uint32_t g_w;              /* stream frames written so far */
static uint32_t g_end;            /* stream frame where the chained blocks end */
static uint32_t g_head_t;         /* estimated time (us) the FIFO head started sounding */
static uint32_t g_last_tick;      /* us */
static uint32_t g_t_out;          /* us at which the cursor reaches g_w (written audio runs out) */
static uint32_t g_lead_ms = LEAD_MS;
static uint8_t  g_quiet_ticks;
static uint32_t g_restarts, g_late, g_skipped_frames;
static uint8_t  g_rewound, g_forgive;      /* this tick follows a rewind / a stall the caller announced */
static uint32_t g_cover_ms;                /* ring_cover(): write this far ahead on the tick in progress */
static uint32_t g_rewinds, g_rw_fetch, g_rw_frames;
static uint32_t g_win_t0, g_win_ms = DECAY_FIRST_MS, g_win_gap, g_bump_gap;   /* lead decay (gaps in us) */
static uint32_t g_voice_joins, g_joins_total;   /* chained handovers on the current voice / ever */
/* voice handover: 0 none, 1 armed (new voice's first block queued, not started),
 * 2 started (old voice still finishing its last block) */
static int      g_ho;
static uint8_t  g_ho_slot, g_ho_tail, g_ho_faded;
static uint32_t g_ho_t, g_handovers;
/* fetch probe: voice+0x50 counts down the frames the mixer has still to pull from
 * the current buffer (in 1024-frame passes), so start + frames - it = how far the
 * mixer has actually read. Diagnostics only for now. */
#define V_REMAIN 0x50u
static uint32_t g_fetch, g_fetch_ok, g_fetch_odd, g_fetch_odd_delta, g_fetch_behind, g_fetch_samples;
static int32_t  g_fetch_min_lead = 0x7fffffff, g_fetch_max_est;

/* The voice's length counter (voice+0x48, set from descriptor+0x34) does not
 * count output frames one-for-one on the device: with it set to a block's frame
 * count, a 200 ms block took ~385 ms, i.e. our PCM followed by as much silence
 * (and the deck ran at half speed). So the counter is stated as frames / g_ctr_div,
 * and the divisor is learnt by timing real blocks rather than assumed:
 *   div 1 measured ~2x long -> div 2;  still wrong after that -> no counter at all
 *   (g_duration_bad; entrain's fallback: right speed, a faint click per join). */
#ifndef RING_FORCE_DIV
static uint32_t g_ctr_div = 1;
#else
static uint32_t g_ctr_div = RING_FORCE_DIV;
#endif
static uint32_t g_cal_t0, g_cal_frames, g_cal_settle, g_cal_ratio, g_cal_steps;

static int playing(const slot_t *s) { return U8(s->desc, D_PLAYING) != 0; }

static uint32_t us_to_frames(uint32_t us) { return (uint32_t)(((uint64_t)us * 441u) / 10000u); }
static uint32_t frames_to_us(uint32_t f)  { return (uint32_t)(((uint64_t)f * 10000u) / 441u); }
static int32_t  since(uint32_t now, uint32_t t) { return (int32_t)(now - t); }

/* descriptor+0x34 so the voice's counter comes out as exactly frames / g_ctr_div;
 * 0 (no counter) if that isn't exact */
static uint32_t counter_for(uint32_t frames) { return frames % g_ctr_div ? 0 : frames / g_ctr_div; }
static uint32_t duration_ms(uint32_t frames)
{
    if (g_duration_bad || !g_out_rate || !frames) return 0;
    frames = counter_for(frames);
    if (!frames) return 0;
    uint32_t num = frames * 1000u;
    if (num % g_out_rate) return 0;
    uint32_t ms = num / g_out_rate;
    return (ms * g_out_rate) / 1000u == frames ? ms : 0;
}

static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

/* Once the mixer rate is known, pick a ~200 ms block whose duration is a whole ms. */
static void choose_block(void)
{
    uint32_t unit = g_out_rate / gcd(g_out_rate, 1000u);   /* 441 @ 44.1k, 48 @ 48k */
    uint32_t n = (g_cap + unit / 2) / unit;
    if (!n) n = 1;
    if (unit * n > g_cap) n = g_cap / unit;
    g_block = n ? unit * n : g_cap;
}

static void learn_rate(const slot_t *s)
{
    if (g_out_rate) return;
    uint32_t v = U32(s->desc, D_VOICE);
    if (!v || (v & 3u)) return;
    uint32_t r = *(volatile uint32_t *)((uint8_t *)U32_TO_PTR(v) + 0x0Cu);
    if (r >= 8000u && r <= 192000u) { g_out_rate = r; choose_block(); }
}

static void desc_build(slot_t *s)
{
    uint8_t *d = s->desc;
    OS_DESC_CTOR(d);
    U32(d, D_BUF_A) = PTR_TO_U32(s->pcm);
    U32(d, D_BUF_B) = PTR_TO_U32(s->pcm);
    U32(d, D_BUF_LEN) = s->frames * 4u;
    U8(d, D_TYPE) = 0;                 /* linear PCM */
    U32(d, D_RATE) = RATE;
    U32(d, D_CHANNELS) = 2;
    U32(d, D_BITS) = 16;
    U32(d, D_TRIM_LO) = 0;
    U32(d, D_TRIM_HI) = 0;
    U32(d, D_DURATION) = duration_ms(s->frames);
    U32(d, D_VOLUME) = 0x7fff;         /* full scale: level is set on the mixer / headphones */
    U8(d, D_PLAYMODE) = 1;
    U8(d, D_FLAGS) = 0;
    U32(d, D_NEXT) = 0;
    U8(d, D_PLAYING) = 0;
}

static void chain(slot_t *s, slot_t *next) { U32(s->desc, D_NEXT) = next ? PTR_TO_U32(next->desc) : 0u; }

static void zero(slot_t *s) { for (uint32_t k = 0; k < g_cap * 2u; k++) s->pcm[k] = 0; }

static void drop_queue(void)
{
    for (uint32_t i = 0; i < g_qn; i++) { slot_t *s = &g_slot[g_q[i]]; chain(s, NULL); s->queued = 0; s->seen = 0; }
    g_qn = 0;
    g_ho = 0;
}

static slot_t *free_slot(void)
{
    for (uint32_t i = 0; i < g_nslots; i++)
        if (!g_slot[i].queued && !playing(&g_slot[i])) return &g_slot[i];
    return NULL;
}

static void free_slots(void)
{
    for (uint32_t i = 0; i < SLOTS; i++) if (g_slot[i].pcm) {
#ifndef RING_HOST_TEST
        hb_os_free(g_slot[i].pcm);
#else
        free(g_slot[i].pcm);
#endif
        g_slot[i].pcm = NULL;
    }
}

int ring_init(ring_render_fn fn, ring_skip_fn skip, void *user)
{
    g_render = fn; g_skip = skip; g_user = user;
    g_stopping = 0; g_running = 0; g_qn = 0; g_ho = 0;   /* (a fresh launch zeroes these anyway) */
    /* the longest blocks that fit, so the per-boot starts last longest */
    static const uint32_t try_ms[] = { RING_BLOCK_MS, 3000u, 1000u };
    int ok = 0;
    for (uint32_t t = 0; t < 3 && !ok; t++) {
        if (try_ms[t] > RING_BLOCK_MS) continue;
        uint32_t cap = 441u * try_ms[t] / 10u;
        ok = 1;
        for (uint32_t i = 0; i < g_nslots && ok; i++) {
            if (OS_LARGEST() < cap * 4u + 256u * 1024u) { ok = 0; break; }   /* never risk the OS allocator */
            g_slot[i].pcm = (int16_t *)OS_ALLOC(cap * 4u);
            if (!g_slot[i].pcm) ok = 0;
        }
        if (!ok) { free_slots(); continue; }
        g_cap = g_block = cap;
        for (uint32_t i = 0; i < g_nslots; i++) zero(&g_slot[i]);
    }
    if (!ok) return -1;
    g_last_tick = OS_NOW_US();
    g_ready = 1;
    return 0;
}

/* time to move the stream to a fresh voice? (never, with RING_HANDOVER_JOINS 0) */
static int handover_due(void)
{
#if RING_HANDOVER_JOINS
    return g_qn && !g_ho && g_voice_joins + g_qn >= RING_HANDOVER_JOINS && g_block >= 2u * XF_FRAMES;
#else
    return 0;
#endif
}

/* Append a zeroed block to the chain (starting a voice if there is none). */
static int queue_block(uint32_t now)
{
    if (g_starts >= g_budget) return 0;     /* out of block starts: let the audio stop, not the iPod */
    slot_t *s = free_slot();
    if (!s) return 0;
    zero(s);
    s->start = g_end;
    s->frames = g_block;
    s->queued = 1; s->seen = 0;
    desc_build(s);
    if (handover_due()) {
        /* Time for a fresh voice: this block isn't chained. It will start a new
         * voice XF_FRAMES before the old voice's last block (the current tail) ends,
         * so it begins with a copy of that block's final XF_FRAMES. */
        s->start = g_end - XF_FRAMES;
        g_ho = 1; g_ho_faded = 0;
        g_ho_slot = (uint8_t)(s - g_slot);
        g_ho_tail = g_q[g_qn - 1];
    } else if (g_qn) {
        chain(&g_slot[g_q[g_qn - 1]], s);   /* the voice picks it up itself */
    } else {
        OS_PLAY(s->desc);                   /* chain was empty: start a voice */
        g_restarts++;
        g_voice_joins = 0;
        g_head_t = now;
        g_running = 1;
        learn_rate(s);
        uint32_t v = U32(s->desc, D_VOICE); /* first block went out with no duration: fix its counter */
        if (g_out_rate && v && !(v & 3u) && duration_ms(s->frames))
            *(volatile uint32_t *)((uint8_t *)U32_TO_PTR(v) + 0x48u) = counter_for(s->frames);
        g_cal_frames = 0; g_cal_settle = g_nslots;   /* start timing afresh */
    }
    g_q[g_qn++] = (uint8_t)(s - g_slot);
    g_end = s->start + s->frames;
    if (g_ho != 1 || g_q[g_qn - 1] != g_ho_slot) g_starts++;   /* (a handover block counts when it starts) */
    return 1;
}

/* Once the overlap has been rendered into the old voice's last block: copy it to
 * the head of the new voice's first block fading in, and fade it out in place. */
static void handover_fade(void)
{
    slot_t *t = &g_slot[g_ho_tail], *n = &g_slot[g_ho_slot];
    if (g_ho != 1 || g_ho_faded || g_w < t->start + t->frames) return;
    int16_t *src = t->pcm + (t->frames - XF_FRAMES) * 2u;
    for (uint32_t i = 0; i < XF_FRAMES; i++) {
        int32_t g = (int32_t)((i * 4096u) / XF_FRAMES);           /* 0 .. 4095 */
        for (uint32_t c = 0; c < 2; c++) {
            int32_t v = src[2 * i + c];
#ifndef RING_HOST_TEST
            n->pcm[2 * i + c] = (int16_t)((v * g) >> 12);
            src[2 * i + c] = (int16_t)((v * (4096 - g)) >> 12);
#else
            n->pcm[2 * i + c] = (int16_t)v;   /* host test: keep the sample values readable */
#endif
        }
    }
    g_ho_faded = 1;
}

/* Start the new voice once the old one has reached the crossfade. The tick that
 * notices is up to a frame late, so first slide the new block's audio left until its
 * first sample is where the old voice is now: the voices then line up to within the
 * cursor estimate, and the stream doesn't drift a little later at every handover.
 * Both fades depend only on the stream position, so they still sum to one at
 * whatever point the new voice comes in. */
static void handover_start(uint32_t now, uint32_t cursor)
{
    slot_t *t = &g_slot[g_ho_tail], *n = &g_slot[g_ho_slot];
    if (g_ho != 1 || !g_ho_faded) return;
    uint32_t aim = cursor + HANDOVER_AIM;                          /* where the old voice will be when the new one is read */
    if (aim < n->start) return;                                    /* not at the crossfade yet */
    uint32_t tail_end = t->start + t->frames;
    if (aim >= tail_end || !playing(t)) return;                    /* missed it: the chain runs dry and restarts */
    uint32_t d = aim - n->start;
    if (d && g_qn && g_q[g_qn - 1] == g_ho_slot) {
        uint32_t keep = n->frames - d;
        for (uint32_t i = 0; i < keep * 2u; i++) n->pcm[i] = n->pcm[i + d * 2u];
        for (uint32_t i = keep * 2u; i < n->frames * 2u; i++) n->pcm[i] = 0;
        n->start += d;
        g_end = n->start + n->frames;
    }
    if (g_starts >= g_budget) return;
    OS_PLAY(n->desc);
    g_starts++;
    g_ho = 2; g_ho_t = now;
    g_voice_joins = 0; g_handovers++;
}

/* Render stream frames [g_w, target) into the queued blocks that hold them. */
static void render_to(uint32_t target)
{
    for (uint32_t i = 0; i < g_qn && g_w < target; i++) {
        slot_t *s = &g_slot[g_q[i]];
        if (g_w < s->start || g_w >= s->start + s->frames) continue;
        uint32_t off = g_w - s->start;
        uint32_t n = s->start + s->frames - g_w;
        if (n > target - g_w) n = target - g_w;
        g_render(s->pcm + off * 2u, n, g_user);
        g_w += n;
    }
}

static void skip(uint32_t frames)
{
    if (!frames) return;
    g_skipped_frames += frames;
    if (g_skip) g_skip(frames, g_user);
}

/* Time real blocks: `k` blocks just finished (seen at this tick). Over ~1.6 s of
 * blocks, compare the time taken with the audio they held. Ticks are ~16 ms apart,
 * so over that window the measurement is good to ~2%. */
#define CAL_FRAMES (12u * 44100u)   /* measure over >= 12 s of blocks */
static void calibrate(uint32_t now, uint32_t k)
{
    if (!g_out_rate) return;
    if (g_cal_settle) {                  /* blocks queued under the old setting */
        g_cal_settle = g_cal_settle > k ? g_cal_settle - k : 0;
        if (!g_cal_settle) { g_cal_t0 = now; g_cal_frames = 0; }
        return;
    }
    if (!g_cal_frames && !g_cal_t0) { g_cal_t0 = now; return; }
    for (uint32_t i = 0; i < k; i++) g_cal_frames += g_slot[g_q[i]].frames;
    if (g_cal_frames < CAL_FRAMES) return;
    uint32_t took = (uint32_t)since(now, g_cal_t0);
    uint32_t want = frames_to_us(g_cal_frames);
    g_cal_ratio = (uint32_t)(((uint64_t)took * 100u) / want);
    g_cal_steps++;
    g_cal_t0 = now; g_cal_frames = 0;
    if (g_duration_bad) return;                                      /* fallback: just keep reporting */
#ifdef RING_FORCE_DIV
    return;                                                          /* experiment: report only */
#endif
    if (g_cal_ratio >= 85u && g_cal_ratio <= 115u) return;           /* right */
    if (g_ctr_div == 1u && g_cal_ratio >= 160u && g_cal_ratio <= 240u) g_ctr_div = 2u;
    else g_duration_bad = 1;                                         /* give up on the counter */
    g_cal_settle = g_nslots;
}

/* Follow the voice down the FIFO. Blocks ahead of the sounding one have finished;
 * when the head moves on, its start time is predicted from the old head's length
 * and pinned between the last tick and now (the only window it can have happened
 * in), which keeps the cursor estimate locked to the DAC. Returns 1 while a
 * block is sounding, 2 if none is but it may be a join, 0 once the chain has
 * stopped. */
static int follow(uint32_t now)
{
    int k = -1;
    for (uint32_t i = 0; i < g_qn; i++) if (playing(&g_slot[g_q[i]])) { k = (int)i; break; }

    if (k < 0) {
        int head_seen = g_qn && g_slot[g_q[0]].seen;
        /* the flag flips across a join in microseconds, so one quiet tick may be a
         * join; two in a row is a stopped voice. A voice that never appeared is
         * given up on after NOSTART_MS. */
        if (head_seen ? ++g_quiet_ticks >= 2 : since(now, g_head_t) > (int32_t)(NOSTART_MS * 1000u)) {
            g_quiet_ticks = 0;
            return 0;
        }
        return 2;
    }
    g_quiet_ticks = 0;

    if (k > 0) {
        calibrate(now, (uint32_t)k);
        g_voice_joins += (uint32_t)k; g_joins_total += (uint32_t)k;
        uint32_t pred = g_head_t + frames_to_us(g_slot[g_q[k]].start - g_slot[g_q[0]].start);
        if (since(pred, g_last_tick) < 0) pred = g_last_tick;
        if (since(pred, now) > 0) pred = now;
        if (g_ho == 2 && g_q[k] == g_ho_slot) { pred = g_ho_t; g_ho = 0; }   /* the new voice: we know when it started */
        g_head_t = pred;
        for (int i = 0; i < k; i++) { slot_t *s = &g_slot[g_q[i]]; chain(s, NULL); s->queued = 0; s->seen = 0; }
        for (uint32_t i = (uint32_t)k; i < g_qn; i++) g_q[i - (uint32_t)k] = g_q[i];
        g_qn -= (uint32_t)k;
    }
    slot_t *h = &g_slot[g_q[0]];
    h->seen = 1;
    {
        uint32_t v = U32(h->desc, D_VOICE);
        if (v && !(v & 3u)) {
            uint32_t rem = *(volatile uint32_t *)((uint8_t *)U32_TO_PTR(v) + V_REMAIN);
            if (rem <= h->frames) {
                uint32_t f = h->start + h->frames - rem;
                if (g_fetch_ok && f != g_fetch) {
                    uint32_t d = f - g_fetch;
                    if (f < g_fetch || d % 1024u) { g_fetch_odd++; g_fetch_odd_delta = d; }
                }
                if (g_fetch_ok && f < g_fetch) { /* went backwards */ }
                g_fetch = f; g_fetch_ok = 1; g_fetch_samples++;
            }
        }
    }
    /* still sounding: it can't have started longer ago than it lasts */
    uint32_t dur = frames_to_us(h->frames);
    if (since(now, g_head_t) > (int32_t)dur) g_head_t = now - dur;
    return 1;
}

void ring_tick(void)
{
    if (!g_ready || g_stopping) return;
    uint32_t now = OS_NOW_US();
    int32_t gap = since(now, g_last_tick);

    if (gap > (int32_t)(JITTER_MS * 1000u)) { g_cal_frames = 0; g_cal_t0 = 0; }
    int f = g_running ? follow(now) : 0;
    if (f == 2) { g_last_tick = now; return; }   /* decide next tick; keep g_t_out as it was (and the rewind / forgive flags) */
    if (g_running && !f) {
        /* the voice ran out (frames stopped: screen locked, app in background).
         * Everything queued has played or never will. */
        drop_queue();
        g_running = 0;
        int32_t missed = since(now, g_t_out);
        if (missed > 0) skip(us_to_frames((uint32_t)missed));
    }

    if (!g_running) {                    /* (re)start the stream */
        g_w = g_end = 0;
        g_quiet_ticks = 0;
        if (!queue_block(now)) { g_last_tick = now; return; }
    }

    /* where the voice has got to */
    slot_t *h = &g_slot[g_q[0]];
    uint32_t in = us_to_frames((uint32_t)(since(now, g_head_t) > 0 ? since(now, g_head_t) : 0));
    if (in > h->frames) in = h->frames;
    uint32_t cursor = h->start + in;

    /* (Straight after a rewind the write point was placed from the mixer's own read
     * position, which the estimate may be a little ahead of: nothing was missed.) */
    if (cursor > g_w && !g_rewound) {
        /* it overtook what we'd written: it played zeros. Move the source on by the
         * same amount so it stays on the wall clock, and if this was ordinary tick
         * jitter rather than a stall, write further ahead from now on. */
        g_late++;
        if (!g_forgive && gap <= (int32_t)(JITTER_MS * 1000u)) {
            if (g_lead_ms < LEAD_MAX_MS) g_lead_ms += 10u;
            if ((uint32_t)gap > g_bump_gap) g_bump_gap = (uint32_t)gap;
            g_win_t0 = now; g_win_ms = DECAY_FIRST_MS; g_win_gap = 0;
        }
        skip(cursor - g_w);
        g_w = cursor;
    }
    /* bring the lead back down once ticks have been punctual for a while */
    if (!g_forgive && gap > 0 && gap <= (int32_t)(JITTER_MS * 1000u) && (uint32_t)gap > g_win_gap) g_win_gap = (uint32_t)gap;
    if (g_lead_ms <= LEAD_MS) { g_bump_gap = 0; g_win_t0 = now; g_win_gap = 0; }
    else if (since(now, g_win_t0) > (int32_t)(g_win_ms * 1000u)) {
        if (g_win_gap + DECAY_ROOM_MS * 1000u < g_bump_gap || !g_bump_gap) {
            g_lead_ms = g_lead_ms > LEAD_MS + DECAY_STEP_MS ? g_lead_ms - DECAY_STEP_MS : LEAD_MS;
            g_win_ms = DECAY_NEXT_MS;
        }
        g_win_t0 = now; g_win_gap = 0;
    }
    g_rewound = 0; g_forgive = 0;

    uint32_t lead = us_to_frames((g_cover_ms > g_lead_ms ? g_cover_ms : g_lead_ms) * 1000u);
    uint32_t target = cursor + lead;
    /* keep a block chained past the write point so the voice always has a successor */
    while (g_end <= target + lead)
        if (!queue_block(now)) break;
    if (target > g_end) target = g_end;
    render_to(target);
    handover_fade();
    handover_start(now, cursor);

    g_t_out = now + frames_to_us(g_w - cursor);
    g_last_tick = now;
    if (g_fetch_ok) {
        int32_t lead = (int32_t)(g_w - g_fetch);
        if (lead < g_fetch_min_lead) g_fetch_min_lead = lead;
        if (lead <= 0) g_fetch_behind++;
        int32_t est = (int32_t)(cursor - g_fetch);
        if (est > g_fetch_max_est) g_fetch_max_est = est;
    }
}

/* Where the mixer has read up to in the sounding block, read off the voice now. */
static int fetch_now(uint32_t *f)
{
    if (!g_running || !g_qn) return 0;
    slot_t *h = &g_slot[g_q[0]];
    if (!playing(h)) return 0;
    uint32_t v = U32(h->desc, D_VOICE);
    if (!v || (v & 3u)) return 0;
    uint32_t rem = *(volatile uint32_t *)((uint8_t *)U32_TO_PTR(v) + V_REMAIN);
    if (rem > h->frames) return 0;
    *f = h->start + h->frames - rem;
    return 1;
}

/* A control changed (cue, play, a loop jump): take back the audio that has been
 * rendered but not yet read by the mixer, so the next ring_tick() renders it again
 * with the new state. Without this a change is only heard a whole lead later.
 * Returns the frames taken back: the caller moves its source back by as many. */
uint32_t ring_rewind(void)
{
    if (!g_ready || g_stopping || !g_running || !g_qn || g_ho) return 0;
    uint32_t now = OS_NOW_US();
    int32_t el = since(now, g_head_t);
    uint32_t cursor = g_slot[g_q[0]].start + us_to_frames((uint32_t)(el > 0 ? el : 0));
    uint32_t safe = cursor + REWIND_EST, f;
    if (fetch_now(&f) && f + FETCH_SANE >= cursor && f <= cursor + FETCH_SANE) { safe = f + REWIND_FETCH; g_rw_fetch++; }
    if (safe >= g_w) return 0;
    uint32_t n = g_w - safe;
    g_w = safe;
    g_rewound = 1; g_rewinds++; g_rw_frames += n;
    return n;
}

/* The caller is about to block for up to `ms` (a file write): render that far
 * ahead now, so the voice has audio to play through it, and don't let a late tick
 * straight after it make the lead grow (it says nothing about the usual cadence).
 * The extra audio is simply played down over the following ticks. */
void ring_cover(uint32_t ms)
{
    g_cover_ms = ms;
    ring_tick();
    g_cover_ms = 0;
    g_forgive = 1;
}

/* NC_FETCH: v1 = odd fetch steps | written-behind-fetch ticks << 16, v2 = last odd step
 * NC_FLEAD: v1 = min frames written ahead of the mixer, v2 = max (estimate - fetch) frames */
void ring_fetch_report(void (*log)(const char *tag, uint32_t a, uint32_t b))
{
    log("NC_FETCH", (g_fetch_odd & 0xffffu) | (g_fetch_behind << 16), g_fetch_odd_delta);
    log("NC_FLEAD", (uint32_t)g_fetch_min_lead, (uint32_t)g_fetch_max_est);
    log("NC_FSAMP", g_fetch_samples, g_fetch);
    g_fetch_min_lead = 0x7fffffff; g_fetch_max_est = -0x7fffffff;
}

/* Resident hook (its own task, no FPU): stop feeding, cut the chain, silence what
 * is queued, and report safe only once no voice still holds one of our descriptors. */
int ring_prepare_unload(void)
{
    g_stopping = 1;
    for (uint32_t i = 0; i < SLOTS; i++) {
        chain(&g_slot[i], NULL);
        if (g_slot[i].pcm) zero(&g_slot[i]);
    }
    for (uint32_t i = 0; i < SLOTS; i++) if (playing(&g_slot[i])) return 0;
    g_qn = 0;
    return 1;   /* buffers are deliberately kept: freeing one a voice might still read is worse */
}

/* After ring_prepare_unload() has returned 1: give the PCM buffers back to the OS
 * (no voice holds them any more) and forget everything. Integer-only. */
void ring_release(void)
{
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (g_slot[i].pcm && !playing(&g_slot[i])) {
#ifndef RING_HOST_TEST
            hb_os_free(g_slot[i].pcm);
#else
            free(g_slot[i].pcm);
#endif
            g_slot[i].pcm = NULL;
        }
        g_slot[i].queued = g_slot[i].seen = 0;
    }
    g_qn = 0; g_ready = 0; g_running = 0;
}

void ring_set_budget(uint32_t starts) { g_budget = starts; }

void ring_stats(ring_stats_t *s)
{
    s->starts = g_starts; s->budget = g_budget;
    s->out_rate = g_out_rate; s->block_frames = g_block; s->queued = g_qn;
    s->restarts = g_restarts; s->lead_ms = g_lead_ms; s->late = g_late;
    s->voice_joins = g_voice_joins; s->joins_total = g_joins_total; s->handovers = g_handovers;
    s->skipped_ms = (uint32_t)(((uint64_t)g_skipped_frames * 10u) / 441u);
    s->duration_ok = !g_duration_bad && duration_ms(g_block) != 0;
    s->ctr_div = g_duration_bad ? 0 : g_ctr_div;
    s->cal_ratio = g_cal_ratio; s->cal_steps = g_cal_steps;
    s->rewinds = g_rewinds; s->rewinds_fetch = g_rw_fetch;
    s->rewind_ms = g_rewinds ? (uint32_t)(((uint64_t)g_rw_frames * 10u) / 441u) / g_rewinds : 0;
}

/* Diagnostics: log where everything is, then 64 words of the live voice, so the
 * voice's real read position can be found by comparing dumps from two ticks. */
void ring_debug_dump(void (*log)(const char *tag, uint32_t a, uint32_t b))
{
    if (!g_running || !g_qn) { log("NC_DNONE", g_running, g_qn); return; }
    uint32_t now = OS_NOW_US();
    slot_t *h = &g_slot[g_q[0]];
    uint32_t in = us_to_frames((uint32_t)(since(now, g_head_t) > 0 ? since(now, g_head_t) : 0));
    log("NC_DNOW", now, g_head_t);
    log("NC_DCUR", h->start + in, g_w);
    for (uint32_t i = 0; i < g_qn; i++) {
        slot_t *s = &g_slot[g_q[i]];
        log("NC_DSLOT", PTR_TO_U32(s->pcm), s->start | ((uint32_t)playing(s) << 31));
    }
    uint32_t v = U32(h->desc, D_VOICE);
    log("NC_DVOICE", v, PTR_TO_U32(h->desc));
    if (!v || (v & 3u)) return;
    for (uint32_t off = 0; off < 0x100u; off += 4u)
        log("NC_V", off, *(volatile uint32_t *)((uint8_t *)U32_TO_PTR(v) + off));
}
