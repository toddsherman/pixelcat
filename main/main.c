// PixelCat: a pixel art cat that purrs and reacts when you pet it.
//
// One loop task drives touch, behaviour and the display at CAT_FPS. Audio has
// its own task inside audio.c; the loop just tells it how hard to purr.

#include <string.h>

#include "audio.h"
#include "battery.h"
#include "button.h"
#include "cat.h"
#include "config.h"
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "touch.h"

#define I2C_PORT I2C_NUM_0

static const char *TAG = "pixelcat";

static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t i2c_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static void cat_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(1000 / CAT_FPS);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();
    int64_t last_log_us = last_us;
    int64_t last_batt_us = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) * 1e-6f;
        last_us = now;
        if (dt > 0.1f) {
            dt = 0.1f;
        }

        touch_state_t ts;
        touch_read(&ts);

        const cat_touch_t ct = {.down = ts.down, .x = ts.x, .y = ts.y};
        const float shake = imu_shake();
        cat_update(dt, &ct, shake, imu_tilt_x());

        audio_set_purr(cat_purr_level());
        if (cat_take_chirp()) {
            audio_chirp();
        }
        if (cat_take_hiss()) {
            audio_hiss();
        }
        if (cat_take_step()) {
            audio_step();
        }
        if (cat_take_boing()) {
            audio_boing();
        }
        if (cat_take_slurp()) {
            audio_slurp();
        }
        if (cat_take_swipe()) {
            audio_swipe();
        }
        const int dash_dir = cat_take_dash();
        if (dash_dir) {
            audio_dash(dash_dir);
        }

        cat_render();

        if (now - last_batt_us > 5000000) {
            last_batt_us = now;
            int pct;
            bool chg;
            if (battery_read(&pct, &chg)) {
                cat_set_battery(pct, chg);
            }
        }

        if (button_take_short_press()) {
            ESP_LOGI(TAG, "PWR pressed");
        }

        if (now - last_log_us > 3000000) {
            last_log_us = now;
            float g[3];
            imu_gravity(g);
            int st1, st2;
            battery_raw(&st1, &st2);
            ESP_LOGI(TAG, "state %d purr %.2f touch %d (%d,%d) flush_err %d | grav %.1f %.1f %.1f tilt %.1f | batt st1 %02x st2 %02x",
                     (int)cat_state(), (double)cat_purr_level(), (int)ts.down, ts.x, ts.y, cat_flush_errors(),
                     (double)g[0], (double)g[1], (double)g[2], (double)imu_tilt_x(), st1, st2);
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

// Push one full black frame. The panel misbehaved when the first real frames
// raced the rest of the init sequence; a warm-up frame straight after panel
// init plus short settle delays (below) is the configuration measured to boot
// reliably. If the screen ever stays black again, suspect this sequencing.
static void warmup_frame(void)
{
    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        memset(buf, 0, LCD_H_RES * BAND_ROWS * sizeof(uint16_t));
        display_flush_band(band, buf);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PixelCat starting");

    ESP_ERROR_CHECK(i2c_init());
    ESP_ERROR_CHECK(display_init(s_i2c_bus));
    ESP_ERROR_CHECK(display_set_brightness(DISPLAY_BRIGHTNESS));

    warmup_frame();
    vTaskDelay(pdMS_TO_TICKS(250));

    if (touch_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no touch: the cat cannot be petted");
    }
    if (imu_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no IMU: the cat cannot be offended by shaking");
    }
    if (battery_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no fuel gauge: battery bar disabled");
    }
    if (audio_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no audio: the cat purrs in spirit only");
    }
    if (button_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the button");
    }

    // Let the codec and I2S settle before frames start flowing.
    vTaskDelay(pdMS_TO_TICKS(750));

    cat_init();

    xTaskCreatePinnedToCore(cat_task, "cat", 6144, NULL, 5, NULL, 0);
}
