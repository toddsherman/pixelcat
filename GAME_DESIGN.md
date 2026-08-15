# PixelCat: the game

A Tamagotchi-spirited care game — except it's a cat, so it doesn't beg, it
*changes*. No death, no sickness, no fail states: neglect makes him aloof,
attention makes him yours, and everything is always recoverable.

## Stats

Three stats, 0–100, persisted in NVS (saved every 5 minutes and on events),
simulated through offline time via the RTC on wake, capped so an absence
reads as "hungry cat", never "tragedy". **Every bar aspires to be full**:
fed, loved, exercised — one direction, no mixed readings.

| Stat | Up | Down | Expressed as |
|---|---|---|---|
| Food | eating | ~6 h fed→hungry while awake and active; ¼ rate asleep; exercise burns it faster | food-seeking, standing near the food spot |
| Affection | petting, play, making peace after a scare | slow decay; visible poop; ignoring him; **being scared (shake)** | distance kept from centre, purr strength, heart frequency, tap responsiveness |
| Exercise | tilt-walking distance, jumps, play sessions | daily reset | a filling bar IS his tiredness: pleasantly worn out, slower pace, longer loafs, earlier naps; the morning reset is a fresh, spry cat |

There is no separate energy stat — a full exercise bar implies a cat who
wants his sleep, and the daily reset hands him a new morning. Petting has
diminishing returns per session — spam doesn't max a cat.

## HUD (no menu)

Four always-visible icons in the sky — world objects are touchable, and so
is the HUD. No navigation state, no open/close.

- **Top-left cluster**: yarn ball (Play), fish (Food), one heart
  (Affection), dumbbell (Exercise). **Top-right**: battery.
- **Every icon is a vertical gauge**: its art exists in quiet grey and
  fills bottom-up with its true colours as the value rises — the fill IS
  the reading, no separate bars needed at a glance.
  - **Fish** (fills with food) → tap drops a bowl into the world; he
    notices, trots over, eats (groom/slurp reuse), and washes up after
    dinner like a real cat. One bowl at a time; an untouched bowl
    despawns. **A full fish refuses the tap** — no dinner for a fed cat.
  - **Yarn ball** (fills with today's play) → tap starts a 60 s play
    session: paw-batting and pouncing jumps, the **only** place those
    animations appear. Play scores double exercise and extra affection.
    **A full ball refuses the tap** — he has had his games today; the
    gauge resets with the new day.
  - **Heart** (fills with affection) → tap for the full-screen status
    page: three stat bars (F / A / X) + battery + poop count, tap to exit.
  - **Dumbbell** (fills with exercise) → pure gauge; full means pleasantly
    worn out and ready for his nap.
  - **Battery** → the existing full-screen gauge on tap.
- Icon hit-boxes take priority over the side-zone leap taps (same precedence
  the battery tap uses today).
- **Buttons** do exactly one thing: wake the device. PWR's 6-second hardware
  power-off is untouchable. Shortcuts can be added later if a need emerges.

## Poop

Some hours after eating, one appears somewhere in the walked world and simply
sits there. Tap to clean (poof + swipe sound). Each visible poop costs a
little affection per hour — he has standards. New art: one small sprite
(ASCII, in-repo).

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
  he is already hiding re-hides him farther immediately.
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
