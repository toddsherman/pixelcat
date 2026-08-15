# PixelCat: the game

A Tamagotchi-spirited care game — except it's a cat, so it doesn't beg, it
*changes*. No death, no sickness, no fail states: neglect makes him aloof,
attention makes him yours, and everything is always recoverable.

## Stats

Five gauges, 0–100, persisted in NVS, **paced to a ~5 minute care
session**: do everything positive once — a feed, a play, a petting, a walk,
a nap — and every gauge fills; leave him be and they all drain within a few
minutes. Every bar aspires to be full.

| Gauge | Up | Down |
|---|---|---|
| Play | yarn-ball sessions (bats and pounces) | drains in ~5 min; faster while he sleeps |
| Food | eating (a bowl is a full meal) | drains in ~6 min; play works up an appetite |
| Love | petting (one gauge row per 5 s of purring), play, making peace | drains in ~5 min; **being scared (shake)** |
| Exercise | **walking and dashing only** — never scares, never pounces | drains in ~5 min; faster while he sleeps |
| Sleep | fills while HE sleeps (~75 s nap fills it) | drains in ~6 min awake |

**His sleep ends the episode**: when the sleep gauge completes, everything
else resets to zero — a fresh session on wake. Behaviour reads the gauges:
a filling exercise bar is a pleasantly worn-out cat (slower pace, longer
loafs, earlier naps); a hungry cat lingers where food appears; an aloof cat
wanders and ignores taps; a loved one rumbles harder and rains hearts.

## HUD (no menu)

Four always-visible icons in the sky — world objects are touchable, and so
is the HUD. No navigation state, no open/close.

- **One row**: yarn ball (Play), fish (Food), heart (Love), dumbbell
  (Exercise), purple Z (Sleep) — all 6 cells tall, each a single colour
  with white as the only accent, no outlines. **Battery is a one-pixel
  hairline along the top edge**, inset clear of the rounded corners:
  charged length in green (blue on USB), the depleted remainder in red.
- **Every icon is a vertical gauge**: grey art that fills bottom-up with
  its colour — the fill IS the reading.
  - **Fish** → tap drops a bowl; he trots over, eats, and washes his paw
    after dinner (the only time that animation appears). One bowl at a
    time. A full fish refuses the tap.
  - **Yarn ball** → tap starts a 60 s play session (paw-batting and
    pounces — the only place those animations live). Yarn balls can keep
    coming, but **only one at a time and never while food is out**.
  - **Heart, dumbbell, Z** → any of them opens the **menu screen**,
    rendered in finer pixels and inset clear of the panel's rounded
    corners: each gauge beside its blocky word (PLAY / FOOD / LOVE /
    EXERCISE / SLEEP), a checkmark or cross for "reached full today", and
    the streak — which counts today the moment the check lands (0X becomes
    1X; tomorrow makes it 2X). The battery sits in the list like any other
    icon with its percentage. Tap to exit.
- Icon hit-boxes take priority over the side-zone leap taps (same precedence
  the battery tap uses today).
- **Buttons** do exactly one thing: wake the device. PWR's 6-second hardware
  power-off is untouchable. Shortcuts can be added later if a need emerges.

## The microphone, absence, and hiding

The mic (ES8311, unused until now) listens **only while the device is awake**,
with detection gated during his own speaker sounds. Detection is deliberately
simple: any sharp sound above an adaptive ambient noise floor. No tuning, no
keywords — a psst, a snap, a knock all work.

Two distinct off-screen states:

- **Absent** (after a wake): the park is empty — he's elsewhere. Any sound
  or a screen tap summons him: he trots in from an edge with footsteps,
  settling into centre. Left alone, he wanders in on his own after a while.
  No grudge.
- **Hiding** (after being scared): shaking the device still triggers the
  arch-and-hiss — and the moment the hiss animation completes he **flees in
  panicked leaps**: the leap cycle chained at elevated speed, bounding off
  the screen edge in under a second — never a walk. He hides at a spot in
  the world, and the scare **costs affection**. Sound does not summon a
  scared cat. While he hides, swiping pans the camera through the looping
  world to search for him.
- **Scares escalate.** Each scare without a reconciliation between pushes
  his hiding spot farther away — up to the loop's maximum (half the world
  from where you're looking) — and costs more affection. Scaring him while
  he is already hiding still earns you one hiss where he stands, then he
  bolts farther.
- **The search is a chase.** While he hides, whenever the panning camera
  has him in view he keeps walking away from you — tap him mid-stride to
  bring him out. Soft footfalls tell you the search is getting warm. Once
  tapped, the camera snaps back and centred behaviour resumes within a
  moment of him being on screen.
- **Reconciliation is purr-gated.** Finding him and tapping brings him out
  wary — the fear level persists. To truly make peace, pet him at his
  hiding spot (or after he emerges) until he actually purrs: that resets
  the escalation to baseline, recovers a little affection, and the camera
  eases back to following him. A purr cannot be faked; it has to be earned.

While he is off-screen, petting, tap-on-cat, leap zones, and **tilt** are
all inert — tilt moves *him*, and he isn't there. The swipe camera exists
**only** while he hides; in normal centred behaviour sideways swipes never
pan the camera — a stroke across him is petting, nothing else. The two
control schemes are mutually exclusive by state, so no gesture is ever
ambiguous. Everything reactivates when he returns to centred behaviour.

## Learning: he figures out your schedule

Two tiny on-device models, all local, all in NVS, learning from day one:

1. **Schedule predictor** — 96 half-hour buckets (weekday/weekend), EMA
   updated by every interaction session and button wake. Exponential
   forgetting adapts to a changed routine in ~2 weeks.
2. **Enticement bandit** — when a proactive wake fires, he performs his
   opening act: jumping about, loud purring, pawing the glass, pacing, or
   meowing. Acts that get you to interact within the window are reinforced;
   he learns what works on *you*, per time of day.

### Proactive wake

While light-sleeping on battery, the RTC is checked against the schedule
model. Entering a high-confidence bucket wakes the device fully: screen on,
cat awake, **meowing for a few seconds** (procedural ~0.7 s meow, repeated
2–3×), then his learned enticement behaviour.

- **Audition window**: 5 minutes. Interaction (touch, tilt, button) = **hit**;
  timeout = **miss**, logged, then straight back to sleep.
- Misses decay that bucket's confidence. A global precision target (~50%)
  auto-raises the wake threshold when he over-predicts: he gets choosier,
  not louder.
- Dormant until ~2 weeks / ~20 sessions of data. Caps: 6 proactive wakes per
  day, none below 30% battery. On USB power the device never sleeps, so the
  enticement behaviours simply run at predicted times.
- **There are no quiet hours by design.** He only predicts hours you taught
  him — but if you taught him 3 am, he will meow at 3 am. Do not keep this
  device in your bedroom. It is a cat.

## Build phases

1. Stats engine + NVS persistence + offline catch-up, running silently.
2. HUD (icons, hearts, status page), feeding, poop.
3. Microphone bring-up, absent/summon entrance, scare→hide→search→forgive
   loop, swipe camera.
4. Behaviour consequences (aloofness, purr scaling, food-seeking).
5. Meow synth + schedule/bandit models + proactive wake, activating as the
   models mature (a proactive wake may open with him meowing from
   off-screen — answer with any sound to call him in).

Each phase is flashed and lived with before the next.
