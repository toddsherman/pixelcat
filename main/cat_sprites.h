#pragma once

// Sprite set for the wandering cat, recreated from the user's reference sheet:
// a minimal white cat, dark outline, ~16px, drawn side-on facing LEFT.
// '.' transparent, '#' outline, 'w' body, 's' shade, 'p' pink, 'k' dark,
// '^' happy closed eye. stamp_flip() mirrors these to face right.
//
// Every pose shares the same head block (3-row eared, eyes inboard of the
// outline) so the cat stays recognisable between poses.

// Sitting upright, tail curled around the front paws.
static const char *const SP_SIT_A[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#......",
    "#wkwwkw#......",
    "#wwwwww#......",
    ".#wwww#.......",
    ".#wwww##......",
    ".#wwwwww#.....",
    ".#wwwwwws#....",
    ".#wwwwwwws#...",
    ".#wwwwwwws#...",
    ".#wwwwwwws#...",
    ".#wwwwwww##s#.",
    ".#w#www#w##s#.",
    ".##.###.##ss#.",
    "....########..",
};

// Sitting, tail raised and flicking behind.
static const char *const SP_SIT_B[] = {
    ".#...#.....#s#",
    "#p#.#p#....#s#",
    "#wp##pw#..#s#.",
    "#wwwwww#..#s#.",
    "#wkwwkw#.#s#..",
    "#wwwwww#.#s#..",
    ".#wwww#..#s#..",
    ".#wwww##.#s#..",
    ".#wwwwww##s#..",
    ".#wwwwwww#s#..",
    ".#wwwwwwws#...",
    ".#wwwwwwws#...",
    ".#wwwwwww#....",
    ".#w#www#w#....",
    ".##.###.##....",
};

// Grooming: head bent down licking a raised front paw.
static const char *const SP_GROOM[] = {
    "..#...#.......",
    ".#p#.#p#......",
    ".#wp##pw#.....",
    ".#wwwwww#.##..",
    ".#wkwwkw##ws#.",
    "..#wwwww##ws#.",
    "..#p#wwwwws#..",
    "..##.#wwwws#..",
    "...#wwwwwws#..",
    "...#wwwwwws#..",
    "...#wwwwwws#..",
    "...#wwwwww#...",
    "...#w#www##...",
    "...##.###.....",
};

// Sitting but head turned back over the shoulder.
static const char *const SP_LOOK_BACK[] = {
    ".......#...#..",
    "......#p#.#p#.",
    ".....#wp##pw#.",
    ".....#wwwwww#.",
    ".....#wkwwkw#.",
    ".....#wwwwww#.",
    "......#wwww#..",
    "....##wwww#...",
    "...#wwwwww#...",
    "..#swwwwww#...",
    "..#swwwwww#...",
    "..#swwwwww#...",
    "..#swwwwww#...",
    "..#w#www#w#...",
    "..##.###.##...",
};

// Standing square on all four legs, tail level behind.
static const char *const SP_STAND[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#......",
    "#wkwwkw#..####",
    "#wwwwww###wws#",
    ".#wwwwwwwws###",
    ".#wwwwwwwww#..",
    ".#wwwwwwwww#..",
    ".#w#ww##ww##..",
    ".#w#.#ww#.#w#.",
    ".#s#.#ss#.#s#.",
    ".##..####..##.",
};

// Walk frame 1: legs scissored.
static const char *const SP_WALK_A[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#......",
    "#wkwwkw#..####",
    "#wwwwww###wws#",
    ".#wwwwwwwws###",
    ".#wwwwwwwww#..",
    ".#wwwwwwwww#..",
    ".#ww#www#ww#..",
    ".#w#...#w##w#.",
    ".#s#...#s#.#s#",
    ".##....##..##.",
};

// Walk frame 2: legs gathered.
static const char *const SP_WALK_B[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#......",
    "#wkwwkw#..####",
    "#wwwwww###wws#",
    ".#wwwwwwwws###",
    ".#wwwwwwwww#..",
    ".#wwwwwwwww#..",
    ".#ww#www#ww#..",
    "..#w##..##w#..",
    "..#s#....#s#..",
    "..##.....##...",
};

// Loaf: lying with paws tucked, tail hooked alongside.
static const char *const SP_LOAF[] = {
    "..#...#.......",
    ".#p#.#p#......",
    ".#wp##pw#.....",
    ".#wwwwww#.....",
    ".#wkwwkw#...#.",
    ".#wwwwww###s##",
    "..#wwwwwwww#s#",
    "..#wwwwwwwws#.",
    "..#wwwwwwwww#.",
    "..#swwwwwwws#.",
    "...#########..",
};

// Sleeping: curled into a circle, head tucked.
static const char *const SP_SLEEP[] = {
    "....######....",
    "..##wwwwww##..",
    ".#wwwwwwwwws#.",
    ".#wwsswwwwws#.",
    "#ww#ww##wwws#.",
    "#w#wws#.#wws#.",
    "#w#ws##.#wws#.",
    ".##ss####wss#.",
    "..#sswwwwss#..",
    "...########...",
};

// Stretch: front low, rear high — the classic wake-up bow.
static const char *const SP_STRETCH[] = {
    "..........###.",
    ".........#wws#",
    "....####.#wws#",
    "..##wwww##ws#.",
    ".#swwwwwwww#..",
    "#swwwwwwwws#..",
    "#ww####wwws#..",
    "#w#....#w#s#..",
    "#s#....#s##s#.",
    "##......##.##.",
};

// Sitting with happy closed eyes, for petting.
static const char *const SP_SIT_HAPPY[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#......",
    "#w^ww^w#......",
    "#wwwwww#......",
    ".#wwww#.......",
    ".#wwww##......",
    ".#wwwwww#.....",
    ".#wwwwwws#....",
    ".#wwwwwwws#...",
    ".#wwwwwwws#...",
    ".#wwwwwwws#...",
    ".#wwwwwww##s#.",
    ".#w#www#w##s#.",
    ".##.###.##ss#.",
    "....########..",
};

// Arched back, bristled tail straight up, hissing: the Halloween silhouette.
// The 'W' marks by the mouth are the hiss.
static const char *const SP_ARCH[] = {
    "...........#s#",
    "......####.#s#",
    ".....#wwww##s#",
    ".#.#.#wwwwws#.",
    "#p#p#wwwwwws#.",
    "#wwwwwwwwwws#.",
    "#wkwkww#wwww#.",
    "#wwww#..#www#.",
    "W#ww#....#ww#.",
    "W.##......##..",
};

// Mid-air jump: legs tucked, ears up.
static const char *const SP_JUMP[] = {
    ".#...#........",
    "#p#.#p#.......",
    "#wp##pw#......",
    "#wwwwww#..####",
    "#wkwwkw####ws#",
    "#wwwwww##wws#.",
    ".#wwwwwwwws#..",
    ".#wwwwwwww#...",
    ".#w#ww#ww##...",
    ".#ss#.#ss#....",
    "..##...##.....",
};

#define SP_ROWS(s) (int)(sizeof(s) / sizeof((s)[0]))
