#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// The PWR side button, read through pin 4 of the TCA9554 IO expander.
//
// Only short presses are reported. Holding PWR for six seconds is wired
// straight into the AXP2101 power chip and cuts power in hardware, so that
// behaviour is left alone.

esp_err_t button_init(i2c_master_bus_handle_t bus);

// Call periodically. Returns true exactly once per completed short press.
bool button_take_short_press(void);
