// PixelCat: a pixel art cat that purrs and reacts when you pet it.
//
// One loop task drives touch, behaviour and the display at CAT_FPS. Audio has
// its own task inside audio.c; the loop just tells it how hard to purr.

#include <math.h>
#include <string.h>
#include <time.h>

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
#include "logbook.h"
#include "model.h"
#include "nvs_flash.h"
#include "rtc.h"
#include "sdcard.h"
#include "stats.h"
#include "world.h"
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

// ---------------------------------------------------------------------------
// Schedule-model bookkeeping and the proactive audition.
// ---------------------------------------------------------------------------

static int64_t s_cur_half;        // absolute half-hour being observed
static bool s_half_hit;
static int64_t s_last_act_us = -1;

static int64_t s_audition_until_us;  // 0 = no audition running
static int s_audition_arm = -1;
static int s_audition_period;
static bool s_entice_started;
static int s_meows_left;
static int64_t s_next_meow_us;
static int s_pending_arm = -1;    // proactive boot handoff into cat_task
static int s_pending_period;

static bool wall_clock(struct tm *lt)
{
    const time_t now = time(NULL);
    localtime_r(&now, lt);
    return lt->tm_year + 1900 >= 2020;
}

// Replay every half-hour slept through as a quiet observation — that is how
// he learns the hours you are never around.
static void model_replay_gap(int64_t upto_half)
{
    int64_t from = model_last_closed() + 1;
    if (model_last_closed() == 0 || from > upto_half) {
        from = upto_half;
    }
    if (upto_half - from > 672) {  // cap at two weeks of replay
        from = upto_half - 672;
    }
    for (int64_t h = from; h < upto_half; h++) {
        const time_t mid = (time_t)(h * 1800 + 900);
        struct tm lt;
        localtime_r(&mid, &lt);
        model_close_bucket(
            model_bucket(lt.tm_wday, lt.tm_hour * 60 + lt.tm_min), false);
    }
    model_note_closed(upto_half - 1);
}

