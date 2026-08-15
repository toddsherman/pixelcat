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
#include "cat_bg.h"
#include "display.h"
#include "model.h"  // ENTICE_* constants only; model.h is pure C

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
    C_BATT_G,
    C_BATT_Y,
    C_BATT_R,
    C_BATT_B,
    C_BLACK,
    C_EYE,     // the cat's eyes: dark by day, glowing green at night
    C_POOP,
    C_UI_DIM,  // quiet HUD icons
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
    [C_BATT_G] = {96, 200, 96},
    [C_BATT_Y] = {228, 190, 70},
    [C_BATT_R] = {224, 80, 70},
    [C_BATT_B] = {90, 170, 235},
    [C_BLACK] = {0, 0, 0},
    [C_EYE] = {44, 38, 48},
    [C_POOP] = {124, 86, 55},
    [C_UI_DIM] = {98, 98, 112},
};

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

// One palette per background variant: the cat and effects ride the same
// colour grade as the scene. UI colours are exempt.
static uint16_t s_pal565[BG_VARIANTS][C_COUNT];

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

static uint8_t s_canvas[CANVAS_W * CANVAS_H];

#define FLOOR_Y 42   // feet on the path (landscape logical rows)
#define SPRITE_W ANIM_W
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
        case 'k': return C_EYE;
        case '^': return C_DARK;
        case 'r': return C_HEART;
        case 'z': return C_SLEEP;
        case 'W': return C_WHITE;
        case 'b': return C_POOP;
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

// World objects: the food bowl (salmon inside), the yarn ball, a poop, and
// the little puff left behind when one is cleaned.
static const char *const BOWL[] = {
    ".#pppp#.",
    "#ssssss#",
    ".#ssss#.",
    "..####..",
};

static const char *const BALL[] = {
    ".####.",
    "#rprp#",
    "#prrr#",
    "#rrpr#",
    "#prpr#",
    ".####.",
};

static const char *const POOP_ART[] = {
    "..b...",
    ".bbb..",
    ".bbbb.",
    "bbbbbb",
};

static const char *const POOF[] = {
    "W.W.W",
    ".W.W.",
    "W.W.W",
};

// HUD icons, top-left: yarn ball (play), fish (feed), heart (status). All
// three are 5 cells tall and sit on the same top line.
static const char *const ICON_BALL[] = {
    ".###.",
    "#rrW#",
    "#rWr#",
    "#Wrr#",
    ".###.",
};

static const char *const ICON_FISH[] = {
    "..####..#",
    ".#zzzz##z",
    "#zWzzzzzz",
    ".#zzzz##z",
    "..####..#",
};

static const char *const ICON_HEART[] = {
    ".r.r.",
    "rrrrr",
    "rrrrr",
    ".rrr.",
    "..r..",
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
    M_EAT,        // at the bowl (clean-paw frames, slurping)
    M_PLAY_PAW,   // batting the yarn ball (pawing frames)
    M_ABSENT,     // elsewhere: the park is empty
    M_ENTER,      // trotting in from an edge to centre
    M_FLEE,       // panicked leaps off the screen after a scare
    M_HIDDEN,     // hiding in the world; the camera is free to search
    M_EMERGE,     // found: the camera eases back to him
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
    [M_EAT] = {ANIM_CLEAN_PAW, ANIM_CLEAN_PAW_N, 5.0f, true},
    [M_PLAY_PAW] = {ANIM_PAWING, ANIM_PAWING_N, 7.0f, true},
    [M_ABSENT] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 3.5f, true},
    [M_ENTER] = {ANIM_TROT, ANIM_TROT_N, 12.0f, true},
    [M_FLEE] = {ANIM_LEAP, ANIM_LEAP_N, 20.0f, true},
    [M_HIDDEN] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 2.5f, true},
    [M_EMERGE] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 3.5f, true},
};

// Behaviour tuning.
#define SHAKE_HISS_THRESHOLD 6.5f
#define RUN_ZONE_L (CANVAS_W / 3)
#define RUN_ZONE_R (CANVAS_W - CANVAS_W / 3)
#define HOLD_S 0.35f
#define DOUBLE_TAP_S 0.9f
#define TROT_SPEED 7.0f
// Tilt thresholds, as a fraction of full gravity (sin of the tilt angle).
// 10-45%% walks him; past 45%% he bounds in chained leaps. Each band has a
// little hysteresis so he does not stutter at the boundary.
#define TILT_G 9.81f
#define TILT_WALK_ON (0.10f * TILT_G)
#define TILT_WALK_OFF (0.08f * TILT_G)
#define TILT_LEAP_ON (0.45f * TILT_G)
#define TILT_LEAP_OFF (0.40f * TILT_G)
#define LEAP_SPEED 34.0f

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
    float pos_x;         // screen position, pinned to CENTRE: the world moves
    float world_x;       // cat's position in the looping world, logical px
    float move_remaining;  // px left in the current wander
    bool wandering;      // trot has a destination (vs tilt-driven)
    bool tilt_walk;      // trot is being driven by device tilt
    bool tilt_leap;      // leap chain is being driven by device tilt
    bool facing_left;
    int move_dir;        // -1 left, +1 right, for held trot and leaps
    float purr;
    float stroke_speed;
    bool chirp;
    bool hiss;
    bool step;
    bool boing;
    bool slurp;
    bool swipe;
    int dash;            // pending dash direction, 0 = none
    float walked;        // logical px moved since last cat_take_walked()
    int prev_frame;

    bool was_down;
    bool press_on_cat;   // the current press began on the cat
    float last_x, last_y, moved, press_t;
    float tap_age;       // time since the last side tap
    int tap_side;        // -1 / +1 / 0 = none pending

    heart_t hearts[MAX_HEARTS];
    float heart_spawn;
    int batt_pct;        // -1 until the first reading arrives
    bool batt_chg;
    bool batt_screen;    // full-screen battery view (tap the corner bar)
    bool status_screen;  // full-screen stat page (tap the hearts)
    int daypart;         // BG_* variant index chosen by the clock

    // World objects and sessions (Phase 2).
    bool bowl_alive, bowl_fresh;
    float bowl_x, bowl_ttl;
    bool ball_alive;
    float ball_x, ball_vx;
    float play_left;     // seconds remaining in the play session
    float poop_x[3];
    bool poop_live[3];
    float food_spot;     // where food last appeared; a hungry cat lingers
    bool food_spot_set;
    float poof_x, poof_t;
    float notice;        // reaction delay before he heads for a new drop
    int goal_kind;       // 0 none, 1 bowl, 2 ball
    float goal_stop;     // world x he trots toward
    bool eat_evt, play_evt, clean_evt;
    int st_f, st_a, st_x;  // stats pushed in for HUD + status page

    // Camera and trust (Phase 3). Normally the camera is pinned to the cat;
    // absence, fleeing and hiding set it free.
    float cam_x;
    bool cam_free;
    bool hiding;         // he is at a hiding spot (or being petted there)
    bool wary;           // emerged after a scare but not yet forgiven
    int scare_level;     // unreconciled scares; sets distance and cost
    int flee_dir;
    float absent_in;     // seconds until he wanders in on his own
    bool summon_evt, reconcile_evt;
    int entice;          // -1 none, else the ENTICE_* act he is performing
    float entice_next;

    uint32_t rng;
} s;

