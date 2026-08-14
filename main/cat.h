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
} cat_state_t;

typedef struct {
    bool down;
    int16_t x;  // panel pixels
    int16_t y;
} cat_touch_t;

void cat_init(void);

// Advance animation and behaviour by dt seconds. shake is the smoothed shake
// magnitude from the IMU in m/s^2 above gravity (0 if there is no IMU).
void cat_update(float dt, const cat_touch_t *touch, float shake);

// Compose the canvas and push every band to the display.
void cat_render(void);

// 0..1, how hard the cat is currently purring — feed to audio_set_purr().
float cat_purr_level(void);

// True exactly once after the cat wakes or petting begins — feed audio_chirp().
bool cat_take_chirp(void);

// True exactly once when the cat starts hissing — feed audio_hiss().
bool cat_take_hiss(void);

cat_state_t cat_state(void);

// Cumulative count of failed band flushes, for diagnostics.
int cat_flush_errors(void);

// Preview-only: force an internal behaviour mode. Numbers are
// implementation-specific; harmless on hardware.
void cat_debug_force(int mode);
