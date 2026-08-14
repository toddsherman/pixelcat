#include "imu.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"

// qmi8658.h defines its own M_PI; drop the math.h one first.
#undef M_PI
#include "qmi8658.h"

static const char *TAG = "imu";

static qmi8658_dev_t s_dev;
static bool s_ready;

static float s_lp[3];       // low-passed accel = gravity estimate
static bool s_primed;
static float s_shake;       // smoothed |accel - gravity|
static int64_t s_last_us;

esp_err_t imu_init(i2c_master_bus_handle_t bus)
{
    uint8_t addr = QMI8658_ADDRESS_HIGH;
    if (i2c_master_probe(bus, addr, 100) != ESP_OK) {
        addr = QMI8658_ADDRESS_LOW;
        if (i2c_master_probe(bus, addr, 100) != ESP_OK) {
            ESP_LOGW(TAG, "QMI8658 not found");
            return ESP_ERR_NOT_FOUND;
        }
    }

    esp_err_t ret = qmi8658_init(&s_dev, bus, addr);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((ret = qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G)) != ESP_OK) return ret;
    if ((ret = qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_250HZ)) != ESP_OK) return ret;
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    if ((ret = qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL)) != ESP_OK) return ret;

    s_last_us = esp_timer_get_time();
    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02x", addr);
    return ESP_OK;
}

float imu_tilt_x(void)
{
    // On the V2 board the IMU is mounted rotated relative to the V1 the
    // fluidbox mapping was measured on: reclining the device registers on the
    // y axis, left/right roll on the x axis. (Confirmed empirically: a stand's
    // backward recline showed up on y and falsely read as sideways tilt.)
    return s_ready ? -s_lp[0] : 0.0f;
}

float imu_shake(void)
{
    if (!s_ready) {
        return 0.0f;
    }

    qmi8658_data_t data;
    if (qmi8658_read_sensor_data(&s_dev, &data) != ESP_OK) {
        return s_shake;
    }

    const int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_us) * 1e-6f;
    s_last_us = now;
    if (dt > 0.2f) dt = 0.2f;

    const float a[3] = {data.accelX, data.accelY, data.accelZ};

    if (!s_primed) {
        s_lp[0] = a[0];
        s_lp[1] = a[1];
        s_lp[2] = a[2];
        s_primed = true;
    }

    // Slow low-pass isolates gravity; what is left is the shake.
    const float kg = 1.0f - expf(-dt * 6.283f * 1.0f);
    float mag2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        s_lp[i] += kg * (a[i] - s_lp[i]);
        const float d = a[i] - s_lp[i];
        mag2 += d * d;
    }

    // Fast attack, slow release, so one flick registers but decays visibly.
    const float mag = sqrtf(mag2);
    const float ks = (mag > s_shake) ? (1.0f - expf(-dt * 12.0f))
                                     : (1.0f - expf(-dt * 2.0f));
    s_shake += ks * (mag - s_shake);
    return s_shake;
}
