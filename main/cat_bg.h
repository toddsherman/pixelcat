#pragma once
#include <stdint.h>

#define BG_VARIANTS 5
#define BG_WORLD_W 2496
#define BG_STRIP_H 368

#define BG_DAY 0
#define BG_DAWN 1
#define BG_DUSK 2
#define BG_TWILIGHT 3
#define BG_NIGHT 4

// The park comes either from the SD card (streamed into PSRAM) or
// from these baked arrays; cat_bg_strips() hides which.
#if WORLD_FROM_SD
const uint16_t (*cat_bg_strips(int variant))[368];
#else
extern const uint16_t cat_bg[5][2496][368];
static inline const uint16_t (*cat_bg_strips(int variant))[368]
{
    return cat_bg[variant];
}
#endif
