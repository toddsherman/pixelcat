#pragma once

#include "config.h"
#include "esp_err.h"

// The streamed park (WORLD_FROM_SD). Call world_init() once, after the SD
// card is mounted, with the daypart the game is starting in — it loads that
// variant synchronously, then keeps a spare buffer warm in the background.
// cat_bg_strips() (declared in cat_bg.h) hands the renderer whichever
// variant is resident and asks for a new one as the sun moves.

#if WORLD_FROM_SD
esp_err_t world_init(int variant);
void world_request(int variant);

// True once this variant is the one actually in front of the renderer.
bool world_is_resident(int variant);
#else
static inline esp_err_t world_init(int variant)
{
    (void)variant;
    return ESP_OK;
}
static inline void world_request(int variant) { (void)variant; }
static inline bool world_is_resident(int variant)
{
    (void)variant;
    return true;
}
#endif
