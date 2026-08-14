#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Minimal AXP2101 fuel-gauge reader: percentage plus whether USB power is in.

esp_err_t battery_init(i2c_master_bus_handle_t bus);

// Returns false on I2C failure. percent is the PMU's fuel-gauge estimate.
bool battery_read(int *percent, bool *charging);

// Raw status registers from the last successful read, for diagnostics.
void battery_raw(int *status1, int *status2);
