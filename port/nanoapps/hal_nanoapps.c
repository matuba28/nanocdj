/* hal.h for the iPod nano 7G running NanoApps (raw-surface app).
 *
 * The NanoApps runtime calls hb_raw_init() once and hb_raw_frame() ~60x per second,
 * then blits the framebuffer itself. We turn its single-pointer touch state and
 * polled buttons into hal events and run one app_tick() per frame.
 *
 * Audio goes out through audio_ring.c (chained OS sound-effect buffers, written just
 * ahead of the play cursor); if the ring can't start, the deck is clocked silently.
 * Tracks stream through hal_file_* (one OS file object, read forward), and the
 * unload hook gives every heap block back when NanoApps unloads the app.
 * See docs/TECHNICAL.md. */
#include "compat.h"
#include <string.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_shared.h"
#include "hb_raw_surface.h"
#include "../../src/hal.h"
#include "../../src/gfx.h"
#include "../../src/library.h"
#include "../../src/app.h"
#include "../../src/deck.h"
#include "audio_ring.h"

/* ---------------- events ---------------- */

#define QN 32
static hal_event q[QN];
static int qh, qt;

static void push(hal_event e)
{
    int n = (qt + 1) % QN;
    if (n == qh) return;            /* full: drop */
    q[qt] = e; qt = n;
}

int hal_poll(hal_event *ev)
{
    if (qh == qt) return 0;
    *ev = q[qh]; qh = (qh + 1) % QN;
    return 1;
}

static int was_down;

static void feed_touch(const hb_spoint_t *p)
{
    hal_event e = { 0 };
    e.id = 0; e.x = p->x; e.y = p->y;
    if (p->down && !was_down) { e.type = EV_TOUCH_DOWN; push(e); }
    else if (p->down) { e.type = EV_TOUCH_MOVE; push(e); }
    else if (was_down) { e.type = EV_TOUCH_UP; push(e); }
    was_down = p->down;
}

static const struct { hb_button_t hw; int ours; } buttons[] = {
    { HB_BTN_PLAY_PAUSE, BTN_PLAYPAUSE },
    { HB_BTN_VOL_UP, BTN_VOLUP },
    { HB_BTN_VOL_DOWN, BTN_VOLDOWN },
    { HB_BTN_HOME, BTN_HOME },
};
static int btn_state[4];

static void feed_buttons(void)
{
    for (int i = 0; i < 4; i++) {
        int now = hb_button_pressed(buttons[i].hw) ? 1 : 0;
        if (now != btn_state[i]) {
            hal_event e = { 0 };
            e.type = now ? EV_BUTTON_DOWN : EV_BUTTON_UP;
            e.button = buttons[i].ours;
            push(e);
            btn_state[i] = now;
        }
    }
}

/* ---------------- video / time ---------------- */

uint32_t hal_ticks_ms(void) { return hb_time_uptime_ms(); }   /* (the runtime blits fb after each frame) */

/* ---------------- audio ----------------
 * Real output goes through audio_ring.c (chained OS sound-effect buffers). If the
 * ring can't start, fall back to a silent clock so the deck still moves. */

static hal_audio_cb acb;
static void *auser;
static int ring_ok;
static uint32_t last_us;
static double frac_frames;
static float scratch[2 * 256];

int hal_audio_start(hal_audio_cb cb, void *user)
{
    acb = cb; auser = user;
    last_us = hb_time_uptime_us();
    return 0;
}
void hal_audio_lock(void) {}     /* single-threaded: audio is rendered from the frame */
void hal_audio_unlock(void) {}

/* ring callback: render float through the deck, convert to int16 */
static void ring_render(int16_t *out, uint32_t frames, void *user)
{
    (void)user;
    while (frames) {
        uint32_t n = frames > 256 ? 256 : frames;
        acb(scratch, (int)n, auser);
        for (uint32_t i = 0; i < 2 * n; i++) {
            float v = scratch[i] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[i] = (int16_t)v;
        }
        out += 2 * n; frames -= n;
    }
}

/* ring callback: the output missed this much time (stall / screen lock) */
static void ring_skip(uint32_t frames, void *user) { (void)user; app_audio_skip(frames); }

