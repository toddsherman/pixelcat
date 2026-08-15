// Host-side gameplay tests: drives the real cat engine with synthetic
// touches and asserts the Phase 2 interaction layer — HUD tap zones, the
// feed and play sessions, poop cleaning, and the retired tap-jump.
//
// Build and run: tools/test/run.sh

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cat.h"
#include "cat_bg.h"
#include "config.h"
#include "stats.h"

// Shortest signed distance in the looping world (the harness must wrap the
// same way the engine does, or a stroll across the seam reads as half the
// world).
static float world_dist(float a, float b)
{
    float d = a - b;
    if (d > (float)BG_WORLD_W * 0.5f) {
        d -= (float)BG_WORLD_W;
    }
    if (d < -(float)BG_WORLD_W * 0.5f) {
        d += (float)BG_WORLD_W;
    }
    return (d < 0.0f) ? -d : d;
}

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

// Boot leaves the park empty; most tests want him present and centred.
static void fresh_present_cat(void)
{
    cat_init();
    cat_debug_force(0);
    drain_events();
}

// Stroke briskly back and forth across the cat until the purr passes the
// target (the reconciliation threshold is 0.55).
static int pet_until_purr(float target, float seconds)
{
    cat_touch_t t = {.down = true};
    float ph = 0.0f;
    for (int i = 0; i < (int)(seconds / DT); i++) {
        ph += DT * 10.0f;
        t.x = (int16_t)(CELL(28) + 60.0f * __builtin_sinf(ph));
        t.y = CELL(38);
        cat_update(DT, &t, 0.0f, 0.0f);
        if (cat_purr_level() > target) {
            return 1;
        }
    }
    return 0;
}

// One hard shake spike, then calm.
static void scare(void)
{
    cat_touch_t t = {0};
    cat_update(DT, &t, 8.0f, 0.0f);
    for (int i = 0; i < (int)(3.5f / DT); i++) {
        cat_update(DT, &t, 0.0f, 0.0f);
    }
}

