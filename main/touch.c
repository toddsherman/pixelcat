#include "touch.h"

#include "config.h"
#include "esp_log.h"

#define ADDR_CST820 0x15
#define ADDR_FT3168 0x38

#define I2C_SPEED_HZ 400000
#define PROBE_TIMEOUT_MS 100
#define XFER_TIMEOUT_MS 20

// CST820 auto-sleeps after a few seconds without touches and then needs its
// reset pin (not wired on this board) or a long wait to come back. Writing 1
// here disables that.
#define CST820_REG_DIS_AUTOSLEEP 0xFE

// Both controllers keep the coordinates in the same layout: a finger-count
// byte, then per-point [event/xh, xl, id/yh, yl], 12-bit coordinates.
#define CST820_REG_FINGER_NUM 0x02
#define FT3168_REG_TD_STATUS 0x02

// The V2 panel is offset 16 columns into the controller's address space, but
// touch coordinates are not, so no correction is needed on either revision.

static const char *TAG = "touch";

static i2c_master_dev_handle_t s_dev;
static bool s_is_cst820;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, XFER_TIMEOUT_MS);
}

esp_err_t touch_init(i2c_master_bus_handle_t bus)
{
    uint8_t addr;
    if (i2c_master_probe(bus, ADDR_CST820, PROBE_TIMEOUT_MS) == ESP_OK) {
        addr = ADDR_CST820;
        s_is_cst820 = true;
    } else if (i2c_master_probe(bus, ADDR_FT3168, PROBE_TIMEOUT_MS) == ESP_OK) {
        addr = ADDR_FT3168;
        s_is_cst820 = false;
    } else {
        ESP_LOGE(TAG, "no touch controller found");
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_is_cst820) {
        const uint8_t dis_sleep[2] = {CST820_REG_DIS_AUTOSLEEP, 0x01};
        ret = i2c_master_transmit(s_dev, dis_sleep, sizeof(dis_sleep), XFER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "could not disable CST820 auto-sleep: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "%s ready at 0x%02x", s_is_cst820 ? "CST820" : "FT3168", addr);
    return ESP_OK;
}

bool touch_read(touch_state_t *out)
{
    out->down = false;
    out->x = 0;
    out->y = 0;

    // finger count + one point: [n, xh, xl, yh, yl]
    uint8_t buf[5];
    const uint8_t base = s_is_cst820 ? CST820_REG_FINGER_NUM : FT3168_REG_TD_STATUS;
    if (read_regs(base, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }

    const int fingers = buf[0] & 0x0F;
    if (fingers < 1) {
        return true;
    }

    int x = ((buf[1] & 0x0F) << 8) | buf[2];
    int y = ((buf[3] & 0x0F) << 8) | buf[4];
    if (x >= LCD_H_RES) x = LCD_H_RES - 1;
    if (y >= LCD_V_RES) y = LCD_V_RES - 1;

    out->down = true;
    out->x = (int16_t)x;
    out->y = (int16_t)y;
    return true;
}
