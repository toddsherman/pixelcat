// The simulated owner: drives the real schedule predictor and enticement
// bandit (main/model.c) through weeks of half-hour buckets and asserts the
// promises GAME_DESIGN.md makes — dormant start, ~50% precision, daily caps,
// the bandit learning what works, and ~2-week adaptation to a new routine.
//
// Build and run: tools/sim/run.sh

#include <stdio.h>

#include "model.h"

static int s_failures;

static void expect(int cond, const char *what)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        s_failures++;
    }
}

static unsigned long s_rng = 0x1234567UL;

static float rnd01(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)((s_rng >> 8) & 0xFFFFFF) / 16777216.0f;
}

// The owner's life. Weeks < shift_week: weekday mornings 7:30-8:30 and
// evenings 18:00-19:30; weekend late mornings. After the shift the routine
// moves three hours later — a new job, say.
static float routine_p(int week, int shift_week, int dow, int half)
{
    const int weekend = (dow == 0 || dow == 6);
    const int shifted = week >= shift_week;
    if (!weekend) {
        const int m0 = shifted ? 21 : 15;  // 10:30 vs 7:30, two buckets
        const int e0 = shifted ? 42 : 36;  // 21:00 vs 18:00, three buckets
        if (half >= m0 && half < m0 + 2) return 0.85f;
        if (half >= e0 && half < e0 + 3) return 0.75f;
    } else {
        const int w0 = shifted ? 24 : 18;  // noon vs 9:00, four buckets
        if (half >= w0 && half < w0 + 4) return 0.70f;
    }
    return 0.015f;
}

// How the owner responds to each opening act in the morning: this owner is
// a sucker for the meow, lukewarm on the rest.
static float arm_affinity(int period, int arm)
{
    if (period == 0) {
        return (arm == ENTICE_MEOW) ? 0.80f : 0.25f;
    }
    return 0.40f;
}

int main(void)
{
    model_reset();

    const int WEEKS = 10, SHIFT_WEEK = 6;
    int week_fires[10] = {0}, week_good[10] = {0};
    int worst_day_fires = 0;
    int shifted_window_fires = 0;

    for (int day = 0; day < WEEKS * 7; day++) {
        const int week = day / 7;
        const int dow = (day + 1) % 7;  // day 0 is a Monday
        const int32_t day_serial = 20260101 + day;
        int fires_today = 0;

        for (int half = 0; half < 48; half++) {
            const int minutes = half * 30;
            const int bucket = model_bucket(dow, minutes);
            const float p = routine_p(week, SHIFT_WEEK, dow, half);
            const int interacts = rnd01() < p;
            const int32_t abs_half = day * 48 + half;

            int audition_hit = 0;
            if (model_should_wake(bucket, 80, day_serial, abs_half)) {
                fires_today++;
                week_fires[week]++;
                if (p > 0.5f) {
                    week_good[week]++;
                    if (week >= SHIFT_WEEK) {
                        shifted_window_fires++;
                    }
                }
                const int period = model_period(minutes);
                const int arm = model_pick_arm(period, rnd01());
                const float respond =
                    (p > 0.5f) ? arm_affinity(period, arm) : 0.05f;
                audition_hit = rnd01() < respond;
                model_audition_result(period, arm, audition_hit);
            }

            const int hit = interacts || audition_hit;
            if (hit) {
                model_note_session(bucket, day_serial);
            }
            model_close_bucket(bucket, hit);
        }
        if (fires_today > worst_day_fires) {
            worst_day_fires = fires_today;
        }
    }

    printf("\nweek: fires (good)  |  threshold end %.2f\n",
           (double)model_threshold());
    for (int w = 0; w < WEEKS; w++) {
        printf("  w%d: %d (%d)\n", w + 1, week_fires[w], week_good[w]);
    }

    // 1. Dormant start: no wakes in the first week.
    expect(week_fires[0] == 0, "week 1 is silent (dormant until mature)");

    // 2. He does eventually dare.
    int total = 0;
    for (int w = 1; w < SHIFT_WEEK; w++) {
        total += week_fires[w];
    }
    expect(total > 0, "proactive wakes fire once mature");

    // 3. Precision: over the settled stretch before the shift, at least
    //    half his wakes land inside the owner's real routine.
    {
        int fires = week_fires[3] + week_fires[4] + week_fires[5];
        int good = week_good[3] + week_good[4] + week_good[5];
        expect(fires > 0 && good * 2 >= fires,
               "settled precision is at least ~50%");
    }

    // 4. The cap holds even on his boldest day.
    expect(worst_day_fires <= 6, "never more than 6 wakes in a day");

    // 5. The bandit learned this owner: mornings, the meow wins.
    {
        int best = 0;
        for (int a = 1; a < MODEL_ARMS; a++) {
            if (model_arm_value(0, a) > model_arm_value(0, best)) {
                best = a;
            }
        }
        expect(best == ENTICE_MEOW, "the bandit learns the morning meow");
    }

    // 6. Adaptation: after the routine shifts, wakes migrate to the new
    //    windows within ~2 weeks and the tail precision recovers.
    {
        int fires = week_fires[8] + week_fires[9];
        int good = week_good[8] + week_good[9];
        expect(shifted_window_fires > 0, "wakes migrate to the new routine");
        expect(fires == 0 || good * 5 >= fires * 2,
               "post-shift precision recovers to at least ~40%");
    }

    // 7. The schedule map itself moved: the old morning bucket faded, the
    //    new one rose.
    {
        const int old_b = model_bucket(1, 15 * 30);
        const int new_b = model_bucket(1, 21 * 30);
        expect(model_bucket_p(new_b) > model_bucket_p(old_b),
               "the schedule map follows the owner");
    }

    printf(s_failures ? "\n%d FAILURES\n" : "\nsimulated owner satisfied\n",
           s_failures);
    return s_failures ? 1 : 0;
}
