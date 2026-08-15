#include "power.h"

#include "audio.h"
#include "battery.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "stats.h"
#include "driver/gpio.h"
#include "esp_log.h"
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

void power_note_activity(void)
{
    s_last_activity = esp_timer_get_time();
}

void power_idle_check(void)
{
    if (esp_timer_get_time() - s_last_activity < (int64_t)POWER_IDLE_S * 1000000) {
        return;
    }

    // On USB power there is nothing to save and everything to lose (light
    // sleep pauses the USB serial port, which makes the board hard to flash
    // and debug). Docked means awake.
    int pct;
    bool plugged;
    if (battery_read(&pct, &plugged) && plugged) {
        power_note_activity();
        return;
    }

    ESP_LOGI(TAG, "idle %ds: sleeping (BOOT or PWR wakes)", POWER_IDLE_S);
    // The wake path is esp_restart, so persist the cat before going dark;
    // boot-time offline catch-up then covers however long the nap lasts.
    stats_apply_offline();
    stats_store_save();
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
    for (;;) {
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
        // Feed the idle task between slices so the watchdog stays calm; this
        // also paces the loop if light sleep keeps being rejected.
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Resume through the known-good boot path (panel hardware reset, NTP).
    esp_restart();
}
