#include "stats.h"

#include <time.h>

// ---------------------------------------------------------------------------
// Tuning. Rates are per real-time second; the derivations are the spec's
// human-scale numbers.
// ---------------------------------------------------------------------------

// Full to hungry in ~6 h awake; a sleeping cat burns a quarter of that.
#define HUNGER_PER_S (100.0f / (6.0f * 3600.0f))
#define HUNGER_SLEEP_FACTOR 0.25f
// Exercise burns it faster: walking and jumping cost extra on top.
#define HUNGER_PER_PX (100.0f / 50000.0f)
#define HUNGER_PER_JUMP 0.15f

// Ignored for two days = affection gone; half rate while he sleeps.
#define AFFECT_DECAY_PER_S (100.0f / (48.0f * 3600.0f))
#define AFFECT_SCARE_COST 8.0f
// Petting: gain scales with purr strength and a per-session budget, so spam
// cannot max a cat. The budget refills over ~40 min of leaving him be.
#define PET_GAIN_PER_S 0.9f
#define PET_SESSION_BUDGET 12.0f
#define PET_BUDGET_REFILL_S (40.0f * 60.0f)
#define PET_PURR_MIN 0.2f  // above the sleeping purr, below any petting purr

// A ~90 min nap fully recharges him; being awake drains slowly on its own.
#define ENERGY_SLEEP_REGEN_PER_S (100.0f / (90.0f * 60.0f))
#define ENERGY_AWAKE_DRAIN_PER_S (100.0f / (16.0f * 3600.0f))
#define ENERGY_PER_PX (100.0f / 80000.0f)
#define ENERGY_PER_JUMP 0.5f

// A good active day: ~3500 logical px of walking, or ~50 jumps, or a mix.
#define EXER_PER_PX (100.0f / 3500.0f)
#define EXER_PER_JUMP 2.0f

// Offline kindness caps: however long the absence, he wakes up hungry and a
// touch distant — never starved, never a stranger.
#define OFFLINE_HUNGER_FLOOR 10.0f
#define OFFLINE_AFFECT_FLOOR 30.0f
#define OFFLINE_AFFECT_FACTOR 0.5f
#define OFFLINE_MAX_S (30.0 * 24.0 * 3600.0)

static stats_t s_stats;
static float s_pet_budget;
static int32_t s_day_serial;  // 0 = unknown

static float clamp01_100(float v)
{
    return (v < 0.0f) ? 0.0f : (v > 100.0f) ? 100.0f : v;
}

void stats_reset(void)
{
    // A new cat arrives fed, curious and a little reserved.
    s_stats.hunger = 85.0f;
    s_stats.affection = 55.0f;
    s_stats.energy = 90.0f;
    s_stats.exercise = 0.0f;
    s_pet_budget = PET_SESSION_BUDGET;
    s_day_serial = 0;
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

    s_stats.hunger -= dt * HUNGER_PER_S * (asleep ? HUNGER_SLEEP_FACTOR : 1.0f);
    s_stats.affection -= dt * AFFECT_DECAY_PER_S * (asleep ? 0.5f : 1.0f);

    if (!asleep && purr > PET_PURR_MIN) {
        const float gain = purr * PET_GAIN_PER_S *
                           (s_pet_budget / PET_SESSION_BUDGET) * dt;
        s_stats.affection += gain;
        s_pet_budget -= gain;
        if (s_pet_budget < 0.0f) {
            s_pet_budget = 0.0f;
        }
    } else {
        s_pet_budget += dt * (PET_SESSION_BUDGET / PET_BUDGET_REFILL_S);
        if (s_pet_budget > PET_SESSION_BUDGET) {
            s_pet_budget = PET_SESSION_BUDGET;
        }
    }

    s_stats.energy += dt * (asleep ? ENERGY_SLEEP_REGEN_PER_S
                                   : -ENERGY_AWAKE_DRAIN_PER_S);

    s_stats.hunger = clamp01_100(s_stats.hunger);
    s_stats.affection = clamp01_100(s_stats.affection);
    s_stats.energy = clamp01_100(s_stats.energy);
}

