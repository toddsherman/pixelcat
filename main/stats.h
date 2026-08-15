#pragma once

#include <stdbool.h>
#include <stdint.h>

// The care stats (GAME_DESIGN.md): three 0..100 values simulated
// continuously while the device runs and caught up through offline gaps at
// boot. The simulation half is pure C (no RTOS, no hardware) so it can also
// run in a host harness; persistence lives behind ESP_PLATFORM.
//
// Every bar aspires to be full: fed, loved, exercised. There is no separate
// energy stat — a filled exercise bar IS a tired cat (he gets sleepy as it
// fills and wakes up fresh when it resets with the new day).

typedef struct {
    float food;       // 100 fed .. 0 hungry
    float affection;  // 100 devoted .. 0 aloof
    float exercise;   // today's activity; full = satisfied and sleepy
} stats_t;

void stats_reset(void);
const stats_t *stats_get(void);

// Advance the live simulation by dt seconds. asleep = the cat (not the
// device) is in his sleep animation; purr feeds petting affection.
void stats_tick(float dt, bool asleep, float purr);

// Event feeds from the cat engine.
void stats_on_walk(float logical_px);
void stats_on_jump(void);         // any leap or big jump launch
void stats_on_scare(int level);   // the shake hiss; higher levels cost more
void stats_on_reconcile(void);    // peace made after a scare
void stats_on_eat(float amount);  // finishing a bowl
void stats_on_play_hit(void);     // one paw-bat or pounce in a play session

// Trust state riding along in the blob (owned by the cat engine).
void stats_set_trust(int scare_level, bool wary);
int stats_trust_level(void);
bool stats_trust_wary(void);

// Poop: eating schedules one a few hours out; the engine spawns it when
// ready. Visible poops cost affection hourly until cleaned.
void stats_note_fed(void);
bool stats_take_poop_ready(void);
void stats_set_poop_count(int n);
int stats_poop_count(void);
void stats_seed(uint32_t seed);

// Local calendar date as y*10000+m*100+d. Resets exercise when it changes.
void stats_note_date(int32_t day_serial);

// Apply an offline gap of this many seconds, treated as the cat sleeping
// somewhere: hunger sinks gently, energy recovers, affection fades a little.
// Kindness caps keep a long absence at "hungry cat", never tragedy.
void stats_offline(double seconds);

#ifdef ESP_PLATFORM
// NVS persistence. Load returns false on a fresh device (stats_reset state
// stays). stats_apply_offline computes the gap between the saved wall-clock
// timestamp and now — call it once, when the system clock is trustworthy
// (after NTP, or a timeout falling back to the PCF-seeded clock); saves are
// refused until then so a reboot can never shrink the offline window.
bool stats_store_load(void);
void stats_store_save(void);
void stats_apply_offline(void);
bool stats_catchup_done(void);
#endif
