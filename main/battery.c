#include "battery.h"

#include "esp_log.h"

#define AXP2101_ADDR 0x34
#define REG_STATUS1 0x00  // bit 5: VBUS good
#define REG_STATUS2 0x01  // bits 6:5 == 01: charging
#define REG_BAT_PERCENT 0xA4

static const char *TAG = "battery";

static i2c_master_dev_handle_t s_dev;
static int s_st1 = -1, s_st2 = -1;

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

esp_err_t battery_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t pct;
    if (read_reg(REG_BAT_PERCENT, &pct) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not answering");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXP2101 fuel gauge ready: %d%%", pct);
    return ESP_OK;
}

bool battery_read(int *percent, bool *charging)
{
    uint8_t pct, st1, st2;
    if (read_reg(REG_BAT_PERCENT, &pct) != ESP_OK ||
        read_reg(REG_STATUS1, &st1) != ESP_OK ||
        read_reg(REG_STATUS2, &st2) != ESP_OK) {
        return false;
    }
    *percent = (pct <= 100) ? pct : 100;
    s_st1 = st1;
    s_st2 = st2;
    // Charging (STATUS2 bits 7:5 == 001), or USB power present (STATUS1 bit
    // 5, VBUS good): either way the bar shows "plugged".
    *charging = ((st2 >> 5) == 0x01) || ((st1 >> 5) & 0x1);
    return true;
}

void battery_raw(int *status1, int *status2)
{
    *status1 = s_st1;
    *status2 = s_st2;
}

// The cell's terminal voltage in mV, or -1. The fuel gauge's percentage is a
// derived estimate and can disagree with physical reality; this cannot.
int battery_millivolts(void)
{
    uint8_t vh, vl;
    if (read_reg(0x34, &vh) != ESP_OK || read_reg(0x35, &vl) != ESP_OK) {
        return -1;
    }
    return (int)((((unsigned)(vh & 0x3F)) << 8) | vl);
}

// Diagnostics: the charger's own configuration. This firmware has only ever
// read from the AXP2101, never written to it, so everything here is whatever
// the PMU powered up with.
void battery_dump(void)
{
    static const struct {
        uint8_t reg;
        const char *what;
    } k[] = {
        {0x00, "STATUS1 (b5 vbus, b3 batt present)"},
        {0x01, "STATUS2 (b6:5 dir, b2:0 chg state)"},
        {0x03, "IC type"},
        {0x14, "min system voltage"},
        {0x15, "input voltage limit"},
        {0x16, "input current limit"},
        {0x18, "charger enable / ctrl"},
        {0x19, "watchdog / ctrl"},
        {0x30, "ADC channel enable"},
        {0x61, "precharge current"},
        {0x62, "constant charge current"},
        {0x63, "termination current"},
        {0x64, "charge target voltage"},
        {0x68, "button battery charge"},
        {0xA4, "fuel gauge percent"},
    };
    ESP_LOGW(TAG, "--- AXP2101 register dump ---");
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        uint8_t v = 0;
        if (read_reg(k[i].reg, &v) == ESP_OK) {
            ESP_LOGW(TAG, "  0x%02X = 0x%02X (%3u)  %s", k[i].reg, v, v,
                     k[i].what);
        } else {
            ESP_LOGW(TAG, "  0x%02X = ????         %s", k[i].reg, k[i].what);
        }
    }
    // Battery voltage settles the question the percentage cannot: a flat cell
    // and an uncalibrated gauge look identical at 0%.
    uint8_t vh = 0, vl = 0;
    if (read_reg(0x34, &vh) == ESP_OK && read_reg(0x35, &vl) == ESP_OK) {
        const unsigned mv = ((unsigned)(vh & 0x3F) << 8) | vl;
        ESP_LOGW(TAG, "  VBAT  = %u mV (raw %02X %02X)", mv, vh, vl);
    }
}
