#include "config.h"
#if !CAT_SPRITE_STYLE

#include "cat.h"

#include <math.h>
#include <string.h>

#include "display.h"

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

enum {
    C_BG = 0,   // room
    C_OUT,      // outline
    C_FUR,      // orange
    C_FURD,     // darker orange (stripes, shading)
    C_CREAM,    // chest, muzzle
    C_PINK,     // ears, nose, blush
    C_IRIS,     // green
    C_DARK,     // pupils, closed-eye lines, whiskers
    C_WHITE,    // eye shine
    C_HEART,    // hearts
    C_SLEEP,    // zzz
    C_COUNT,
};

static const uint8_t s_pal_rgb[C_COUNT][3] = {
    [C_BG] = {14, 14, 22},
    [C_OUT] = {26, 18, 20},
    [C_FUR] = {232, 150, 60},
    [C_FURD] = {192, 106, 40},
    [C_CREAM] = {244, 231, 200},
    [C_PINK] = {232, 120, 152},
    [C_IRIS] = {88, 200, 88},
    [C_DARK] = {38, 32, 32},
    [C_WHITE] = {255, 255, 255},
    [C_HEART] = {232, 80, 112},
    [C_SLEEP] = {144, 168, 192},
};

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

static uint16_t s_pal565[C_COUNT];

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

static uint8_t s_canvas[CANVAS_W * CANVAS_H];

// The cat lives in its own 56x64 local frame, pasted at this canvas offset.
#define CAT_W 56
#define CAT_H 64
#define CAT_X ((CANVAS_W - CAT_W) / 2)
#define CAT_Y (CANVAS_H - CAT_H - 8)

static inline void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H) {
        s_canvas[y * CANVAS_W + x] = c;
    }
}

static inline uint8_t get(int x, int y)
{
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) {
        return C_BG;
    }
    return s_canvas[y * CANVAS_W + x];
}

// Stamp an ASCII sprite at canvas position (its own top-left). '.' is
// transparent; every other character indexes the palette via this map.
static uint8_t char_color(char ch)
{
    switch (ch) {
        case '#': return C_OUT;
        case 'o': return C_FUR;
        case 'O': return C_FURD;
        case 'w': return C_CREAM;
        case 'p': return C_PINK;
        case 'g': return C_IRIS;
        case 'k': return C_DARK;
        case 'W': return C_WHITE;
        case 'r': return C_HEART;
        case 'z': return C_SLEEP;
        default: return C_BG;
    }
}