/* ---------------- block-start budget ----------------
 * RetailOS's sound-effect player allows only ~1,300 block starts per boot, across
 * every app session, and the iPod hard-resets past that (see audio_ring.c). The
 * starts used this boot are saved to a small file, so reopening nanoCDJ carries the
 * count on, and the ring stops the audio at BOOT_START_LIMIT instead of crashing.
 * A boot is recognised by its start time: the real-time clock minus the 64-bit
 * hardware uptime counter (the SDK's ms clock restarts with each app). */
#define BOOT_START_LIMIT 1200u                 /* crashes seen at ~1,230-1,300 */
#define STATE_DIR  "/Data/nanoCDJ"
#define STATE_PATH "/Data/nanoCDJ/boot.bin"
#define STATE_MAGIC 0x4e434253u                /* 'NCBS' */
typedef struct { uint32_t magic; int32_t boot_s; uint32_t starts; } boot_state_t;
static int32_t  boot_s;                        /* this boot's start, s since 2000 */
static uint32_t prev_starts, saved_starts;     /* used by earlier sessions this boot / last saved total */

static uint64_t hw_uptime_us(void)             /* the SoC's free-running us counter, 64-bit */
{
    volatile uint32_t *hi = (volatile uint32_t *)0x3c700080u, *lo = (volatile uint32_t *)0x3c700084u;
    uint32_t h1, l, h2;
    do { h1 = *hi; l = *lo; h2 = *hi; } while (h1 != h2);
    return ((uint64_t)h1 << 32) | l;
}

static int32_t days_since_2000(int y, int m, int d)
{
    y -= m <= 2; int era = y / 400, yoe = y - era * 400;                       /* civil-from-days, inverted */
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 730425;                                       /* 2000-03-01 based -> 2000-01-01 */
}

static void budget_init(void)
{
    hb_rtc_time_t t; hb_rtc_read(&t);
    int32_t now_s = days_since_2000(t.year, t.month, t.day_of_month) * 86400 + t.hours * 3600 + t.minutes * 60 + t.seconds;
    boot_s = now_s - (int32_t)(hw_uptime_us() / 1000000u);
    boot_state_t st;
    prev_starts = 0;
    if (hb_fs_read(STATE_PATH, &st, sizeof st) == sizeof st && st.magic == STATE_MAGIC &&
        st.boot_s - boot_s < 30 && boot_s - st.boot_s < 30)                   /* same boot */
        prev_starts = st.starts;
    saved_starts = prev_starts;
    ring_set_budget(prev_starts < BOOT_START_LIMIT ? BOOT_START_LIMIT - prev_starts : 0);
    hb_trace_log("NC_BOOT", prev_starts, (uint32_t)boot_s);
}

/* Writing the file blocks for a while (it syncs to flash). So: while the deck is
 * silent it is saved within a few seconds of changing; while it plays, every 30 s,
 * with enough audio rendered ahead first (ring_cover) that the write isn't heard
 * and doesn't count as a late frame. */
#define SAVE_COVER_MS 400u
static void budget_save(void)
{
    static uint32_t last; static int dir;
    uint32_t now = hb_time_uptime_ms();
    int silent = app_deck_silent();
    if (now - last < (silent ? 5000u : 30000u)) return;
    last = now;
    ring_stats_t r; ring_stats(&r);
    uint32_t total = prev_starts + r.starts;
    if (total == saved_starts) return;
    ring_cover(silent ? 0 : SAVE_COVER_MS);
    if (!dir) { hb_fs_mkdir(STATE_DIR); dir = 1; }
    boot_state_t st = { STATE_MAGIC, boot_s, total };
    if (hb_fs_write(STATE_PATH, &st, sizeof st)) saved_starts = total;
}

/* seconds of playback left before the iPod needs a restart; -1 if not known */
int hal_audio_time_left_s(void)
{
    if (!ring_ok) return -1;
    ring_stats_t r; ring_stats(&r);
    uint32_t left = r.budget > r.starts ? r.budget - r.starts : 0;
    return (int)((uint64_t)left * r.block_frames / 44100u);
}

/* What the audio ring takes when it can have its longest blocks: two 6 s buffers
 * (2 x 1.06 MB) and the 256 KB it insists on leaving the OS. */
