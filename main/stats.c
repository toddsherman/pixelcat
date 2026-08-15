#include "stats.h"

#include <time.h>

// ---------------------------------------------------------------------------
// Tuning. Rates are per real-time second. Everything is paced to a ~5 minute
// care session: one feed, one play session, a good petting, a walk and a nap
// fills all five gauges; neglect drains them within a few minutes.
// ---------------------------------------------------------------------------

// Drain rates while he is awake.
#define FOOD_DECAY_PER_S (100.0f / (6.0f * 60.0f))
#define LOVE_DECAY_PER_S (100.0f / (5.0f * 60.0f))
#define EXER_DECAY_PER_S (100.0f / (5.0f * 60.0f))
#define PLAY_DECAY_PER_S (100.0f / (5.0f * 60.0f))
#define SLEEP_DECAY_PER_S (100.0f / (6.0f * 60.0f))

// While HE sleeps: the sleep gauge fills in ~75 s, and exercise and play
// deplete several times faster — his nap winds the day down.
#define SLEEP_FILL_PER_S (100.0f / 75.0f)
#define ASLEEP_XP_FACTOR 4.0f

// Petting fills the heart one gauge row per ~5 seconds of purring.
#define PET_FILL_PER_S 4.0f
#define PET_PURR_MIN 0.2f  // above the sleeping purr, below any petting purr

// Exercise accrues ONLY from walking and deliberate dashes.
#define EXER_PER_PX (100.0f / 3500.0f)
#define EXER_PER_DASH 3.0f

// Play sessions: each bat or pounce scores the play gauge and a little
// affection, and works up an appetite. No exercise credit — that is what
// walking is for.
#define PLAY_GAUGE_PER_HIT 5.0f
#define PLAY_LOVE_PER_HIT 0.6f
#define PLAY_FOOD_PER_HIT 0.2f

// Scares cost more each time until peace is made; a reconciliation recovers
// a little of what fear took.
#define AFFECT_SCARE_COST 8.0f
#define AFFECT_SCARE_STEP 3.0f
#define AFFECT_SCARE_MAX 20.0f
#define AFFECT_RECONCILE 4.0f

// A gauge counts as "achieved" for the day's streak once it touches this.
#define STREAK_FULL 99.5f

static stats_t s_stats;
static float s_pet_budget;  // vestigial: kept only for blob layout stability
static int32_t s_day_serial;  // 0 = unknown
static int s_trust_level;
static bool s_trust_wary;
static uint8_t s_streak[ST_COUNT];
static uint8_t s_hit_today[ST_COUNT];

void stats_set_trust(int scare_level, bool wary)
{
    s_trust_level = scare_level;
    s_trust_wary = wary;
}

int stats_trust_level(void)
{
    return s_trust_level;
}

bool stats_trust_wary(void)
{
    return s_trust_wary;
}

static uint32_t s_rng = 0x2545F491u;

static float srand01(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) * (1.0f / 16777216.0f);
}

void stats_seed(uint32_t seed)
{
    s_rng = seed ? seed : 0x2545F491u;
}

static float clamp01_100(float v)
{
    return (v < 0.0f) ? 0.0f : (v > 100.0f) ? 100.0f : v;
}

void stats_reset(void)
{
    // A new cat arrives mid-session: partly fed, mildly fond, ready to go.
    s_stats.food = 60.0f;
    s_stats.affection = 40.0f;
    s_stats.exercise = 0.0f;
    s_stats.play = 0.0f;
    s_stats.sleep = 50.0f;
    s_pet_budget = 0.0f;
    s_day_serial = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        s_streak[i] = 0;
        s_hit_today[i] = 0;
    }
}

const stats_t *stats_get(void)
{
    return &s_stats;
}

// Streak bookkeeping: note any gauge touching full today.
static void note_hits(void)
{
    const float *v[ST_COUNT] = {&s_stats.play, &s_stats.food,
                                &s_stats.affection, &s_stats.exercise,
                                &s_stats.sleep};
    for (int i = 0; i < ST_COUNT; i++) {
        if (*v[i] >= STREAK_FULL) {
            s_hit_today[i] = 1;
        }
    }
}

// His sleep completing rolls the episode: everything else starts over.
static void sleep_completes(void)
{
    s_stats.food = 0.0f;
    s_stats.affection = 0.0f;
    s_stats.exercise = 0.0f;
    s_stats.play = 0.0f;
}

