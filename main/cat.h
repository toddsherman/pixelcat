#pragma once

#include <stdbool.h>
#include <stdint.h>

// The cat: sprite composition, animation state machine, and band rendering.
// Pure logic and pixels — no RTOS, no hardware — so it also compiles on the
// host for previewing.

typedef enum {
    CAT_IDLE,
    CAT_PETTED,
    CAT_STARTLED,
    CAT_SLEEPING,
    CAT_ABSENT,   // the park is empty; sound or a tap summons him
    CAT_HIDING,   // scared off screen; swipe to search, purr to make peace
} cat_state_t;

typedef struct {
    bool down;
    int16_t x;  // panel pixels
    int16_t y;
} cat_touch_t;

void cat_init(void);

// Advance animation and behaviour by dt seconds. shake is the smoothed shake
// magnitude from the IMU in m/s^2 above gravity; tilt is the screen-x gravity
// component in m/s^2, positive = right side down (both 0 if there is no IMU).
void cat_update(float dt, const cat_touch_t *touch, float shake, float tilt);

// Feed the fuel gauge reading for the corner battery bar (-1 = unknown).
void cat_set_battery(int percent, bool charging);

// Select the background time-of-day variant (a BG_* index from cat_bg.h).
void cat_set_daypart(int variant);

// Compose the canvas and push every band to the display.
void cat_render(void);

// 0..1, how hard the cat is currently purring — feed to audio_set_purr().
float cat_purr_level(void);

// True exactly once after the cat wakes or petting begins — feed audio_chirp().
bool cat_take_chirp(void);

// True exactly once when the cat starts hissing — feed audio_hiss().
bool cat_take_hiss(void);

// One-shot event getters for sound effects: footstep contact frames while
// trotting, jump/leap launches, paw-lick frames while grooming.
bool cat_take_step(void);
bool cat_take_boing(void);
bool cat_take_slurp(void);
bool cat_take_swipe(void);

// 0 = none, else the leap direction (-1 left, +1 right).
int cat_take_dash(void);

// Logical pixels walked/leapt since the last call — feeds the exercise stat.
float cat_take_walked(void);

// Current gauge values pushed in for the HUD and the menu screen.
void cat_set_stats(int food, int affection, int exercise, int play,
                   int sleep_v);

// Day streaks + done-today flags for the menu, in menu order: play, food,
// love, exercise, sleep.
void cat_set_streaks(const int streaks[5], const int hits[5]);

// One-shot events out of the engine:
bool cat_take_bite(void);       // one mouthful -> one row of the fish gauge
bool cat_take_eat(void);        // the bowl is finished
bool cat_take_play_hit(void);   // one paw-bat or pounce landed
bool cat_take_summon(void);     // a sound or tap called him in from absent
bool cat_take_reconcile(void);  // an earned purr made peace after a scare

// The mic heard a sharp sound (only summons an absent cat, never a hiding one).
void cat_hear_sound(void);

// Trust state, persisted through the stats blob: each unreconciled scare
// raises the level (hides farther, costs more); wary = emerged but not yet
// forgiven.
int cat_scare_level(void);
bool cat_wary(void);
void cat_restore_trust(int scare_level, bool wary);

// Proactive-wake opening acts (ENTICE_* from model.h): start performing the
// given act until stopped or until the audition ends.
void cat_entice(int kind);
void cat_entice_stop(void);

cat_state_t cat_state(void);

// Cumulative count of failed band flushes, for diagnostics.
int cat_flush_errors(void);

// The button-driven test menu: PWR opens and steps, BOOT chooses. Items
// the engine cannot perform itself (gauges, auditions, sleep) come back as
// a TEST_* code for main to carry out.
enum {
    TEST_DAYPART = 0,  // force a time of day, to check the park's art
    TEST_WEATHER,      // reserved: lights up when weather exists
    TEST_ANIM,         // browse every animation, playing in the park
    TEST_FILL, TEST_EMPTY, TEST_SCARE, TEST_SUMMON, TEST_AUDITION,
    TEST_SLEEP,
    TEST_EXIT_MENU,  // put the menu away; anything forced stays forced
    TEST_EXIT_TEST,  // and this hands everything back to the clock
    TEST_COUNT,
};

// The two buttons, handed straight to the engine: it owns the menu and the
// animation browser and only hands back the actions it cannot perform
// itself (gauges, auditions, sleep). -1 means nothing for main to do.
int cat_button_pwr(void);
int cat_button_boot(void);
bool cat_test_is_open(void);
void cat_test_close(void);

// Two short lines of whatever is worth knowing, shown under the test menu.
void cat_set_debug_lines(const char *a, const char *b);

// Preview-only: force an internal behaviour mode. Numbers are
// implementation-specific; harmless on hardware.
void cat_debug_force(int mode);

// Test-only: his current world position and the camera's, logical px.
float cat_debug_world(void);
float cat_debug_cam(void);