#define RING_WANTS (2u * 441u * 6000u / 10u * 4u + 256u * 1024u)

static void start_audio_output(void)
{
    /* The track stream's buffers first, kept for the whole session. Before this,
     * the ring took its 2 MB and each track load then looked for ~1.5 MB more in a
     * heap that had been allocated and freed around for a while: when it wasn't
     * there in one piece, the track didn't stream. Reserved here, the stream leaves
     * the ring its full size when memory allows, and always gets its minimum. */
    int reserved = mp3s_reserve(RING_WANTS);
    hb_trace_log("NC_MEM", (uint32_t)reserved, hb_os_heap_largest());   /* stream buffers reserved / largest block left */
    ring_ok = acb && ring_init(ring_render, ring_skip, NULL) == 0;
    if (ring_ok) budget_init();
    hb_trace_log("NC_AUDIO", (uint32_t)ring_ok, hb_os_heap_largest());
}

/* Diagnostics, read back with tools/trace.py (docs/TECHNICAL.md has the table). Every 5 s:
 * NC_RING v1 = restarts | late ticks << 16, v2 = mixer rate | lead ms << 20
 * NC_HEAP v1 = OS heap free, v2 = largest free block (a leak shows as a trend)
 * NC_GAP  v1 = longest gap between frames (us), v2 = frames over 25 ms apart
 * NC_COST v1 = slowest app_tick (us), v2 = slowest ring_tick (us)
 * NC_CAL  v1 = counter divisor (0 = fallback) | measurements << 8, v2 = last block time / length % */
static uint32_t st_gap_max, st_gap_n, st_app_max, st_ring_max, st_last_frame, st_read_max;

static void tlog(const char *t, uint32_t a, uint32_t b) { hb_trace_log(t, a, b); }

static void audio_trace(void)
{
    static uint32_t last;
    uint32_t now = hb_time_uptime_ms();
    if (now - last < 5000u) return;
    last = now;
    if (ring_ok) {
        ring_stats_t st; ring_stats(&st);
        hb_trace_log("NC_RING", st.restarts | (st.late << 16), st.out_rate | (st.lead_ms << 20));
        hb_trace_log("NC_CAL", st.ctr_div | (st.cal_steps << 8), st.cal_ratio);
        /* NC_JOIN v1 = starts this session | earlier sessions this boot << 16, v2 = block frames */
        hb_trace_log("NC_JOIN", st.starts | (prev_starts << 16), st.block_frames);
        /* NC_RWD v1 = rewinds | of which placed from the mixer's read position << 16, v2 = average ms taken back */
        hb_trace_log("NC_RWD", (st.rewinds & 0xffffu) | (st.rewinds_fetch << 16), st.rewind_ms);
    }
    hb_trace_log("NC_HEAP", hb_os_heap_free(), hb_os_heap_largest());
    mp3s_stats_t ms;
    /* NC_NOSTR: the loaded track plays clicks instead of audio. v1 = why
     * (1 not .mp3, 2 file wouldn't open, 3 no memory, 4 no MP3 frames found),
     * v2 = detail (2: path length | non-ASCII bytes << 16, 4: bytes searched) */
    if (deck_stream_error) hb_trace_log("NC_NOSTR", (uint32_t)deck_stream_error, deck_stream_detail);
    if (app_stream_stats(&ms)) {
        static uint32_t last_bytes;
        /* NC_MP3A v1 = KB read in the last 5 s, v2 = decode misses (silence) total
         * NC_MP3B v1 = window KB | reopens << 16, v2 = longest read time in one frame (us) */
        hb_trace_log("NC_MP3A", (ms.bytes_read - last_bytes) / 1024u, ms.misses);
        hb_trace_log("NC_MP3B", (ms.window_bytes / 1024u) | (ms.reopens << 16), st_read_max);
        /* NC_MP3C v1 = measured read speed KB/s, v2 = ms of audio read ahead of the playhead */
        hb_trace_log("NC_MP3C", ms.read_kbps, ms.ahead_ms);
        last_bytes = ms.bytes_read;
        st_read_max = 0;
    }
    hb_trace_log("NC_GAP", st_gap_max, st_gap_n);
    hb_trace_log("NC_COST", st_app_max, st_ring_max);
    st_gap_max = st_gap_n = st_app_max = st_ring_max = 0;
}