int main(void)
{
    // --- Feed: fish icon tap spawns the bowl; he walks over and eats ---
    fresh_present_cat();
    cat_set_stats(35, 62, 20, 0);
    tap(CELL(13), CELL(4));  // fish icon zone
    int ate = 0, slurps = 0, post_slurps = 0;
    for (int i = 0; i < (int)(14.0f / DT); i++) {
        idle_frames(1);
        if (cat_take_eat()) {
            ate++;
            // A real cat washes up after dinner: grooming slurps follow.
            for (int j = 0; j < (int)(4.0f / DT); j++) {
                idle_frames(1);
                post_slurps += cat_take_slurp() ? 1 : 0;
            }
            break;
        }
        slurps += cat_take_slurp() ? 1 : 0;
    }
    expect(ate == 1, "fish tap feeds him within 14 s");
    expect(slurps >= 3, "eating slurps");
    expect(post_slurps >= 1, "he washes up after dinner");

    // --- Play: ball icon tap starts a session with bats or pounces ---
    fresh_present_cat();
    cat_set_stats(80, 62, 20, 0);
    tap(CELL(4), CELL(4));  // yarn ball icon zone
    int hits = 0;
    for (int i = 0; i < (int)(20.0f / DT); i++) {
        idle_frames(1);
        hits += cat_take_play_hit() ? 1 : 0;
    }
    expect(hits >= 2, "play session scores hits");
    expect(cat_state() == CAT_IDLE, "session over, back to idle");

    // --- Full gauges gate their taps: no dinner for a fed cat ---
    fresh_present_cat();
    cat_set_stats(100, 62, 20, 0);
    tap(CELL(13), CELL(4));
    int ate_full = 0;
    for (int i = 0; i < (int)(12.0f / DT); i++) {
        idle_frames(1);
        ate_full += cat_take_eat() ? 1 : 0;
    }
    expect(ate_full == 0, "a full fish gauge refuses the tap");

    // --- ...and no new ball once he has had his games ---
    fresh_present_cat();
    cat_set_stats(80, 62, 20, 100);
    tap(CELL(4), CELL(4));
    int hits_full = 0;
    for (int i = 0; i < (int)(8.0f / DT); i++) {
        idle_frames(1);
        hits_full += cat_take_play_hit() ? 1 : 0;
    }
    expect(hits_full == 0, "a full play gauge refuses the ball");

    // --- Poop: tap cleans it ---
    fresh_present_cat();
    cat_debug_force(52);  // poop 5 cells left of him
    expect(cat_poop_count() == 1, "debug poop spawned");
    tap(CELL(14), CELL(40));
    expect(cat_take_poop_clean(), "tap on the poop cleans it");
    expect(cat_poop_count() == 0, "count back to zero");

    // --- Status page: hearts tap opens, next tap closes ---
    fresh_present_cat();
    tap(CELL(25), CELL(4));  // hearts zone
    // The status screen swallows behaviour: a side tap must not leap.
    tap(CELL(50), CELL(25));
    expect(cat_take_dash() == 0, "status page swallows side taps");

    // --- Retired: a tap on the cat is a glance, never a jump ---
    fresh_present_cat();
    tap(CELL(28), CELL(38));
    int boing_after_tap = cat_take_boing() ? 1 : 0;
    idle_frames(10);
    boing_after_tap += cat_take_boing() ? 1 : 0;
    expect(boing_after_tap == 0, "tap on the cat no longer jumps");

    // --- Still alive: double tap on a side still leaps ---
    fresh_present_cat();
    tap(CELL(50), CELL(25));
    tap(CELL(50), CELL(25));
    int dashed = 0;
    for (int i = 0; i < 10; i++) {
        idle_frames(1);
        dashed += (cat_take_dash() != 0) ? 1 : 0;
    }
    expect(dashed >= 1, "double side tap still leaps");

    // --- Busy: tilt cannot pull him off a fresh bowl ---
    fresh_present_cat();
    cat_debug_force(50);  // bowl drop
    for (int i = 0; i < (int)(3.0f / DT); i++) {
        cat_touch_t t = {0};
        cat_update(DT, &t, 0.0f, 6.0f);  // hard right tilt, walk band+
    }
    const float walked = cat_take_walked();
    expect(walked < 30.0f * PIX_SCALE,
           "tilt ignored while dinner is on (goal walk only)");

    // ---------------- Phase 3 ----------------

    // --- Boot: the park is empty; a sound summons him in ---
    cat_init();
    drain_events();
    expect(cat_state() == CAT_ABSENT, "boot starts absent");
    cat_hear_sound();
    expect(cat_take_summon(), "sound summons: event fires");
    int steps = 0;
    for (int i = 0; i < (int)(8.0f / DT) && cat_state() == CAT_ABSENT; i++) {
        idle_frames(1);
        steps += cat_take_step() ? 1 : 0;
    }
    for (int i = 0; i < (int)(8.0f / DT) && cat_state() != CAT_IDLE; i++) {
        idle_frames(1);
        steps += cat_take_step() ? 1 : 0;
    }
    expect(cat_state() == CAT_IDLE, "he trots in and settles");
    expect(steps >= 4, "entrance has footsteps");

    // --- Left alone, he wanders in on his own; no grudge ---
    cat_init();
    drain_events();
    for (int i = 0; i < (int)(70.0f / DT) && cat_state() != CAT_IDLE; i++) {
        idle_frames(1);
    }
    expect(cat_state() == CAT_IDLE, "he comes back on his own within ~a minute");

    // --- Scare: hiss, panicked flee, hidden; tilt inert; sound ignored ---
    fresh_present_cat();
    scare();
    expect(cat_take_hiss(), "shake hisses");
    expect(cat_state() == CAT_HIDING, "after the hiss he flees and hides");
    expect(cat_scare_level() == 1, "one scare on the books");
    cat_take_walked();
    for (int i = 0; i < (int)(2.0f / DT); i++) {
        cat_touch_t t = {0};
        cat_update(DT, &t, 0.0f, 6.0f);  // hard tilt
    }
    expect(cat_take_walked() == 0.0f, "tilt is inert while he hides");
    cat_hear_sound();
    expect(cat_state() == CAT_HIDING, "sound does not summon a scared cat");

    // --- Re-scare while hidden escalates ---
    {
        cat_touch_t t = {0};
        cat_update(DT, &t, 8.0f, 0.0f);
        idle_frames(5);
    }
    expect(cat_scare_level() == 2, "scaring a hiding cat escalates");

    // --- Search: swipe pans the camera; a tap on him brings him out wary ---
    fresh_present_cat();
    scare();
    drain_events();
    // Pan with long drags until he is on screen (near the centre), then tap.
    int found = 0;
    for (int swipes = 0; swipes < 60 && !found; swipes++) {
        // Drag high across the sky, well clear of the cat's petting box.
        cat_touch_t t = {.down = true, .x = CELL(46), .y = CELL(10)};
        cat_update(DT, &t, 0.0f, 0.0f);
        for (int i = 0; i < 10; i++) {
            t.x -= (int16_t)(4 * PIX_SCALE);
            cat_update(DT, &t, 0.0f, 0.0f);
        }
        t.down = false;
        cat_update(DT, &t, 0.0f, 0.0f);
        // Probe: does a tap at centre reach him now?
        tap(CELL(28), CELL(38));
        for (int i = 0; i < 5; i++) {
            idle_frames(1);
        }
        if (cat_state() == CAT_IDLE) {
            found = 1;
        }
    }
    expect(found, "swipe search finds him; a tap brings him back");
    expect(cat_wary(), "he came out wary, not forgiven");
    expect(cat_scare_level() == 1, "a plain tap does not reset the score");

    // --- Reconciliation: pet to a real purr; the slate wipes clean ---
    expect(pet_until_purr(0.55f, 10.0f), "petting earns a purr");
    idle_frames(5);
    expect(cat_take_reconcile(), "the purr makes peace");
    expect(!cat_wary() && cat_scare_level() == 0, "fear resets to baseline");

    // ---------------- Phase 4 ----------------

    // --- Affection shows in the purr ---
    fresh_present_cat();
    cat_set_stats(80, 100, 50, 0);
    pet_until_purr(2.0f, 5.0f);  // never reached: just pet for 5 s
    const float purr_devoted = cat_purr_level();
    fresh_present_cat();
    cat_set_stats(80, 0, 50, 0);
    pet_until_purr(2.0f, 5.0f);
    const float purr_aloof = cat_purr_level();
    expect(purr_devoted > purr_aloof + 0.1f,
           "a devoted cat purrs harder than an aloof one");

    // --- A full exercise bar shows in his pace: pleasantly worn out ---
    int fresh_frames = 0;
    {
        fresh_present_cat();
        cat_set_stats(80, 60, 0, 0);  // a fresh morning cat
        cat_touch_t t = {0};
        cat_update(DT, &t, 8.0f, 0.0f);
        while (cat_state() != CAT_HIDING && fresh_frames < 500) {
            cat_update(DT, &t, 0.0f, 0.0f);
            fresh_frames++;
        }
    }
    int worn_frames = 0;
    {
        fresh_present_cat();
        cat_set_stats(80, 60, 100, 0);  // his whole day already had
        cat_touch_t t = {0};
        cat_update(DT, &t, 8.0f, 0.0f);
        while (cat_state() != CAT_HIDING && worn_frames < 500) {
            cat_update(DT, &t, 0.0f, 0.0f);
            worn_frames++;
        }
    }
    expect(worn_frames > fresh_frames + 10,
           "a well-exercised cat runs a beat slower than a fresh one");

    // --- ...and in his bedtime: he dozes off sooner once the bar fills ---
    int doze_worn = 0, doze_fresh = 0;
    {
        fresh_present_cat();
        cat_set_stats(80, 60, 100, 0);
        while (cat_state() != CAT_SLEEPING && doze_worn < 4000) {
            idle_frames(1);
            doze_worn++;
        }
        fresh_present_cat();
        cat_set_stats(80, 60, 0, 0);
        while (cat_state() != CAT_SLEEPING && doze_fresh < 4000) {
            idle_frames(1);
            doze_fresh++;
        }
    }
    expect(doze_worn < doze_fresh,
           "a full exercise bar means an earlier nap");

    // --- Hunger pulls him back to where food appears ---
    fresh_present_cat();
    cat_set_stats(80, 60, 50, 0);
    cat_debug_force(50);  // bowl: establishes the food spot
    for (int i = 0; i < (int)(14.0f / DT); i++) {
        idle_frames(1);
    }
    drain_events();
    const float spot = cat_debug_world();
    // Carry him well away on a tilt, then leave a hungry cat to his
    // stomach: the food-seek stroll should bring him back on its own.
    for (int i = 0; i < (int)(3.0f / DT); i++) {
        cat_touch_t t = {0};
        cat_update(DT, &t, 0.0f, 3.0f);
    }
    expect(world_dist(cat_debug_world(), spot) > 150.0f,
           "the tilt carried him away from the spot");
    cat_set_stats(10, 60, 50, 0);
    int returned = 0;
    for (int i = 0; i < (int)(60.0f / DT) && !returned; i++) {
        idle_frames(1);
        if (world_dist(cat_debug_world(), spot) < 80.0f) {
            returned = 1;
        }
    }
    expect(returned, "a hungry cat drifts back to where food appears");

    // --- The heart fills one gauge row (20 pts) per 5 s of petting ---
    stats_reset();
    const float a0 = stats_get()->affection;
    for (int i = 0; i < (int)(5.0f / DT); i++) {
        stats_tick(DT, false, 0.8f);  // purring under a stroke
    }
    const float gained = stats_get()->affection - a0;
    expect(gained > 18.0f && gained < 22.0f,
           "5 s of petting fills one heart row");

    printf(s_failures ? "\n%d FAILURES\n" : "\nall tests pass\n", s_failures);
    return s_failures ? 1 : 0;
}
