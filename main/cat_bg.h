#pragma once
#include <stdint.h>

#define BG_FRAMES 4
#define BG_VARIANTS 5
#define BG_W 368
#define BG_H 448

#define BG_DAY 0
#define BG_DAWN 1
#define BG_DUSK 2
#define BG_TWILIGHT 3
#define BG_NIGHT 4

extern const uint16_t cat_bg[5][4][164864];
