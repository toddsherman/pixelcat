#include "logbook.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdcard.h"

static const char *TAG = "logbook";

#define EVENTS_PATH SDCARD_DIR "/events.csv"
#define BOOTS_PATH SDCARD_DIR "/boots.csv"
#define LOG_PATH SDCARD_DIR "/log.txt"
#define LOG_OLD_PATH SDCARD_DIR "/log.old.txt"

// Two generations of console log, capped, so a chatty fault cannot fill a
// card or grind it with writes.
#define LOG_CAP (192 * 1024)

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

// ---------------------------------------------------------------------------
// The console tee.
//
// Every ESP_LOGW and ESP_LOGE also lands in a small RAM buffer, written to
// the card on the same flush timer as the events. Console output otherwise
// exists only on the USB cable, so every warning the board emits while it is
// off the desk — the interesting ones — is lost. Info lines are deliberately
// not captured: the telemetry line alone would be megabytes a day.

#define CONSOLE_RING 3072

static char s_console[CONSOLE_RING];
static volatile int s_console_head;
static volatile bool s_console_dropped;
static volatile bool s_console_draining;
static vprintf_like_t s_prev_vprintf;
static portMUX_TYPE s_console_lock = portMUX_INITIALIZER_UNLOCKED;

// Copy without the colour escapes, so the file reads as plain text.
static int strip_ansi(const char *src, int n, char *dst, int cap)
{
    int o = 0;
    for (int i = 0; i < n && o < cap; i++) {
        if (src[i] == '\033') {
            while (i < n && src[i] != 'm') {
                i++;
            }
            continue;
        }
        dst[o++] = src[i];
    }
    return o;
}

static int console_tee(const char *fmt, va_list args)
{
    // The real console first, and with the untouched va_list — this must
    // behave exactly as before even if everything below fails.
    va_list copy;
    va_copy(copy, args);
    const int n = s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;

    // Not from an interrupt (no file-backed anything is safe there), and not
    // while the buffer is being written out.
    if (xPortInIsrContext() || s_console_draining) {
        va_end(copy);
        return n;
    }
    char raw[192];
    int len = vsnprintf(raw, sizeof(raw), fmt, copy);
    va_end(copy);
    if (len <= 0) {
        return n;
    }
    if (len > (int)sizeof(raw) - 1) {
        len = (int)sizeof(raw) - 1;
    }
    char line[192];
    const int m = strip_ansi(raw, len, line, sizeof(line));
    // "W (1234) tag: ..." — the level is the first character once the colour
    // is gone. Warnings and errors only.
    if (m <= 0 || (line[0] != 'W' && line[0] != 'E')) {
        return n;
    }
    portENTER_CRITICAL_SAFE(&s_console_lock);
    if (s_console_head + m <= CONSOLE_RING) {
        memcpy(s_console + s_console_head, line, (size_t)m);
        s_console_head += m;
    } else {
        s_console_dropped = true;  // the flush is overdue; say so in the file
    }
    portEXIT_CRITICAL_SAFE(&s_console_lock);
    return n;
}

// Written straight out of the shared buffer rather than through a copy: the
// main loop's stack has no room for another 3 KB. Lines logged during the
// write are dropped instead (s_console_draining), which costs a warning at
// most and keeps this off the stack entirely.
static void console_write(void)
{
    if (s_console_head == 0 && !s_console_dropped) {
        return;
    }
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_CAP) {
        remove(LOG_OLD_PATH);
        rename(LOG_PATH, LOG_OLD_PATH);
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        s_console_head = 0;  // no card, or a full one: do not wedge
        s_console_dropped = false;
        return;
    }
    s_console_draining = true;
    fprintf(f, "--- %lld up %llus ---\n", (long long)now_epoch(),
            (unsigned long long)(esp_timer_get_time() / 1000000));
    if (s_console_head > 0) {
        fwrite(s_console, 1, (size_t)s_console_head, f);
    }
    if (s_console_dropped) {
        fputs("W (----) logbook: console buffer overflowed\n", f);
    }
    s_console_head = 0;
    s_console_dropped = false;
    s_console_draining = false;
    fclose(f);
}

void logbook_capture_console(void)
{
    if (!s_prev_vprintf) {
        s_prev_vprintf = esp_log_set_vprintf(console_tee);
    }
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
    if (!sdcard_ready()) {
        s_head = 0;  // no card: neither ring must wedge
        s_console_head = 0;
        s_console_dropped = false;
        return;
    }
    console_write();
    if (s_head == 0) {
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
    // The whole enum through IDF 5.5. The tail matters: pwr_glitch and
    // cpu_lockup are how this board has actually misbehaved, and a short
    // table logged both of them as "?".
    static const char *const k_reasons[] = {
        "unknown", "power_on", "external", "software",   "panic",
        "int_wdt", "task_wdt", "wdt",      "deep_sleep", "brownout",
        "sdio",    "usb",      "jtag",     "efuse",      "pwr_glitch",
        "cpu_lockup",
    };
    const esp_reset_reason_t why = esp_reset_reason();
    const char *reason = ((int)why < (int)(sizeof(k_reasons) / sizeof(k_reasons[0])))
                             ? k_reasons[why]
                             : "?";

    uint32_t last_uptime = 0;
    uint8_t last_batt = 0xFF;
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "uptime", &last_uptime);
        nvs_get_u8(h, "lastbatt", &last_batt);
        nvs_set_u32(h, "uptime", 0);
        nvs_commit(h);
        nvs_close(h);
    }

    // A flat battery and a crash look identical in the reset reason, so the
    // last charge seen is what tells them apart after the fact.
    char batt[16] = ", battery ?";
    if (last_batt <= 100) {
        snprintf(batt, sizeof(batt), ", at %u%%", (unsigned)last_batt);
    }
    ESP_LOGI(TAG, "boot: %s (previous run %u s%s)", reason,
             (unsigned)last_uptime, batt);
    logbook_add(LOG_BOOT, (int)why, (int)last_uptime);

    if (!sdcard_ready()) {
        return;
    }
    FILE *f = fopen(BOOTS_PATH, "a");
    if (f) {
        fprintf(f, "%lld,%s,%u,%d\n", (long long)now_epoch(), reason,
                (unsigned)last_uptime,
                (last_batt <= 100) ? (int)last_batt : -1);
        fclose(f);
    }

    // What is actually on the card, so a silently failing writer shows up as
    // a log that never grows rather than as nothing at all.
    struct stat st;
    long ev = (stat(EVENTS_PATH, &st) == 0) ? (long)st.st_size : -1;
    long bo = (stat(BOOTS_PATH, &st) == 0) ? (long)st.st_size : -1;
    long lg = (stat(LOG_PATH, &st) == 0) ? (long)st.st_size : -1;
    ESP_LOGI(TAG, "card: events %ld B, boots %ld B, log %ld B", ev, bo, lg);
}

// Called from the flush timer: remember how long this run has lasted and how
// much charge was left, so the next boot can report both even if this one
// ends without warning. Pass -1 for an unreadable gauge.
void logbook_mark_uptime(int batt_pct)
{
    nvs_handle_t h;
    if (nvs_open("pixelcat", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "uptime", (uint32_t)(esp_timer_get_time() / 1000000));
        nvs_set_u8(h, "lastbatt",
                   (batt_pct >= 0 && batt_pct <= 100) ? (uint8_t)batt_pct
                                                      : 0xFF);
        nvs_commit(h);
        nvs_close(h);
    }
}