static void stamp(int x0, int y0, const char *const *rows, int nrows)
{
    for (int y = 0; y < nrows; y++) {
        const char *row = rows[y];
        for (int x = 0; row[x]; x++) {
            if (row[x] != '.') {
                px(x0 + x, y0 + y, char_color(row[x]));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Procedural body
// ---------------------------------------------------------------------------

// Everything below works in cat-local coordinates (0..CAT_W-1, 0..CAT_H-1).

static inline bool in_ellipse(int x, int y, float cx, float cy, float rx, float ry)
{
    const float dx = (x - cx) / rx;
    const float dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

// Signed area test: is p left of the line a->b.
static inline float edge(float ax, float ay, float bx, float by, float px_, float py_)
{
    return (bx - ax) * (py_ - ay) - (by - ay) * (px_ - ax);
}

static inline bool in_tri(int x, int y, const float t[6])
{
    const float e0 = edge(t[0], t[1], t[2], t[3], x, y);
    const float e1 = edge(t[2], t[3], t[4], t[5], x, y);
    const float e2 = edge(t[4], t[5], t[0], t[1], x, y);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

// breath: 0..1, swells the chest. ear_twitch rotates the right ear tip.
static void draw_body(float breath, bool ear_twitch)
{
    uint8_t local[CAT_W * CAT_H];
    memset(local, C_BG, sizeof(local));

    // Ear triangles: base sits inside the head so the join is seamless.
    const float ear_l[6] = {6, 18, 11, 1, 22, 10};
    const float ear_r_up[6] = {50, 18, 45, 1, 34, 10};
    const float ear_r_tw[6] = {50, 18, 49, 4, 34, 10};
    const float *ear_r = ear_twitch ? ear_r_tw : ear_r_up;

    const float body_ry = 15.0f + 1.0f * breath;

    for (int y = 0; y < CAT_H; y++) {
        for (int x = 0; x < CAT_W; x++) {
            bool fur = false;
            // Head: wide ellipse, the star of a kawaii cat.
            fur |= in_ellipse(x, y, 28.0f, 22.0f, 23.5f, 17.5f);
            // Body: sitting loaf under the head, flat-bottomed.
            fur |= (y <= 61) && in_ellipse(x, y, 28.0f, 47.0f, 18.5f, body_ry);
            fur |= in_tri(x, y, ear_l);
            fur |= in_tri(x, y, ear_r);
            if (fur) {
                local[y * CAT_W + x] = C_FUR;
            }
        }
    }

    // Inner ears.
    const float ear_li[6] = {9, 14, 12, 4, 18, 10};
    const float ear_ri[6] = {47, 14, 44, 4, 38, 10};
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < CAT_W; x++) {
            if (local[y * CAT_W + x] != C_FUR) {
                continue;
            }
            if (in_tri(x, y, ear_li) || (!ear_twitch && in_tri(x, y, ear_ri))) {
                local[y * CAT_W + x] = C_PINK;
            }
        }
    }

    // Chest and muzzle patches.
    for (int y = 0; y < CAT_H; y++) {
        for (int x = 0; x < CAT_W; x++) {
            if (local[y * CAT_W + x] == C_BG) {
                continue;
            }
            if (in_ellipse(x, y, 28.0f, 29.0f, 9.5f, 6.5f) ||
                in_ellipse(x, y, 28.0f, 52.0f, 10.0f, 11.0f)) {
                local[y * CAT_W + x] = C_CREAM;
            }
        }
    }

    // Outline: any filled cell touching background.
    for (int y = 0; y < CAT_H; y++) {
        for (int x = 0; x < CAT_W; x++) {
            if (local[y * CAT_W + x] == C_BG) {
                continue;
            }
            const bool rim =
                (x == 0 || local[y * CAT_W + x - 1] == C_BG) ||
                (x == CAT_W - 1 || local[y * CAT_W + x + 1] == C_BG) ||
                (y == 0 || local[(y - 1) * CAT_W + x] == C_BG) ||
                (y == CAT_H - 1 || local[(y + 1) * CAT_W + x] == C_BG);
            if (rim) {
                local[y * CAT_W + x] = C_OUT;
            }
        }
    }

    // Paste to canvas.
    for (int y = 0; y < CAT_H; y++) {
        for (int x = 0; x < CAT_W; x++) {
            const uint8_t c = local[y * CAT_W + x];
            if (c != C_BG) {
                px(CAT_X + x, CAT_Y + y, c);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Face and detail sprites (cat-local anchors)
// ---------------------------------------------------------------------------

static const char *const EYE_OPEN[] = {
    "..###..",
    ".#ggg#.",
    "#gkkgg#",
    "#gkkgW#",
    ".#ggg#.",
    "..###..",
};

static const char *const EYE_HALF[] = {
    ".......",
    ".#####.",
    "#gkkgg#",
    "#ggggW#",
    ".#####.",
    ".......",
};

static const char *const EYE_CLOSED[] = {
    ".......",
    ".......",
    ".kk....",
    "...kkk.",
    ".......",
    ".......",
};

static const char *const EYE_HAPPY[] = {
    ".......",
    "..kkk..",
    ".k...k.",
    "k.....k",
    ".......",
    ".......",
};

static const char *const NOSE_MOUTH[] = {
    "...pp...",
    "....k...",
    ".kk.k.kk",
    "...k.k..",
};

static const char *const NOSE_MOUTH_OPEN[] = {
    "...pp...",
    "....k...",
    ".kkkkkk.",
    "..k.p.k.",
    "...kkk..",
};

static const char *const STRIPES_HEAD[] = {
    "OO..OO..OO",
    ".OO..O..O.",
};

static const char *const BLUSH[] = {
    "pp.p",
};

static const char *const TAIL_REST[] = {
    "......##",
    "....##oo",
    "..##oOo#",
    ".#oOo##.",
    ".#oo#...",
    "#oo#....",
    "#oo#....",
    "#oo#....",
    ".##.....",
};

static const char *const TAIL_MID[] = {
    "........",
    "......##",
    "..###oo#",
    ".#oOoo#.",
    "#oOo##..",
    "#oo#....",
    "#oo#....",
    "#oo#....",
    ".##.....",
};

static const char *const TAIL_UP[] = {
    "....##..",
    "...#oo#.",
    "...#oO#.",
    "..#oOo#.",
    ".#oOo#..",
    "#oo##...",
    "#oo#....",
    "#oo#....",
    ".##.....",
};

static const char *const PAWS[] = {
    ".##..##.",
    "#ww##ww#",
    "#w.w#w.w",
    ".##..##.",
};

static const char *const HEART[] = {
    ".r.r.",
    "rrrrr",
    ".rrr.",
    "..r..",
};

static const char *const ZED[] = {
    "zzz",
    "..z",
    ".z.",
    "zzz",
};

static const char *const BANG[] = {
    "WW",
    "WW",
    "WW",
    "..",
    "WW",
};

#define ROWS(s) (int)(sizeof(s) / sizeof((s)[0]))

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

typedef struct {
    float x, y, age;
    bool alive;
} heart_t;

#define MAX_HEARTS 6

static struct {
    cat_state_t state;
    float t;             // seconds in current state
    float since_touch;   // seconds since last contact
    float blink_timer;   // counts down to next blink
    float blink_t;       // >0 while mid-blink
    float ear_timer;     // counts down to next twitch
    float ear_t;         // >0 while twitching
    float breath_ph;
    float purr;          // displayed/audible petting level 0..1
    float stroke_speed;  // smoothed, logical px/s
    bool chirp;          // pending chirp for the audio side

    bool was_down;
    float down_x, down_y;    // where the press started (canvas px)
    float last_x, last_y;    // previous sample (canvas px)
    float moved;             // total distance this press (canvas px)

    heart_t hearts[MAX_HEARTS];
    float heart_spawn;
    uint32_t rng;
} s;

static float frand01(void)
{
    s.rng ^= s.rng << 13;
    s.rng ^= s.rng >> 17;
    s.rng ^= s.rng << 5;
    return (float)(s.rng >> 8) * (1.0f / 16777216.0f);
}

void cat_init(void)
{
    memset(&s, 0, sizeof(s));
    s.state = CAT_IDLE;
    s.blink_timer = 2.5f;
    s.ear_timer = 5.0f;
    s.rng = 0xC0FFEE21;

    for (int i = 0; i < C_COUNT; i++) {
        const uint16_t v = (uint16_t)(((s_pal_rgb[i][0] & 0xF8) << 8) |
                                      ((s_pal_rgb[i][1] & 0xFC) << 3) |
                                      (s_pal_rgb[i][2] >> 3));
        s_pal565[i] = SWAP16(v);
    }
}

static void spawn_heart(float cx, float cy)
{
    for (int i = 0; i < MAX_HEARTS; i++) {
        if (!s.hearts[i].alive) {
            s.hearts[i].alive = true;
            s.hearts[i].x = cx + (frand01() - 0.5f) * 10.0f;
            s.hearts[i].y = cy - 2.0f;
            s.hearts[i].age = 0.0f;
            return;
        }
    }
}

void cat_update(float dt, const cat_touch_t *touch, float shake)
{
    (void)shake;  // the big cat has no shake reaction
    s.t += dt;
    s.breath_ph += dt * ((s.state == CAT_SLEEPING) ? 0.45f : 0.9f);

    // --- touch bookkeeping, in canvas pixels ---
    const float tx = (float)touch->x / PIX_SCALE;
    const float ty = (float)touch->y / PIX_SCALE;
    float speed = 0.0f;

    if (touch->down) {
        if (!s.was_down) {
            s.down_x = tx;
            s.down_y = ty;
            s.moved = 0.0f;
        } else {
            const float dx = tx - s.last_x;
            const float dy = ty - s.last_y;
            const float d = sqrtf(dx * dx + dy * dy);
            s.moved += d;
            if (dt > 1e-4f) {
                speed = d / dt;
            }
        }
        s.last_x = tx;
        s.last_y = ty;
        s.since_touch = 0.0f;
    } else {
        s.since_touch += dt;
    }

    // Smooth the stroke speed; it decays quickly once the finger lifts.
    const float k = 1.0f - expf(-dt * 6.0f);
    s.stroke_speed += k * (speed - s.stroke_speed);

    const bool stroking = touch->down && s.moved >= PET_MOVE_MIN_PX && s.stroke_speed > 8.0f;
    const bool poked = !touch->down && s.was_down && s.moved < PET_MOVE_MIN_PX &&
                       s.state != CAT_SLEEPING;

    // --- state machine ---
    switch (s.state) {
        case CAT_IDLE:
            if (stroking) {
                s.state = CAT_PETTED;
                s.t = 0.0f;
                s.chirp = true;
            } else if (poked) {
                s.state = CAT_STARTLED;
                s.t = 0.0f;
            } else if (s.since_touch > SLEEP_AFTER_S) {
                s.state = CAT_SLEEPING;
                s.t = 0.0f;
            }
            break;

        case CAT_PETTED:
            if (!touch->down && s.stroke_speed < 5.0f && s.t > 0.6f) {
                s.state = CAT_IDLE;
                s.t = 0.0f;
            }
            break;

        case CAT_STARTLED:
            if (s.t > STARTLE_S) {
                s.state = CAT_IDLE;
                s.t = 0.0f;
            }
            break;

        case CAT_SLEEPING:
            if (touch->down) {
                s.state = CAT_IDLE;
                s.t = 0.0f;
                s.since_touch = 0.0f;
                s.chirp = true;
            }
            break;
    }

    // --- purr level chases petting ---
    float target = 0.0f;
    if (s.state == CAT_PETTED) {
        target = 0.35f + 0.65f * fminf(s.stroke_speed / PET_SPEED_FULL, 1.0f);
        if (!touch->down) {
            target = 0.0f;
        }
    }
    const float rate = (target > s.purr) ? PET_RAMP_UP : PET_RAMP_DOWN;
    const float step = rate * dt;
    if (fabsf(target - s.purr) <= step) {
        s.purr = target;
    } else {
        s.purr += (target > s.purr) ? step : -step;
    }

    // --- blink / ear twitch timers ---
    if (s.blink_t > 0.0f) {
        s.blink_t -= dt;
    } else {
        s.blink_timer -= dt;
        if (s.blink_timer <= 0.0f) {
            s.blink_t = 0.13f;
            s.blink_timer = 2.0f + 4.0f * frand01();
        }
    }
    if (s.ear_t > 0.0f) {
        s.ear_t -= dt;
    } else {
        s.ear_timer -= dt;
        if (s.ear_timer <= 0.0f) {
            s.ear_t = 0.18f;
            s.ear_timer = 4.0f + 7.0f * frand01();
        }
    }

    // --- hearts ---
    if (s.state == CAT_PETTED && touch->down) {
        s.heart_spawn -= dt;
        if (s.heart_spawn <= 0.0f) {
            spawn_heart(tx, ty);
            s.heart_spawn = 0.35f;
        }
    }
    for (int i = 0; i < MAX_HEARTS; i++) {
        if (!s.hearts[i].alive) {
            continue;
        }
        s.hearts[i].age += dt;
        s.hearts[i].y -= dt * 9.0f;
        s.hearts[i].x += sinf(s.hearts[i].age * 5.0f) * dt * 3.0f;
        if (s.hearts[i].age > 1.6f || s.hearts[i].y < 2.0f) {
            s.hearts[i].alive = false;
        }
    }

    s.was_down = touch->down;
}

float cat_purr_level(void)
{
    return s.purr;
}

bool cat_take_hiss(void)
{
    return false;  // the big cat does not hiss
}

bool cat_take_chirp(void)
{
    const bool c = s.chirp;
    s.chirp = false;
    return c;
}

cat_state_t cat_state(void)
{
    return s.state;
}

// ---------------------------------------------------------------------------
// Composition + band rendering
// ---------------------------------------------------------------------------

static void compose(void)
{
    memset(s_canvas, C_BG, sizeof(s_canvas));

    const float breath = 0.5f + 0.5f * sinf(s.breath_ph * 6.28318f * 0.2f);

    // Tail cycles slowly while idle, faster and higher while petted.
    const char *const *tail = TAIL_REST;
    int tail_rows = ROWS(TAIL_REST);
    if (s.state == CAT_PETTED || s.state == CAT_STARTLED) {
        const int ph = ((int)(s.t * 6.0f)) % 2;
        tail = ph ? TAIL_UP : TAIL_MID;
        tail_rows = ph ? ROWS(TAIL_UP) : ROWS(TAIL_MID);
    } else if (s.state == CAT_IDLE) {
        const int ph = ((int)(s.t * 1.2f)) % 3;
        if (ph == 1) { tail = TAIL_MID; tail_rows = ROWS(TAIL_MID); }
    }
    draw_body(breath, s.ear_t > 0.0f);

    // Stripes on the head.
    stamp(CAT_X + 23, CAT_Y + 7, STRIPES_HEAD, ROWS(STRIPES_HEAD));

    // Eyes at head height; pick the pair for the mood.
    const char *const *eye = EYE_OPEN;
    if (s.state == CAT_SLEEPING) {
        eye = EYE_CLOSED;
    } else if (s.state == CAT_PETTED) {
        eye = EYE_HAPPY;
    } else if (s.blink_t > 0.0f) {
        eye = EYE_CLOSED;
    } else if (s.since_touch > SLEEP_AFTER_S * 0.7f) {
        eye = EYE_HALF;
    }
    stamp(CAT_X + 12, CAT_Y + 17, eye, 6);
    stamp(CAT_X + 37, CAT_Y + 17, eye, 6);

    // Nose and mouth.
    if (s.state == CAT_STARTLED) {
        stamp(CAT_X + 24, CAT_Y + 25, NOSE_MOUTH_OPEN, ROWS(NOSE_MOUTH_OPEN));
    } else {
        stamp(CAT_X + 24, CAT_Y + 25, NOSE_MOUTH, ROWS(NOSE_MOUTH));
    }

    // Whiskers.
    for (int i = 0; i < 3; i++) {
        px(CAT_X + 2 + i, CAT_Y + 24, C_DARK);
        px(CAT_X + 1 + i, CAT_Y + 28, C_DARK);
        px(CAT_X + CAT_W - 5 + i, CAT_Y + 24, C_DARK);
        px(CAT_X + CAT_W - 4 + i, CAT_Y + 28, C_DARK);
    }

    // Blush while petted.
    if (s.state == CAT_PETTED) {
        stamp(CAT_X + 8, CAT_Y + 26, BLUSH, 1);
        stamp(CAT_X + 44, CAT_Y + 26, BLUSH, 1);
    }

    // Front paws, then the tail curled in front of the loaf.
    stamp(CAT_X + 20, CAT_Y + CAT_H - 5, PAWS, ROWS(PAWS));
    stamp(CAT_X + 40, CAT_Y + CAT_H - 12, tail, tail_rows);

    // Startle mark.
    if (s.state == CAT_STARTLED) {
        stamp(CAT_X + CAT_W - 2, CAT_Y - 8, BANG, ROWS(BANG));
    }

    // Sleep zzz, drifting gently.
    if (s.state == CAT_SLEEPING) {
        const int wob = (int)(2.0f * sinf(s.t * 1.3f));
        stamp(CAT_X + CAT_W - 8, CAT_Y - 6 + wob, ZED, ROWS(ZED));
        stamp(CAT_X + CAT_W - 1, CAT_Y - 13 - wob, ZED, ROWS(ZED));
    }

    // Hearts.
    for (int i = 0; i < MAX_HEARTS; i++) {
        if (s.hearts[i].alive) {
            stamp((int)s.hearts[i].x - 2, (int)s.hearts[i].y - 2, HEART, ROWS(HEART));
        }
    }
}

static int s_flush_errors;

int cat_flush_errors(void)
{
    return s_flush_errors;
}

void cat_render(void)
{
    compose();

    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        const int y0 = band * BAND_ROWS;

        for (int row = 0; row < BAND_ROWS; row++) {
            const uint8_t *crow = &s_canvas[((y0 + row) / PIX_SCALE) * CANVAS_W];
            uint16_t *out = buf + row * LCD_H_RES;
            for (int cx = 0; cx < CANVAS_W; cx++) {
                const uint16_t c = s_pal565[crow[cx]];
                out[0] = c;
                out[1] = c;
                out[2] = c;
                out[3] = c;
                out += PIX_SCALE;
            }
        }

        if (display_flush_band(band, buf) != 0) {
            s_flush_errors++;
        }
    }
}

void cat_debug_force(int mode)
{
    // The big cat has no sub-modes to force; map a few for preview parity.
    (void)mode;
}

#endif  // !CAT_SPRITE_STYLE
