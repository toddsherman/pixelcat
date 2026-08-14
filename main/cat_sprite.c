#include "config.h"
#if CAT_SPRITE_STYLE

// The animated sprite cat, driven by the user's 10-row animation sheet:
//   1 portrait+tail  2 profile+tail  3 cleaning paw  4 cleaning ear
//   5 trotting       6 leaping       7 sleeping      8 pawing
//   9 big jump      10 angry
// Rows 1/2/3/4/8 rotate as passive idle states with occasional wandering.
// Facing is locked when an animation starts and only reconsidered when the
// next one begins, so left and right cycles never interleave.

#include "cat.h"

#include <math.h>
#include <string.h>

#include "cat_anims.h"
#include "display.h"

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

enum {
    C_BG = 0,
    C_BG2,
    C_OUT,
    C_BODY,
    C_SHADE,
    C_PINK,
    C_DARK,
    C_HEART,
    C_SLEEP,
    C_WHITE,
    C_COUNT,
};

static const uint8_t s_pal_rgb[C_COUNT][3] = {
    [C_BG] = {36, 33, 48},
    [C_BG2] = {29, 26, 39},
    [C_OUT] = {47, 47, 46},
    [C_BODY] = {224, 224, 224},
    [C_SHADE] = {181, 181, 181},
    [C_PINK] = {222, 117, 134},
    [C_DARK] = {44, 38, 48},
    [C_HEART] = {232, 80, 112},
    [C_SLEEP] = {150, 170, 196},
    [C_WHITE] = {255, 255, 255},
};

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

static uint16_t s_pal565[C_COUNT];

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

static uint8_t s_canvas[CANVAS_W * CANVAS_H];

#define FLOOR_Y 46
#define CHECKER 6
#define SPRITE_W ANIM_W
#define POS_MIN 1.0f
#define POS_MAX (CANVAS_W - SPRITE_W - 1.0f)
#define CENTRE ((CANVAS_W - SPRITE_W) / 2.0f)

static inline void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H) {
        s_canvas[y * CANVAS_W + x] = c;
    }
}

static uint8_t char_color(char ch)
{
    switch (ch) {
        case '#': return C_OUT;
        case 'w': return C_BODY;
        case 's': return C_SHADE;
        case 'p': return C_PINK;
        case 'k': return C_DARK;
        case '^': return C_DARK;
        case 'r': return C_HEART;
        case 'z': return C_SLEEP;
        case 'W': return C_WHITE;
        default: return C_BG;
    }
}

static void stampf(int x0, int y0, const char *const *rows, int nrows, bool flip)
{
    for (int y = 0; y < nrows; y++) {
        const char *row = rows[y];
        for (int x = 0; row[x] && x < SPRITE_W; x++) {
            if (row[x] != '.') {
                const int dx = flip ? (SPRITE_W - 1 - x) : x;
                px(x0 + dx, y0 + y, char_color(row[x]));
            }
        }
    }
}

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

// Small overlay sprites are stamped unflipped with their own widths.
static void stamp_fx(int x0, int y0, const char *const *rows, int nrows)
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
// Modes and animations
// ---------------------------------------------------------------------------

typedef enum {
    M_PORTRAIT,   // sheet row 1
    M_PROFILE,    // row 2
    M_CLEAN_PAW,  // row 3
    M_CLEAN_EAR,  // row 4
    M_TROT,       // row 5: held side, and wandering
    M_LEAP,       // row 6: double-tap on a side
    M_SLEEP,      // row 7: inactivity
    M_PAWING,     // row 8: part of the passive rotation
    M_BIG_JUMP,   // row 9: tap on the cat
    M_ANGRY,      // row 10: shaken
    M_PET,        // rubbed: purr + hearts (portrait pose)
    M_MODE_COUNT,
} mode_t;

typedef struct {
    const cat_frame_t *frames;
    uint8_t count;
    float fps;
    bool loop;
} anim_desc_t;

