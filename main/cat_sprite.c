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
#include <stdio.h>
#include <string.h>

#include "cat_anims.h"
#include "cat_bg.h"
#include "display.h"
#include "model.h"  // ENTICE_* constants only; model.h is pure C
#include "stats.h"  // STATS_GAUGE_ROWS: the gauges and the art agree

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
    C_ORANGE,  // the exercise gauge (Strava orange)
    C_PURPLE,  // the sleep gauge
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
    [C_ORANGE] = {252, 76, 2},
    [C_PURPLE] = {156, 84, 224},
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

// Every draw helper writes through px() to the current surface: the world
// canvas normally, the menu's finer canvas while it composes.
static uint8_t *s_surf = s_canvas;
static int s_surf_w = CANVAS_W;
static int s_surf_h = CANVAS_H;

static inline void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < s_surf_w && y >= 0 && y < s_surf_h) {
        s_surf[y * s_surf_w + x] = c;
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
        case 'g': return C_BATT_G;
        case 'o': return C_ORANGE;
        case 'y': return C_BATT_Y;
        case 'B': return C_BATT_B;
        case 'u': return C_PURPLE;
        default: return C_BG;
    }
}

// The menu renders on its own finer-grained canvas (6 px cells instead of
// 8), centred and inset so the panel's rounded corners cut nothing off.
#define MENU_SCALE 6
#define MENU_W (VIEW_W / MENU_SCALE)
#define MENU_H (VIEW_H / MENU_SCALE)
static uint8_t s_menu[MENU_W * MENU_H];

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

// His sleeping Zs, in the same purple as the sleep gauge they fill.
static const char *const ZED[] = {
    "uuu",
    "..u",
    ".u.",
    "uuu",
};

// World objects: the food bowl (salmon inside) and the yarn ball.
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

// HUD icons: yarn ball (play), fish (food), heart (affection), dumbbell
// (exercise), moon (his sleep). All 6 cells tall on the same top line,
// each a single colour with white as the only accent — no outlines — and
// each rendered as a gauge: grey art filled bottom-up with its colour.
static const char *const ICON_BALL[] = {
    ".rrrr....",
    "rrrrrW...",
    "rrrrWr...",
    "rrrWrr...",
    "rrWrrr...",
    ".rrrr.rrr",  // the loose thread trails off the bottom edge
};

// The fish is vivid blue (C_SLEEP was a near-match for the sky).
static const char *const ICON_FISH[] = {
    "..BBBB...B",
    ".BBBBBB.BB",
    "BWBBBBBBB.",
    "BBBBBBBBB.",
    ".BBBBBB.BB",
    "..BBBB...B",
};

static const char *const ICON_HEART[] = {
    ".rr.rr.",
    "rrrrrrr",
    "rrrrrrr",
    ".rrrrr.",
    "..rrr..",
    "...r...",
};

static const char *const ICON_EXER[] = {
    ".oo...oo.",
    "ooo...ooo",
    "ooooooooo",
    "ooooooooo",
    "ooo...ooo",
    ".oo...oo.",
};

static const char *const ICON_SLEEP[] = {
    "uuuuuu",
    "....uu",
    "...uu.",
    "..uu..",
    ".uu...",
    "uuuuuu",
};

// The menu's battery row icon: a solid block with its terminal nub.
static const char *const ICON_BATT[] = {
    "ggggggggg.",
    "ggggggggg.",
    "gggggggggg",
    "gggggggggg",
    "ggggggggg.",
    "ggggggggg.",
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
    [M_HIDDEN] = {ANIM_TROT, ANIM_TROT_N, 8.0f, true},
    [M_EMERGE] = {ANIM_PORTRAIT_TAIL, ANIM_PORTRAIT_TAIL_N, 3.5f, true},
};

