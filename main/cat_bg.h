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

extern const uint16_t cat_bg[5][2496][368];