/* Render audio now. Twice a frame: straight after input (app_tick calls
 * hal_audio_pump), so a press is rendered before the track is read and the screen
 * drawn, and again after drawing, which halves the longest wait between renders. */
static void pump(void)
{
    if (!ring_ok) return;
    uint32_t t0 = hb_time_uptime_us();
    ring_tick();
    uint32_t dt = hb_time_uptime_us() - t0;
    if (dt > st_ring_max) st_ring_max = dt;
}
void hal_audio_pump(void) { pump(); }
unsigned hal_audio_rewind(void) { return ring_ok ? ring_rewind() : 0; }

static void run_audio(void)
{
    if (!acb) return;
    audio_trace();
    if (ring_ok) { budget_save(); pump(); return; }
    /* fallback: silent clock */
    uint32_t now = hb_time_uptime_us();
    uint32_t dt = now - last_us;
    last_us = now;
    if (dt > 200000) dt = 200000;
    frac_frames += dt * (AUDIO_RATE / 1e6);
    int frames = (int)frac_frames;
    frac_frames -= frames;
    while (frames > 0) {
        int n = frames > 256 ? 256 : frames;
        acb(scratch, n, auser);
        frames -= n;
    }
}

/* ---------------- keep the screen on ----------------
 * A deck must not auto-lock mid-mix. Same idle-clock poke as hb_wake_lock() (which
 * lives in the LVGL surface and isn't available to raw-surface apps): tell the OS's
 * power/idle model there was touch activity, every 10 s. */
#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define KEVENT_TOUCHACTIVITY    4

static void stay_awake(void)
{
    static uint32_t last;
    static int first = 1;
    uint32_t now = hb_time_uptime_ms();
    if (!first && now - last < 10000u) return;
    first = 0; last = now;
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, KEVENT_TOUCHACTIVITY);
}

/* ---------------- files ---------------- */

void *hal_read_file(const char *path, size_t *size)
{
    uint32_t n = hb_fs_size(path);
    if (!n) return NULL;
    void *b = malloc(n);
    if (!b) return NULL;
    if (hb_fs_read_chunked(path, b, n) != n) { free(b); return NULL; }
    if (size) *size = n;
    return b;
}

/* Streaming reads: one OS file object kept open and read forward a chunk at a
 * time (the OS file object has no seek). Same calls and object/cache sizes as the
 * SDK's hb_fs.c, which documents them; the object is generously sized and static
 * because the OS read path writes past a 0x60-byte object. */
typedef void (*fobj_open_fn) (void *o, const char *path, int a2, int a3, uint32_t a4, uint32_t a5, void *a6);
typedef void (*fobj_close_fn)(void *o);
typedef int  (*fobj_ready_fn)(void *o);
typedef int  (*fobj_read_fn) (void *o, uint32_t n, void *buf, uint32_t *got);
#define FOBJ_OPEN  ((fobj_open_fn) (0x084137a8u | 1u))
#define FOBJ_CLOSE ((fobj_close_fn)(0x08423be0u | 1u))
#define FOBJ_READY ((fobj_ready_fn)(0x08417e18u | 1u))
#define FOBJ_READ  ((fobj_read_fn) (0x0841cddcu | 1u))
#define FS_MAIN_VOLUME 0
static uint8_t fobj[0x400] __attribute__((aligned(8)));
static uint8_t fcache[0x1010] __attribute__((aligned(32)));
static int fobj_open;

void *hal_file_open(const char *path)
{
    if (fobj_open) return NULL;                       /* one at a time */
    memset(fcache, 0, sizeof fcache);
    FOBJ_OPEN(fobj, path, 1 /* read */, FS_MAIN_VOLUME, 0x1000, 1, fcache);
    if (!FOBJ_READY(fobj)) { FOBJ_CLOSE(fobj); return NULL; }
    fobj_open = 1;
    return fobj;
}

