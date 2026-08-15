#include "stats.h"

#include <time.h>

// ---------------------------------------------------------------------------
// Tuning. Rates are per real-time second; the derivations are the spec's
// human-scale numbers.
// ---------------------------------------------------------------------------

// Fed to hungry in ~6 h awake; a sleeping cat burns a quarter of that.
#define FOOD_PER_S (100.0f / (6.0f * 3600.0f))
#define FOOD_SLEEP_FACTOR 0.25f
// Exercise burns it faster: walking and jumping cost extra on top.
#define FOOD_PER_PX (100.0f / 50000.0f)
#define FOOD_PER_JUMP 0.15f

// Ignored for two days = affection gone; half rate while he sleeps.
#define AFFECT_DECAY_PER_S (100.0f / (48.0f * 3600.0f))
// Scares cost more each time until peace is made; a reconciliation recovers
// a little of what fear took.
#define AFFECT_SCARE_COST 8.0f
#define AFFECT_SCARE_STEP 3.0f
#define AFFECT_SCARE_MAX 20.0f
#define AFFECT_RECONCILE 4.0f
// Petting fills the heart one gauge row (20 points) per 5 seconds: a
// deliberate, readable rate — 25 s of genuine petting fills it from empty.
#define PET_FILL_PER_S (20.0f / 5.0f)
#define PET_PURR_MIN 0.2f  // above the sleeping purr, below any petting purr

// A good active day: ~3500 logical px of walking, or ~50 jumps, or a mix.
#define EXER_PER_PX (100.0f / 3500.0f)
#define EXER_PER_JUMP 2.0f

// Play sessions score double exercise and extra hearts (the spec's words).
// The play gauge itself fills in about a session and a half — a full ball
// means he has had his games for the day.
#define PLAY_EXER_PER_HIT (2.0f * EXER_PER_JUMP)
#define PLAY_AFFECT_PER_HIT 0.6f
#define PLAY_FOOD_PER_HIT 0.2f
#define PLAY_GAUGE_PER_HIT 3.0f

// A meal shows up again as a poop 2-4 h later; each one left visible has an
// hourly affection price — he has standards.
#define POOP_MIN_S (2.0f * 3600.0f)
#define POOP_SPAN_S (2.0f * 3600.0f)
#define POOP_AFFECT_PER_S (0.5f / 3600.0f)

// Offline kindness caps: however long the absence, he wakes up hungry and a
// touch distant — never starved, never a stranger.
#define OFFLINE_HUNGER_FLOOR 10.0f
#define OFFLINE_AFFECT_FLOOR 30.0f
#define OFFLINE_AFFECT_FACTOR 0.5f
#define OFFLINE_MAX_S (30.0 * 24.0 * 3600.0)

static stats_t s_stats;
static float s_pet_budget;  // vestigial: kept only for blob layout stability
static int32_t s_day_serial;  // 0 = unknown
static float s_poop_due_s;    // >0 counting down, 0 none, -1 ready to spawn
static int s_poop_count;
static int s_trust_level;
static bool s_trust_wary;

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
    // A new cat arrives fed, curious and a little reserved.
    s_stats.food = 85.0f;
    s_stats.affection = 55.0f;
    s_stats.exercise = 0.0f;
    s_stats.play = 0.0f;
    s_pet_budget = 0.0f;
    s_day_serial = 0;
    s_poop_due_s = 0.0f;
    s_poop_count = 0;
}

const stats_t *stats_get(void)
{
    return &s_stats;
}

void stats_tick(float dt, bool asleep, float purr)
{
    if (dt <= 0.0f) {
        return;
    }

    s_stats.food -= dt * FOOD_PER_S * (asleep ? FOOD_SLEEP_FACTOR : 1.0f);
    s_stats.affection -= dt * (AFFECT_DECAY_PER_S * (asleep ? 0.5f : 1.0f) +
                               (float)s_poop_count * POOP_AFFECT_PER_S);

    if (s_poop_due_s > 0.0f) {
        s_poop_due_s -= dt * (asleep ? 0.25f : 1.0f);
        if (s_poop_due_s <= 0.0f) {
            s_poop_due_s = -1.0f;  // ready: the engine will place it
        }
    }

    if (!asleep && purr > PET_PURR_MIN) {
        s_stats.affection += dt * PET_FILL_PER_S;
    }

    s_stats.food = clamp01_100(s_stats.food);
    s_stats.affection = clamp01_100(s_stats.affection);
}

void stats_on_walk(float logical_px)
{
    if (logical_px <= 0.0f) {
        return;
    }
    s_stats.exercise = clamp01_100(s_stats.exercise + logical_px * EXER_PER_PX);
    s_stats.food = clamp01_100(s_stats.food - logical_px * FOOD_PER_PX);
}

void stats_on_jump(void)
{
    s_stats.exercise = clamp01_100(s_stats.exercise + EXER_PER_JUMP);
    s_stats.food = clamp01_100(s_stats.food - FOOD_PER_JUMP);
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
}

void stats_on_play_hit(void)
{
    s_stats.exercise = clamp01_100(s_stats.exercise + PLAY_EXER_PER_HIT);
    s_stats.affection = clamp01_100(s_stats.affection + PLAY_AFFECT_PER_HIT);
    s_stats.food = clamp01_100(s_stats.food - PLAY_FOOD_PER_HIT);
    s_stats.play = clamp01_100(s_stats.play + PLAY_GAUGE_PER_HIT);
}

