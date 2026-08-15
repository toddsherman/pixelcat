#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// The microSD slot: SDMMC in 1-bit mode on CLK 2 / CMD 1 / D0 3. D1 and D2
// are strapped to 3V3 on this board, so 4-bit is physically impossible, and
// there is no card-detect line. The card's DAT3 doubles as SPI chip-select
// and hangs off the TCA9554 expander (EXIO7) — it has to be driven HIGH
// before init or the card comes up in SPI mode and SDMMC enumeration fails.
//
// Everything here fails soft: with no card the game runs exactly as before,
// on baked-in art and without a logbook.

esp_err_t sdcard_mount(i2c_master_bus_handle_t bus);
bool sdcard_ready(void);

// Total and free bytes on the card (0 if absent).
void sdcard_usage(uint64_t *total, uint64_t *freeb);

#define SDCARD_ROOT "/sdcard"
#define SDCARD_DIR SDCARD_ROOT "/pixelcat"
