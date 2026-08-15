# PixelCat

A pixel-art cat who lives on a Waveshare ESP32-S3-Touch-AMOLED-1.8 — and a
Tamagotchi-spirited care game with no death, no sickness, no fail states.
Neglect makes him aloof, attention makes him yours, and everything is always
recoverable. He wanders an endless side-scrolling park that follows the real
sun for ZIP 94403 (dawn, day, dusk, twilight, and night with glowing green
eyes), purrs when petted, hisses when shaken, learns your daily rhythm, and
will eventually start meowing for you at the hours you taught him.

## The game

Five gauges — play, food, love, exercise, sleep — paced to a ~5 minute care
session: do everything positive once and every gauge fills; leave him be and
they drain within minutes. His sleep gauge fills while HE naps, and a
completed sleep rolls the episode — everything else resets to zero, fresh
session on wake. The gauges read directly on him: a hungry cat lingers where
food appears, an aloof cat wanders off and ignores your taps, a loved one
rumbles harder and rains hearts, a well-exercised one runs a beat slower and
naps sooner. He washes up after every meal, as one does. The menu (tap the
heart, dumbbell or Z) shows every gauge with its word, a done-today check,
and its streak of consecutive full days.

**HUD** (tappable gauge-icons in the sky): every icon fills bottom-up with
its stat — yarn ball with today's play, fish with food, a heart with
affection, a dumbbell with exercise, a purple Z with his sleep. Tapping the
fish drops a bowl, the ball starts a 60 s play session (bat-and-chase,
pounces — the only place his jump lives); heart, dumbbell or Z opens the
menu. The battery is a hairline along the top edge, red where depleted. The fish refuses a tap when he is
already fed, and yarn balls come one at a time — never while food is out.

**The ear**: the mic listens (only while awake, gated during his own sounds)
for any sharp sound above the room's ambient floor. After a wake the park is
empty — a psst, a snap, or a tap calls him trotting in from the edge.

**Scares**: shake the device and he hisses, then flees in panicked leaps and
*hides* somewhere in the world. Swipe to search for him (the only time swipes
pan the camera). Each unreconciled scare hides him farther and costs more
affection. A tap brings him out wary; only petting him to a real, earned purr
makes peace and resets the slate.

**The models**: a 96-bucket schedule EMA learns *when* you show up (weekday
and weekend separately, ~2-week forgetting), and an epsilon-greedy bandit
learns *which* opening act gets you to respond — jumping about, loud purring,
pawing the glass, pacing, or meowing. Once mature (~20 sessions), the device
wakes itself at high-confidence hours: screen on, meows, his learned act, a
5-minute audition. You respond: hit. You don't: logged as a miss, straight
back to sleep, and a precision governor (~50% target) makes him choosier,
not louder. Caps: 6 wakes a day, none below 30% battery. **There are no
quiet hours by design** — he only predicts hours you taught him, but if you
taught him 3 am, he will meow at 3 am. Do not keep this device in your
bedroom. It is a cat.

## Interactions

| You | The cat |
|---|---|
| Rub on the cat | Purrs (harder the faster you stroke — and the more he loves you), hearts float up |
| Tap the cat | A glance and a tail flick (if he feels like it) |
| Tap the fish | Bowl drops; he trots over and eats |
| Tap the yarn ball | 60 s play session: bats, chases, pounces |
| Tap the heart, dumbbell or Z | The menu: gauges, words, streaks, battery |
| Tilt 10–45% | Walks downhill |
| Tilt past 45% | Bounds in chained leaps with directional whooshes |
| Double-tap a screen edge | Leaps that way |
| Shake the device | Hiss → panicked flight → hiding (see above) |
| Make any sharp sound | Summons him when the park is empty |
| Ignore it for a minute | Sleeps (device light-sleeps on battery; both buttons wake) |

All audio is synthesized live — purr, hiss, chirp, footsteps, boing, slurp,
swipe, directional dash whooshes, and three candidate meow voices — no audio
files anywhere.

## Building

ESP-IDF v5.5:

```bash
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

Works on both board revisions (SH8601/FT3168 and the V2 CO5300/CST820 —
detected at boot). Wi-Fi credentials go in `main/wifi_secrets.h` (copy the
`.example`); without them the clock free-runs from the RTC.

## Testing without hardware

The engine and the models are pure C and run on the host:

```bash
tools/test/run.sh      # gameplay tests: drives the real engine with synthetic touches
tools/sim/run.sh       # simulated owner: weeks of routine prove the learning models
tools/preview/build.sh # renders real composited screens to PNGs
```

`MODEL_DEBUG_FIRE_S` in `main/config.h` force-fires a proactive audition
shortly after boot for end-to-end verification by serial telemetry.

## The art pipeline

Every animation frame is ASCII art, one character per pixel, in
`tools/anim_src/anim_*.txt` (see `STYLE_GUIDE.md` there). To change art:

```bash
python3 tools/ascii2png.py tools/anim_src/anim_trot.txt /tmp/trot.png 8  # preview
python3 tools/gen_anims.py tools/anim_src main/cat_anims.h               # regenerate
```

Backgrounds are baked from PNGs in `background/` by `tools/gen_world.py`
into pre-rotated looping world strips.

Behaviour lives in `main/cat_sprite.c` (animation table, gesture detection,
state machine, world objects, the free camera); care stats in
`main/stats.c`; the learning models in `main/model.c`; tunables in
`main/config.h`.
