#include "rtc.h"

#include <string.h>

#include "esp_log.h"

#define PCF85063_ADDR 0x51
#define REG_SECONDS 0x04  // bit 7: oscillator-stop (time invalid)

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return ((v >> 4) & 0x0F) * 10 + (v & 0x0F); }

typedef struct {
    int year, mon, day, hour, min, sec;  // civil local time, year 2000+
} rtc_tm_t;

// Comparable scalar; exact calendar math is unnecessary for ordering.
static long long serial(const rtc_tm_t *t)
{
    return ((((long long)t->year * 12 + t->mon) * 31 + t->day) * 24 + t->hour) * 3600 +
           t->min * 60 + t->sec;
}

// __DATE__ is "Mmm dd yyyy", __TIME__ is "hh:mm:ss". The build system touches
// this file every build so these are always the current build's timestamps.
static rtc_tm_t build_time(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    const char *t = __TIME__;
    rtc_tm_t bt;
    const char *m = strstr(months, (char[]){d[0], d[1], d[2], 0});
    bt.mon = m ? (int)(m - months) / 3 + 1 : 1;
    bt.day = (d[4] == ' ' ? 0 : (d[4] - '0') * 10) + (d[5] - '0');
    bt.year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    bt.hour = (t[0] - '0') * 10 + (t[1] - '0');
    bt.min = (t[3] - '0') * 10 + (t[4] - '0');
    bt.sec = (t[6] - '0') * 10 + (t[7] - '0');
    return bt;
}

static esp_err_t read_time(rtc_tm_t *out, bool *os_flag)
{
    uint8_t reg = REG_SECONDS;
    uint8_t b[7];
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 50);
    if (ret != ESP_OK) {
        return ret;
    }
    *os_flag = (b[0] & 0x80) != 0;
    out->sec = from_bcd(b[0] & 0x7F);
    out->min = from_bcd(b[1] & 0x7F);
    out->hour = from_bcd(b[2] & 0x3F);
    out->day = from_bcd(b[3] & 0x3F);
    out->mon = from_bcd(b[5] & 0x1F);
    out->year = 2000 + from_bcd(b[6]);
    return ESP_OK;
}

static esp_err_t write_time(const rtc_tm_t *t)
{
    const uint8_t buf[8] = {
        REG_SECONDS,
        to_bcd(t->sec),  // also clears the oscillator-stop flag
        to_bcd(t->min),
        to_bcd(t->hour),
        to_bcd(t->day),
        0,  // weekday, unused
        to_bcd(t->mon),
        to_bcd(t->year - 2000),
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

esp_err_t pcf_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    rtc_tm_t now;
    bool os = false;
    ret = read_time(&now, &os);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not answering");
        return ret;
    }

    const rtc_tm_t bt = build_time();
    if (os || serial(&now) < serial(&bt)) {
        // Clock never set, lost power, or older than this firmware: sync it
        // to the host clock captured at build time.
        ret = write_time(&bt);
        if (ret != ESP_OK) {
            return ret;
        }
        now = bt;
        ESP_LOGI(TAG, "clock set from build time");
    }
    ESP_LOGI(TAG, "time %04d-%02d-%02d %02d:%02d", now.year, now.mon, now.day,
             now.hour, now.min);
    s_ready = true;
    return ESP_OK;
}

esp_err_t pcf_set_civil(int year, int mon, int day, int hour, int min, int sec)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const rtc_tm_t t = {.year = year, .mon = mon, .day = day,
                        .hour = hour, .min = min, .sec = sec};
    return write_time(&t);
}

bool pcf_date(int *year, int *mon, int *day)
{
    if (!s_ready) {
        return false;
    }
    rtc_tm_t now;
    bool os;
    if (read_time(&now, &os) != ESP_OK) {
        return false;
    }
    *year = now.year;
    *mon = now.mon;
    *day = now.day;
    return true;
}

int pcf_minutes_of_day(void)
{
    if (!s_ready) {
        return -1;
    }
    rtc_tm_t now;
    bool os;
    if (read_time(&now, &os) != ESP_OK) {
        return -1;
    }
    return now.hour * 60 + now.min;
}
