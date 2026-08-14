#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// Owns the AMOLED panel and the pair of band buffers that feed it.
//
// The screen is pushed out one horizontal band at a time. Callers render into
// the buffer returned by display_acquire_band() and hand it to
// display_flush_band(), which starts a DMA transfer and returns immediately.
// Acquiring blocks only if both buffers are still in flight, so drawing one
// band overlaps with transmitting the previous one.

esp_err_t display_init(i2c_master_bus_handle_t i2c_bus);

// Blocks until a band buffer is free, then returns it. BAND_ROWS * LCD_H_RES
// pixels. Buffers rotate per call rather than per band index, because the
// renderer skips bands that are already black on the panel, and two transfers
// in flight must never land in the same buffer.
uint16_t *display_acquire_band(void);

// Queues an asynchronous transfer of one band. Does not block.
esp_err_t display_flush_band(int band_index, const uint16_t *buffer);

// 0..255. The panel dims itself; there is no backlight pin on an AMOLED.
esp_err_t display_set_brightness(uint8_t level);
