#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// Minimal QMI8658 wrapper: all the cat needs to know is how hard it is being
// shaken.

esp_err_t imu_init(i2c_master_bus_handle_t bus);

// Smoothed shake magnitude in m/s^2 above gravity: ~0 at rest, 2-5 when
// handled, well past 8 when genuinely shaken.
float imu_shake(void);
