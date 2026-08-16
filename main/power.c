#include "power.h"

#include <time.h>

#include "audio.h"
#include "battery.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "imu.h"
#include "touch.h"
#include "logbook.h"
#include "model.h"
#include "stats.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POWER_IDLE_S 60
#define BOOT_BUTTON GPIO_NUM_0
#define POLL_US 300000

// Two different idle states, because USB power changes what is worth saving.
//
// On battery: light sleep. The CPU stops, and that is the whole point.
//
// On USB: dozing. The screen goes off and the cat stops being drawn, but the
// CPU stays up and the USB peripheral stays enumerated — light sleep suspends
// it, which drops the serial port and makes the board unflashable until
// somebody touches it. Charging is the PMU's job either way and continues
// through both. Dozing costs power that a plugged-in board is not short of,
// and buys a device that is still there when you reach for it.

// Wake on being handled, not just on buttons. Neither touch nor the IMU has
// an interrupt line routed on this board, so both are polled in the same
// 300 ms slice that already polls the PWR button over I2C.
#define WAKE_TOUCH 1
#define WAKE_MOTION_MSS 2.0f  // vector change that counts as a deliberate move
#define WAKE_MOTION_SLICES 2  // consecutive, so a desk bump is not enough

static const char *TAG = "power";

static int64_t s_last_activity;

// Dozing: screen off, everything else running, USB still enumerated.
static bool s_dozing;
static int64_t s_last_doze_check;

// Survives the esp_restart that ends every sleep: the sleep loop's decision
// to wake proactively, handed to main on the next boot.
RTC_DATA_ATTR static int g_proactive;
RTC_DATA_ATTR static int g_proactive_arm;
RTC_DATA_ATTR static int g_proactive_period;

void power_note_activity(void)
{
    s_last_activity = esp_timer_get_time();
}

static int s_sim_secs;

void power_simulate_wake(int seconds)
{
    s_sim_secs = seconds;
    power_sleep_now();
}

void power_sleep_now(void)
{
    s_last_activity =
        esp_timer_get_time() - (int64_t)(POWER_IDLE_S + 1) * 1000000;
}

bool power_take_proactive(int *arm, int *period)
{
    if (!g_proactive) {
        return false;
    }
    g_proactive = 0;
    *arm = g_proactive_arm;
    *period = g_proactive_period;
    return true;
}

// While light-sleeping, ask the schedule model (already loaded in RAM)
// whether this half-hour deserves a wake. Called every ~20 s of sleep.
static bool proactive_check(void)
{
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2020) {
        return false;  // no trustworthy clock, no predictions
    }
    int pct = -1;
    bool chg = false;
    battery_read(&pct, &chg);
    // The 30% floor exists to stop him spending a nearly flat battery on
    // guesses. Plugged in there is nothing to conserve, so the floor is
    // waived — which also keeps a badly-reading fuel gauge from silencing
    // him permanently on a desk. -1 is the "don't check" value.
    if (chg) {
        pct = -1;
    }
    const int minutes = lt.tm_hour * 60 + lt.tm_min;
    const int bucket = model_bucket(lt.tm_wday, minutes);
    const int32_t day_serial = (lt.tm_year + 1900) * 10000 +
                               (lt.tm_mon + 1) * 100 + lt.tm_mday;
    if (!model_should_wake(bucket, pct, day_serial,
                           (int32_t)(now / 1800))) {
        return false;
    }
    const int period = model_period(minutes);
    g_proactive_arm =
        model_pick_arm(period, (float)(esp_random() % 1000) / 1000.0f);
    g_proactive_period = period;
    g_proactive = 1;
    model_store_save();  // the fire is on the books before the restart
    ESP_LOGI(TAG, "proactive wake: bucket %d p %.2f arm %d", bucket,
             (double)model_bucket_p(bucket), g_proactive_arm);
    return true;
}

bool power_dozing(void)
{
    return s_dozing;
}

// Bring the panel back and count it as activity. Safe to call when not
// dozing, so the main loop can fire it on any sign of life.
void power_wake_screen(void)
{
    if (!s_dozing) {
        return;
    }
    s_dozing = false;
    audio_set_muted(false);
    display_power_on();
    power_note_activity();
    ESP_LOGW(TAG, "screen back on");
}