void stats_on_walk(float logical_px)
{
    if (logical_px <= 0.0f) {
        return;
    }
    s_stats.exercise = clamp01_100(s_stats.exercise + logical_px * EXER_PER_PX);
    s_stats.hunger = clamp01_100(s_stats.hunger - logical_px * HUNGER_PER_PX);
    s_stats.energy = clamp01_100(s_stats.energy - logical_px * ENERGY_PER_PX);
}

void stats_on_jump(void)
{
    s_stats.exercise = clamp01_100(s_stats.exercise + EXER_PER_JUMP);
    s_stats.hunger = clamp01_100(s_stats.hunger - HUNGER_PER_JUMP);
    s_stats.energy = clamp01_100(s_stats.energy - ENERGY_PER_JUMP);
}

void stats_on_scare(void)
{
    s_stats.affection = clamp01_100(s_stats.affection - AFFECT_SCARE_COST);
}

void stats_on_eat(float amount)
{
    s_stats.hunger = clamp01_100(s_stats.hunger + amount);
}

void stats_note_date(int32_t day_serial)
{
    if (day_serial <= 0) {
        return;
    }
    if (s_day_serial != 0 && day_serial != s_day_serial) {
        s_stats.exercise = 0.0f;  // a new day, a fresh walk to take
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

    // He slept through it: gentle hunger, full-rate rest, mild fading — each
    // floored so an absence never reads as tragedy.
    float h = s_stats.hunger - sec * HUNGER_PER_S * HUNGER_SLEEP_FACTOR;
    if (h < OFFLINE_HUNGER_FLOOR) {
        h = (s_stats.hunger < OFFLINE_HUNGER_FLOOR) ? s_stats.hunger
                                                    : OFFLINE_HUNGER_FLOOR;
    }
    s_stats.hunger = clamp01_100(h);

    float a = s_stats.affection - sec * AFFECT_DECAY_PER_S * OFFLINE_AFFECT_FACTOR;
    if (a < OFFLINE_AFFECT_FLOOR) {
        a = (s_stats.affection < OFFLINE_AFFECT_FLOOR) ? s_stats.affection
                                                       : OFFLINE_AFFECT_FLOOR;
    }
    s_stats.affection = clamp01_100(a);

    s_stats.energy = clamp01_100(s_stats.energy + sec * ENERGY_SLEEP_REGEN_PER_S);

    s_pet_budget = PET_SESSION_BUDGET;
}

// ---------------------------------------------------------------------------
// Persistence (ESP only): one NVS blob, written every few minutes and before
// sleep, read once at boot.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "nvs.h"

#define STATS_MAGIC 0x50435331u  // 'PCS1'

typedef struct {
    uint32_t magic;
    float hunger, affection, energy, exercise;
    float pet_budget;
    int64_t saved_epoch;   // UTC seconds at save; 0 = clock was not valid
    int32_t day_serial;
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
    stats_blob_t b;
    size_t len = sizeof(b);
    const esp_err_t ret = nvs_get_blob(h, "stats", &b, &len);
    nvs_close(h);
    if (ret != ESP_OK || len != sizeof(b) || b.magic != STATS_MAGIC) {
        return false;
    }

    s_stats.hunger = clamp01_100(b.hunger);
    s_stats.affection = clamp01_100(b.affection);
    s_stats.energy = clamp01_100(b.energy);
    s_stats.exercise = clamp01_100(b.exercise);
    s_pet_budget = clamp01_100(b.pet_budget);
    s_day_serial = b.day_serial;
    s_loaded_epoch = b.saved_epoch;
    ESP_LOGI(TAG, "loaded H %.0f A %.0f E %.0f X %.0f (saved epoch %lld)",
             (double)s_stats.hunger, (double)s_stats.affection,
             (double)s_stats.energy, (double)s_stats.exercise,
             (long long)s_loaded_epoch);
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
        .hunger = s_stats.hunger,
        .affection = s_stats.affection,
        .energy = s_stats.energy,
        .exercise = s_stats.exercise,
        .pet_budget = s_pet_budget,
        .saved_epoch = clock_valid() ? (int64_t)time(NULL) : 0,
        .day_serial = s_day_serial,
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
            ESP_LOGI(TAG, "offline %.0f s -> H %.0f A %.0f E %.0f X %.0f",
                     gap, (double)s_stats.hunger, (double)s_stats.affection,
                     (double)s_stats.energy, (double)s_stats.exercise);
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