int32_t hal_file_read(void *f, void *buf, uint32_t n)
{
    if (f != fobj || !fobj_open) return -1;
    uint64_t got64 = 0;                               /* the OS writes a 64-bit count */
    if (FOBJ_READ(fobj, n, buf, (uint32_t *)&got64) != 0) return -1;
    return (int32_t)(uint32_t)got64;
}

void hal_file_close(void *f)
{
    if (f != fobj || !fobj_open) return;
    FOBJ_CLOSE(fobj);
    fobj_open = 0;
}

uint32_t hal_time_us(void) { return hb_time_uptime_us(); }

/* hb_os_alloc reboots the iPod when it fails, so never plan on the last 768 KB */
size_t hal_mem_avail(void)
{
    uint32_t l = hb_os_heap_largest();
    return l > 768u * 1024u ? l - 768u * 1024u : 0;
}

int hal_decode_audio(const char *path, hal_pcm *out)
{
    (void)path;
    out->data = NULL; out->frames = 0;
    return HAL_NO_DECODER;
}

void hal_free_pcm(hal_pcm *p) { free(p->data); p->data = NULL; p->frames = 0; }

/* ---------------- called from entry.c (Thumb glue) once the FPU is attached ---------------- */

static library lib;

void nanocdj_device_fatal(const char *msg)
{
    uint32_t *p = hb_raw_fb();
    for (int i = 0; i < hb_raw_w() * hb_raw_h(); i++) p[i] = 0xff000000u;
    fb = p;
    gfx_text_center(SCREEN_W / 2, 200, "nanoCDJ", RGB(255, 140, 0), 2);
    gfx_text_center(SCREEN_W / 2, 230, msg, RGB(235, 235, 235), 1);
}

static int ok;

/* Resident hook, called before NanoApps unloads the app (on its own task, with no
 * FPU context: everything here is integer-only). Once no voice is still playing our
 * audio, close the stream's file and hand every heap block back: they live in the
 * OS heap, outside the app image, so nothing else would free them. The resident
 * zeroes our static data on the next launch, which then starts from scratch. */
static int nanocdj_prepare_unload(void)
{
    if (ring_ok && !ring_prepare_unload()) return 0;     /* the iPod is still reading our audio */
    ok = 0;                                              /* no more frames from here on */
    if (ring_ok) ring_release();
    ring_ok = 0;
    if (fobj_open) { FOBJ_CLOSE(fobj); fobj_open = 0; }
    uint32_t blocks = libc_free_all();
    hb_trace_log("NC_UNLD", blocks, hb_os_heap_free());
    return 1;
}

void nanocdj_device_init(int w, int h)
{
    if (hb_shared) hb_shared->prepare_unload = nanocdj_prepare_unload;
    fb = hb_raw_fb();
    if (w != SCREEN_W || h != SCREEN_H) { nanocdj_device_fatal("unexpected screen size"); return; }
    memset(&lib, 0, sizeof lib);
    hb_trace_log("NC_PDB", hb_os_heap_free(), hb_os_heap_largest());
    if (lib_load_pdb(&lib, "") != 0)
        snprintf(lib.source, sizeof lib.source, "NO REKORDBOX EXPORT");
    hb_trace_log("NC_LIB", (uint32_t)lib.nt, (uint32_t)lib.np);
    app_init(&lib);
    start_audio_output();
    hb_trace_log("NC_APP", 0, 0);
    ok = 1;
}

void nanocdj_device_frame(const hb_spoint_t *touch)
{
    static int frames;
    if (!ok) return;
    feed_touch(touch);
    feed_buttons();
    uint32_t t0 = hb_time_uptime_us();
    if (st_last_frame) {
        uint32_t gap = t0 - st_last_frame;
        if (gap > st_gap_max) st_gap_max = gap;
        if (gap > 25000u) st_gap_n++;
    }
    st_last_frame = t0;
    app_tick();          /* input + draw */
    uint32_t dt = hb_time_uptime_us() - t0;
    if (dt > st_app_max) st_app_max = dt;
    {
        mp3s_stats_t ms;
        if (app_stream_stats(&ms) && ms.read_us > st_read_max) st_read_max = ms.read_us;
    }
    run_audio();         /* then render audio with the latest control state */
    stay_awake();
    if (++frames == 1) hb_trace_log("NC_FRAME1", 0, 0);
}
