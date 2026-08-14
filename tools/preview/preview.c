// Renders PixelCat frames on the host and writes a PPM to stdout, so the art
// can be judged without a flash cycle. Compiles the real cat.c.
//
// Usage: preview [state] [seconds] > frame.ppm
//   state: 0 idle, 1 petted, 2 startled, 3 sleeping
//   seconds: how much animation time to advance before the shot

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cat.h"
#include "config.h"

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static uint16_t s_bands[2][BAND_PIXELS];
static uint16_t s_frame[LCD_V_RES * LCD_H_RES];

uint16_t *display_acquire_band(void)
{
    static unsigned next;
    return s_bands[next++ & 1];
}

int display_flush_band(int band_index, const uint16_t *buffer)
{
    memcpy(&s_frame[band_index * BAND_ROWS * LCD_H_RES], buffer,
           BAND_PIXELS * sizeof(uint16_t));
    return 0;
}

int main(int argc, char **argv)
{
    const int want = (argc > 1) ? atoi(argv[1]) : 0;
    const float seconds = (argc > 2) ? (float)atof(argv[2]) : 1.0f;

    cat_init();

    // Drive the behaviour to the requested state with synthetic touches.
    cat_touch_t t = {0};
    const float dt = 1.0f / CAT_FPS;

    if (want >= 10) {
        // Force an internal pose directly (sprite cat modes), no behaviour.
        cat_debug_force(want - 10);
    } else if (want == 1) {
        // Stroke across the cat (it starts centred, near the floor line).
        for (float el = 0; el < seconds; el += dt) {
            t.down = true;
            t.x = (int16_t)(130 + 100.0f * (0.5f + 0.5f * __builtin_sinf(el * 6.0f)));
            t.y = 340;
            cat_update(dt, &t, 0.0f, 0.0f);
        }
    } else if (want == 2) {
        // Poke: press and release without moving, then a beat.
        t.down = true; t.x = 184; t.y = 340;
        cat_update(dt, &t, 0.0f, 0.0f);
        cat_update(dt, &t, 0.0f, 0.0f);
        t.down = false;
        for (float el = 0; el < 0.3f; el += dt) {
            cat_update(dt, &t, 0.0f, 0.0f);
        }
    } else if (want == 3) {
        cat_debug_force(6);  // sprite cat: M_SLEEP; big cat: ignored
        for (float el = 0; el < 2.0f; el += 0.5f) {
            cat_update(0.5f, &t, 0.0f, 0.0f);
        }
    } else {
        for (float el = 0; el < seconds; el += dt) {
            cat_update(dt, &t, 0.0f, 0.0f);
        }
    }

    cat_render();

    printf("P6\n%d %d\n255\n", LCD_H_RES, LCD_V_RES);
    for (int i = 0; i < LCD_V_RES * LCD_H_RES; i++) {
        const uint16_t v = (uint16_t)((s_frame[i] >> 8) | (s_frame[i] << 8));
        const unsigned char rgb[3] = {
            (unsigned char)(((v >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((v >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((v & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, stdout);
    }
    return 0;
}
