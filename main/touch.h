#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Capacitive touch, polled. Handles both revisions of the board: CST820 at
// 0x15 (V2, May 2026 onward) and FT3168 at 0x38 (original), probed at init.

typedef struct {
    bool down;    // finger on the glass right now
    int16_t x;    // panel pixels, 0..LCD_H_RES-1
    int16_t y;    // panel pixels, 0..LCD_V_RES-1
} touch_state_t;

esp_err_t touch_init(i2c_master_bus_handle_t bus);

// Reads the controller once. Returns false (and down=false) on I2C failure.
bool touch_read(touch_state_t *out);
