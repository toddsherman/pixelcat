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

// The charge at which everything worth keeping is written to flash, on the
// assumption that there may not be a later chance.
#define LOW_BATT_PCT 12

// And the voltage at which he stops rather than browns out. Left to itself
// this board runs the AMOLED until the cell collapses, resets, and does it
// again — the boot log has fourteen consecutive power_on resets from one
// such night, which is both a dead battery and a cell being deep-cycled to
// death. Voltage rather than the fuel gauge decides: the gauge reads far
// lower than the cell measures and cannot be trusted with this.
#define CUTOFF_MV 3450
#define CUTOFF_SAMPLES 2  // consecutive 5 s reads, so one sag cannot trip it

// While the fuel gauge is under investigation: print the battery history on
// every boot. Plugging in is what ends a discharge run, and plugging in is
// also what reboots the board, so this lands the data at exactly the moment
// there is a cable to read it over. Set to 0 when the question is settled.
#define BATTERY_DEBUG 1

// Reproduce the doze/wake cycle on the bench: doze at 20 s, wake at 35 s, so
// the freeze can be caught over the cable rather than by hand. 0 = off.
#define DOZE_SELFTEST 0

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
    int64_t last_flush_us = 0;
    int64_t last_mark_us = 0;
    int batt_pct = -1;
    bool low_saved = false;
    int low_volt = 0;
    int64_t last_bsamp_us = 0;
    int64_t settle_since_us = 0;
    int plugged_before = -1;  // -1 until the first reading
    int daypart_for_log = 0;

    // Start on the sky the clock says. world_init() has already loaded it,
    // but the renderer asks for s.daypart, which is still BG_DAY here — so
    // the first frame would request DAY, the loader would obediently swap to
    // it, and the park would visibly flip a second later when the periodic
    // update put it right. Setting it before the first frame closes that.
    {
        int yy, mm, dd;
        if (pcf_date(&yy, &mm, &dd)) {
            daypart_for_log = daypart_for(yy, mm, dd, pcf_minutes_of_day());
            cat_set_daypart(daypart_for_log);
        }
    }

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
        // Either a BOOT press or a deliberate hold of PWR picks, so the
        // bench is usable even when BOOT is not.
        const bool boot_press = boot_take_press() || button_take_long_press();
        if (button_press) {
            ESP_LOGI(TAG, "PWR pressed");
        }

        const bool user_act =
            ts.down || shake > 0.8f || fabsf(tilt) > 2.0f || button_press;
        if (user_act) {
            // Ends a doze (screen back on) and keeps the idle timer fed.
            power_wake_screen();
            power_note_activity();
        }

        // The buttons belong to the test bench: PWR steps, BOOT picks.
        if (button_press) {
            cat_button_pwr();
        }
        if (boot_press) {
            switch (cat_button_boot()) {
                case ACT_AUDITION: {
                    struct tm lt;
                    const int mins =
                        wall_clock(&lt) ? lt.tm_hour * 60 + lt.tm_min : 12 * 60;
                    audition_start(ENTICE_MEOW, model_period(mins), now);
                    break;
                }
                case ACT_SLEEP:
                    power_sleep_now();
                    break;
                case ACT_FORGET:
                    model_forget();
                    logbook_forget();
                    cat_test_close();
                    break;
                case ACT_GAUGES:
                    stats_debug_set((float)cat_icon_fill());
                    break;
                case ACT_SIM_WAKE:
                    ESP_LOGI(TAG, "rehearsal: sleeping, back in 30 s");
                    power_simulate_wake(30);
                    break;
                case ACT_DEFAULTS:
                    // Put the world's own daypart back and hold the screen
                    // dark until that park is actually resident.
                    {
                        int yy, mm, dd;
                        if (pcf_date(&yy, &mm, &dd)) {
                            daypart_for_log =
                                daypart_for(yy, mm, dd, pcf_minutes_of_day());
                            cat_set_daypart(daypart_for_log);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        if (cat_icon_fill() >= 0 && button_press) {
            // Stepping the icon browser sets the gauges it is showing.
            stats_debug_set((float)cat_icon_fill());
        }

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
        }
        // How far this run got, once a minute. On the five-minute save timer
        // any run that died sooner than that reported "0 s, battery ?" — the
        // early crash, which is the one worth reading about.
        if (now - last_mark_us > 60 * 1000000LL) {
            last_mark_us = now;
            logbook_mark_uptime(batt_pct);
        }
        // The card gets whatever has accumulated every 10 s. Both buffers are
        // usually empty, so this costs nothing most of the time — but on the
        // old five-minute timer a reset took the whole window's events with
        // it, which during a flashing session meant nearly all of them.
        if (now - last_flush_us > 10 * 1000000LL) {
            last_flush_us = now;
            logbook_flush();
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

        // Release the hold as soon as the right park is up — or after a
        // moment regardless. A dark screen is the worst failure this thing
        // has, and it should never be reachable by waiting for something
        // that is not coming: better a brief wrong sky than a dead device.
        if (world_is_resident(daypart_for_log)) {
            cat_set_settling(false);
            settle_since_us = 0;
        } else if (settle_since_us == 0) {
            settle_since_us = now;
        } else if (now - settle_since_us > 3000000) {
            ESP_LOGW(TAG, "park %d still not resident after 3 s; showing "
                          "the screen anyway", daypart_for_log);
            cat_set_settling(false);
            settle_since_us = 0;
        }

        // Nothing is drawn while dozing: the panel is asleep, and the cat
        // carries on living regardless.
        if (!power_dozing()) {
            cat_render();
        }

#if DOZE_SELFTEST
        {
            const int64_t up = now / 1000000;
            static int st_stage;
            if (st_stage == 0 && up >= 20) {
                st_stage = 1;
                ESP_LOGW(TAG, "selftest: forcing doze");
                power_sleep_now();
            } else if (st_stage == 1 && up >= 35) {
                st_stage = 2;
                ESP_LOGW(TAG, "selftest: forcing wake");
                power_wake_screen();
                power_note_activity();
            }
        }
#endif
        power_idle_check();

        // A doze-time proactive fire lands here rather than through a reboot.
        {
            int parm, pper;
            if (power_take_proactive(&parm, &pper)) {
                audition_start(parm, pper, now);
            }
        }

        if (now - last_batt_us > 5000000) {
            last_batt_us = now;
            int pct;
            bool chg;
            if (battery_read(&pct, &chg)) {
                cat_set_battery(pct, chg);
                // Plugging in or unplugging is a hand on the device, so it
                // counts as activity. Without this, pulling the cable on a
                // dozing cat dropped it straight into sleep: the idle
                // countdown had already been spent dozing, so the sleep
                // branch fired on the very next tick and everything —
                // screen, cat, sound — stopped at once.
                if (plugged_before < 0 || (chg ? 1 : 0) != plugged_before) {
                    if (plugged_before >= 0) {
                        ESP_LOGW(TAG, "power source changed: now on %s",
                                 chg ? "USB" : "battery");
                        power_wake_screen();
                        power_note_activity();
                    }
                    plugged_before = chg ? 1 : 0;
                }
                batt_pct = pct;
                int st1_s, st2_s;
                battery_raw(&st1_s, &st2_s);
                // Below this, the next brownout could be the last thing that
                // happens, and a five-minute save timer is no use to a board
                // that is about to go dark. Get the cat and the model onto
                // flash while there is still power to write with. It re-arms
                // only once charging lifts it clear, so a flat battery costs
                // one save rather than one every five seconds.
                if (!chg && pct >= 0 && pct <= LOW_BATT_PCT) {
                    if (!low_saved) {
                        low_saved = true;
                        ESP_LOGW(TAG, "battery %d%%: saving state now", pct);
                        stats_store_save();
                        model_store_save();
                        logbook_mark_uptime(pct);
                        logbook_flush();
                    }
                } else if (pct > LOW_BATT_PCT + 5) {
                    low_saved = false;
                }

                // One line a minute to the card, awake or asleep, so the
                // gauge's percentage can be plotted against the cell's real
                // voltage across a whole cycle.
                if (now - last_bsamp_us > 60 * 1000000LL) {
                    last_bsamp_us = now;
                    logbook_battery_sample(pct, battery_millivolts(), st1_s,
                                           st2_s);
                }

                // The cutoff. On battery, a cell this low cannot hold the
                // panel up much longer; stopping deliberately beats being
                // stopped by a brownout, and the sleep loop already wakes
                // again the moment USB power appears.
                const int mv = battery_millivolts();
                if (!chg && mv > 0 && mv <= CUTOFF_MV) {
                    if (++low_volt >= CUTOFF_SAMPLES) {
                        ESP_LOGW(TAG,
                                 "battery %d mV (gauge %d%%): shutting down "
                                 "before the brownout does it for us",
                                 mv, pct);
                        stats_store_save();
                        model_store_save();
                        logbook_add(LOG_BOOT, -1, mv);
                        logbook_mark_uptime(pct);
                        logbook_flush();
                        power_sleep_now();  // next idle check sleeps for real
                    }
                } else {
                    low_volt = 0;
                }
            }
            stats_set_trust(cat_scare_level(), cat_wary());

            // What the model knows, for its page: the hour it most expects
            // company, and which opening act has been working.
            {
                int peak = -1;
                float best_p = 0.0f;
                struct tm lt;
                const int dow = wall_clock(&lt) ? lt.tm_wday : 1;
                for (int b = 0; b < 48; b++) {
                    const float p = model_bucket_p(model_bucket(dow, b * 30));
                    if (p > best_p) {
                        best_p = p;
                        peak = b * 30;
                    }
                }
                static const char *const acts[MODEL_ARMS] = {
                    "JUMP", "PURR", "PAW", "PACE", "MEOW",
                };
                const int per = wall_clock(&lt)
                                    ? model_period(lt.tm_hour * 60 + lt.tm_min)
                                    : 0;
                int best_arm = 0;
                for (int a = 1; a < MODEL_ARMS; a++) {
                    if (model_arm_value(per, a) > model_arm_value(per, best_arm)) {
                        best_arm = a;
                    }
                }
                int hits, misses;
                model_wake_stats(&hits, &misses);
                cat_set_model_info(model_sessions(), stats_streak(ST_SLEEP) + 1,
                                   model_mature(),
                                   (int)(model_threshold() * 100.0f), hits,
                                   misses, acts[best_arm], peak);
            }

            // Two lines of context under the test menu.
            {
                uint64_t total = 0, freeb = 0;
                sdcard_usage(&total, &freeb);
                char a[24], b[24];
                // Both must fit the menu: 15 characters across the footer,
                // 6 in the header's right corner.
                // Must fit the footer beside the button legend: 14 chars.
                snprintf(a, sizeof(a), "SD%uG UP%us",
                         (unsigned)(freeb / (1024 * 1024 * 1024)),
                         (unsigned)(now / 1000000));
                snprintf(b, sizeof(b), "F%d E%d", cat_scare_level(),
                         cat_flush_errors());
                cat_set_debug_lines(a, b);
            }

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
            uint32_t tx_ok, tx_err;
            int tx_free;
            display_stats(&tx_ok, &tx_err, &tx_free);
            const stats_t *st = stats_get();
            float mic_rms, mic_amb;
            audio_mic_levels(&mic_rms, &mic_amb);
            ESP_LOGI(TAG, "state %d purr %.2f touch %d (%d,%d) flush_err %d tx %u/%u free %d | grav %.1f %.1f %.1f tilt %.1f | batt %d%% %dmV st1 %02x st2 %02x | clock %d part %d | F %d A %d X %d PL %d S %d | mic %.3f amb %.3f | fear %d%s",
                     (int)cat_state(), (double)cat_purr_level(), (int)ts.down, ts.x, ts.y, cat_flush_errors(),
                     tx_ok, tx_err, tx_free,
                     (double)g[0], (double)g[1], (double)g[2], (double)imu_tilt_x(),
                     batt_pct, battery_millivolts(), st1, st2,
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
    logbook_capture_console();  // from here on, warnings outlive the cable

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
#if BATTERY_DEBUG
    logbook_dump_battery(400);
#endif

    // Load the park for whatever hour it is. The clock may still be at the
    // epoch this early, in which case daylight is the safe opening guess;
    // cat_task sets the engine's own daypart to match before it draws.
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