static const anim_desc_t k_anim[M_MODE_COUNT] = {
    [M_PORTRAIT] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 3.5f, true},
    [M_PROFILE] = {ANIM_PROFILE_TAIL, ANIM_PROFILE_TAIL_N, 3.5f, true},
    [M_CLEAN_PAW] = {ANIM_CLEAN_PAW, ANIM_CLEAN_PAW_N, 5.0f, true},
    [M_CLEAN_EAR] = {ANIM_CLEAN_EAR, ANIM_CLEAN_EAR_N, 5.0f, true},
    [M_TROT] = {ANIM_TROT, ANIM_TROT_N, 12.0f, true},
    [M_LEAP] = {ANIM_LEAP, ANIM_LEAP_N, 14.0f, false},
    [M_SLEEP] = {ANIM_SLEEP, ANIM_SLEEP_N, 2.0f, true},
    [M_PAWING] = {ANIM_PAWING, ANIM_PAWING_N, 6.0f, true},
    [M_BIG_JUMP] = {ANIM_BIG_JUMP, ANIM_BIG_JUMP_N, 12.0f, false},
    [M_ANGRY] = {ANIM_ANGRY, ANIM_ANGRY_N, 9.0f, true},
    [M_PET] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 5.5f, true},
};

// Behaviour tuning.
#define SHAKE_HISS_THRESHOLD 6.5f
#define RUN_ZONE_L (CANVAS_W / 3)
#define RUN_ZONE_R (CANVAS_W - CANVAS_W / 3)
#define HOLD_S 0.35f
#define DOUBLE_TAP_S 0.9f
#define TROT_SPEED 7.0f
#define LEAP_SPEED 20.0f
#define WANDER_FAR 12.0f  // beyond this from centre, the next wander goes home

typedef struct {
    float x, y, age;
    bool alive;
} heart_t;

#define MAX_HEARTS 6

static struct {
    mode_t mode;
    float t;
    float decide_in;
    float since_touch;
    float pos_x;
    float move_target;   // trot destination while wandering
    bool wandering;      // trot has a destination (vs held)
    bool facing_left;
    int move_dir;        // -1 left, +1 right, for held trot and leaps
    float purr;
    float stroke_speed;
    bool chirp;
    bool hiss;
    bool step;
    bool boing;
    bool slurp;
    int prev_frame;

