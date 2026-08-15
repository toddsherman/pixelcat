#include "power.h"

#include "audio.h"
#include "button.h"
#include "config.h"
#include "display.h"
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

    ESP_LOGI(TAG, "idle %ds: sleeping (BOOT or PWR wakes)", POWER_IDLE_S);
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
        esp_light_sleep_start();

        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_GPIO) {
            ESP_LOGI(TAG, "woken by BOOT");
            break;
        }
        if (button_pressed_raw()) {
            ESP_LOGI(TAG, "woken by PWR");
            break;
        }
        // Feed the idle task between sleep slices so the watchdog stays calm.
        vTaskDelay(1);
    }

    // Resume through the known-good boot path (panel hardware reset, NTP).
    esp_restart();
}