static void audition_start(int arm, int period, int64_t now_us)
{
    s_audition_until_us = now_us + 5LL * 60 * 1000000;
    s_audition_arm = arm;
    s_audition_period = period;
    s_entice_started = false;
    s_meows_left = 3;
    s_next_meow_us = now_us + 1200000;
    ESP_LOGI("model", "audition: arm %d period %d", arm, period);
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

    if (s_pending_arm >= 0) {
        // This boot was the model's idea: he opens with meows from
        // off-screen and his learned act once he pads in.
        audition_start(s_pending_arm, s_pending_period, esp_timer_get_time());
        s_pending_arm = -1;
    }

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
        const float tilt = imu_tilt_x();
        cat_update(dt, &ct, shake, tilt);

        const bool button_press = button_take_short_press();
        if (button_press) {
            ESP_LOGI(TAG, "PWR pressed");
        }
        const bool user_act =
            ts.down || shake > 0.8f || fabsf(tilt) > 2.0f || button_press;

        if (ts.down || shake > 0.8f || button_press) {
            power_note_activity();
        }
        power_idle_check();

        // --- schedule model: he watches when you show up ---
        if (user_act && s_cur_half != 0) {
            s_half_hit = true;
            if (s_last_act_us < 0 ||
                now - s_last_act_us > 15LL * 60 * 1000000) {
                struct tm lt;
                if (wall_clock(&lt)) {
                    const int bucket =
                        model_bucket(lt.tm_wday, lt.tm_hour * 60 + lt.tm_min);
                    model_note_session(
                        bucket, (lt.tm_year + 1900) * 10000 +
                                    (lt.tm_mon + 1) * 100 + lt.tm_mday);
                    logbook_add(LOG_SESSION, bucket,
                                s_audition_until_us ? 1 : 0);
                }
            }
            s_last_act_us = now;
        }

        // --- the audition: meows, the act, and the verdict ---
        if (s_audition_until_us) {
            power_note_activity();  // he holds the stage until it resolves
            if (now >= s_next_meow_us &&
                (s_meows_left > 0 || s_audition_arm == ENTICE_MEOW)) {
                audio_meow(MEOW_VARIANT);
                if (s_meows_left > 0) {
                    s_meows_left--;
                }
                s_next_meow_us =
                    now + ((s_meows_left > 0) ? 2500000LL : 6000000LL);
            }
            if (!s_entice_started && cat_state() != CAT_ABSENT) {
                cat_entice(s_audition_arm);
                s_entice_started = true;
            }
            if (user_act) {
                model_audition_result(s_audition_period, s_audition_arm,
                                      true);
                model_store_save();
                logbook_add(LOG_AUDITION, s_audition_arm, 1);
                cat_entice_stop();
                s_audition_until_us = 0;
                ESP_LOGI("model", "audition hit");
            } else if (now > s_audition_until_us) {
                model_audition_result(s_audition_period, s_audition_arm,
                                      false);
                model_store_save();
                logbook_add(LOG_AUDITION, s_audition_arm, 0);
                cat_entice_stop();
                s_audition_until_us = 0;
                ESP_LOGI("model", "audition miss; straight back to sleep");
                power_sleep_now();
            }
        }
#if MODEL_DEBUG_FIRE_S
        {
            static bool debug_fired;
            if (!debug_fired && !s_audition_until_us &&
                now > (int64_t)MODEL_DEBUG_FIRE_S * 1000000) {
                debug_fired = true;
                struct tm lt;
                const int mins = wall_clock(&lt)
                                     ? lt.tm_hour * 60 + lt.tm_min
                                     : 12 * 60;
                audition_start(ENTICE_MEOW, model_period(mins), now);
            }
        }
#endif

        // The stats engine waits for a trustworthy clock before applying the
        // offline gap: NTP normally lands within seconds; past 45 s settle
        // for the PCF-seeded clock (which under-counts, kindly).
        if (!stats_catchup_done() &&
            (wifi_time_synced() || now > 45 * 1000000LL)) {
            stats_apply_offline();
        }
        stats_tick(dt, cat_state() == CAT_SLEEPING, cat_purr_level());
        stats_on_walk(cat_take_walked());

        // The ear: any sharp sound summons an absent cat (and nothing else).
        if (audio_take_sound()) {
            cat_hear_sound();
        }
        if (cat_take_summon()) {
            power_note_activity();
        }
        if (cat_take_reconcile()) {
            stats_on_reconcile();
            stats_store_save();
            logbook_add(LOG_RECONCILE, 0, 0);
        }

        audio_set_purr(cat_purr_level());
        if (cat_take_chirp()) {
            audio_chirp();
        }
        if (cat_take_hiss()) {
            audio_hiss();
            stats_on_scare(cat_scare_level());
            stats_store_save();
            logbook_add(LOG_SCARE, cat_scare_level(), 0);
        }
        // The heart emptying, one row and one falling tone at a time.
        {
            const int beep = stats_take_scare_beep();
            if (beep >= 0) {
                audio_beep(beep);
            }
        }
        if (cat_take_step()) {
            audio_step();
        }
        if (cat_take_boing()) {
            audio_boing();  // pounces are play, not exercise
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
            // Exercise comes from deliberate dashes only — panicked flight
            // after a scare earns him nothing.
            if (cat_state() != CAT_HIDING) {
                stats_on_dash();
            }
        }
        if (cat_take_bite()) {
            // One mouthful, one gauge row: a bowl is a full meal, eaten.
            stats_on_eat(100.0f / STATS_GAUGE_ROWS);
        }
        if (cat_take_eat()) {
            stats_store_save();
            logbook_add(LOG_FEED, 0, 0);
        }
        if (cat_take_play_hit()) {
            stats_on_play_hit();
        }

        if (stats_catchup_done() && now - last_save_us > 300 * 1000000LL) {
            last_save_us = now;
            stats_store_save();
            logbook_flush();
            logbook_mark_uptime();
        }

        // Gauges refresh every frame so each pixel row lights the moment
        // its threshold crosses, never two at once.
        {
            const stats_t *sv = stats_get();
            cat_set_stats((int)sv->food, (int)sv->affection,
                          (int)sv->exercise, (int)sv->play, (int)sv->sleep);
            const int streaks[5] = {
                stats_streak(ST_PLAY), stats_streak(ST_FOOD),
                stats_streak(ST_LOVE), stats_streak(ST_EXER),
                stats_streak(ST_SLEEP),
            };
            const int hits[5] = {
                stats_hit_today(ST_PLAY), stats_hit_today(ST_FOOD),
                stats_hit_today(ST_LOVE), stats_hit_today(ST_EXER),
                stats_hit_today(ST_SLEEP),
            };
            cat_set_streaks(streaks, hits);
        }

        cat_render();

        if (now - last_batt_us > 5000000) {
            last_batt_us = now;
            int pct;
            bool chg;
            if (battery_read(&pct, &chg)) {
                cat_set_battery(pct, chg);
            }
            stats_set_trust(cat_scare_level(), cat_wary());

            // Half-hour bucket bookkeeping, once the clock is trustworthy.
            struct tm lt;
            if (stats_catchup_done() && wall_clock(&lt)) {
                const int64_t half = (int64_t)time(NULL) / 1800;
                if (s_cur_half == 0) {
                    model_replay_gap(half);  // slept-through = quiet
                    s_cur_half = half;
                } else if (half != s_cur_half) {
                    const time_t mid = (time_t)(s_cur_half * 1800 + 900);
                    struct tm bt;
                    localtime_r(&mid, &bt);
                    const int b = model_bucket(bt.tm_wday,
                                               bt.tm_hour * 60 + bt.tm_min);
                    model_close_bucket(b, s_half_hit);
                    model_note_closed(s_cur_half);
                    ESP_LOGI("model", "bucket %d closed hit %d p %.2f (%d sessions)",
                             b, (int)s_half_hit, (double)model_bucket_p(b),
                             model_sessions());
                    s_half_hit = false;
                    s_cur_half = half;
                    model_store_save();
                }
            }
            int yy, mm, dd;
            if (pcf_date(&yy, &mm, &dd)) {
                daypart_for_log = daypart_for(yy, mm, dd, pcf_minutes_of_day());
                cat_set_daypart(daypart_for_log);
                stats_note_date(yy * 10000 + mm * 100 + dd);
            }
        }

        if (now - last_log_us > 3000000) {
            last_log_us = now;
            float g[3];
            imu_gravity(g);
            int st1, st2;
            battery_raw(&st1, &st2);
            const stats_t *st = stats_get();
            float mic_rms, mic_amb;
            audio_mic_levels(&mic_rms, &mic_amb);
            ESP_LOGI(TAG, "state %d purr %.2f touch %d (%d,%d) flush_err %d | grav %.1f %.1f %.1f tilt %.1f | batt st1 %02x st2 %02x | clock %d part %d | F %d A %d X %d PL %d S %d | mic %.3f amb %.3f | fear %d%s",
                     (int)cat_state(), (double)cat_purr_level(), (int)ts.down, ts.x, ts.y, cat_flush_errors(),
                     (double)g[0], (double)g[1], (double)g[2], (double)imu_tilt_x(), st1, st2,
                     pcf_minutes_of_day(), daypart_for_log,
                     (int)st->food, (int)st->affection, (int)st->exercise,
                     (int)st->play, (int)st->sleep,
                     (double)mic_rms, (double)mic_amb,
                     cat_scare_level(), cat_wary() ? " wary" : "");
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

    // The card carries the park and keeps the logbook; without one the game
    // runs on a plain procedural sky and remembers nothing.
    sdcard_mount(s_i2c_bus);
    logbook_init();

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
    stats_seed((uint32_t)esp_timer_get_time() ^ 0xC0FFEEu);
    if (nvs == ESP_OK && stats_store_load()) {
        ESP_LOGI(TAG, "stats restored");
    } else {
        ESP_LOGI(TAG, "fresh cat");
    }
    model_reset();
    if (nvs == ESP_OK) {
        model_store_load();
    }
    if (power_take_proactive(&s_pending_arm, &s_pending_period)) {
        ESP_LOGI(TAG, "proactive wake boot: arm %d", s_pending_arm);
    } else {
        s_pending_arm = -1;
    }
    logbook_note_boot();

    // Load the park for whatever hour it is. The clock may still be at the
    // epoch this early; daylight is the safe opening guess and the loader
    // swaps in the right one within seconds of the first battery poll.
    {
        int yy, mm, dd, want = BG_DAY;
        if (pcf_date(&yy, &mm, &dd)) {
            want = daypart_for(yy, mm, dd, pcf_minutes_of_day());
        }
        if (world_init(want) != ESP_OK) {
            ESP_LOGW(TAG, "no park: the sky will be plain");
        }
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
    // Unreconciled fear survives the night.
    cat_restore_trust(stats_trust_level(), stats_trust_wary());
    power_note_activity();

    xTaskCreatePinnedToCore(cat_task, "cat", 6144, NULL, 5, NULL, 0);
}