// Every cycle worth looking at, in the order the sprite sheet had them.
static const struct {
    mode_t mode;
    const char *name;
} k_anims[] = {
    {M_PORTRAIT, "PORTRAIT"}, {M_PROFILE, "PROFILE"},
    {M_CLEAN_PAW, "CLEAN PAW"}, {M_CLEAN_EAR, "CLEAN EAR"},
    {M_TROT, "TROT"},           {M_LEAP, "LEAP"},
    {M_SLEEP, "SLEEP"},         {M_PAWING, "PAWING"},
    {M_BIG_JUMP, "BIG JUMP"},   {M_ANGRY, "ANGRY"},
    {M_PET, "PET"},             {M_EAT, "EAT"},
    {M_PLAY_PAW, "PLAY PAW"},
};
#define ANIM_COUNT ((int)(sizeof(k_anims) / sizeof(k_anims[0])))

// A little text, drawn by the menus and the animation browser.
static int draw_text(int x, int y, const char *str, uint8_t col);

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
    bool status_screen;  // the menu screen (tap heart, dumbbell or moon)
    int daypart;         // BG_* variant index chosen by the clock

    // World objects and sessions (Phase 2).
    bool bowl_alive, bowl_fresh;
    float bowl_x, bowl_ttl;
    bool ball_alive;
    float ball_x, ball_vx;
    float play_left;     // seconds remaining in the play session
    float food_spot;     // where food last appeared; a hungry cat lingers
    bool food_spot_set;
    float notice;        // reaction delay before he heads for a new drop
    int goal_kind;       // 0 none, 1 bowl, 2 ball
    float goal_stop;     // world x he trots toward
    bool eat_evt, play_evt, bite_evt;
    int bites_taken;     // mouthfuls this meal, one gauge row each
    int st_f, st_a, st_x, st_p, st_s;  // gauge values pushed in by main
    int streaks[5];                    // play, food, love, exercise, sleep
    int hits[5];                       // gauge reached full today
    bool test_menu;                    // the button-driven test menu is up
    int test_sel;
    bool anim_browse;                  // watching a single cycle play
    int anim_sel;
    bool daypart_forced;               // a forced daypart outranks the clock
    char dbg_line1[24], dbg_line2[24];

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

// The passive pool: sheet rows 1, 2 and 4. Pawing became a play move, and
// paw-cleaning is reserved for the after-dinner wash-up — idle grooming is
// ear-cleaning only.
static mode_t random_passive(void)
{
    const float r = frand01();
    if (r < 0.38f) return M_PORTRAIT;
    if (r < 0.70f) return M_PROFILE;
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
    // Relative to what you can see: his position normally, the camera's
    // view if he is off wandering or hiding.
    const float base = s.cam_free ? s.cam_x : s.world_x;
    const float dir = s.facing_left ? -1.0f : 1.0f;
    s.bowl_x = wrapf(base + dir * 13.0f * PIX_SCALE);
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
    const float base = s.cam_free ? s.cam_x : s.world_x;
    const float dir = s.facing_left ? -1.0f : 1.0f;
    s.ball_x = wrapf(base + dir * 12.0f * PIX_SCALE);
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
    s.bites_taken = 0;
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
           idx == C_UI_DIM || idx == C_ORANGE || idx == C_PURPLE;
}

