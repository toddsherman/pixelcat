#pragma once

#include <stdbool.h>
#include <stdint.h>

// The care gauges: five 0..100 values tuned to a ~5 minute session — do
// everything positive once and every gauge fills; leave him be and they all
// drain within a few minutes. His sleep ends the episode: while he sleeps
// the sleep gauge fills (and exercise/play deplete quickly), and a completed
// sleep resets everything else to zero — a fresh session on wake.
//
// The simulation half is pure C (no RTOS, no hardware) so it also runs in
// the host harness; persistence lives behind ESP_PLATFORM.

typedef struct {
    float food;       // 100 fed .. 0 hungry
    float affection;  // 100 loved .. 0 aloof
    float exercise;   // walking and dashing; drains in minutes
    float play;       // yarn-ball sessions; drains in minutes
    float sleep;      // fills while HE sleeps; full = the day rolls over
} stats_t;

// Menu order, for gauges and streaks alike.
enum { ST_PLAY = 0, ST_FOOD, ST_LOVE, ST_EXER, ST_SLEEP, ST_COUNT };

// Every HUD gauge is this many pixel rows tall; the scare drain steps
// through them one at a time, so the numbers have to agree with the art.
#define STATS_GAUGE_ROWS 6

void stats_reset(void);
const stats_t *stats_get(void);

// Advance the live simulation by dt seconds. asleep = the cat (not the
// device) is in his sleep animation; purr feeds petting affection.
void stats_tick(float dt, bool asleep, float purr);

// Event feeds from the cat engine. Exercise accrues ONLY from walking and
// dashing (stats_on_walk / stats_on_dash) — never from scares or pounces.
void stats_on_walk(float logical_px);
void stats_on_dash(void);         // a deliberate leap
void stats_on_scare(int level);   // the shake hiss: empties the heart
void stats_on_reconcile(void);    // peace made after a scare
void stats_on_eat(float amount);  // finishing a bowl
void stats_on_play_hit(void);     // one paw-bat or pounce in a play session

// A scare drains the heart one gauge row at a time. Returns the index of
// a row that just fell (0 = the first, each one a lower beep), or -1.
int stats_take_scare_beep(void);

// Streaks: consecutive days on which a gauge reached full at least once,
// and whether it has already reached full today (today is not yet counted
// in the streak until midnight rolls it over).
int stats_streak(int item);
int stats_hit_today(int item);

// Trust state riding along in the blob (owned by the cat engine).
void stats_set_trust(int scare_level, bool wary);
int stats_trust_level(void);
bool stats_trust_wary(void);

void stats_seed(uint32_t seed);

// Local calendar date as y*10000+m*100+d. Rolls the streak ledger.
void stats_note_date(int32_t day_serial);

// Apply an offline gap of this many seconds: he slept through it — the
// sleep gauge fills and everything else drains (and resets once his sleep
// completes). Gaps under two minutes are reboots, not naps.
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
