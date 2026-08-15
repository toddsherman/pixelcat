#include "model.h"

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// EMA step per bucket observation. A daily bucket seen 14 times (2 weeks)
// with the opposite outcome moves most of the way there: (1-a)^14 ~ 0.10.
#define EMA_ALPHA 0.15f

// Dormant until he has actually seen you: ~20 sessions across ~1-2 weeks.
#define MATURE_SESSIONS 20
#define MATURE_DAYS 7

// Precision governor: target ~50% hits. Misses raise the bar (he gets
// choosier, not louder); sustained precision lowers it gently.
#define THRESH_START 0.45f
#define THRESH_MIN 0.25f
#define THRESH_MAX 0.90f
#define THRESH_UP 0.05f
#define THRESH_DOWN 0.02f
#define PRECISION_TARGET 0.50f

// Bandit: epsilon-greedy with optimistic start so every act gets tried.
#define BANDIT_EPSILON 0.20f
#define BANDIT_ALPHA 0.30f
#define BANDIT_INIT 0.60f

#define WAKES_PER_DAY_MAX 6
#define WAKE_BATTERY_MIN 30

static struct {
    float p[MODEL_BUCKETS];
    float arm_v[MODEL_PERIODS][MODEL_ARMS];
    float threshold;
    int sessions;
    int days_seen;
    int32_t last_session_day;
    int wake_hits, wake_misses;
    int wakes_today;
    int32_t wakes_day;
    int32_t last_fire_halfhour;
    int64_t last_closed_halfhour;
} m;

int64_t model_last_closed(void)
{
    return m.last_closed_halfhour;
}

void model_note_closed(int64_t abs_halfhour)
{
    m.last_closed_halfhour = abs_halfhour;
}

int model_bucket(int dow, int minutes_of_day)
{
    const int half = (minutes_of_day < 0) ? 0 : (minutes_of_day / 30) % 48;
    const bool weekend = (dow == 0 || dow == 6);
    return (weekend ? 48 : 0) + half;
}

int model_period(int minutes_of_day)
{
    const int h = minutes_of_day / 60;
    if (h >= 5 && h < 11) return 0;
    if (h >= 11 && h < 17) return 1;
    if (h >= 17 && h < 23) return 2;
    return 3;
}

void model_reset(void)
{
    for (int i = 0; i < MODEL_BUCKETS; i++) {
        m.p[i] = 0.0f;
    }
    for (int pd = 0; pd < MODEL_PERIODS; pd++) {
        for (int a = 0; a < MODEL_ARMS; a++) {
            m.arm_v[pd][a] = BANDIT_INIT;
        }
    }
    m.threshold = THRESH_START;
    m.sessions = 0;
    m.days_seen = 0;
    m.last_session_day = 0;
    m.wake_hits = 0;
    m.wake_misses = 0;
    m.wakes_today = 0;
    m.wakes_day = 0;
    m.last_fire_halfhour = -1;
    m.last_closed_halfhour = 0;
}

void model_note_session(int bucket, int32_t day_serial)
{
    (void)bucket;
    m.sessions++;
    if (day_serial != m.last_session_day) {
        m.last_session_day = day_serial;
        m.days_seen++;
    }
}

void model_close_bucket(int bucket, bool hit)
{
    if (bucket < 0 || bucket >= MODEL_BUCKETS) {
        return;
    }
    m.p[bucket] += EMA_ALPHA * ((hit ? 1.0f : 0.0f) - m.p[bucket]);
}

bool model_mature(void)
{
    return m.sessions >= MATURE_SESSIONS && m.days_seen >= MATURE_DAYS;
}

bool model_should_wake(int bucket, int battery_pct, int32_t day_serial,
                       int32_t abs_halfhour)
{
    if (!model_mature()) {
        return false;
    }
    if (battery_pct >= 0 && battery_pct < WAKE_BATTERY_MIN) {
        return false;
    }
    if (day_serial != m.wakes_day) {
        m.wakes_day = day_serial;
        m.wakes_today = 0;
    }
    if (m.wakes_today >= WAKES_PER_DAY_MAX) {
        return false;
    }
    if (abs_halfhour == m.last_fire_halfhour) {
        return false;  // one shot per half-hour slot
    }
    if (bucket < 0 || bucket >= MODEL_BUCKETS ||
        m.p[bucket] < m.threshold) {
        return false;
    }
    m.wakes_today++;
    m.last_fire_halfhour = abs_halfhour;
    return true;
}

int model_pick_arm(int period, float rnd01)
{
    if (period < 0 || period >= MODEL_PERIODS) {
        period = 0;
    }
    if (rnd01 < BANDIT_EPSILON) {
        // Explore: spread the remaining randomness across the arms.
        return (int)((rnd01 / BANDIT_EPSILON) * MODEL_ARMS) % MODEL_ARMS;
    }
    int best = 0;
    for (int a = 1; a < MODEL_ARMS; a++) {
        if (m.arm_v[period][a] > m.arm_v[period][best]) {
            best = a;
        }
    }
    return best;
}

