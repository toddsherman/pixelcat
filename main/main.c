// PixelCat: a pixel art cat that purrs and reacts when you pet it.
//
// One loop task drives touch, behaviour and the display at CAT_FPS. Audio has
// its own task inside audio.c; the loop just tells it how hard to purr.

#include <string.h>

#include "audio.h"
#include "battery.h"
#include "power.h"
#include "button.h"
#include "cat.h"
#include "config.h"
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cat_bg.h"
#include "imu.h"
#include "nvs_flash.h"
#include "rtc.h"
#include "stats.h"
#include "wifi_time.h"
#include "sun_table.h"
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

// ---------------------------------------------------------------------------
// The lake follows the real sun at ZIP 94403: the table holds sunrise/sunset
// in PST, and the US DST rule (second Sunday of March to first Sunday of
// November) adds the hour when active.
// ---------------------------------------------------------------------------

static int day_of_year(int year, int mon, int day)
{
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int doy = cum[mon - 1] + day;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (leap && mon > 2) {
        doy++;
    }
    return doy;
}

static int weekday(int y, int m, int d)  // 0 = Sunday (Sakamoto)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static bool dst_active(int year, int mon, int day)
{
    if (mon < 3 || mon > 11) return false;
    if (mon > 3 && mon < 11) return true;
    if (mon == 3) {
        const int second_sunday = 8 + (7 - weekday(year, 3, 8)) % 7;
        return day >= second_sunday;
    }
    const int first_sunday = 1 + (7 - weekday(year, 11, 1)) % 7;
    return day < first_sunday;
}

static int daypart_for(int year, int mon, int day, int minutes)
{
    if (minutes < 0) {
        return BG_DAY;
    }
    const int doy = day_of_year(year, mon, day);
    const int dst = dst_active(year, mon, day) ? 60 : 0;
    const int rise = k_sunrise[doy - 1] + dst;
    const int set = k_sunset[doy - 1] + dst;

    // The sky only visibly brightens in the last ~10 minutes before sunrise,
    // so night holds until then.
    if (minutes >= rise - 10 && minutes < rise + 25) return BG_DAWN;
    if (minutes >= rise + 25 && minutes < set - 30) return BG_DAY;
    // Real darkness lands about half an hour after sunset.
    if (minutes >= set - 30 && minutes < set + 10) return BG_DUSK;
    if (minutes >= set + 10 && minutes < set + 30) return BG_TWILIGHT;
    return BG_NIGHT;
}

static void cat_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(1000 / CAT_FPS);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();
    int64_t last_log_us = last_us;
    int64_t last_batt_us = 0;
    int64_t last_save_us = 0;
    int daypart_for_log = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) * 1e-6f;
        last_us = now;
        if (dt > 0.1f) {
            dt = 0.1f;
        }

        touch_state_t ts;
        touch_read(&ts);

        // Rotate touch into the logical landscape frame: viewer x runs down
        // the portrait panel, viewer y runs from panel right to panel left.
        const cat_touch_t ct = {.down = ts.down,
                                .x = ts.y,
                                .y = (int16_t)(LCD_H_RES - 1 - ts.x)};
        const float shake = imu_shake();
        cat_update(dt, &ct, shake, imu_tilt_x());

        if (ts.down || shake > 0.8f) {
            power_note_activity();
        }
        power_idle_check();

        // The stats engine waits for a trustworthy clock before applying the
        // offline gap: NTP normally lands within seconds; past 45 s settle
        // for the PCF-seeded clock (which under-counts, kindly).
        if (!stats_catchup_done() &&
            (wifi_time_synced() || now > 45 * 1000000LL)) {
            stats_apply_offline();
        }
        stats_tick(dt, cat_state() == CAT_SLEEPING, cat_purr_level());
        stats_on_walk(cat_take_walked());

        audio_set_purr(cat_purr_level());
        if (cat_take_chirp()) {
            audio_chirp();
        }
        if (cat_take_hiss()) {
            audio_hiss();
            stats_on_scare();
            stats_store_save();
        }
        if (cat_take_step()) {
            audio_step();
        }
        if (cat_take_boing()) {
            audio_boing();
            stats_on_jump();
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
            stats_on_jump();
        }

        if (stats_catchup_done() && now - last_save_us > 300 * 1000000LL) {
            last_save_us = now;
            stats_store_save();
        }

        cat_render();

        if (now - last_batt_us > 5000000) {
            last_batt_us = now;
            int pct;
            bool chg;
            if (battery_read(&pct, &chg)) {
                cat_set_battery(pct, chg);
            }
            int yy, mm, dd;
            if (pcf_date(&yy, &mm, &dd)) {
                daypart_for_log = daypart_for(yy, mm, dd, pcf_minutes_of_day());
                cat_set_daypart(daypart_for_log);
                stats_note_date(yy * 10000 + mm * 100 + dd);
            }
        }

        if (button_take_short_press()) {
            ESP_LOGI(TAG, "PWR pressed");
            power_note_activity();
        }

        if (now - last_log_us > 3000000) {
            last_log_us = now;
            float g[3];
            imu_gravity(g);
            int st1, st2;
            battery_raw(&st1, &st2);
            const stats_t *st = stats_get();
            ESP_LOGI(TAG, "state %d purr %.2f touch %d (%d,%d) flush_err %d | grav %.1f %.1f %.1f tilt %.1f | batt st1 %02x st2 %02x | clock %d part %d | H %d A %d E %d X %d",
                     (int)cat_state(), (double)cat_purr_level(), (int)ts.down, ts.x, ts.y, cat_flush_errors(),
                     (double)g[0], (double)g[1], (double)g[2], (double)imu_tilt_x(), st1, st2,
                     pcf_minutes_of_day(), daypart_for_log,
                     (int)st->hunger, (int)st->affection, (int)st->energy, (int)st->exercise);
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
    if (pcf_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no RTC: the scene stays in daylight");
    }

    // NVS before anything that touches it (wifi_time's own init call is then
    // a harmless no-op) — the stats blob loads from here.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    stats_reset();
    if (nvs == ESP_OK && stats_store_load()) {
        ESP_LOGI(TAG, "stats restored");
    } else {
        ESP_LOGI(TAG, "fresh cat");
    }

    wifi_time_start();
    if (audio_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "no audio: the cat purrs in spirit only");
    }
    if (button_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the button");
    }

    // Let the codec and I2S settle before frames start flowing.
    vTaskDelay(pdMS_TO_TICKS(750));

    cat_init();
    power_note_activity();

    xTaskCreatePinnedToCore(cat_task, "cat", 6144, NULL, 5, NULL, 0);
}