static float frand01(void)
{
    s.rng ^= s.rng << 13;
    s.rng ^= s.rng >> 17;
    s.rng ^= s.rng << 5;
    return (float)(s.rng >> 8) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------
// How the stats read on him (Phase 4). Each returns a 0..1 fraction.
// ---------------------------------------------------------------------------

static float f01(int v)
{
    return (v < 0) ? 1.0f : (v > 100) ? 1.0f : (float)v / 100.0f;
}

// A cat who has done his day's exercise is pleasantly worn out: everything
// runs a beat slower as the bar fills, and a fresh morning cat is snappy.
static float anim_pace(void)
{
    return 1.1f - 0.3f * f01(s.st_x);
}

// Affection shows in the purr: a devoted cat rumbles harder. The floor
// keeps reconciliation purrs reachable even for an aloof cat.
static float purr_scale(void)
{
    return 0.7f + 0.3f * f01(s.st_a);
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

// The passive pool: sheet rows 1, 2, 3 and 4. Pawing left the rotation when
// it became a play move — it only appears batting the yarn ball now.
static mode_t random_passive(void)
{
    const float r = frand01();
    if (r < 0.34f) return M_PORTRAIT;
    if (r < 0.62f) return M_PROFILE;
    if (r < 0.82f) return M_CLEAN_PAW;
    return M_CLEAN_EAR;
}


static void to_passive(void)
{
    enter(random_passive());
    // A well-exercised cat loafs longer between decisions; a fresh one is
    // quick to move on to the next thing.
    const float loaf = 1.0f + 0.8f * f01(s.st_x);
    s.decide_in = (3.0f + 4.0f * frand01()) * loaf;
}

// Start a wandering trot: the world is endless, so wander is just a
// direction and a distance.
static void start_wander(void)
{
    s.move_dir = (frand01() < 0.5f) ? -1 : 1;
    s.move_remaining = (6.0f + 10.0f * frand01()) * PIX_SCALE;
    s.wandering = true;
    s.facing_left = s.move_dir < 0;
    enter(M_TROT);
}

// ---------------------------------------------------------------------------
// World objects: positions live in the looping world, like the cat.
// ---------------------------------------------------------------------------

static float wrapf(float x)
{
    x = fmodf(x, (float)BG_WORLD_W);
    return (x < 0.0f) ? x + (float)BG_WORLD_W : x;
}

// Shortest signed distance from one world x to another.
static float world_delta(float from, float to)
{
    float d = fmodf(to - from, (float)BG_WORLD_W);
    if (d < -(float)BG_WORLD_W * 0.5f) d += (float)BG_WORLD_W;
    if (d > (float)BG_WORLD_W * 0.5f) d -= (float)BG_WORLD_W;
    return d;
}

// Canvas cell of a world x (object centres), relative to the camera.
static int obj_canvas_x(float wx)
{
    return (int)(CENTRE + world_delta(s.cam_x, wx) / PIX_SCALE + 0.5f);
}

static void drop_bowl(void)
{
    if (s.bowl_alive) {
        return;  // one bowl at a time
    }
    const float dir = s.facing_left ? -1.0f : 1.0f;
    s.bowl_x = wrapf(s.world_x + dir * 13.0f * PIX_SCALE);
    s.bowl_alive = true;
    s.bowl_fresh = true;
    s.bowl_ttl = 90.0f;
    s.notice = 0.9f;
    s.food_spot = s.bowl_x;  // hungry cats will remember this place
    s.food_spot_set = true;
}

static void drop_ball(void)
{
    if (s.ball_alive) {
        return;
    }
    const float dir = s.facing_left ? -1.0f : 1.0f;
    s.ball_x = wrapf(s.world_x + dir * 12.0f * PIX_SCALE);
    s.ball_vx = 0.0f;
    s.ball_alive = true;
    s.play_left = 60.0f;
    s.notice = 0.7f;
}

// Trot toward the bowl or ball, stopping with it at his face.
static void goal_arrive(void);

static void start_goal(int kind)
{
    const float obj = (kind == 1) ? s.bowl_x
                      : (kind == 2) ? s.ball_x
                                    : s.food_spot;
    const float d = world_delta(s.world_x, obj);
    const int dir = (d < 0.0f) ? -1 : 1;
    const float off = (dir > 0) ? (SPRITE_W - 1.0f) * PIX_SCALE
                                : -4.0f * PIX_SCALE;
    s.goal_kind = kind;
    s.goal_stop = wrapf(obj - off);
    s.facing_left = d < 0.0f;
    s.wandering = false;
    s.tilt_walk = false;
    s.tilt_leap = false;
    const float d2 = world_delta(s.world_x, s.goal_stop);
    if (fabsf(d2) < 4.0f) {
        goal_arrive();
        return;
    }
    s.move_dir = (d2 < 0.0f) ? -1 : 1;
    enter(M_TROT);
}

static void goal_arrive(void)
{
    const int kind = s.goal_kind;
    s.goal_kind = 0;
    if (kind == 3) {
        // Food-seeking: he stands where dinner usually appears and waits,
        // pointedly.
        enter(M_PORTRAIT);
        s.decide_in = 5.0f + 5.0f * frand01();
        return;
    }
    const float obj = (kind == 1) ? s.bowl_x : s.ball_x;
    s.facing_left = world_delta(s.world_x, obj) < 0.0f;
    enter((kind == 1) ? M_EAT : M_PLAY_PAW);
}

// ---------------------------------------------------------------------------
// Absence, fleeing and hiding: the camera comes unpinned from the cat.
// ---------------------------------------------------------------------------

static void begin_absent(void)
{
    s.cam_free = true;
    s.absent_in = 25.0f + 35.0f * frand01();
    enter(M_ABSENT);
}

static void begin_enter(void)
{
    // He comes in from a random edge, headed for the middle of the view.
    const int from = (frand01() < 0.5f) ? -1 : 1;
    s.world_x = wrapf(s.cam_x + (float)from * (CENTRE + SPRITE_W + 3.0f) *
                                    PIX_SCALE);
    s.facing_left = from > 0;
    s.cam_free = true;  // the camera holds still while he walks in
    enter(M_ENTER);
}

static void begin_flee(void)
{
    s.flee_dir = (frand01() < 0.5f) ? -1 : 1;
    s.facing_left = s.flee_dir < 0;
    s.cam_free = true;  // the world stops following him: he runs out of it
    s.goal_kind = 0;
    s.tilt_walk = false;
    s.tilt_leap = false;
    s.wandering = false;
    enter(M_FLEE);
    s.dash = s.flee_dir;
}

// Farther with every unreconciled scare, up to the loop's maximum.
static float hide_distance(void)
{
    float d = 500.0f + 550.0f * (float)s.scare_level;
    const float cap = (float)BG_WORLD_W * 0.5f - 150.0f;
    return (d > cap) ? cap : d;
}

static void place_hide(void)
{
    s.world_x = wrapf(s.cam_x + (float)s.flee_dir * hide_distance());
    s.hiding = true;
    enter(M_HIDDEN);
}

// The same grade curves gen_bg.py bakes into the scene, applied to the
// sprite palette so cat and world always match.
static void grade_rgb(int variant, int r, int g, int b, int *ro, int *go, int *bo)
{
    const float gy = (r * 30 + g * 59 + b * 11) / 100.0f;
    float rf = r, gf = g, bf = b;
    switch (variant) {
        case BG_DAWN:
            rf = r * .82f + 34; gf = g * .68f + 16; bf = b * .78f + 26;
            break;
        case BG_DUSK:
            // Soft rose rather than orange, to sit in the pink sunset art.
            rf = r * .88f + 20; gf = g * .70f + 12; bf = b * .64f + 16;
            break;
        case BG_TWILIGHT:
            rf = (r + (gy - r) * .3f) * .48f + 22;
            gf = (g + (gy - g) * .3f) * .38f + 10;
            bf = (b + (gy - b) * .3f) * .62f + 34;
            break;
        case BG_NIGHT:
            rf = (r + (gy - r) * .45f) * .24f + 6;
            gf = (g + (gy - g) * .45f) * .28f + 9;
            bf = (b + (gy - b) * .45f) * .52f + 28;
            break;
        default:
            break;
    }
    *ro = (rf < 0) ? 0 : (rf > 255) ? 255 : (int)rf;
    *go = (gf < 0) ? 0 : (gf > 255) ? 255 : (int)gf;
    *bo = (bf < 0) ? 0 : (bf > 255) ? 255 : (int)bf;
}

static bool palette_ungraded(int idx)
{
    return idx == C_BATT_G || idx == C_BATT_Y || idx == C_BATT_R ||
           idx == C_BATT_B || idx == C_BLACK || idx == C_WHITE ||
           idx == C_UI_DIM;
}

void cat_init(void)
{
    memset(&s, 0, sizeof(s));
    s.decide_in = 3.0f;
    s.pos_x = CENTRE;
    s.facing_left = true;
    s.batt_pct = -1;
    s.st_f = s.st_a = s.st_x = 100;  // until main pushes real values
    s.rng = 0x9E3779B9;
    s.entice = -1;
    // Every boot is a wake: the park starts empty until something calls him.
    begin_absent();

    for (int v = 0; v < BG_VARIANTS; v++) {
        for (int i = 0; i < C_COUNT; i++) {
            int r = s_pal_rgb[i][0], g = s_pal_rgb[i][1], b = s_pal_rgb[i][2];
            if (i == C_EYE && v == BG_NIGHT) {
                // Bright green cat eyes in the dark.
                r = 90; g = 255; b = 110;
            } else if (!palette_ungraded(i)) {
                grade_rgb(v, r, g, b, &r, &g, &b);
            }
            const uint16_t val = (uint16_t)(((r & 0xF8) << 8) |
                                            ((g & 0xFC) << 3) | (b >> 3));
            s_pal565[v][i] = SWAP16(val);
        }
    }
}

void cat_update(float dt, const cat_touch_t *touch, float shake, float tilt)
{
    s.t += dt * anim_pace();  // a tired cat runs a beat slow, everywhere
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
            // The search camera: ONLY while he hides, and only for strokes
            // that began off the cat — a press that started on him is a pet,
            // and a pan that crosses his body stays a pan (a purr is earned,
            // never brushed in passing).
            if (s.mode == M_HIDDEN && !s.press_on_cat) {
                s.cam_x = wrapf(s.cam_x - dx * PIX_SCALE * 1.5f);
            }
        } else {
            s.moved = 0.0f;
            s.press_t = 0.0f;
            s.press_on_cat = touch_near_cat(tx, ty);
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

    // The battery and status views swallow all interaction: one tap exits.
    const bool tap_on_batt = released_tap && s.last_x >= 43.0f &&
                             s.last_y <= 9.0f;
    if (s.batt_screen || s.status_screen) {
        if (released_tap) {
            s.batt_screen = false;
            s.status_screen = false;
        }
        // Let any purr fade out while a gauge is up.
        s.purr = fmaxf(0.0f, s.purr - PET_RAMP_DOWN * dt);
        s.was_down = touch->down;
        return;
    }
    if (tap_on_batt && s.batt_pct >= 0) {
        s.batt_screen = true;
        s.was_down = touch->down;
        return;
    }

    // HUD icon row (top-left): ball, fish, hearts. Icon hit-boxes outrank
    // every world gesture, same as the battery corner.
    bool tap_claimed = false;
    if (released_tap && s.last_y < 9.0f && s.last_x < 43.0f) {
        if (s.last_x >= 19.0f) {
            s.status_screen = true;
            s.was_down = touch->down;
            return;
        }
        if (s.last_x >= 9.0f) {
            drop_bowl();
        } else {
            drop_ball();
        }
        tap_claimed = true;
    }

    // A tap on a poop cleans it: puff, swipe sound, one less indignity.
    if (released_tap && !tap_claimed) {
        for (int i = 0; i < 3; i++) {
            if (!s.poop_live[i]) {
                continue;
            }
            const float cxp = CENTRE + world_delta(s.world_x, s.poop_x[i]) /
                                           PIX_SCALE;
            if (s.last_x >= cxp - 5.0f && s.last_x <= cxp + 5.0f &&
                s.last_y > FLOOR_Y - 10.0f) {
                s.poop_live[i] = false;
                s.clean_evt = true;
                s.swipe = true;
                s.poof_x = s.poop_x[i];
                s.poof_t = 0.5f;
                tap_claimed = true;
                break;
            }
        }
    }

    // While the park is empty, any tap calls him in; while he hides, a tap
    // on him (once found) brings him out — wary.
    if (released_tap && !tap_claimed) {
        if (s.mode == M_ABSENT) {
            s.summon_evt = true;
            begin_enter();
            tap_claimed = true;
        } else if (s.mode == M_HIDDEN &&
                   touch_near_cat(s.last_x, s.last_y)) {
            s.hiding = false;
            enter(M_EMERGE);
            tap_claimed = true;
        }
    }

    // Meals and play sessions hold his attention: no dozing off, and tilt or
    // side taps cannot yank him away from the bowl or the ball. A stroll to
    // the food spot (goal 3) is not busy — anything can interrupt it.
    const bool busy = (s.goal_kind != 0 && s.goal_kind != 3) ||
                      s.mode == M_EAT || s.mode == M_PLAY_PAW ||
                      s.play_left > 0.0f || (s.bowl_alive && s.bowl_fresh);
    if (busy) {
        s.since_touch = 0.0f;
    }

    const bool interruptible = s.mode != M_LEAP && s.mode != M_BIG_JUMP;

    const int tilt_dir = (tilt < 0) ? -1 : 1;
    const float tilt_mag = fabsf(tilt);
    if (tilt_mag > TILT_WALK_ON && s.mode != M_SLEEP) {
        s.since_touch = 0.0f;
    }
    // Tilt moves *him*, and off-screen he is not there to move.
    const bool tilt_ok = interruptible && s.mode != M_ANGRY && s.mode != M_PET &&
                         s.mode != M_SLEEP && !busy && !s.cam_free;
    if (tilt_ok && tilt_mag > TILT_LEAP_ON && s.mode != M_LEAP) {
        // Steep tilt: bound that way in chained leaps.
        s.move_dir = tilt_dir;
        s.facing_left = tilt_dir < 0;
        s.wandering = false;
        s.tilt_walk = false;
        s.tilt_leap = true;
        s.goal_kind = 0;  // a food-spot stroll yields to the tilt
        enter(M_LEAP);
        s.dash = tilt_dir;
    } else if (tilt_ok && tilt_mag > TILT_WALK_ON && tilt_mag <= TILT_LEAP_ON &&
               !(s.mode == M_TROT && s.tilt_walk && tilt_dir == s.move_dir)) {
        s.move_dir = tilt_dir;
        s.facing_left = tilt_dir < 0;
        s.wandering = false;
        s.tilt_walk = true;
        s.goal_kind = 0;
        enter(M_TROT);
    }

    // --- global triggers, in priority order ---
    if (shake > SHAKE_HISS_THRESHOLD && s.mode == M_HIDDEN) {
        // Scaring a cat that is already hiding re-hides him farther, now.
        s.scare_level = (s.scare_level < 4) ? s.scare_level + 1 : 4;
        s.hiss = true;
        s.flee_dir = (frand01() < 0.5f) ? -1 : 1;
        place_hide();
    } else if (shake > SHAKE_HISS_THRESHOLD && s.mode != M_ANGRY &&
               !s.cam_free) {
        s.scare_level = (s.scare_level < 4) ? s.scare_level + 1 : 4;
        s.wary = true;
        enter(M_ANGRY);
        s.hiss = true;
    } else if (interruptible && s.mode != M_ANGRY) {
        if (stroking && s.mode != M_PET && s.mode != M_EAT &&
            (!s.cam_free || (s.mode == M_HIDDEN && s.press_on_cat))) {
            if (s.mode == M_SLEEP) {
                s.chirp = true;
            }
            enter(M_PET);
        } else if (tap_on_cat && !tap_claimed && !busy && !s.cam_free) {
            // A tap earns a glance and a tail flick; jumping is a play move.
            // An aloof cat sometimes cannot be bothered to give even that.
            if (s.mode == M_SLEEP) {
                s.chirp = true;
                enter(M_PORTRAIT);
                s.decide_in = 1.5f;
            } else if (frand01() < 0.25f + 0.75f * f01(s.st_a)) {
                enter(M_PORTRAIT);
                s.decide_in = 1.5f;
            }
        } else if (tap_on_side && !tap_claimed && !busy && !s.cam_free &&
                   s.mode != M_PET) {
            if (s.tap_side == side_of) {
                // Second tap on the same side: leap that way.
                s.move_dir = side_of;
                s.facing_left = side_of < 0;
                s.tap_side = 0;
                if (s.mode == M_SLEEP) {
                    s.chirp = true;
                }
                s.tilt_leap = false;
                enter(M_LEAP);
                s.dash = side_of;
            } else {
                s.tap_side = side_of;
                s.tap_age = 0.0f;
            }
        }
    }

    // --- per-mode continuous behaviour ---
    switch (s.mode) {
        case M_ANGRY:
            if (shake > SHAKE_HISS_THRESHOLD) {
                s.t = fminf(s.t, 0.75f);  // keep looping while shaken
            } else if (s.t > 1.8f) {
                // The hiss is done: he bolts. Never a walk.
                begin_flee();
            }
            break;

        case M_ABSENT:
            s.pos_x = -1000.0f;  // nothing to pet out there
            s.absent_in -= dt;
            if (s.absent_in <= 0.0f) {
                begin_enter();  // he wanders back on his own, no grudge
            }
            break;

        case M_ENTER: {
            const float d = world_delta(s.world_x, s.cam_x);
            const float step = TROT_SPEED * PIX_SCALE * 1.2f * dt;
            if (fabsf(d) <= step) {
                s.world_x = s.cam_x;
                s.cam_free = false;
                to_passive();
            } else {
                s.world_x = wrapf(s.world_x + ((d < 0.0f) ? -step : step));
            }
            break;
        }

        case M_FLEE: {
            const float step = 3.0f * LEAP_SPEED * PIX_SCALE * dt;
            s.world_x = wrapf(s.world_x + (float)s.flee_dir * step);
            if (fabsf(world_delta(s.cam_x, s.world_x)) >
                (CENTRE + SPRITE_W + 4.0f) * PIX_SCALE) {
                place_hide();
            }
            break;
        }

        case M_HIDDEN:
            break;  // he sits tight; the searching is yours to do

        case M_EMERGE: {
            // The camera eases back to following him.
            const float d = world_delta(s.cam_x, s.world_x);
            const float k = 1.0f - expf(-dt * 3.0f);
            if (fabsf(d) < 3.0f) {
                s.cam_x = s.world_x;
                s.cam_free = false;
                to_passive();
            } else {
                s.cam_x = wrapf(s.cam_x + d * k);
            }
            break;
        }

        case M_TROT:
            if (s.goal_kind) {
                const float d = world_delta(s.world_x, s.goal_stop);
                const float step = TROT_SPEED * PIX_SCALE * dt;
                if (fabsf(d) <= step) {
                    s.world_x = s.goal_stop;
                    goal_arrive();
                } else {
                    s.world_x += (d < 0.0f) ? -step : step;
                    s.walked += step;
                }
                break;
            }
            if (s.tilt_walk) {
                if (tilt_mag < TILT_WALK_OFF) {
                    s.tilt_walk = false;
                    to_passive();
                    break;
                }
                // Brisker toward the top of the walking band.
                const float mag = fminf((tilt_mag - TILT_WALK_ON) /
                                        (TILT_LEAP_ON - TILT_WALK_ON), 1.0f);
                const float tstep = TROT_SPEED * PIX_SCALE *
                                    (0.7f + 0.5f * mag) * dt;
                s.world_x += (float)s.move_dir * tstep;
                s.walked += tstep;
                break;
            }
            if (s.wandering) {
                const float step = TROT_SPEED * PIX_SCALE * dt;
                s.world_x += (float)s.move_dir * step;
                s.walked += step;
                s.move_remaining -= step;
                if (s.move_remaining <= 0.0f) {
                    to_passive();
                }
            } else {
                to_passive();
            }
            break;

        case M_LEAP: {
            // Travel during the airborne middle of the cycle.
            const int f = anim_frame();
            if (f >= 1 && f <= 5) {
                const float lstep = LEAP_SPEED * PIX_SCALE * dt;
                s.world_x += (float)s.move_dir * lstep;
                s.walked += lstep;
            }
            if (anim_done()) {
                if (s.tilt_leap && tilt_mag > TILT_LEAP_OFF) {
                    // Still held steep: bound again, tracking the direction.
                    s.move_dir = tilt_dir;
                    s.facing_left = tilt_dir < 0;
                    enter(M_LEAP);
                    s.dash = tilt_dir;
                } else if (s.tilt_leap && tilt_mag > TILT_WALK_OFF) {
                    // Eased into the walking band mid-bound: land into a trot.
                    s.tilt_leap = false;
                    s.tilt_walk = true;
                    s.move_dir = tilt_dir;
                    s.facing_left = tilt_dir < 0;
                    enter(M_TROT);
                } else if (!s.tilt_leap && s.tap_side == s.move_dir &&
                           s.tap_age < DOUBLE_TAP_S) {
                    // A pending tap on the same side chains another leap.
                    s.tap_side = 0;
                    enter(M_LEAP);
                    s.dash = s.move_dir;
                } else {
                    s.tilt_leap = false;
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
            if (s.hiding && s.purr > 0.55f && s.t > 1.2f) {
                // An earned purr at the hiding spot: peace is made.
                s.hiding = false;
                s.wary = false;
                s.scare_level = 0;
                s.reconcile_evt = true;
                enter(M_EMERGE);
                break;
            }
            if (!touch->down && s.stroke_speed < 4.0f && s.t > 0.6f) {
                if (s.hiding) {
                    enter(M_HIDDEN);  // not convinced yet; back to his spot
                } else {
                    to_passive();
                }
            }
            break;

        case M_EAT:
            if (!s.bowl_alive) {
                to_passive();
                break;
            }
            if (s.t > 6.5f) {
                // Bowl finished: the refill fires through main, and like any
                // self-respecting cat he washes up after dinner.
                s.bowl_alive = false;
                s.bowl_fresh = false;
                s.eat_evt = true;
                enter((frand01() < 0.5f) ? M_CLEAN_PAW : M_CLEAN_EAR);
                s.decide_in = 5.0f + 3.0f * frand01();
            }
            break;

        case M_PLAY_PAW: {
            if (!s.ball_alive) {
                to_passive();
                break;
            }
            const float dobj = world_delta(s.world_x, s.ball_x);
            if (fabsf(dobj) > 22.0f * PIX_SCALE) {
                start_goal(2);  // it rolled away — chase
                break;
            }
            if (s.t > 1.4f) {
                const float r = frand01();
                if (r < 0.35f) {
                    // Pounce! The only place the big jump lives now.
                    s.boing = true;
                    s.play_evt = true;
                    enter(M_BIG_JUMP);
                } else if (fabsf(dobj) > 6.0f * PIX_SCALE) {
                    start_goal(2);
                } else {
                    s.t = 0.0f;  // keep batting
                }
            }
            break;
        }

        case M_SLEEP:
            break;  // woken only by the triggers above

        default:  // passive pool
            if (s.entice >= 0 && !s.cam_free) {
                // His opening act, performed until the audition resolves.
                s.since_touch = 0.0f;
                s.entice_next -= dt;
                if (s.entice_next <= 0.0f) {
                    switch (s.entice) {
                        case ENTICE_JUMP:
                            s.boing = true;
                            enter(M_BIG_JUMP);
                            s.entice_next = 2.2f + frand01();
                            break;
                        case ENTICE_PAW:
                            enter(M_PAWING);  // pawing the glass
                            s.entice_next = 2.8f + frand01();
                            break;
                        case ENTICE_PACE:
                            start_wander();
                            s.entice_next = 1.5f;
                            break;
                        default:  // loud purr / meow: he holds and calls
                            enter(M_PORTRAIT);
                            s.entice_next = 2.0f;
                            break;
                    }
                }
                break;
            }
            if (s.bowl_alive && s.bowl_fresh && s.notice <= 0.0f) {
                start_goal(1);
                break;
            }
            if (s.ball_alive && s.notice <= 0.0f) {
                start_goal(2);
                break;
            }
            // A full exercise bar is a cat who wants his nap: he dozes off
            // sooner the more of his day he has already had.
            if (s.since_touch > SLEEP_AFTER_S * (1.4f - 0.8f * f01(s.st_x))) {
                enter(M_SLEEP);
                break;
            }
            s.decide_in -= dt;
            if (s.decide_in <= 0.0f) {
                // A hungry cat drifts to where food appears and waits there.
                if (s.st_f < 35 && s.food_spot_set && frand01() < 0.5f &&
                    fabsf(world_delta(s.world_x, s.food_spot)) >
                        8.0f * PIX_SCALE) {
                    start_goal(3);
                    break;
                }
                // An aloof cat keeps moving away; a devoted one stays close.
                const float wander_p = 0.20f + 0.35f * (1.0f - f01(s.st_a));
                if (frand01() < wander_p) {
                    start_wander();
                } else {
                    to_passive();
                }
            }
            break;
    }

    // --- world object upkeep ---
    if (s.notice > 0.0f) {
        s.notice -= dt;
    }
    if (s.bowl_alive) {
        s.bowl_ttl -= dt;
        if (s.bowl_ttl <= 0.0f) {
            // Untouched long enough: the bowl despawns.
            s.bowl_alive = false;
            s.bowl_fresh = false;
            if (s.goal_kind == 1) {
                s.goal_kind = 0;
                to_passive();
            }
        }
    }
    if (s.ball_alive) {
        s.ball_x = wrapf(s.ball_x + s.ball_vx * dt);
        s.ball_vx -= s.ball_vx * 3.0f * dt;
        s.play_left -= dt;
        if (s.play_left <= 0.0f) {
            s.ball_alive = false;
            s.play_left = 0.0f;
            if (s.goal_kind == 2) {
                s.goal_kind = 0;
            }
            if (s.mode == M_PLAY_PAW || (s.mode == M_TROT && !s.tilt_walk &&
                                         !s.wandering)) {
                to_passive();
            }
        }
    }
    if (s.poof_t > 0.0f) {
        s.poof_t -= dt;
    }

    // --- purr chases petting ---
    float target = 0.0f;
    if (s.mode == M_PET && touch->down) {
        target = (0.35f + 0.65f * fminf(s.stroke_speed / PET_SPEED_FULL, 1.0f)) *
                 purr_scale();
    } else if (s.mode == M_SLEEP) {
        target = 0.16f;  // the low sleeping purr
    } else if (s.entice == ENTICE_PURR && !s.cam_free) {
        target = 0.75f;  // the loud invitation purr
    }
    const float rate = (target > s.purr) ? PET_RAMP_UP : PET_RAMP_DOWN;
    const float step = rate * dt;
    if (fabsf(target - s.purr) <= step) {
        s.purr = target;
    } else {
        s.purr += (target > s.purr) ? step : -step;
    }

    // A purring cat is a cat at peace: an earned purr after he emerged wary
    // also resets the escalation. A purr cannot be faked.
    if (s.wary && !s.hiding && s.mode == M_PET && s.purr > 0.55f) {
        s.wary = false;
        s.scare_level = 0;
        s.reconcile_evt = true;
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
            // Hearts come thick from a devoted cat, sparingly from an aloof
            // one.
            s.heart_spawn = 0.25f + 0.75f * (1.0f - f01(s.st_a));
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
        if ((s.mode == M_TROT || s.mode == M_ENTER) &&
            (frame == 1 || frame == 5)) {
            s.step = true;
        }
        if (s.mode == M_FLEE && frame == 0) {
            s.dash = s.flee_dir;  // a whoosh per panicked leap
        }
        if ((s.mode == M_CLEAN_PAW || s.mode == M_CLEAN_EAR ||
             s.mode == M_EAT) && frame == 1) {
            s.slurp = true;
        }
        if (s.mode == M_PAWING && frame == 2) {
            s.swipe = true;
        }
        if (s.mode == M_PLAY_PAW && frame == 2) {
            // A bat connects: score it and send the ball rolling, usually
            // forward, sometimes squirting back through his legs.
            s.swipe = true;
            s.play_evt = true;
            float dir = s.facing_left ? -1.0f : 1.0f;
            if (frand01() < 0.2f) {
                dir = -dir;
            }
            s.ball_vx = dir * (40.0f + 60.0f * frand01());
        }
        s.prev_frame = frame;
    }

    s.world_x = fmodf(fmodf(s.world_x, (float)BG_WORLD_W) + (float)BG_WORLD_W,
                      (float)BG_WORLD_W);

    // Camera: pinned to him in normal life; free while he is gone, fleeing,
    // hidden or being walked back to.
    if (!s.cam_free) {
        s.cam_x = s.world_x;
        s.pos_x = CENTRE;
    } else if (s.mode != M_ABSENT) {
        s.pos_x = CENTRE + world_delta(s.cam_x, s.world_x) / PIX_SCALE;
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

bool cat_take_swipe(void)
{
    const bool v = s.swipe;
    s.swipe = false;
    return v;
}

int cat_take_dash(void)
{
    const int v = s.dash;
    s.dash = 0;
    return v;
}

float cat_take_walked(void)
{
    const float v = s.walked;
    s.walked = 0.0f;
    return v;
}

void cat_set_stats(int food, int affection, int exercise)
{
    s.st_f = food;
    s.st_a = affection;
    s.st_x = exercise;
}

void cat_spawn_poop(void)
{
    for (int i = 0; i < 3; i++) {
        if (!s.poop_live[i]) {
            const float sign = (frand01() < 0.5f) ? -1.0f : 1.0f;
            s.poop_x[i] = wrapf(s.world_x + sign *
                                (150.0f + frand01() * 750.0f));
            s.poop_live[i] = true;
            return;
        }
    }
}

int cat_poop_count(void)
{
    int n = 0;
    for (int i = 0; i < 3; i++) {
        n += s.poop_live[i] ? 1 : 0;
    }
    return n;
}

bool cat_take_eat(void)
{
    const bool v = s.eat_evt;
    s.eat_evt = false;
    return v;
}

bool cat_take_play_hit(void)
{
    const bool v = s.play_evt;
    s.play_evt = false;
    return v;
}

bool cat_take_poop_clean(void)
{
    const bool v = s.clean_evt;
    s.clean_evt = false;
    return v;
}

void cat_set_battery(int percent, bool charging)
{
    s.batt_pct = percent;
    s.batt_chg = charging;
}

void cat_set_daypart(int variant)
{
    if (variant >= 0 && variant < BG_VARIANTS) {
        s.daypart = variant;
    }
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
        case M_PET: return s.hiding ? CAT_HIDING : CAT_PETTED;
        case M_ANGRY: return CAT_STARTLED;
        case M_SLEEP: return CAT_SLEEPING;
        case M_ABSENT:
        case M_ENTER: return CAT_ABSENT;
        case M_FLEE:
        case M_HIDDEN:
        case M_EMERGE: return CAT_HIDING;
        default: return CAT_IDLE;
    }
}

bool cat_take_summon(void)
{
    const bool v = s.summon_evt;
    s.summon_evt = false;
    return v;
}

bool cat_take_reconcile(void)
{
    const bool v = s.reconcile_evt;
    s.reconcile_evt = false;
    return v;
}

void cat_hear_sound(void)
{
    // Sound summons an absent cat; it never summons a scared one.
    if (s.mode == M_ABSENT) {
        s.summon_evt = true;
        begin_enter();
    }
}

int cat_scare_level(void)
{
    return s.scare_level;
}

bool cat_wary(void)
{
    return s.wary;
}

void cat_restore_trust(int scare_level, bool wary)
{
    s.scare_level = (scare_level < 0) ? 0 : (scare_level > 4) ? 4 : scare_level;
    s.wary = wary;
}

void cat_entice(int kind)
{
    s.entice = (kind >= 0 && kind < MODEL_ARMS) ? kind : ENTICE_MEOW;
    s.entice_next = 0.4f;
    if (s.mode == M_SLEEP) {
        enter(M_PORTRAIT);
    }
}

void cat_entice_stop(void)
{
    s.entice = -1;
    if (s.mode == M_PAWING) {
        to_passive();
    }
}

float cat_debug_world(void)
{
    return s.world_x;
}

void cat_debug_force(int mode)
{
    // 50+: scene setups for the host preview (harmless on hardware).
    if (mode == 50) {
        drop_bowl();
    } else if (mode == 51) {
        drop_ball();
    } else if (mode == 52) {
        s.poop_x[0] = wrapf(s.world_x - 5.0f * PIX_SCALE);
        s.poop_live[0] = true;
    } else if (mode == 53) {
        s.status_screen = true;
    } else if (mode == 60) {
        begin_absent();
    } else if (mode == 61) {
        // Hidden with the camera parked right on his spot (found).
        s.cam_free = true;
        s.hiding = true;
        s.wary = true;
        s.world_x = s.cam_x;
        enter(M_HIDDEN);
    } else if (mode >= 0 && mode < M_MODE_COUNT) {
        // Preview/test forcing implies a present, centred cat.
        s.cam_free = false;
        s.cam_x = s.world_x;
        s.pos_x = CENTRE;
        s.hiding = false;
        enter((mode_t)mode);
        s.wandering = false;
        s.since_touch = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Composition + rendering
// ---------------------------------------------------------------------------

// Icons stamp in their own colours when lit, uniform grey when quiet.
static void stamp_icon(int x0, int y0, const char *const *rows, int nrows,
                       bool bright)
{
    for (int y = 0; y < nrows; y++) {
        const char *row = rows[y];
        for (int x = 0; row[x]; x++) {
            if (row[x] != '.') {
                px(x0 + x, y0 + y, bright ? char_color(row[x]) : C_UI_DIM);
            }
        }
    }
}

// One HUD heart: 2 = full, 1 = half, 0 = empty outline.
static void draw_heart_icon(int x0, int y0, int level)
{
    for (int y = 0; y < 5; y++) {
        const char *row = ICON_HEART[y];
        for (int x = 0; x < 5; x++) {
            if (row[x] == 'r') {
                const bool lit = (level == 2) || (level == 1 && x < 3);
                px(x0 + x, y0 + y, lit ? C_HEART : C_UI_DIM);
            }
        }
    }
}

static int min3(int a, int b, int c)
{
    const int m = a < b ? a : b;
    return m < c ? m : c;
}

static void compose(void)
{
    // Index 0 = transparent: the painted scene shows through.
    memset(s_canvas, 0, sizeof(s_canvas));

    // World objects sit on the path, behind the cat.
    for (int i = 0; i < 3; i++) {
        if (s.poop_live[i]) {
            const int cx = obj_canvas_x(s.poop_x[i]);
            if (cx > -6 && cx < CANVAS_W + 6) {
                stamp_fx(cx - 3, FLOOR_Y - 3, POOP_ART, 4);
            }
        }
    }
    if (s.bowl_alive) {
        const int cx = obj_canvas_x(s.bowl_x);
        if (cx > -8 && cx < CANVAS_W + 8) {
            stamp_fx(cx - 4, FLOOR_Y - 3, BOWL, 4);
        }
    }
    if (s.ball_alive) {
        const int cx = obj_canvas_x(s.ball_x);
        if (cx > -6 && cx < CANVAS_W + 6) {
            stamp_fx(cx - 3, FLOOR_Y - 5, BALL, 6);
        }
    }
    if (s.poof_t > 0.0f) {
        const int cx = obj_canvas_x(s.poof_x);
        stamp_fx(cx - 2, FLOOR_Y - 6, POOF, 3);
    }

    const anim_desc_t *a = &k_anim[s.mode];
    const cat_frame_t *f = &a->frames[anim_frame()];

    const int x0 = (int)(s.pos_x + 0.5f);
    const int y0 = FLOOR_Y - f->nrows - f->lift;
    if (s.mode != M_ABSENT) {
        stampf(x0, y0, f->rows, f->nrows, s.facing_left);
    }

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

    // HUD, top-left: yarn ball, fish, three hearts — all 5 cells tall, all
    // 2 cells below the top edge, clear of the left edge. Quiet grey by
    // default; an icon brightens as an invitation when its stat wants
    // attention.
    stamp_icon(3, 2, ICON_BALL, 5, s.ball_alive || s.st_x < 35);
    stamp_icon(10, 2, ICON_FISH, 5,
               (s.bowl_alive && s.bowl_fresh) || s.st_f < 40);
    const int worst = min3(s.st_f, s.st_a, s.st_x);
    const int halves = (worst * 6 + 50) / 100;
    for (int i = 0; i < 3; i++) {
        int lvl = halves - 2 * i;
        lvl = (lvl < 0) ? 0 : (lvl > 2) ? 2 : lvl;
        draw_heart_icon(21 + i * 6, 2, lvl);
    }

    // Battery, top right, on the same line at the same height: a closed
    // 9x5 rectangle with 7 fill cells.
    if (s.batt_pct >= 0) {
        const int bx = 45, by = 2;
        for (int x = 0; x < 9; x++) {
            px(bx + x, by, C_OUT);
            px(bx + x, by + 4, C_OUT);
        }
        for (int y = 1; y < 4; y++) {
            px(bx, by + y, C_OUT);
            px(bx + 8, by + y, C_OUT);
        }
        const int fill = (s.batt_pct * 7 + 50) / 100;
        uint8_t col = C_BATT_G;
        if (s.batt_chg) {
            col = C_BATT_B;
        } else if (s.batt_pct <= 15) {
            col = C_BATT_R;
        } else if (s.batt_pct <= 40) {
            col = C_BATT_Y;
        }
        for (int y = 1; y < 4; y++) {
            for (int x = 0; x < 7; x++) {
                px(bx + 1 + x, by + y, (x < fill) ? col : C_DARK);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Full-screen battery view: black screen, a large gauge, percent below.
// ---------------------------------------------------------------------------

// 3x5 font, rows top to bottom, bit 2 = left pixel. Digits, %, then the
// letters the status page needs.
enum { GL_PCT = 10, GL_F, GL_A, GL_E, GL_X, GL_B, GL_P };

static const uint8_t k_font[17][5] = {
    {7, 5, 5, 5, 7},  // 0
    {2, 6, 2, 2, 7},  // 1
    {7, 1, 7, 4, 7},  // 2
    {7, 1, 7, 1, 7},  // 3
    {5, 5, 7, 1, 1},  // 4
    {7, 4, 7, 1, 7},  // 5
    {7, 4, 7, 5, 7},  // 6
    {7, 1, 1, 1, 1},  // 7
    {7, 5, 7, 5, 7},  // 8
    {7, 5, 7, 1, 7},  // 9
    {5, 1, 2, 4, 5},  // %
    {7, 4, 6, 4, 4},  // F
    {2, 5, 7, 5, 5},  // A
    {7, 4, 6, 4, 7},  // E
    {5, 5, 2, 5, 5},  // X
    {6, 5, 6, 5, 6},  // B
    {6, 5, 6, 4, 4},  // P
};

static void draw_glyph(int gx, int gy, int glyph, uint8_t col)
{
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 3; x++) {
            if (k_font[glyph][y] & (4 >> x)) {
                px(gx + x, gy + y, col);
            }
        }
    }
}

static int draw_number(int x, int y, int v, uint8_t col)  // returns next x
{
    int g[3], n = 0;
    if (v >= 100) {
        g[n++] = v / 100;
    }
    if (v >= 10) {
        g[n++] = (v / 10) % 10;
    }
    g[n++] = v % 10;
    for (int i = 0; i < n; i++) {
        draw_glyph(x, y, g[i], col);
        x += 4;
    }
    return x;
}

// Full-screen status page (tap the hearts): three labelled stat bars — all
// aspiring to be full — then battery and the poop count along the bottom.
static void compose_status(void)
{
    memset(s_canvas, C_BLACK, sizeof(s_canvas));

    const struct {
        int glyph;
        int val;
    } rows[3] = {
        {GL_F, s.st_f}, {GL_A, s.st_a}, {GL_X, s.st_x},
    };

    const int bx = 8, bw = 34;
    for (int i = 0; i < 3; i++) {
        const int by = 5 + i * 10;
        const int val = (rows[i].val < 0) ? 0 : rows[i].val;
        draw_glyph(3, by + 1, rows[i].glyph, C_WHITE);
        for (int x = 0; x < bw; x++) {
            px(bx + x, by, C_WHITE);
            px(bx + x, by + 6, C_WHITE);
        }
        for (int y = 1; y < 6; y++) {
            px(bx, by + y, C_WHITE);
            px(bx + bw - 1, by + y, C_WHITE);
        }
        const uint8_t col = (val > 50) ? C_BATT_G
                            : (val > 25) ? C_BATT_Y
                                         : C_BATT_R;
        const int fill = (val * (bw - 2) + 50) / 100;
        for (int y = 1; y < 6; y++) {
            for (int x = 0; x < fill; x++) {
                px(bx + 1 + x, by + y, col);
            }
        }
        draw_number(bx + bw + 2, by + 1, val, C_WHITE);
    }

    // Bottom line: battery left, poops right.
    const int by = 38;
    draw_glyph(4, by, GL_B, C_WHITE);
    if (s.batt_pct >= 0) {
        const int nx = draw_number(9, by, s.batt_pct, C_WHITE);
        draw_glyph(nx, by, GL_PCT, C_WHITE);
    }
    draw_glyph(38, by, GL_P, C_WHITE);
    draw_number(43, by, cat_poop_count(), C_WHITE);
}

static void compose_battery(void)
{
    memset(s_canvas, C_BLACK, sizeof(s_canvas));

    const int pct = (s.batt_pct >= 0) ? s.batt_pct : 0;
    uint8_t col = C_BATT_G;
    if (s.batt_chg) {
        col = C_BATT_B;
    } else if (pct <= 15) {
        col = C_BATT_R;
    } else if (pct <= 40) {
        col = C_BATT_Y;
    }

    // Gauge: 32x8 outline centred, 30x6 fill inside.
    const int bx = (CANVAS_W - 32) / 2, by = 22;
    for (int x = 0; x < 32; x++) {
        px(bx + x, by, C_WHITE);
        px(bx + x, by + 7, C_WHITE);
    }
    for (int y = 1; y < 7; y++) {
        px(bx, by + y, C_WHITE);
        px(bx + 31, by + y, C_WHITE);
    }
    const int fill = (pct * 30 + 50) / 100;
    for (int y = 1; y < 7; y++) {
        for (int x = 0; x < fill; x++) {
            px(bx + 1 + x, by + y, col);
        }
    }

    // "NN%" centred below.
    int glyphs[4], n = 0;
    if (pct >= 100) glyphs[n++] = 1;
    if (pct >= 100) glyphs[n++] = 0;
    if (pct < 100 && pct >= 10) glyphs[n++] = pct / 10;
    if (pct < 10) glyphs[n++] = pct;
    else glyphs[n++] = pct % 10;
    glyphs[n++] = 10;  // %
    const int tw = n * 4 - 1;
    int tx = (CANVAS_W - tw) / 2;
    for (int i = 0; i < n; i++) {
        draw_glyph(tx, by + 11, glyphs[i], C_WHITE);
        tx += 4;
    }
}

static int s_flush_errors;

int cat_flush_errors(void)
{
    return s_flush_errors;
}

void cat_render(void)
{
    if (s.batt_screen) {
        compose_battery();
    } else if (s.status_screen) {
        compose_status();
    } else {
        compose();
    }

    const uint16_t (*world)[BG_STRIP_H] = cat_bg[s.daypart];
    // The camera sets the scroll: pinned to him normally, free while he is
    // absent or hiding (the swipe search pans it).
    const int scroll = (int)s.cam_x - (int)(CENTRE * PIX_SCALE);

    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        const int y0 = band * BAND_ROWS;

        for (int row = 0; row < BAND_ROWS; row++) {
            uint16_t *out = buf + row * LCD_H_RES;
            // Each panel row is one pre-rotated world-column strip.
            const int wx = ((scroll + y0 + row) % BG_WORLD_W + BG_WORLD_W) % BG_WORLD_W;
            memcpy(out, world[wx], LCD_H_RES * sizeof(uint16_t));

            // Rotated overlay: a panel row is a logical column. The panel row
            // fixes the logical x cell; walking panel columns descends the
            // logical y cells (panel x = 367 maps to logical y = 0).
            const int lx_cell = (y0 + row) / PIX_SCALE;
            const uint16_t *pal = s_pal565[(s.batt_screen || s.status_screen)
                                               ? BG_DAY
                                               : s.daypart];
            for (int pxb = 0; pxb < LCD_H_RES / PIX_SCALE; pxb++) {
                const int ly_cell = (LCD_H_RES / PIX_SCALE - 1) - pxb;
                const uint8_t ci = s_canvas[ly_cell * CANVAS_W + lx_cell];
                if (ci) {
                    const uint16_t c = pal[ci];
                    uint16_t *o = out + pxb * PIX_SCALE;
                    for (int r = 0; r < PIX_SCALE; r++) {
                        o[r] = c;
                    }
                }
            }
        }

        if (display_flush_band(band, buf) != 0) {
            s_flush_errors++;
        }
    }
}

#endif  // CAT_SPRITE_STYLE