void power_idle_check(void)
{
    if (esp_timer_get_time() - s_last_activity < (int64_t)POWER_IDLE_S * 1000000) {
        return;
    }

    int pct = -1;
    bool plugged = false;
    if (!battery_read(&pct, &plugged)) {
        pct = -1;
        plugged = false;
    }

    // Plugged in: doze instead of sleeping. Screen off, USB intact. Returns
    // every tick so the main loop keeps running — which is what keeps the
    // port alive and lets touch, movement or a button end it immediately.
    if (plugged && s_sim_secs <= 0) {
        if (!s_dozing) {
            s_dozing = true;
            audio_set_muted(true);
            display_power_off();
            ESP_LOGW(TAG, "idle %ds: screen off, staying awake on USB",
                     POWER_IDLE_S);
        }
        // Still his schedule to keep. A fire here does not restart the
        // board; main picks the audition up on the next tick.
        if (esp_timer_get_time() - s_last_doze_check > 20000000) {
            s_last_doze_check = esp_timer_get_time();
            if (proactive_check()) {
                power_wake_screen();
            }
        }
        return;
    }

    ESP_LOGW(TAG, "idle %ds: sleeping (BOOT or PWR wakes)", POWER_IDLE_S);
    // The wake path is esp_restart, so persist the cat before going dark;
    // boot-time offline catch-up then covers however long the nap lasts.
    stats_apply_offline();
    stats_store_save();
    // The card sleeps too: get the events, the warnings, and how far this
    // run got down before it does.
    {
        int spct = -1;
        bool splugged;
        if (!battery_read(&spct, &splugged)) {
            spct = -1;
        }
        logbook_mark_uptime(spct);
    }
    logbook_flush();
    audio_stop();
    display_power_off();

    // BOOT is a real GPIO: instant wake on press (active low, external pullup).
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    gpio_wakeup_enable(BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    // The PWR button lives behind the I2C expander (the AXP2101 IRQ line is
    // not routed on this board), so wake for ~1 ms every 300 ms to poll it.
    int slices = 0;
    int batt_slices = 0;
    int moved = 0;
    bool was_plugged = plugged;  // USB wakes on the edge, not the level
    const int64_t slept_at = esp_timer_get_time();
    for (;;) {
        // A rehearsal: wake on the clock and pretend the model asked.
        if (s_sim_secs > 0 &&
            esp_timer_get_time() - slept_at >= (int64_t)s_sim_secs * 1000000) {
            s_sim_secs = 0;
            g_proactive_arm = ENTICE_MEOW;
            g_proactive_period = 0;
            g_proactive = 1;
            ESP_LOGI(TAG, "rehearsed wake");
            break;
        }
        esp_sleep_enable_timer_wakeup(POLL_US);
        const esp_err_t slept = esp_light_sleep_start();

        if (slept == ESP_OK &&
            esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
            ESP_LOGW(TAG, "woken by BOOT");
            break;
        }
        // Raw fallbacks cover the sleep-rejected path (e.g. USB active) and
        // the expander-bound PWR button alike.
        if (gpio_get_level(BOOT_BUTTON) == 0) {
            ESP_LOGI(TAG, "woken by BOOT (poll)");
            break;
        }
        if (button_pressed_raw()) {
            ESP_LOGW(TAG, "woken by PWR");
            break;
        }
        // A finger on the glass wakes him, charging or not.
#if WAKE_TOUCH
        if (s_sim_secs <= 0) {
            touch_state_t tsl;
            if (touch_read(&tsl) && tsl.down) {
                ESP_LOGW(TAG, "woken by touch");
                break;
            }
        }
#endif
        // So does being picked up. Two consecutive slices of real movement,
        // so setting something down on the same desk does not count.
        if (s_sim_secs <= 0) {
            if (imu_delta() > WAKE_MOTION_MSS) {
                if (++moved >= WAKE_MOTION_SLICES) {
                    ESP_LOGW(TAG, "woken by movement");
                    break;
                }
            } else {
                moved = 0;
            }
        }
        // VBUS is an edge now, not a level: he sleeps quite happily while
        // charging, so only the act of plugging in wakes him. Sampled on the
        // same schedule as before.
        if ((slices % 20) == 19 && s_sim_secs <= 0) {
            int pct;
            bool now_plugged;
            if (battery_read(&pct, &now_plugged)) {
                if (now_plugged && !was_plugged) {
                    ESP_LOGW(TAG, "woken by USB power");
                    break;
                }
                was_plugged = now_plugged;
                // Most of a discharge happens right here, asleep. Sampling
                // only while awake would miss the part of the curve the
                // question is actually about. Every ~5 min of sleep.
                if (++batt_slices >= 50) {
                    batt_slices = 0;
                    int s1, s2;
                    battery_raw(&s1, &s2);
                    logbook_battery_sample(pct, battery_millivolts(), s1, s2);
                }
            }
        }
        // Every ~20 s of sleep, let the schedule model consider waking him.
        if (++slices >= 64) {
            slices = 0;
            if (proactive_check()) {
                break;
            }
        }
        // Feed the idle task between slices so the watchdog stays calm; this
        // also paces the loop if light sleep keeps being rejected.
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Resume through the known-good boot path (panel hardware reset, NTP).
    esp_restart();
}
