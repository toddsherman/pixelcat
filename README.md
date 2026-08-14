# PixelCat

A pixel-art virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.8. A small
white cat lives on the screen: it swishes its tail, grooms, wanders, loafs and
sleeps on its own — and reacts to you.

## Interactions

| You | The cat |
|---|---|
| Rub on the cat | Purrs from the speaker (harder the faster you stroke), hearts float up |
| Tap the cat | Big vertical jump |
| Hold the left/right edge of the screen | Trots off in that direction until you let go |
| Double-tap a screen edge | Leaps that way (keep tapping to chain leaps) |
| Shake the device | Arches up, bristles and audibly hisses |
| Ignore it for a minute | Curls up and sleeps; wakes with a chirp |

The purr is synthesized live (~24 Hz amplitude-modulated rumble with a breath
cycle), the hiss is a shaped noise burst — no audio files anywhere.

## Building

ESP-IDF v5.5:

```bash
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

Works on both board revisions (SH8601/FT3168 and the V2 CO5300/CST820 —
detected at boot).

## The art pipeline

Every animation frame is ASCII art, one character per pixel, in
`tools/anim_src/anim_*.txt` (see `STYLE_GUIDE.md` there). To change art:

```bash
python3 tools/ascii2png.py tools/anim_src/anim_trot.txt /tmp/trot.png 8  # preview
python3 tools/gen_anims.py tools/anim_src main/cat_anims.h               # regenerate
```

`tools/preview/build.sh` renders full screens through the real compositing
code on the host, no flashing needed.

Behaviour lives in `main/cat_sprite.c` (animation table, gesture detection,
state machine); tunables in `main/config.h`. `CAT_SPRITE_STYLE 0` switches to
an earlier, larger single-pose cat kept in `main/cat.c`.