void stats_tick(float dt, bool asleep, float purr)
{
    if (dt <= 0.0f) {
        return;
    }

    s_stats.food -= dt * FOOD_DECAY_PER_S;
    s_stats.affection -= dt * LOVE_DECAY_PER_S;
    const float xp = asleep ? ASLEEP_XP_FACTOR : 1.0f;
    s_stats.exercise -= dt * EXER_DECAY_PER_S * xp;
    s_stats.play -= dt * PLAY_DECAY_PER_S * xp;

    if (asleep) {
        const bool was_full = s_stats.sleep >= STREAK_FULL;
        s_stats.sleep += dt * SLEEP_FILL_PER_S;
        if (!was_full && s_stats.sleep >= STREAK_FULL) {
            s_hit_today[ST_SLEEP] = 1;
            sleep_completes();
        }
    } else {
        s_stats.sleep -= dt * SLEEP_DECAY_PER_S;
        if (purr > PET_PURR_MIN) {
            s_stats.affection += dt * PET_FILL_PER_S;
        }
    }

    s_stats.food = clamp01_100(s_stats.food);
    s_stats.affection = clamp01_100(s_stats.affection);
    s_stats.exercise = clamp01_100(s_stats.exercise);
    s_stats.play = clamp01_100(s_stats.play);
    s_stats.sleep = clamp01_100(s_stats.sleep);
    note_hits();
}

void stats_on_walk(float logical_px)
{
    if (logical_px <= 0.0f) {
        return;
    }
    s_stats.exercise = clamp01_100(s_stats.exercise + logical_px * EXER_PER_PX);
    note_hits();
}

void stats_on_dash(void)
{
    s_stats.exercise = clamp01_100(s_stats.exercise + EXER_PER_DASH);
    note_hits();
}

void stats_on_scare(int level)
{
    float cost = AFFECT_SCARE_COST + AFFECT_SCARE_STEP * (float)(level - 1);
    if (cost < AFFECT_SCARE_COST) {
        cost = AFFECT_SCARE_COST;
    }
    if (cost > AFFECT_SCARE_MAX) {
        cost = AFFECT_SCARE_MAX;
    }
    s_stats.affection = clamp01_100(s_stats.affection - cost);
}

void stats_on_reconcile(void)
{
    s_stats.affection = clamp01_100(s_stats.affection + AFFECT_RECONCILE);
}

void stats_on_eat(float amount)
{
    s_stats.food = clamp01_100(s_stats.food + amount);
    note_hits();
}

void stats_on_play_hit(void)
{
    s_stats.play = clamp01_100(s_stats.play + PLAY_GAUGE_PER_HIT);
    s_stats.affection = clamp01_100(s_stats.affection + PLAY_LOVE_PER_HIT);
    s_stats.food = clamp01_100(s_stats.food - PLAY_FOOD_PER_HIT);
    note_hits();
}

int stats_streak(int item)
{
    return (item >= 0 && item < ST_COUNT) ? s_streak[item] : 0;
}

int stats_hit_today(int item)
{
    return (item >= 0 && item < ST_COUNT) ? s_hit_today[item] : 0;
}

void stats_note_date(int32_t day_serial)
{
    if (day_serial <= 0) {
        return;
    }
    if (s_day_serial != 0 && day_serial != s_day_serial) {
        // Roll the streak ledger: a day with a full gauge extends its run.
        for (int i = 0; i < ST_COUNT; i++) {
            if (s_hit_today[i]) {
                if (s_streak[i] < 255) {
                    s_streak[i]++;
                }
            } else {
                s_streak[i] = 0;
            }
            s_hit_today[i] = 0;
        }
    }
    s_day_serial = day_serial;
}

void stats_offline(double seconds)
{
    if (seconds <= 0.0) {
        return;
    }
    // A short gap (a reboot, a flash cycle) is not a nap: the gauges just
    // decay a little and the session survives.
    if (seconds < 120.0) {
        const float sec = (float)seconds;
        s_stats.food = clamp01_100(s_stats.food - sec * FOOD_DECAY_PER_S);
        s_stats.affection =
            clamp01_100(s_stats.affection - sec * LOVE_DECAY_PER_S);
        s_stats.exercise =
            clamp01_100(s_stats.exercise - sec * EXER_DECAY_PER_S);
        s_stats.play = clamp01_100(s_stats.play - sec * PLAY_DECAY_PER_S);
        return;
    }

    // A real absence: he slept through it. His sleep gauge fills first; if
    // it completes inside the gap, the episode rolled over — everything
    // else is back at zero for a fresh session.
    const float sec = (seconds > 24.0 * 3600.0) ? 24.0f * 3600.0f
                                                : (float)seconds;
    const float to_full = (100.0f - s_stats.sleep) / SLEEP_FILL_PER_S;
    const float t1 = (sec < to_full) ? sec : to_full;

    s_stats.food = clamp01_100(s_stats.food - t1 * FOOD_DECAY_PER_S);
    s_stats.affection = clamp01_100(s_stats.affection - t1 * LOVE_DECAY_PER_S);
    s_stats.exercise = clamp01_100(
        s_stats.exercise - t1 * EXER_DECAY_PER_S * ASLEEP_XP_FACTOR);
    s_stats.play = clamp01_100(
        s_stats.play - t1 * PLAY_DECAY_PER_S * ASLEEP_XP_FACTOR);
    s_stats.sleep = clamp01_100(s_stats.sleep + t1 * SLEEP_FILL_PER_S);

    if (sec >= to_full) {
        s_hit_today[ST_SLEEP] = 1;
        sleep_completes();
    }
}

