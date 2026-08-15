#pragma once

#include <stdbool.h>
#include <stdint.h>

// The two tiny on-device models (GAME_DESIGN.md):
//
// 1. Schedule predictor — 96 half-hour buckets (48 weekday + 48 weekend),
//    each an EMA of "did an interaction session land in this bucket". Every
//    elapsed bucket is an observation, including the ones slept through;
//    exponential forgetting adapts to a changed routine in ~2 weeks.
// 2. Enticement bandit — per coarse time-of-day (morning/midday/evening/
//    night), an epsilon-greedy value table over his five opening acts.
//    Acts that get you to interact within the audition window are
//    reinforced.
//
// Pure C (no RTOS, no hardware): the simulated-owner harness compiles this
// file on the host. Persistence lives behind ESP_PLATFORM.

#define MODEL_BUCKETS 96
#define MODEL_ARMS 5
#define MODEL_PERIODS 4

// The five opening acts, in bandit-arm order.
enum {
    ENTICE_JUMP = 0,   // jumping about
    ENTICE_PURR,       // loud purring
    ENTICE_PAW,        // pawing the glass
    ENTICE_PACE,       // pacing
    ENTICE_MEOW,       // more meowing
};

// Bucket index for a local time. dow: 0 = Sunday.
int model_bucket(int dow, int minutes_of_day);
// Coarse period for the bandit: 0 morning 5-11, 1 midday 11-17,
// 2 evening 17-23, 3 night 23-5.
int model_period(int minutes_of_day);

void model_reset(void);

// An interaction session began (first activity after a quiet gap): marks
// the current bucket hit and feeds the maturity counters.
void model_note_session(int bucket, int32_t day_serial);

// A half-hour bucket ended: fold its outcome into the EMA.
void model_close_bucket(int bucket, bool hit);

// Bookkeeping for offline catch-up: the absolute half-hour (epoch/1800)
// most recently folded in. Slept-through buckets are quiet observations —
// main replays them as misses at boot.
int64_t model_last_closed(void);
void model_note_closed(int64_t abs_halfhour);

// Should a proactive wake fire for this bucket right now? Applies maturity
// (dormant ~2 weeks / ~20 sessions), the precision-governed threshold, the
// daily cap, the battery floor, and never-twice-per-bucket.
bool model_should_wake(int bucket, int battery_pct, int32_t day_serial,
                       int32_t abs_halfhour);

// Pick the opening act for this period (epsilon-greedy; rnd01 in [0,1)).
int model_pick_arm(int period, float rnd01);

// The audition ended: reward the arm and let the precision governor adjust
// the wake threshold toward ~50% hits.
void model_audition_result(int period, int arm, bool hit);

// Introspection (telemetry + harness).
float model_bucket_p(int bucket);
float model_arm_value(int period, int arm);
int model_sessions(void);
bool model_mature(void);
float model_threshold(void);
void model_wake_stats(int *hits, int *misses);

#ifdef ESP_PLATFORM
bool model_store_load(void);
void model_store_save(void);
#endif
