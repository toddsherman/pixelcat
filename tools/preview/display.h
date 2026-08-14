#pragma once

// Host stub for the real display driver, so cat.c compiles on a Mac.

#include <stdbool.h>
#include <stdint.h>

uint16_t *display_acquire_band(void);
int display_flush_band(int band_index, const uint16_t *buffer);