// ---------------------------------------------------------------------------
// Persistence (ESP only): one NVS blob, written every few minutes and before
// sleep, read once at boot.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "nvs.h"

#define STATS_MAGIC_V1 0x50435331u  // 'PCS1'
#define STATS_MAGIC_V2 0x50435332u  // 'PCS2': adds the poop fields
#define STATS_MAGIC_V3 0x50435333u  // 'PCS3': adds trust (scares, wariness)
#define STATS_MAGIC_V4 0x50435334u  // 'PCS4': energy retired; hunger is food
#define STATS_MAGIC_V5 0x50435335u  // 'PCS5': adds the daily play gauge
#define STATS_MAGIC 0x50435336u     // 'PCS6': sleep gauge + streak ledger

// The retired layouts, exactly as they were written (sizeof must match the
// stored blob lengths, trailing padding included).
typedef struct {
    uint32_t magic;
    float hunger, affection, energy, exercise;
    float pet_budget;
    int64_t saved_epoch;   // UTC seconds at save; 0 = clock was not valid
    int32_t day_serial;
} stats_blob_v1_t;

typedef struct {
    uint32_t magic;
    float hunger, affection, energy, exercise;
    float pet_budget;
    int64_t saved_epoch;
    int32_t day_serial;
    float poop_due_s;
    int32_t poop_count;
} stats_blob_v2_t;

typedef struct {
    uint32_t magic;
    float hunger, affection, energy, exercise;
    float pet_budget;
    int64_t saved_epoch;
    int32_t day_serial;
    float poop_due_s;
    int32_t poop_count;
    int32_t trust_level;
    int32_t trust_wary;
} stats_blob_v3_t;

typedef struct {
    uint32_t magic;
    float food, affection, exercise;
    float pet_budget;
    int64_t saved_epoch;
    int32_t day_serial;
    float poop_due_s;
    int32_t poop_count;
    int32_t trust_level;
    int32_t trust_wary;
} stats_blob_v4_t;

typedef struct {
    uint32_t magic;
    float food, affection, exercise, play;
    float pet_budget;
    int64_t saved_epoch;
    int32_t day_serial;
    float poop_due_s;
    int32_t poop_count;
    int32_t trust_level;
    int32_t trust_wary;
} stats_blob_v5_t;

typedef struct {
    uint32_t magic;
    float food, affection, exercise, play, sleep;
    float pet_budget;
    int64_t saved_epoch;
    int32_t day_serial;
    float poop_due_s;
    int32_t poop_count;
    int32_t trust_level;
    int32_t trust_wary;
    uint8_t streak[ST_COUNT];
    uint8_t hit_today[ST_COUNT];
} stats_blob_t;

static const char *TAG = "stats";

static bool s_catchup_done;
static int64_t s_loaded_epoch;

static bool clock_valid(void)
{
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_year + 1900 >= 2020;
}

static int32_t today_serial(void)
{
    if (!clock_valid()) {
        return 0;
    }
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return (int32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 +
                     lt.tm_mday);
}

