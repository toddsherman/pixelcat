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
