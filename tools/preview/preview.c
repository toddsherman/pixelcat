// Renders PixelCat frames on the host and writes a PPM to stdout, so the art
// can be judged without a flash cycle. Compiles the real cat.c.
//
// Usage: preview [state] [seconds] > frame.ppm
//   state: 0 idle, 1 petted, 2 startled, 3 sleeping
//   10+n forced mode, 30+n night pose, 40+d daypart portrait
//   50 bowl drop, 51 play session, 52 poop nearby, 53 status page
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
    // Varied levels so every gauge shows a different fill.
    cat_set_stats(35, 62, 20, 55, 70);
    cat_set_battery(72, false);
    if (want < 10) {
        cat_debug_force(0);  // boot is absent; these states want him present
    }

    // Drive the behaviour to the requested state with synthetic touches.
    cat_touch_t t = {0};
    const float dt = 1.0f / CAT_FPS;

    if (want >= 70) {
        // 70 + daypart: the park and HUD in that scene, with no cat in it —
        // the plate the website animation draws its own cat onto.
        cat_set_daypart(want - 70);
        cat_debug_force(60);  // absent
        for (float el = 0; el < seconds; el += dt) {
            cat_update(dt, &t, 0.0f, 0.0f);
        }
    } else if (want >= 50) {
        cat_debug_force(want);
        for (float el = 0; el < seconds; el += dt) {
            cat_update(dt, &t, 0.0f, 0.0f);
        }
    } else if (want >= 40) {
        // want = 40 + daypart: portrait pose in that scene variant.
        cat_set_daypart(want - 40);
        cat_debug_force(0);
    } else if (want >= 30) {
        // Night variants: force daypart NIGHT plus a pose.
        cat_set_daypart(4);
        cat_debug_force(want - 30);
    } else if (want >= 10) {
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

    // Emit the viewer's landscape frame: logical(lx, ly) lives at panel
    // (px = LCD_H_RES - 1 - ly, py = lx).
    printf("P6\n%d %d\n255\n", LCD_V_RES, LCD_H_RES);
    for (int n = 0; n < LCD_V_RES * LCD_H_RES; n++) {
        const int lx = n % LCD_V_RES;
        const int ly = n / LCD_V_RES;
        const int i = lx * LCD_H_RES + (LCD_H_RES - 1 - ly);
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