void model_audition_result(int period, int arm, bool hit)
{
    if (period >= 0 && period < MODEL_PERIODS && arm >= 0 &&
        arm < MODEL_ARMS) {
        m.arm_v[period][arm] +=
            BANDIT_ALPHA * ((hit ? 1.0f : 0.0f) - m.arm_v[period][arm]);
    }
    if (hit) {
        m.wake_hits++;
    } else {
        m.wake_misses++;
    }
    // The governor: below-target precision raises the bar, comfortably
    // above-target lowers it a little.
    const int n = m.wake_hits + m.wake_misses;
    if (n >= 4) {
        const float precision = (float)m.wake_hits / (float)n;
        if (precision < PRECISION_TARGET) {
            m.threshold += THRESH_UP;
        } else if (precision > PRECISION_TARGET + 0.10f) {
            m.threshold -= THRESH_DOWN;
        }
        if (m.threshold > THRESH_MAX) m.threshold = THRESH_MAX;
        if (m.threshold < THRESH_MIN) m.threshold = THRESH_MIN;
    }
}

float model_bucket_p(int bucket)
{
    return (bucket >= 0 && bucket < MODEL_BUCKETS) ? m.p[bucket] : 0.0f;
}

float model_arm_value(int period, int arm)
{
    return (period >= 0 && period < MODEL_PERIODS && arm >= 0 &&
            arm < MODEL_ARMS)
               ? m.arm_v[period][arm]
               : 0.0f;
}

int model_sessions(void)
{
    return m.sessions;
}

float model_threshold(void)
{
    return m.threshold;
}

void model_wake_stats(int *hits, int *misses)
{
    *hits = m.wake_hits;
    *misses = m.wake_misses;
}

// ---------------------------------------------------------------------------
// Persistence (ESP only): one NVS blob under the pixelcat namespace.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "nvs.h"

#define MODEL_MAGIC 0x50434D31u  // 'PCM1'

typedef struct {
    uint32_t magic;
    float p[MODEL_BUCKETS];
    float arm_v[MODEL_PERIODS][MODEL_ARMS];
    float threshold;
    int32_t sessions, days_seen, last_session_day;
    int32_t wake_hits, wake_misses;
    int32_t wakes_today, wakes_day;
    int32_t last_fire_halfhour;
    int64_t last_closed_halfhour;
} model_blob_t;

static const char *TAG = "model";

bool model_store_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    model_blob_t b;
    size_t len = sizeof(b);
    const esp_err_t ret = nvs_get_blob(h, "model", &b, &len);
    nvs_close(h);
    if (ret != ESP_OK || len != sizeof(b) || b.magic != MODEL_MAGIC) {
        return false;
    }
    for (int i = 0; i < MODEL_BUCKETS; i++) {
        m.p[i] = b.p[i];
    }
    for (int pd = 0; pd < MODEL_PERIODS; pd++) {
        for (int a = 0; a < MODEL_ARMS; a++) {
            m.arm_v[pd][a] = b.arm_v[pd][a];
        }
    }
    m.threshold = b.threshold;
    m.sessions = b.sessions;
    m.days_seen = b.days_seen;
    m.last_session_day = b.last_session_day;
    m.wake_hits = b.wake_hits;
    m.wake_misses = b.wake_misses;
    m.wakes_today = b.wakes_today;
    m.wakes_day = b.wakes_day;
    m.last_fire_halfhour = b.last_fire_halfhour;
    m.last_closed_halfhour = b.last_closed_halfhour;
    ESP_LOGI(TAG, "loaded: %d sessions, %d days, thresh %.2f, %d/%d wakes",
             m.sessions, m.days_seen, (double)m.threshold, m.wake_hits,
             m.wake_hits + m.wake_misses);
    return true;
}

void model_store_save(void)
{
    model_blob_t b = {.magic = MODEL_MAGIC};
    for (int i = 0; i < MODEL_BUCKETS; i++) {
        b.p[i] = m.p[i];
    }
    for (int pd = 0; pd < MODEL_PERIODS; pd++) {
        for (int a = 0; a < MODEL_ARMS; a++) {
            b.arm_v[pd][a] = m.arm_v[pd][a];
        }
    }
    b.threshold = m.threshold;
    b.sessions = m.sessions;
    b.days_seen = m.days_seen;
    b.last_session_day = m.last_session_day;
    b.wake_hits = m.wake_hits;
    b.wake_misses = m.wake_misses;
    b.wakes_today = m.wakes_today;
    b.wakes_day = m.wakes_day;
    b.last_fire_halfhour = m.last_fire_halfhour;
    b.last_closed_halfhour = m.last_closed_halfhour;
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed; model not saved");
        return;
    }
    if (nvs_set_blob(h, "model", &b, sizeof(b)) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "NVS write failed; model not saved");
    }
    nvs_close(h);
}

#endif  // ESP_PLATFORM
