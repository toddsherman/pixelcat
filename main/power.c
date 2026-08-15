#include "power.h"

#include <time.h>

#include "audio.h"
#include "battery.h"
#include "button.h"
#include "config.h"
#include "display.h"
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

static const char *TAG = "power";

static int64_t s_last_activity;

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
    bool chg;
    battery_read(&pct, &chg);
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

void power_idle_check(void)
{
    if (esp_timer_get_time() - s_last_activity < (int64_t)POWER_IDLE_S * 1000000) {
        return;
    }

    // On USB power there is nothing to save and everything to lose (light
    // sleep pauses the USB serial port, which makes the board hard to flash
    // and debug). Docked means awake — and an unreadable fuel gauge must
    // fail the same way, or one glitched I2C read puts a plugged-in board
    // to sleep and takes the serial port with it. A rehearsal overrides
    // this on purpose: it was asked for.
    int pct;
    bool plugged;
    if (s_sim_secs <= 0 && (!battery_read(&pct, &plugged) || plugged)) {
        power_note_activity();
        return;
    }

    ESP_LOGI(TAG, "idle %ds: sleeping (BOOT or PWR wakes)", POWER_IDLE_S);
    // The wake path is esp_restart, so persist the cat before going dark;
    // boot-time offline catch-up then covers however long the nap lasts.
    stats_apply_offline();
    stats_store_save();
    logbook_flush();  // the card sleeps too; get the day's events down first
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
            ESP_LOGI(TAG, "woken by BOOT");
            break;
        }
        // Raw fallbacks cover the sleep-rejected path (e.g. USB active) and
        // the expander-bound PWR button alike.
        if (gpio_get_level(BOOT_BUTTON) == 0) {
            ESP_LOGI(TAG, "woken by BOOT (poll)");
            break;
        }
        if (button_pressed_raw()) {
            ESP_LOGI(TAG, "woken by PWR");
            break;
        }
        // Docked means awake, even when the docking happens mid-sleep:
        // every ~6 s of sleep, peek at VBUS so plugging the board in
        // revives it (and its USB serial port) without a button press.
        if ((slices % 20) == 19 && s_sim_secs <= 0) {
            int pct;
            bool plugged;
            if (battery_read(&pct, &plugged) && plugged) {
                ESP_LOGI(TAG, "woken by USB power");
                break;
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