bool stats_store_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READONLY, &h) != ESP_OK) {
        return false;  // first boot: the namespace does not exist yet
    }
    union {
        stats_blob_t v6;
        stats_blob_v5_t v5;
        stats_blob_v4_t v4;
        stats_blob_v3_t v3;  // v1/v2 are prefixes of this layout
    } u;
    size_t len = sizeof(u);
    const esp_err_t ret = nvs_get_blob(h, "stats", &u, &len);
    nvs_close(h);
    if (ret != ESP_OK) {
        return false;
    }

    stats_blob_t b;
    if (len == sizeof(stats_blob_t) && u.v6.magic == STATS_MAGIC) {
        b = u.v6;
    } else if (len == sizeof(stats_blob_v5_t) && u.v5.magic == STATS_MAGIC_V5) {
        b = (stats_blob_t){
            .magic = STATS_MAGIC,
            .food = u.v5.food,
            .affection = u.v5.affection,
            .exercise = u.v5.exercise,
            .play = u.v5.play,
            .sleep = 50.0f,
            .pet_budget = 0.0f,
            .saved_epoch = u.v5.saved_epoch,
            .day_serial = u.v5.day_serial,
            .poop_due_s = u.v5.poop_due_s,
            .poop_count = u.v5.poop_count,
            .trust_level = u.v5.trust_level,
            .trust_wary = u.v5.trust_wary,
        };
    } else if (len == sizeof(stats_blob_v4_t) && u.v4.magic == STATS_MAGIC_V4) {
        b = (stats_blob_t){
            .magic = STATS_MAGIC,
            .food = u.v4.food,
            .affection = u.v4.affection,
            .exercise = u.v4.exercise,
            .sleep = 50.0f,
            .saved_epoch = u.v4.saved_epoch,
            .day_serial = u.v4.day_serial,
            .poop_due_s = u.v4.poop_due_s,
            .poop_count = u.v4.poop_count,
            .trust_level = u.v4.trust_level,
            .trust_wary = u.v4.trust_wary,
        };
    } else if ((len == sizeof(stats_blob_v1_t) && u.v3.magic == STATS_MAGIC_V1) ||
               (len == sizeof(stats_blob_v2_t) && u.v3.magic == STATS_MAGIC_V2) ||
               (len == sizeof(stats_blob_v3_t) && u.v3.magic == STATS_MAGIC_V3)) {
        const bool has_poop = len >= sizeof(stats_blob_v2_t);
        const bool has_trust = len >= sizeof(stats_blob_v3_t);
        b = (stats_blob_t){
            .magic = STATS_MAGIC,
            .food = u.v3.hunger,
            .affection = u.v3.affection,
            .exercise = u.v3.exercise,
            .sleep = 50.0f,
            .saved_epoch = u.v3.saved_epoch,
            .day_serial = u.v3.day_serial,
            .poop_due_s = has_poop ? u.v3.poop_due_s : 0.0f,
            .poop_count = has_poop ? u.v3.poop_count : 0,
            .trust_level = has_trust ? u.v3.trust_level : 0,
            .trust_wary = has_trust ? u.v3.trust_wary : 0,
        };
    } else {
        return false;
    }

    s_stats.food = clamp01_100(b.food);
    s_stats.affection = clamp01_100(b.affection);
    s_stats.exercise = clamp01_100(b.exercise);
    s_stats.play = clamp01_100(b.play);
    s_stats.sleep = clamp01_100(b.sleep);
    s_day_serial = b.day_serial;
    s_loaded_epoch = b.saved_epoch;
    s_trust_level = (b.trust_level < 0) ? 0 : (b.trust_level > 4)
                                                  ? 4
                                                  : b.trust_level;
    s_trust_wary = b.trust_wary != 0;
    if (b.magic == STATS_MAGIC) {
        for (int i = 0; i < ST_COUNT; i++) {
            s_streak[i] = b.streak[i];
            s_hit_today[i] = b.hit_today[i] ? 1 : 0;
        }
    }
    ESP_LOGI(TAG, "loaded F %.0f A %.0f X %.0f PL %.0f S %.0f (epoch %lld)",
             (double)s_stats.food, (double)s_stats.affection,
             (double)s_stats.exercise, (double)s_stats.play,
             (double)s_stats.sleep, (long long)s_loaded_epoch);
    return true;
}

void stats_store_save(void)
{
    if (!s_catchup_done) {
        // Saving now would stamp a fresh epoch over an offline window that
        // has not been applied yet.
        return;
    }
    stats_blob_t b = {
        .magic = STATS_MAGIC,
        .food = s_stats.food,
        .affection = s_stats.affection,
        .exercise = s_stats.exercise,
        .play = s_stats.play,
        .sleep = s_stats.sleep,
        .pet_budget = 0.0f,
        .saved_epoch = clock_valid() ? (int64_t)time(NULL) : 0,
        .day_serial = s_day_serial,
        .poop_due_s = 0.0f,   // poop retired; fields kept for blob layout
        .poop_count = 0,
        .trust_level = s_trust_level,
        .trust_wary = s_trust_wary ? 1 : 0,
    };
    for (int i = 0; i < ST_COUNT; i++) {
        b.streak[i] = s_streak[i];
        b.hit_today[i] = s_hit_today[i];
    }
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed; stats not saved");
        return;
    }
    if (nvs_set_blob(h, "stats", &b, sizeof(b)) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "NVS write failed; stats not saved");
    }
    nvs_close(h);
}

void stats_apply_offline(void)
{
    if (s_catchup_done) {
        return;
    }
    s_catchup_done = true;

    if (s_loaded_epoch > 0 && clock_valid()) {
        const double gap = difftime(time(NULL), (time_t)s_loaded_epoch);
        if (gap > 60.0) {
            stats_offline(gap);
            ESP_LOGI(TAG, "offline %.0f s -> F %.0f A %.0f X %.0f S %.0f",
                     gap, (double)s_stats.food, (double)s_stats.affection,
                     (double)s_stats.exercise, (double)s_stats.sleep);
        }
    }
    stats_note_date(today_serial());
    stats_store_save();
}

bool stats_catchup_done(void)
{
    return s_catchup_done;
}

#endif  // ESP_PLATFORM