void cat_init(void)
{
    memset(&s, 0, sizeof(s));
    s.decide_in = 3.0f;
    s.pos_x = CENTRE;
    s.facing_left = true;
    s.batt_pct = -1;
    s.st_f = s.st_a = s.st_x = s.st_p = s.st_s = 100;  // until main pushes
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
    if (s.anim_browse) {
        // A cycle on display: let it play and nothing else.
        s.t += dt;
        const int f = anim_frame();
        if (f != s.prev_frame) {
            s.prev_frame = f;
        }
        if (anim_done()) {
            s.t = 0.0f;  // loop the one-shots too, so they can be studied
        }
        s.was_down = touch->down;
        return;
    }
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

    // The menus swallow all interaction; the test menu is button-driven.
    if (s.test_menu) {
        s.purr = fmaxf(0.0f, s.purr - PET_RAMP_DOWN * dt);
        s.was_down = touch->down;
        return;
    }
    if (s.status_screen) {
        if (released_tap) {
            s.status_screen = false;
        }
        // Let any purr fade out while the menu is up.
        s.purr = fmaxf(0.0f, s.purr - PET_RAMP_DOWN * dt);
        s.was_down = touch->down;
        return;
    }

    // HUD gauge row: ball, fish, heart, dumbbell, moon. Hit-boxes outrank
    // every world gesture. Heart, dumbbell and moon all open the menu; the
    // fish refuses a tap when he is already fed; a new yarn ball needs the
    // old one gone and no dinner on the ground.
    bool tap_claimed = false;
    if (released_tap && s.last_y < 10.0f) {
        if (s.last_x >= 24.0f) {
            s.status_screen = true;
            s.was_down = touch->down;
            return;
        }
        if (s.last_x >= 12.0f) {
            if (s.st_f < 95) {
                drop_bowl();
            }
        } else if (!s.ball_alive && !(s.bowl_alive && s.bowl_fresh)) {
            drop_ball();
        }
        tap_claimed = true;
    }

    // While the park is empty, any tap calls him in; while he hides, a tap
    // on him (once found, mid-stride) brings him out — wary.
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
        // Scaring a cat that is already hiding: he still gives you one hiss
        // where he stands, then bolts farther.
        s.scare_level = (s.scare_level < 4) ? s.scare_level + 1 : 4;
        s.hiss = true;
        s.wary = true;
        enter(M_ANGRY);
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

        case M_HIDDEN: {
            // If the search camera has him in view he keeps walking away —
            // finding him is a chase, not a discovery of a statue.
            const float d = world_delta(s.cam_x, s.world_x);
            if (fabsf(d) < (CENTRE + SPRITE_W) * PIX_SCALE) {
                const float dir = (d >= 0.0f) ? 1.0f : -1.0f;
                s.facing_left = dir < 0.0f;
                s.world_x = wrapf(s.world_x +
                                  dir * TROT_SPEED * 0.6f * PIX_SCALE * dt);
            }
            break;
        }

        case M_EMERGE: {
            // The camera snaps back to following him: as soon as he is on
            // screen, centred behaviour is moments away.
            const float d = world_delta(s.cam_x, s.world_x);
            const float k = 1.0f - expf(-dt * 5.0f);
            if (fabsf(d) < 8.0f) {
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
            if (touch->down) {
                // He turns toward the hand that pets him; the dead zone
                // keeps a stroke across his middle from flip-flopping him.
                const float centre_x = s.pos_x + SPRITE_W * 0.5f;
                if (tx < centre_x - 2.0f) {
                    s.facing_left = true;
                } else if (tx > centre_x + 2.0f) {
                    s.facing_left = false;
                }
            }
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
            // A meal arrives a mouthful at a time, so the fish gauge climbs
            // one row per bite rather than jumping full at the last frame.
            if (s.bites_taken < STATS_GAUGE_ROWS &&
                s.t >= (float)(s.bites_taken + 1) * (6.0f / STATS_GAUGE_ROWS)) {
                s.bites_taken++;
                s.bite_evt = true;
            }
            if (s.t > 6.5f) {
                // Bowl finished: the refill fires through main, and like any
                // self-respecting cat he washes his paw after dinner — the
                // only time that animation appears.
                s.bowl_alive = false;
                s.bowl_fresh = false;
                s.eat_evt = true;
                enter(M_CLEAN_PAW);
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
        // A full play gauge ends the game: he has had his fill, and the
        // ball goes away with him.
        if (s.st_p >= 100) {
            s.play_left = 0.0f;
        }
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
        if (s.mode == M_HIDDEN && (frame == 1 || frame == 5) &&
            fabsf(world_delta(s.cam_x, s.world_x)) <
                (CENTRE + SPRITE_W) * PIX_SCALE) {
            s.step = true;  // soft footfalls when the search gets warm
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

void cat_set_stats(int food, int affection, int exercise, int play,
                   int sleep_v)
{
    s.st_f = food;
    s.st_a = affection;
    s.st_x = exercise;
    s.st_p = play;
    s.st_s = sleep_v;
}

void cat_set_streaks(const int streaks[5], const int hits[5])
{
    for (int i = 0; i < 5; i++) {
        s.streaks[i] = streaks[i];
        s.hits[i] = hits[i];
    }
}

bool cat_take_eat(void)
{
    const bool v = s.eat_evt;
    s.eat_evt = false;
    return v;
}

bool cat_take_bite(void)
{
    const bool v = s.bite_evt;
    s.bite_evt = false;
    return v;
}

bool cat_take_play_hit(void)
{
    const bool v = s.play_evt;
    s.play_evt = false;
    return v;
}

void cat_set_battery(int percent, bool charging)
{
    s.batt_pct = percent;
    s.batt_chg = charging;
}

void cat_set_daypart(int variant)
{
    // A daypart chosen in the test menu sticks until the next reboot, so
    // the art can be checked without waiting for the sun.
    if (!s.daypart_forced && variant >= 0 && variant < BG_VARIANTS) {
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

float cat_debug_cam(void)
{
    return s.cam_x;
}

void cat_debug_force(int mode)
{
    // 50+: scene setups for the host preview (harmless on hardware).
    if (mode == 50) {
        drop_bowl();
    } else if (mode == 51) {
        drop_ball();
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

// An icon as a vertical gauge: the art exists in quiet grey, and the bottom
// rows light up in their true colours as the value rises.
static void stamp_gauge(int x0, int y0, const char *const *rows, int nrows,
                        int value)
{
    const int lit_rows = ((value < 0 ? 0 : value > 100 ? 100 : value) *
                              nrows + 50) / 100;
    for (int y = 0; y < nrows; y++) {
        const char *row = rows[y];
        const bool lit = (nrows - 1 - y) < lit_rows;
        for (int x = 0; row[x]; x++) {
            if (row[x] != '.') {
                px(x0 + x, y0 + y, lit ? char_color(row[x]) : C_UI_DIM);
            }
        }
    }
}

static void compose(void)
{
    s_surf = s_canvas;
    s_surf_w = CANVAS_W;
    s_surf_h = CANVAS_H;
    // Index 0 = transparent: the painted scene shows through.
    memset(s_canvas, 0, sizeof(s_canvas));

    // World objects sit on the path, behind the cat.
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

    if (s.anim_browse) {
        // Its name, and how to get out, along the bottom.
        draw_text(2, CANVAS_H - 6, k_anims[s.anim_sel].name, C_WHITE);
        return;
    }

    // HUD: five gauges — yarn ball (play), fish (food), heart (affection),
    // dumbbell (exercise), moon (his sleep) — all 6 cells tall, 2 below the
    // top edge. Each fills bottom-up with its stat.
    stamp_gauge(2, 2, ICON_BALL, 6, s.st_p);
    stamp_gauge(13, 2, ICON_FISH, 6, s.st_f);
    stamp_gauge(25, 2, ICON_HEART, 6, s.st_a);
    stamp_gauge(34, 2, ICON_EXER, 6, s.st_x);
    stamp_gauge(45, 2, ICON_SLEEP, 6, s.st_s);

    // Battery: a hairline along the top edge, inset clear of the panel's
    // rounded corners. Charged length in green (blue on USB), the depleted
    // remainder in red.
    if (s.batt_pct >= 0) {
        const int x0 = 4, span = CANVAS_W - 8;
        const int fill = (s.batt_pct * span + 50) / 100;
        const uint8_t col = s.batt_chg ? C_BATT_B : C_BATT_G;
        for (int x = 0; x < span; x++) {
            px(x0 + x, 0, (x < fill) ? col : C_BATT_R);
        }
    }
}

// ---------------------------------------------------------------------------
// The menu screen and its little blocky font.
// ---------------------------------------------------------------------------

// A 3x5 blocky font, indexed by character: 5 rows per glyph, bit 2 is the
// leftmost pixel. Enough of an alphabet to write words on screen rather
// than assemble them from glyph ids.
static const uint8_t k_font[][5] = {
    {0, 0, 0, 0, 0},  // space
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
    {2, 5, 7, 5, 5},  // A
    {6, 5, 6, 5, 6},  // B
    {7, 4, 4, 4, 7},  // C
    {6, 5, 5, 5, 6},  // D
    {7, 4, 6, 4, 7},  // E
    {7, 4, 6, 4, 4},  // F
    {7, 4, 5, 5, 7},  // G
    {5, 5, 7, 5, 5},  // H
    {7, 2, 2, 2, 7},  // I
    {1, 1, 1, 5, 7},  // J
    {5, 5, 6, 5, 5},  // K
    {4, 4, 4, 4, 7},  // L
    {5, 7, 7, 5, 5},  // M
    {5, 7, 7, 7, 5},  // N
    {7, 5, 5, 5, 7},  // O
    {6, 5, 6, 4, 4},  // P
    {7, 5, 5, 7, 1},  // Q
    {6, 5, 6, 5, 5},  // R
    {7, 4, 7, 1, 7},  // S
    {7, 2, 2, 2, 2},  // T
    {5, 5, 5, 5, 7},  // U
    {5, 5, 5, 5, 2},  // V
    {5, 5, 7, 7, 5},  // W
    {5, 5, 2, 5, 5},  // X
    {5, 5, 2, 2, 2},  // Y
    {7, 1, 2, 4, 7},  // Z
    {5, 1, 2, 4, 5},  // %
    {1, 1, 5, 3, 2},  // ' (checkmark)
    {4, 2, 1, 2, 4},  // > (cursor)
    {0, 2, 0, 2, 0},  // :
    {0, 0, 7, 0, 0},  // -
};

#define GL_SPACE 0
#define GL_DIGIT0 1
#define GL_A 11
#define GL_PCT 37
#define GL_CHK 38
#define GL_CURSOR 39
#define GL_COLON 40
#define GL_DASH 41

static int glyph_of(char c)
{
    if (c >= '0' && c <= '9') return GL_DIGIT0 + (c - '0');
    if (c >= 'A' && c <= 'Z') return GL_A + (c - 'A');
    if (c >= 'a' && c <= 'z') return GL_A + (c - 'a');
    switch (c) {
        case '%': return GL_PCT;
        case '\'': return GL_CHK;
        case '>': return GL_CURSOR;
        case ':': return GL_COLON;
        case '-': return GL_DASH;
        default: return GL_SPACE;
    }
}

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

// Returns the x just past the text.
static int draw_text(int x, int y, const char *str, uint8_t col)
{
    for (; *str; str++) {
        draw_glyph(x, y, glyph_of(*str), col);
        x += 4;
    }
    return x;
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
        draw_glyph(x, y, GL_DIGIT0 + g[i], col);
        x += 4;
    }
    return x;
}

// The menu (tap the heart, dumbbell or Z): each gauge beside its word, a
// check or cross for "done today", and the streak — which counts today the
// moment its gauge reaches full. The battery sits in the list like any
// other icon, with its percentage.
static void compose_status(void)
{
    s_surf = s_menu;
    s_surf_w = MENU_W;
    s_surf_h = MENU_H;
    memset(s_menu, C_BLACK, sizeof(s_menu));

    const struct {
        const char *const *art;
        const char *word;
        int val;
        int streak;
        int hit;
    } rows[5] = {
        {ICON_BALL, "PLAY", s.st_p, s.streaks[0], s.hits[0]},
        {ICON_FISH, "FOOD", s.st_f, s.streaks[1], s.hits[1]},
        {ICON_HEART, "LOVE", s.st_a, s.streaks[2], s.hits[2]},
        {ICON_EXER, "EXERCISE", s.st_x, s.streaks[3], s.hits[3]},
        {ICON_SLEEP, "SLEEP", s.st_s, s.streaks[4], s.hits[4]},
    };

    for (int i = 0; i < 5; i++) {
        const int by = 3 + i * 9;
        stamp_gauge(10, by, rows[i].art, 6, rows[i].val);
        draw_text(23, by + 1, rows[i].word, C_WHITE);
        // Done today?
        draw_glyph(55, by + 1, rows[i].hit ? GL_CHK : glyph_of('X'),
                   rows[i].hit ? C_BATT_G : C_UI_DIM);
        // The streak counts today as soon as today is done.
        int v = rows[i].streak + (rows[i].hit ? 1 : 0);
        if (v > 99) {
            v = 99;
        }
        const uint8_t col = rows[i].hit ? C_WHITE : C_UI_DIM;
        const int digits = (v >= 10) ? 2 : 1;
        const int nx = draw_number(68 - digits * 4, by + 1, v, col);
        draw_glyph(nx, by + 1, glyph_of('X'), col);
    }

    // Battery, one of the icons now: a gauge of charge plus its number.
    const int by = 3 + 5 * 9;
    stamp_gauge(10, by, ICON_BATT, 6, (s.batt_pct >= 0) ? s.batt_pct : 0);
    draw_text(23, by + 1, "BATT", C_WHITE);
    if (s.batt_pct >= 0) {
        const int nx = draw_number(55, by + 1, s.batt_pct, C_WHITE);
        draw_glyph(nx, by + 1, GL_PCT, C_WHITE);
    }
}

// ---------------------------------------------------------------------------
// The test menu: hold PWR to open, PWR to move, BOOT to choose. Everything
// here was painful to reach otherwise — forcing a daypart meant waiting for
// dusk, testing an audition meant a rebuild.
// ---------------------------------------------------------------------------

static const char *const k_test_items[TEST_COUNT] = {
    "DAYPART", "WEATHER", "ANIMATIONS", "FILL ALL", "EMPTY ALL",
    "SCARE HIM", "SUMMON", "AUDITION", "SLEEP NOW", "EXIT",
};


// Short enough to sit in the value column without running off the edge.
static const char *const k_daypart_names[BG_VARIANTS] = {
    "DAY", "DAWN", "DUSK", "TWI", "NIGHT",
};

static void compose_test(void)
{
    s_surf = s_menu;
    s_surf_w = MENU_W;
    s_surf_h = MENU_H;
    memset(s_menu, C_BLACK, sizeof(s_menu));

    // The canvas is MENU_H rows and every glyph is 5 tall: header on row 0,
    // ten items five apart ending at 55, one footer line at 56..60. Nothing
    // overlaps and nothing falls off the bottom.
    draw_text(2, 0, "TEST MENU", C_BATT_Y);
    draw_text(48, 0, s.dbg_line2, C_UI_DIM);

    for (int i = 0; i < TEST_COUNT; i++) {
        const int y = 6 + i * 5;
        const bool on = (i == s.test_sel);
        if (on) {
            draw_glyph(2, y, GL_CURSOR, C_BATT_G);
        }
        draw_text(6, y, k_test_items[i], on ? C_WHITE : C_UI_DIM);
    }

    // What the three art rows would do, in a column clear of the labels.
    draw_text(50, 6, k_daypart_names[s.daypart], C_BATT_Y);
    draw_text(50, 11, "-", C_UI_DIM);
    {
        char n[12];
        snprintf(n, sizeof(n), "%d/%d", (s.anim_sel + 1) % 100,
                 ANIM_COUNT % 100);
        draw_text(50, 16, n, C_BATT_Y);
    }

    draw_text(2, MENU_H - 5, s.dbg_line1, C_UI_DIM);
}

// Browsing animations: the menu steps aside so the cycle can be watched in
// the park itself, with its name along the bottom. PWR walks the list, BOOT
// returns to the menu.
static void anim_show(void)
{
    s.cam_free = false;
    s.cam_x = s.world_x;
    s.pos_x = CENTRE;
    s.hiding = false;
    s.wandering = false;
    s.tilt_walk = false;
    s.tilt_leap = false;
    s.goal_kind = 0;
    enter(k_anims[s.anim_sel].mode);
}

bool cat_test_is_open(void)
{
    return s.test_menu || s.anim_browse;
}

void cat_test_close(void)
{
    s.test_menu = false;
    s.anim_browse = false;
}

int cat_button_pwr(void)
{
    if (s.anim_browse) {
        s.anim_sel = (s.anim_sel + 1) % ANIM_COUNT;
        anim_show();
        return -1;
    }
    if (!s.test_menu) {
        s.test_menu = true;
        s.test_sel = 0;
        s.status_screen = false;
        return -1;
    }
    s.test_sel = (s.test_sel + 1) % TEST_COUNT;
    return -1;
}

int cat_button_boot(void)
{
    if (s.anim_browse) {
        // Back to the menu, and back to living his life.
        s.anim_browse = false;
        s.test_menu = true;
        to_passive();
        return -1;
    }
    if (!s.test_menu) {
        return -1;
    }
    const int item = s.test_sel;
    switch (item) {
        case TEST_DAYPART:
            s.daypart = (s.daypart + 1) % BG_VARIANTS;
            s.daypart_forced = true;
            break;
        case TEST_WEATHER:
            break;  // nothing to cycle until weather exists
        case TEST_ANIM:
            s.test_menu = false;
            s.anim_browse = true;
            anim_show();
            break;
        case TEST_SCARE:
            s.scare_level = (s.scare_level < 4) ? s.scare_level + 1 : 4;
            s.wary = true;
            s.test_menu = false;
            if (!s.cam_free) {
                enter(M_ANGRY);
                s.hiss = true;
            }
            break;
        case TEST_SUMMON:
            s.test_menu = false;
            if (s.mode == M_ABSENT) {
                s.summon_evt = true;
                begin_enter();
            } else {
                begin_absent();
            }
            break;
        case TEST_EXIT:
            s.test_menu = false;
            break;
        default:
            break;  // FILL/EMPTY/AUDITION/SLEEP are main's to carry out
    }
    return item;
}

void cat_set_debug_lines(const char *a, const char *b)
{
    snprintf(s.dbg_line1, sizeof(s.dbg_line1), "%s", a ? a : "");
    snprintf(s.dbg_line2, sizeof(s.dbg_line2), "%s", b ? b : "");
}

static int s_flush_errors;

int cat_flush_errors(void)
{
    return s_flush_errors;
}

void cat_render(void)
{
    if (s.test_menu) {
        compose_test();
    } else if (s.status_screen) {
        compose_status();
    } else {
        compose();
    }

    // Whichever variant is resident — baked in flash, or streamed from the
    // card into PSRAM. NULL only if the park has not landed yet.
    const uint16_t (*world)[BG_STRIP_H] = cat_bg_strips(s.daypart);
    // The camera sets the scroll: pinned to him normally, free while he is
    // absent or hiding (the swipe search pans it).
    const int scroll = (int)s.cam_x - (int)(CENTRE * PIX_SCALE);

    for (int band = 0; band < BAND_COUNT; band++) {
        uint16_t *buf = display_acquire_band();
        const int y0 = band * BAND_ROWS;

        for (int row = 0; row < BAND_ROWS; row++) {
            uint16_t *out = buf + row * LCD_H_RES;

            if (s.status_screen || s.test_menu) {
                // The menu: finer 6 px cells on black, no world behind.
                const int lx_cell = (y0 + row) / MENU_SCALE;
                const uint16_t *pal = s_pal565[BG_DAY];
                const uint16_t black = pal[C_BLACK];
                for (int p = 0; p < LCD_H_RES; p++) {
                    const int ly_cell = (LCD_H_RES - 1 - p) / MENU_SCALE;
                    uint8_t ci = C_BLACK;
                    if (lx_cell < MENU_W && ly_cell < MENU_H) {
                        ci = s_menu[ly_cell * MENU_W + lx_cell];
                    }
                    out[p] = ci ? pal[ci] : black;
                }
                continue;
            }

            // Each panel row is one pre-rotated world-column strip.
            const int wx = ((scroll + y0 + row) % BG_WORLD_W + BG_WORLD_W) % BG_WORLD_W;
            if (world) {
                memcpy(out, world[wx], LCD_H_RES * sizeof(uint16_t));
            } else {
                memset(out, 0, LCD_H_RES * sizeof(uint16_t));
            }

            // Rotated overlay: a panel row is a logical column. The panel row
            // fixes the logical x cell; walking panel columns descends the
            // logical y cells (panel x = 367 maps to logical y = 0).
            const int lx_cell = (y0 + row) / PIX_SCALE;
            const uint16_t *pal = s_pal565[s.daypart];
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
