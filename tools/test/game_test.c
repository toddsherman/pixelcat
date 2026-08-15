// Host-side gameplay tests: drives the real cat engine with synthetic
// touches and asserts the Phase 2 interaction layer — HUD tap zones, the
// feed and play sessions, poop cleaning, and the retired tap-jump.
//
// Build and run: tools/test/run.sh

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cat.h"
#include "config.h"

// Display shim (same shape as the preview's).
#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)
static uint16_t s_bands[2][BAND_PIXELS];
uint16_t *display_acquire_band(void)
{
    static unsigned next;
    return s_bands[next++ & 1];
}
int display_flush_band(int band_index, const uint16_t *buffer)
{
    (void)band_index;
    (void)buffer;
    return 0;
}

static int s_failures;

static void expect(int cond, const char *what)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        s_failures++;
    }
}

static const float DT = 1.0f / CAT_FPS;

static void idle_frames(int n)
{
    cat_touch_t t = {0};
    for (int i = 0; i < n; i++) {
        cat_update(DT, &t, 0.0f, 0.0f);
    }
}

// One clean tap: press a frame, release, settle a beat.
static void tap(int lx, int ly)
{
    cat_touch_t t = {.down = true, .x = (int16_t)lx, .y = (int16_t)ly};
    cat_update(DT, &t, 0.0f, 0.0f);
    cat_update(DT, &t, 0.0f, 0.0f);
    t.down = false;
    for (int i = 0; i < 4; i++) {
        cat_update(DT, &t, 0.0f, 0.0f);
    }
}

static void drain_events(void)
{
    cat_take_chirp();
    cat_take_hiss();
    cat_take_step();
    cat_take_boing();
    cat_take_slurp();
    cat_take_swipe();
    cat_take_dash();
    cat_take_eat();
    cat_take_play_hit();
    cat_take_poop_clean();
    cat_take_walked();
}

// Canvas cell -> logical px, centre of the cell.
#define CELL(c) ((c) * PIX_SCALE + PIX_SCALE / 2)

int main(void)
{
    // --- Feed: fish icon tap spawns the bowl; he walks over and eats ---
    cat_init();
    cat_set_stats(35, 62, 80, 20);
    drain_events();
    tap(CELL(13), CELL(4));  // fish icon zone
    int ate = 0, slurps = 0;
    for (int i = 0; i < (int)(14.0f / DT); i++) {
        idle_frames(1);
        ate += cat_take_eat() ? 1 : 0;
        slurps += cat_take_slurp() ? 1 : 0;
    }
    expect(ate == 1, "fish tap feeds him within 14 s");
    expect(slurps >= 3, "eating slurps");

    // --- Play: ball icon tap starts a session with bats or pounces ---
    cat_init();
    cat_set_stats(80, 62, 80, 20);
    drain_events();
    tap(CELL(4), CELL(4));  // yarn ball icon zone
    int hits = 0;
    for (int i = 0; i < (int)(20.0f / DT); i++) {
        idle_frames(1);
        hits += cat_take_play_hit() ? 1 : 0;
    }
    expect(hits >= 2, "play session scores hits");
    expect(cat_state() == CAT_IDLE, "session over, back to idle");

    // --- Poop: tap cleans it ---
    cat_init();
    drain_events();
    cat_debug_force(52);  // poop 5 cells left of him
    expect(cat_poop_count() == 1, "debug poop spawned");
    tap(CELL(14), CELL(40));
    expect(cat_take_poop_clean(), "tap on the poop cleans it");
    expect(cat_poop_count() == 0, "count back to zero");

    // --- Status page: hearts tap opens, next tap closes ---
    cat_init();
    drain_events();
    tap(CELL(25), CELL(4));  // hearts zone
    // The status screen swallows behaviour: a side tap must not leap.
    tap(CELL(50), CELL(25));
    expect(cat_take_dash() == 0, "status page swallows side taps");

    // --- Retired: a tap on the cat is a glance, never a jump ---
    cat_init();
    drain_events();
    tap(CELL(28), CELL(38));
    int boing_after_tap = cat_take_boing() ? 1 : 0;
    idle_frames(10);
    boing_after_tap += cat_take_boing() ? 1 : 0;
    expect(boing_after_tap == 0, "tap on the cat no longer jumps");

    // --- Still alive: double tap on a side still leaps ---
    cat_init();
    drain_events();
    tap(CELL(50), CELL(25));
    tap(CELL(50), CELL(25));
    int dashed = 0;
    for (int i = 0; i < 10; i++) {
        idle_frames(1);
        dashed += (cat_take_dash() != 0) ? 1 : 0;
    }
    expect(dashed >= 1, "double side tap still leaps");

    // --- Busy: tilt cannot pull him off a fresh bowl ---
    cat_init();
    drain_events();
    cat_debug_force(50);  // bowl drop
    for (int i = 0; i < (int)(3.0f / DT); i++) {
        cat_touch_t t = {0};
        cat_update(DT, &t, 0.0f, 6.0f);  // hard right tilt, walk band+
    }
    const float walked = cat_take_walked();
    expect(walked < 30.0f * PIX_SCALE,
           "tilt ignored while dinner is on (goal walk only)");

    printf(s_failures ? "\n%d FAILURES\n" : "\nall tests pass\n", s_failures);
    return s_failures ? 1 : 0;
}