    bool was_down;
    float last_x, last_y, moved, press_t;
    float tap_age;       // time since the last side tap
    int tap_side;        // -1 / +1 / 0 = none pending

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

static void enter(mode_t m)
{
    s.mode = m;
    s.t = 0.0f;
}

static bool anim_done(void)
{
    const anim_desc_t *a = &k_anim[s.mode];
    return !a->loop && s.t * a->fps >= (float)a->count;
}

static int anim_frame(void)
{
    const anim_desc_t *a = &k_anim[s.mode];
    int idx = (int)(s.t * a->fps);
    if (a->loop) {
        idx %= a->count;
    } else if (idx > a->count - 1) {
        idx = a->count - 1;
    }
    return idx;
}

static bool touch_near_cat(float cx, float cy)
{
    return cx > s.pos_x - 5.0f && cx < s.pos_x + SPRITE_W + 5.0f &&
           cy > FLOOR_Y - 24.0f && cy < FLOOR_Y + 8.0f;
}

// The passive pool: sheet rows 1, 2, 3, 4 and 8.
static mode_t random_passive(void)
{
    const float r = frand01();
    if (r < 0.30f) return M_PORTRAIT;
    if (r < 0.55f) return M_PROFILE;
    if (r < 0.72f) return M_CLEAN_PAW;
    if (r < 0.87f) return M_CLEAN_EAR;
    return M_PAWING;
}


static void to_passive(void)
{
    enter(random_passive());
    s.decide_in = 3.0f + 4.0f * frand01();
}

// Start a wandering trot. Far from centre, the destination is home; nearby,
// it is a modest hop either way. Facing locks toward the destination.
static void start_wander(void)
{
    float target;
    if (fabsf(s.pos_x - CENTRE) > WANDER_FAR) {
        target = CENTRE + (frand01() - 0.5f) * 4.0f;
    } else {
        const float d = 6.0f + 10.0f * frand01();
        target = s.pos_x + ((frand01() < 0.5f) ? -d : d);
        if (target < POS_MIN + 2.0f) target = s.pos_x + d;
        if (target > POS_MAX - 2.0f) target = s.pos_x - d;
    }
    s.move_target = target;
    s.wandering = true;
    s.facing_left = target < s.pos_x;
    enter(M_TROT);
}

void cat_init(void)
{
    memset(&s, 0, sizeof(s));
    s.mode = M_PORTRAIT;
    s.decide_in = 3.0f;
    s.pos_x = CENTRE;
    s.facing_left = true;
    s.rng = 0x9E3779B9;

    for (int i = 0; i < C_COUNT; i++) {
        const uint16_t v = (uint16_t)(((s_pal_rgb[i][0] & 0xF8) << 8) |
                                      ((s_pal_rgb[i][1] & 0xFC) << 3) |
                                      (s_pal_rgb[i][2] >> 3));
        s_pal565[i] = SWAP16(v);
    }
}

void cat_update(float dt, const cat_touch_t *touch, float shake)
{
    s.t += dt;
    s.tap_age += dt;
    if (s.tap_age > DOUBLE_TAP_S) {
        s.tap_side = 0;
    }

    // --- touch bookkeeping, canvas pixels ---
    const float tx = (float)touch->x / PIX_SCALE;
    const float ty = (float)touch->y / PIX_SCALE;
    float speed = 0.0f;

    if (touch->down) {
        if (s.was_down) {
            const float dx = tx - s.last_x;
            const float dy = ty - s.last_y;
            const float d = sqrtf(dx * dx + dy * dy);
            s.moved += d;
            if (dt > 1e-4f) {
                speed = d / dt;
            }
            s.press_t += dt;
        } else {
            s.moved = 0.0f;
            s.press_t = 0.0f;
        }
        s.last_x = tx;
        s.last_y = ty;
        s.since_touch = 0.0f;
    } else {
        s.since_touch += dt;
    }

    const float k = 1.0f - expf(-dt * 6.0f);
    s.stroke_speed += k * (speed - s.stroke_speed);

    const bool near = touch->down && touch_near_cat(tx, ty);
    const bool stroking = near && s.moved >= PET_MOVE_MIN_PX && s.stroke_speed > 4.0f;
    const bool released_tap = !touch->down && s.was_down &&
                              s.moved < PET_MOVE_MIN_PX && s.press_t < HOLD_S;
    const bool tap_on_cat = released_tap && touch_near_cat(s.last_x, s.last_y);
    const int side_of = (s.last_x < RUN_ZONE_L) ? -1 : (s.last_x > RUN_ZONE_R) ? 1 : 0;
    const bool tap_on_side = released_tap && !tap_on_cat && side_of != 0;
    const bool hold_side = touch->down && s.press_t >= HOLD_S && s.moved < 8.0f &&
                           !touch_near_cat(tx, ty) &&
                           (tx < RUN_ZONE_L || tx > RUN_ZONE_R);
    const int hold_dir = (tx < RUN_ZONE_L) ? -1 : 1;

    const bool interruptible = s.mode != M_LEAP && s.mode != M_BIG_JUMP;

    // --- global triggers, in priority order ---
    if (shake > SHAKE_HISS_THRESHOLD && s.mode != M_ANGRY) {
        enter(M_ANGRY);
        s.hiss = true;
    } else if (interruptible && s.mode != M_ANGRY) {
        if (stroking && s.mode != M_PET) {
            if (s.mode == M_SLEEP) {
                s.chirp = true;
            }
            enter(M_PET);
        } else if (tap_on_cat) {
            if (s.mode == M_SLEEP) {
                s.chirp = true;
            }
            enter(M_BIG_JUMP);
            s.boing = true;
        } else if (tap_on_side && s.mode != M_PET) {
            if (s.tap_side == side_of) {
                // Second tap on the same side: leap that way.
                s.move_dir = side_of;
                s.facing_left = side_of < 0;
                s.tap_side = 0;
                if (s.mode == M_SLEEP) {
                    s.chirp = true;
                }
                enter(M_LEAP);
                s.boing = true;
            } else {
                s.tap_side = side_of;
                s.tap_age = 0.0f;
            }
        } else if (hold_side && s.mode != M_TROT && s.mode != M_PET) {
            if (s.mode == M_SLEEP) {
                s.chirp = true;
            }
            s.move_dir = hold_dir;
            s.facing_left = hold_dir < 0;
            s.wandering = false;
            enter(M_TROT);
        }
    }

    // --- per-mode continuous behaviour ---
    switch (s.mode) {
        case M_ANGRY:
            if (shake > SHAKE_HISS_THRESHOLD) {
                s.t = fminf(s.t, 0.75f);  // keep looping while shaken
            } else if (s.t > 1.8f) {
                to_passive();
            }
            break;

        case M_TROT:
            if (s.wandering) {
                const float dir = (s.move_target > s.pos_x) ? 1.0f : -1.0f;
                s.pos_x += dir * TROT_SPEED * dt;
                if ((dir > 0 && s.pos_x >= s.move_target) ||
                    (dir < 0 && s.pos_x <= s.move_target)) {
                    s.pos_x = s.move_target;
                    to_passive();
                }
            } else {
                // Held: trot toward the held side until released or the wall.
                if (!hold_side) {
                    to_passive();
                    break;
                }
                s.pos_x += (float)s.move_dir * TROT_SPEED * dt;
            }
            if (s.pos_x < POS_MIN) s.pos_x = POS_MIN;
            if (s.pos_x > POS_MAX) s.pos_x = POS_MAX;
            break;

        case M_LEAP: {
            // Travel during the airborne middle of the cycle.
            const int f = anim_frame();
            if (f >= 1 && f <= 5) {
                s.pos_x += (float)s.move_dir * LEAP_SPEED * dt;
                if (s.pos_x < POS_MIN) s.pos_x = POS_MIN;
                if (s.pos_x > POS_MAX) s.pos_x = POS_MAX;
            }
            if (anim_done()) {
                // A pending tap on the same side chains another leap.
                if (s.tap_side == s.move_dir && s.tap_age < DOUBLE_TAP_S) {
                    s.tap_side = 0;
                    enter(M_LEAP);
                    s.boing = true;
                } else {
                    to_passive();
                }
            }
            break;
        }

        case M_BIG_JUMP:
            if (anim_done()) {
                to_passive();
            }
            break;

        case M_PET:
            if (!touch->down && s.stroke_speed < 4.0f && s.t > 0.6f) {
                to_passive();
            }
            break;

        case M_SLEEP:
            break;  // woken only by the triggers above

        default:  // passive pool
            if (s.since_touch > SLEEP_AFTER_S) {
                enter(M_SLEEP);
                break;
            }
            s.decide_in -= dt;
            if (s.decide_in <= 0.0f) {
                if (frand01() < 0.35f) {
                    start_wander();
                } else {
                    to_passive();
                }
            }
            break;
    }

    // --- purr chases petting ---
    float target = 0.0f;
    if (s.mode == M_PET && touch->down) {
        target = 0.35f + 0.65f * fminf(s.stroke_speed / PET_SPEED_FULL, 1.0f);
    } else if (s.mode == M_SLEEP) {
        target = 0.16f;  // the low sleeping purr
    }
    const float rate = (target > s.purr) ? PET_RAMP_UP : PET_RAMP_DOWN;
    const float step = rate * dt;
    if (fabsf(target - s.purr) <= step) {
        s.purr = target;
    } else {
        s.purr += (target > s.purr) ? step : -step;
    }

    // --- hearts ---
    if (s.mode == M_PET && touch->down) {
        s.heart_spawn -= dt;
        if (s.heart_spawn <= 0.0f) {
            for (int i = 0; i < MAX_HEARTS; i++) {
                if (!s.hearts[i].alive) {
                    s.hearts[i].alive = true;
                    s.hearts[i].x = tx + (frand01() - 0.5f) * 6.0f;
                    s.hearts[i].y = ty - 2.0f;
                    s.hearts[i].age = 0.0f;
                    break;
                }
            }
            s.heart_spawn = 0.4f;
        }
    }
    for (int i = 0; i < MAX_HEARTS; i++) {
        if (!s.hearts[i].alive) {
            continue;
        }
        s.hearts[i].age += dt;
        s.hearts[i].y -= dt * 6.0f;
        s.hearts[i].x += sinf(s.hearts[i].age * 5.0f) * dt * 2.0f;
        if (s.hearts[i].age > 1.6f || s.hearts[i].y < 1.0f) {
            s.hearts[i].alive = false;
        }
    }

    // Frame-edge sound events, emitted exactly once per frame change.
    const int frame = anim_frame();
    if (frame != s.prev_frame) {
        if (s.mode == M_TROT && (frame == 1 || frame == 5)) {
            s.step = true;
        }
        if (s.mode == M_CLEAN_PAW && frame == 1) {
            s.slurp = true;
        }
        s.prev_frame = frame;
    }

    s.was_down = touch->down;
}

bool cat_take_step(void)
{
    const bool v = s.step;
    s.step = false;
    return v;
}

bool cat_take_boing(void)
{
    const bool v = s.boing;
    s.boing = false;
    return v;
}

bool cat_take_slurp(void)
{
    const bool v = s.slurp;
    s.slurp = false;
    return v;
}

float cat_purr_level(void)
{
    return s.purr;
}

bool cat_take_chirp(void)
{
    const bool c = s.chirp;
    s.chirp = false;
    return c;
}

bool cat_take_hiss(void)
{
    const bool h = s.hiss;
    s.hiss = false;
    return h;
}

cat_state_t cat_state(void)
{
    switch (s.mode) {
        case M_PET: return CAT_PETTED;
        case M_ANGRY: return CAT_STARTLED;
        case M_SLEEP: return CAT_SLEEPING;
        default: return CAT_IDLE;
    }
}

void cat_debug_force(int mode)
{
    if (mode >= 0 && mode < M_MODE_COUNT) {
        enter((mode_t)mode);
        s.wandering = false;
        s.since_touch = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Composition + rendering
// ---------------------------------------------------------------------------

static void compose(void)
{
    for (int y = 0; y < CANVAS_H; y++) {
        const int cy = y / CHECKER;
        uint8_t *row = &s_canvas[y * CANVAS_W];
        for (int x = 0; x < CANVAS_W; x++) {
            row[x] = (((x / CHECKER) + cy) & 1) ? C_BG2 : C_BG;
        }
    }

    const anim_desc_t *a = &k_anim[s.mode];
    const cat_frame_t *f = &a->frames[anim_frame()];

    const int x0 = (int)(s.pos_x + 0.5f);
    const int y0 = FLOOR_Y - f->nrows - f->lift;
    stampf(x0, y0, f->rows, f->nrows, s.facing_left);

    if (s.mode == M_SLEEP) {
        const int wob = (int)(1.5f * sinf(s.t * 1.3f));
        stamp_fx(x0 + SPRITE_W, y0 - 3 + wob, ZED, 4);
        stamp_fx(x0 + SPRITE_W + 4, y0 - 8 - wob, ZED, 4);
    }
    for (int i = 0; i < MAX_HEARTS; i++) {
        if (s.hearts[i].alive) {
            stamp_fx((int)s.hearts[i].x - 2, (int)s.hearts[i].y - 2, HEART, 4);
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
                for (int r = 0; r < PIX_SCALE; r++) {
                    out[r] = c;
                }
                out += PIX_SCALE;
            }
        }

        if (display_flush_band(band, buf) != 0) {
            s_flush_errors++;
        }
    }
}

#endif  // CAT_SPRITE_STYLE
