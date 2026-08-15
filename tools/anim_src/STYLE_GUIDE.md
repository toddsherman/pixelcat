# PixelCat animation frame style guide

You are drawing ONE animation cycle for a minimal pixel-art white cat,
16 px wide, side-view **facing LEFT** (the engine mirrors for right).

The cat is derived from Elthen's 2D Pixel Art Cat Sprites, modified — see
CREDITS.md. Match his proportions and silhouette; new cycles are extensions
of that character, not a fresh design.

## Charset (one char = one pixel)
- `.` transparent (checkered room shows through)
- `#` dark outline — EVERY silhouette edge pixel must be `#`
- `w` white fur (body fill)
- `s` grey shade — use sparingly: rump, far limbs, tail tip, belly shadow
- `p` pink — inner ears only
- `k` dark pixel — eyes, nose
- `^` happy-closed eye (renders like k)
- `W` pure white — effects only (hiss marks); not fur

## Hard rules
- Every row is EXACTLY 16 characters. The render script rejects anything else.
- Max 18 rows per frame.
- Feet rest on the bottom row of the frame (the common baseline). For airborne
  frames, keep the sprite drawn to its own bottom row and add `lift=N`
  (pixels above the floor) at the top of that frame block.
- Frames are separated by a line `---`.
- Consistency beats detail: between consecutive frames, move as few pixels as
  possible. Re-use the head block verbatim in every frame unless the action
  moves the head.

## Canonical head block (sitting/upright poses, facing left)
```
.#...#..
#p#.#p#.
#wp##pw#
#wwwwww#
#wkwwkw#
#wwwwww#
```
(For profile/walking poses the same head sits on a horizontal body; see the
canonical stand below.)

## Canonical standing body (profile, facing left)
```
.#...#..........
#p#.#p#.........
#wp##pw#........
#wwwwww#....####
#wkwwkw#..##wws#
#wwwwww###wws###
.#wwwwwwwww#....
.#wwwwwwwww#....
.#w#ww##ww##....
.#w#.#ww#.#w#...
.#s#.#ss#.#s#...
.##..####..##...
```

## Workflow (you MUST do this)
1. Write your frames to your assigned `.txt` file.
2. Render: `python3 /Users/toddsherman/Projects/waveshare/pixelcat/tools/ascii2png.py <yourfile>.txt <yourfile>.png 8`
3. Read the PNG and LOOK at it. Judge: does the action read at a glance? Do
   frames flow (flip through them mentally in order)? Any stray pixels,
   broken outlines, floating limbs?
4. Fix and re-render. Do at least two look-fix rounds before finishing.
