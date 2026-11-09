/* UI tests: drives app.c with scripted touches and a fake clock. Run: make test-app */
#include "../src/app.c"      /* include directly to inspect the static UI state */
#include <stdio.h>

/* ---- fake HAL ---- */
static hal_event evq[64]; static int evh, evt;
static uint32_t now_ms = 1000;
int hal_poll(hal_event *e) { if (evh == evt) return 0; *e = evq[evh++]; return 1; }
uint32_t hal_ticks_ms(void) { return now_ms; }
int hal_audio_start(hal_audio_cb cb, void *u) { (void)cb; (void)u; return 0; }
void hal_audio_lock(void) {}
void hal_audio_unlock(void) {}
void *hal_read_file(const char *p, size_t *s) { (void)p; (void)s; return NULL; }
int hal_decode_audio(const char *p, hal_pcm *o) { (void)p; o->frames = AUDIO_RATE * 60; o->data = calloc((size_t)o->frames * 2, sizeof(float)); return 0; }
void hal_free_pcm(hal_pcm *p) { free(p->data); p->data = NULL; }
#include "hal_files_host.h"

static void ev(hal_ev_type type, int x, int y) { hal_event e = { 0 }; e.type = type; e.x = x; e.y = y; evq[evt++] = e; }
static void tap(int x, int y) { ev(EV_TOUCH_DOWN, x, y); ev(EV_TOUCH_UP, x, y); app_tick(); }
static void drag(int x, int y0, int y1) { ev(EV_TOUCH_DOWN, x, y0); ev(EV_TOUCH_MOVE, x, (y0 + y1) / 2); ev(EV_TOUCH_MOVE, x, y1); ev(EV_TOUCH_UP, x, y1); app_tick(); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    library L = { 0 };
    snprintf(L.source, sizeof L.source, "TEST USB");
    /* playlist order: 1,2,3. titles/artists/BPM chosen so every sort gives a different order */
    static const char *titles[] = { "Track 1", "acelera", "Zebra" };
    static const char *artists[] = { "Nicola Cruz", "", "KiNK" };
    static const uint32_t bpms[] = { 13300, 0, 12300 };
    for (int i = 0; i < 3; i++) {
        lib_track *t = lib_add_track(&L);
        t->id = (uint32_t)(i + 1); t->bpm100 = bpms[i];
        snprintf(t->title, sizeof t->title, "%s", titles[i]);
        snprintf(t->artist, sizeof t->artist, "%s", artists[i]);
        snprintf(t->path, sizeof t->path, "/x/%d.mp3", i + 1);
    }
    lib_playlist *p = lib_add_playlist(&L); p->id = 7; snprintf(p->name, sizeof p->name, "Warm-up");
    for (int i = 0; i < 3; i++) lib_add_entry(&L, 7, (uint32_t)(i + 1), (uint32_t)i);
    lib_sort_entries(&L);

    app_init(&L);
    app_tick();
    CHECK(ui.nitems == 2, "root items %d", ui.nitems);               /* ALL TRACKS + Warm-up */

    /* browser rows are 48 px under a 40 px header: row 1 = Warm-up */
    tap(120, BR_HDR_H + ROW_H * 1 + 20);
    CHECK(ui.in_tracks && ui.playlist == 7 && ui.nitems == 3, "open playlist");

    /* sorting: segment centres in the sort bar under the header */
    int sy = BR_HDR_H + SORT_H / 2, sx[SORT_COUNT], acc = 0;
    for (int m = 0; m < SORT_COUNT; m++) { sx[m] = acc + sort_w[m] / 2; acc += sort_w[m]; }
#define ORDER_IS(a, b, c, what) CHECK(ui.items[0].id == a && ui.items[1].id == b && ui.items[2].id == c, \
        "%s: got %u %u %u", what, ui.items[0].id, ui.items[1].id, ui.items[2].id)
    ORDER_IS(1, 2, 3, "playlist order");
    tap(sx[SORT_BPM], sy);    ORDER_IS(3, 1, 2, "bpm ascending, missing BPM last");
    tap(sx[SORT_BPM], sy);    ORDER_IS(1, 3, 2, "bpm descending, missing BPM still last");
    tap(sx[SORT_ARTIST], sy); ORDER_IS(3, 1, 2, "artist A-Z, case-insensitive, no artist last");
    tap(sx[SORT_ARTIST], sy); ORDER_IS(1, 3, 2, "artist Z-A, no artist still last");
    tap(sx[SORT_NAME], sy);   ORDER_IS(2, 1, 3, "name A-Z, case-insensitive");
    tap(sx[SORT_NAME], sy);   ORDER_IS(3, 1, 2, "name Z-A");
    /* the sort sticks when leaving and re-entering the playlist */
    tap(20, 10); tap(120, BR_HDR_H + ROW_H * 1 + 20);
    ORDER_IS(3, 1, 2, "sort kept across playlists");
    /* a drag across the sort bar doesn't change the sort */
    ev(EV_TOUCH_DOWN, sx[SORT_ORDER], sy); ev(EV_TOUCH_MOVE, sx[SORT_ORDER] + 30, sy); ev(EV_TOUCH_UP, sx[SORT_ORDER] + 30, sy); app_tick();
    CHECK(ui.sort_mode == SORT_NAME, "drag on sort bar ignored");
    tap(sx[SORT_ORDER], sy);  ORDER_IS(1, 2, 3, "back to playlist order");

    /* rows sit below the sort bar: bottom of row 2 = track 3 */
    tap(120, list_top() + ROW_H * 2 + ROW_H - 2);
    CHECK(ui.scr == SCR_DECK && ui.cur && ui.cur->id == 3, "load track 3 (cur=%d)", ui.cur ? (int)ui.cur->id : -1);

    /* the header's right arrow returns to the deck from the browser */
    tap(20, 10);                                                       /* deck header "<" -> browser */
    CHECK(ui.scr == SCR_BROWSE, "back to browser");
    tap(SCREEN_W - 10, 20);
    CHECK(ui.scr == SCR_DECK, "right arrow -> deck");

    int fx = SL_X + SL_W / 2, fy = SL_TRACK_Y + SL_TRACK_H / 2;

    /* dragging the fader changes pitch (down = faster) */
    drag(fx, fy, fy + 40);
    CHECK(ui.deck.pitch > 0.03, "drag down raises pitch: %.4f", ui.deck.pitch);
    double after_drag = ui.deck.pitch;

    /* a drag followed by one tap must not reset */
    now_ms += 100; tap(fx, fy);
    CHECK(ui.deck.pitch == after_drag, "single tap keeps pitch");

    /* slow double tap (500 ms apart) must not reset */
    now_ms += 500; tap(fx, fy);
    CHECK(ui.deck.pitch == after_drag, "slow double tap keeps pitch");

    /* real double tap resets to 0 */
    now_ms += 200; tap(fx, fy);
    CHECK(ui.deck.pitch == 0, "double tap resets: %.4f", ui.deck.pitch);

    /* the touch that reset doesn't then drag the fader */
    deck_set_pitch(&ui.deck, 0.05);
    now_ms += 1000; tap(fx, fy);
    now_ms += 150; ev(EV_TOUCH_DOWN, fx, fy); ev(EV_TOUCH_MOVE, fx, fy + 30); ev(EV_TOUCH_UP, fx, fy + 30); app_tick();
    CHECK(ui.deck.pitch == 0, "reset touch doesn't drag: %.4f", ui.deck.pitch);

    /* a third quick tap after a reset doesn't count as another double tap start */
    deck_set_pitch(&ui.deck, 0.02);
    now_ms += 100; tap(fx, fy);
    CHECK(ui.deck.pitch == 0.02, "tap right after a reset is a fresh first tap");

    /* pitch nudge buttons: a tap is one fine step; holding repeats after 400 ms */
    {
        deck_set_pitch(&ui.deck, 0);
        int px = SL_X + SL_W / 2, py = SL_TRACK_Y + SL_TRACK_H + 4 + SL_FINE_H / 2;   /* "+" */
        now_ms += 1000; tap(px, py);
        CHECK(fabs(ui.deck.pitch - 0.0002) < 1e-9, "tap + is one 0.02%% step: %.5f", ui.deck.pitch);
        ev(EV_TOUCH_DOWN, px, py); app_tick();
        for (int i = 0; i < 60; i++) { now_ms += 16; app_tick(); }                 /* hold ~1 s */
        ev(EV_TOUCH_UP, px, py); app_tick();
        int steps = (int)lround(ui.deck.pitch / 0.0002);
        CHECK(steps >= 10 && steps <= 13, "holding + for 1 s repeats (%d steps)", steps);
        double held = ui.deck.pitch;
        for (int i = 0; i < 30; i++) { now_ms += 16; app_tick(); }
        CHECK(ui.deck.pitch == held, "repeat stops on release");
        int mx = px, my = SL_Y + SL_FINE_H / 2;                                     /* "-" */
        ev(EV_TOUCH_DOWN, mx, my); app_tick();
        for (int i = 0; i < 60; i++) { now_ms += 16; app_tick(); }
        ev(EV_TOUCH_UP, mx, my); app_tick();
        CHECK(ui.deck.pitch < held, "holding - lowers pitch");
    }

    /* the side buttons do nothing in nanoCDJ (the iPod's system owns them) */
    {
        double p0 = ui.deck.pitch; int playing = ui.deck.playing;
        hal_event e = { 0 }; e.type = EV_BUTTON_DOWN;
        e.button = BTN_VOLUP; evq[evt++] = e; e.button = BTN_VOLDOWN; evq[evt++] = e; e.button = BTN_PLAYPAUSE; evq[evt++] = e;
        app_tick();
        CHECK(ui.deck.pitch == p0 && ui.deck.playing == playing, "volume and play/pause buttons ignored");
    }

    printf(fails ? "%d FAILED\n" : "all app tests passed\n", fails);
    return fails != 0;
}
