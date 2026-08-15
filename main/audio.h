#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// ES8311 codec + speaker, running a continuous procedural purr synthesizer on
// its own task. The cat's animation loop just sets the intensity.

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus);

// 0..1. 0 is silent; the synth ramps smoothly toward whatever is set here.
void audio_set_purr(float intensity);

// Queue a short rising "mrrp?" chirp, mixed over the purr.
void audio_chirp(void);

// Queue an angry hiss: a sharp burst of high-tilted noise.
void audio_hiss(void);

// One soft footstep tap (alternates subtly between two timbres).
void audio_step(void);

// A little spring "boing" for jumps.
void audio_boing(void);

// A quiet slurp for paw-licking.
void audio_slurp(void);

// A soft airy swipe for pawing at things.
void audio_swipe(void);

// A dash whoosh for leaps: pitch sweeps up for dir > 0 (right), down for
// dir < 0 (left).
void audio_dash(int dir);

// Stop the synth and close the codec (speaker amp off), for the idle sleep.
void audio_stop(void);