void stats_note_fed(void)
{
    if (s_poop_due_s == 0.0f) {
        s_poop_due_s = POOP_MIN_S + srand01() * POOP_SPAN_S;
    }
}

bool stats_take_poop_ready(void)
{
    if (s_poop_due_s < 0.0f) {
        s_poop_due_s = 0.0f;
        return true;
    }
    return false;
}

void stats_set_poop_count(int n)
{
    s_poop_count = (n < 0) ? 0 : n;
}

int stats_poop_count(void)
{
    return s_poop_count;
}

void stats_note_date(int32_t day_serial)
{
    if (day_serial <= 0) {
        return;
    }
    if (s_day_serial != 0 && day_serial != s_day_serial) {
        // A new day: a fresh walk to take, fresh games to play.
        s_stats.exercise = 0.0f;
        s_stats.play = 0.0f;
    }
    s_day_serial = day_serial;
}

void stats_offline(double seconds)
{
    if (seconds <= 0.0) {
        return;
    }
    if (seconds > OFFLINE_MAX_S) {
        seconds = OFFLINE_MAX_S;
    }
    const float sec = (float)seconds;

    // He slept through it: gentle appetite, mild fading — each floored so an
    // absence never reads as tragedy.
    float h = s_stats.food - sec * FOOD_PER_S * FOOD_SLEEP_FACTOR;
    if (h < OFFLINE_HUNGER_FLOOR) {
        h = (s_stats.food < OFFLINE_HUNGER_FLOOR) ? s_stats.food
                                                  : OFFLINE_HUNGER_FLOOR;
    }
    s_stats.food = clamp01_100(h);

    float a = s_stats.affection - sec * AFFECT_DECAY_PER_S * OFFLINE_AFFECT_FACTOR;
    if (a < OFFLINE_AFFECT_FLOOR) {
        a = (s_stats.affection < OFFLINE_AFFECT_FLOOR) ? s_stats.affection
                                                       : OFFLINE_AFFECT_FLOOR;
    }
    s_stats.affection = clamp01_100(a);

    if (s_poop_due_s > 0.0f) {
        s_poop_due_s -= sec * 0.25f;
        if (s_poop_due_s <= 0.0f) {
            s_poop_due_s = -1.0f;
        }
    }

    s_pet_budget = 0.0f;
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
#define STATS_MAGIC 0x50435335u     // 'PCS5': adds the daily play gauge

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
        stats_blob_t v5;
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
    if (len == sizeof(stats_blob_t) && u.v5.magic == STATS_MAGIC) {
        b = u.v5;
    } else if (len == sizeof(stats_blob_v4_t) && u.v4.magic == STATS_MAGIC_V4) {
        b = (stats_blob_t){
            .magic = STATS_MAGIC,
            .food = u.v4.food,
            .affection = u.v4.affection,
            .exercise = u.v4.exercise,
            .play = 0.0f,
            .pet_budget = u.v4.pet_budget,
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
        // Older cats migrate: energy is simply retired, missing tails get
        // their defaults.
        const bool has_poop = len >= sizeof(stats_blob_v2_t);
        const bool has_trust = len >= sizeof(stats_blob_v3_t);
        b = (stats_blob_t){
            .magic = STATS_MAGIC,
            .food = u.v3.hunger,
            .affection = u.v3.affection,
            .exercise = u.v3.exercise,
            .play = 0.0f,
            .pet_budget = u.v3.pet_budget,
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
    s_pet_budget = clamp01_100(b.pet_budget);
    s_day_serial = b.day_serial;
    s_loaded_epoch = b.saved_epoch;
    s_poop_due_s = b.poop_due_s;
    s_poop_count = (b.poop_count < 0) ? 0 : (b.poop_count > 3) ? 3
                                                               : b.poop_count;
    s_trust_level = (b.trust_level < 0) ? 0 : (b.trust_level > 4)
                                                  ? 4
                                                  : b.trust_level;
    s_trust_wary = b.trust_wary != 0;
    ESP_LOGI(TAG, "loaded F %.0f A %.0f X %.0f (saved epoch %lld)",
             (double)s_stats.food, (double)s_stats.affection,
             (double)s_stats.exercise, (long long)s_loaded_epoch);
    return true;
}

void stats_store_save(void)
{
    if (!s_catchup_done) {
        // Saving now would stamp a fresh epoch over an offline window that
        // has not been applied yet.
        return;
    }
    const stats_blob_t b = {
        .magic = STATS_MAGIC,
        .food = s_stats.food,
        .affection = s_stats.affection,
        .exercise = s_stats.exercise,
        .play = s_stats.play,
        .pet_budget = s_pet_budget,
        .saved_epoch = clock_valid() ? (int64_t)time(NULL) : 0,
        .day_serial = s_day_serial,
        .poop_due_s = s_poop_due_s,
        .poop_count = s_poop_count,
        .trust_level = s_trust_level,
        .trust_wary = s_trust_wary ? 1 : 0,
    };
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
            ESP_LOGI(TAG, "offline %.0f s -> F %.0f A %.0f X %.0f",
                     gap, (double)s_stats.food, (double)s_stats.affection,
                     (double)s_stats.exercise);
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
