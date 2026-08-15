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

// Call periodically. Returns true exactly once per completed tap (under
// 400 ms) — the test bench uses it to move.
bool button_take_short_press(void);

// And once per deliberate hold (400-1500 ms), which the bench uses to pick.
// This exists because the BOOT button is small, awkward, and on at least
// one unit unreliable enough to have latched the chip into download mode.
bool button_take_long_press(void);

// Instantaneous PWR level, for the sleep poll.
bool button_pressed_raw(void);

// True once per press of the BOOT button (GPIO0), for the test menu.
bool boot_take_press(void);
