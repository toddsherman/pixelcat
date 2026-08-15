#include "logbook.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdcard.h"

static const char *TAG = "logbook";

#define EVENTS_PATH SDCARD_DIR "/events.csv"
#define BOOTS_PATH SDCARD_DIR "/boots.csv"

// Small enough to cost nothing, large enough that a busy minute of play
// never spills between flushes.
#define RING 48

typedef struct {
    int64_t epoch;
    uint8_t ev;
    int16_t a, b;
} entry_t;

static entry_t s_ring[RING];
static int s_head;
static bool s_overflowed;

static const char *const k_names[] = {
    "boot", "session", "audition", "wake", "full", "scare",
    "reconcile", "feed", "play", "sleep", "day",
};

static int64_t now_epoch(void)
{
    const time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (lt.tm_year + 1900 >= 2020) ? (int64_t)t : 0;
}

void logbook_init(void)
{
    s_head = 0;
    s_overflowed = false;
}

void logbook_add(log_event_t ev, int a, int b)
{
    if (s_head >= RING) {
        s_overflowed = true;  // flush is overdue; drop rather than block
        return;
    }
    s_ring[s_head].epoch = now_epoch();
    s_ring[s_head].ev = (uint8_t)ev;
    s_ring[s_head].a = (int16_t)a;
    s_ring[s_head].b = (int16_t)b;
    s_head++;
}

void logbook_flush(void)
{
    if (s_head == 0 || !sdcard_ready()) {
        s_head = 0;  // no card: the ring must not wedge
        return;
    }
    FILE *f = fopen(EVENTS_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "cannot append to %s", EVENTS_PATH);
        s_head = 0;
        return;
    }
    for (int i = 0; i < s_head; i++) {
        const entry_t *e = &s_ring[i];
        const char *name = (e->ev < sizeof(k_names) / sizeof(k_names[0]))
                               ? k_names[e->ev]
                               : "?";
        fprintf(f, "%lld,%s,%d,%d\n", (long long)e->epoch, name, e->a, e->b);
    }
    if (s_overflowed) {
        fprintf(f, "%lld,overflow,0,0\n", (long long)now_epoch());
        s_overflowed = false;
    }
    fclose(f);
    s_head = 0;
}

void logbook_forget(void)
{
    s_head = 0;
    s_overflowed = false;
    if (sdcard_ready()) {
        remove(EVENTS_PATH);
    }
    ESP_LOGI(TAG, "event history deleted");
}

// Boot bookkeeping. The previous run's length lives in NVS so an unexpected
// reset is visible as "ran 43 s, then brownout" rather than a silent gap.
void logbook_note_boot(void)
{
    static const char *const k_reasons[] = {
        "unknown", "power_on", "external", "software", "panic",
        "int_wdt",  "task_wdt", "wdt", "deep_sleep", "brownout", "sdio",
    };
    const esp_reset_reason_t why = esp_reset_reason();
    const char *reason = ((int)why < (int)(sizeof(k_reasons) / sizeof(k_reasons[0])))
                             ? k_reasons[why]
                             : "?";

    uint32_t last_uptime = 0;
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "uptime", &last_uptime);
        nvs_set_u32(h, "uptime", 0);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "boot: %s (previous run %u s)", reason, (unsigned)last_uptime);
    logbook_add(LOG_BOOT, (int)why, (int)last_uptime);

    if (!sdcard_ready()) {
        return;
    }
    FILE *f = fopen(BOOTS_PATH, "a");
    if (f) {
        fprintf(f, "%lld,%s,%u\n", (long long)now_epoch(), reason,
                (unsigned)last_uptime);
        fclose(f);
    }
}

// Called from the flush timer: remember how long this run has lasted, so the
// next boot can report it even if this one ends abruptly.
void logbook_mark_uptime(void)
{
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "uptime", (uint32_t)(esp_timer_get_time() / 1000000));
        nvs_commit(h);
        nvs_close(h);
    }
}
