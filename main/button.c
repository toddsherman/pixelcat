#include "button.h"

#include "esp_log.h"
#include "esp_timer.h"

#define EXPANDER_ADDR 0x20
#define EXPANDER_SPEED_HZ 400000
#define EXPANDER_REG_INPUT 0x00
#define EXPANDER_REG_CONFIG 0x03

#define PWR_BIT (1 << 4)  // EXIO4, reads high while the button is held

#define DEBOUNCE_SAMPLES 2
#define SHORT_PRESS_MAX_MS 1500

static const char *TAG = "button";

static i2c_master_dev_handle_t s_dev;
static bool s_stable_state;
static int s_agree_count;
static int64_t s_press_started_us;
static bool s_pending_short_press;
static uint8_t s_last_raw = 0xFF;

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 100);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_dev, payload, sizeof(payload), 100);
}

esp_err_t button_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXPANDER_ADDR,
        .scl_speed_hz = EXPANDER_SPEED_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not reachable: %s", esp_err_to_name(ret));
        return ret;
    }

    // Only force EXIO4 to be an input. The other pins drive the display and SD
    // card resets, so their direction and level are left exactly as found.
    uint8_t config = 0xFF;
    ret = read_reg(EXPANDER_REG_CONFIG, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((config & PWR_BIT) == 0) {
        ret = write_reg(EXPANDER_REG_CONFIG, (uint8_t)(config | PWR_BIT));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "config write failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    uint8_t input = 0;
    read_reg(EXPANDER_REG_INPUT, &input);
    s_last_raw = input;
    ESP_LOGI(TAG, "PWR button ready on EXIO4 (config 0x%02x, input 0x%02x)", config, input);
    return ESP_OK;
}

bool button_take_short_press(void)
{
    uint8_t input = 0;
    if (read_reg(EXPANDER_REG_INPUT, &input) != ESP_OK) {
        return false;
    }

    // Silent at the default log level. Raise the "button" tag to debug to see
    // exactly what the expander reports if the button ever stops responding.
    if (input != s_last_raw) {
        ESP_LOGD(TAG, "expander input 0x%02x -> 0x%02x", s_last_raw, input);
        s_last_raw = input;
    }

    const bool pressed = (input & PWR_BIT) != 0;

    if (pressed == s_stable_state) {
        s_agree_count = 0;
    } else if (++s_agree_count >= DEBOUNCE_SAMPLES) {
        s_agree_count = 0;
        s_stable_state = pressed;

        if (pressed) {
            s_press_started_us = esp_timer_get_time();
        } else {
            // A long hold is the hardware power-off gesture; ignore it here.
            const int64_t held_ms = (esp_timer_get_time() - s_press_started_us) / 1000;
            if (held_ms <= SHORT_PRESS_MAX_MS) {
                s_pending_short_press = true;
            }
        }
    }

    if (s_pending_short_press) {
        s_pending_short_press = false;
        return true;
    }
    return false;
}
