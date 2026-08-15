# PixelCat: the game

A Tamagotchi-spirited care game — except it's a cat, so it doesn't beg, it
*changes*. No death, no sickness, no fail states: neglect makes him aloof,
attention makes him yours, and everything is always recoverable.

## Stats

Four stats, 0–100, persisted in NVS (saved every 5 minutes and on events),
simulated through offline time via the RTC on wake, capped so an absence
reads as "hungry cat", never "tragedy".

| Stat | Up | Down | Expressed as |
|---|---|---|---|
| Hunger | eating | ~6 h full→hungry while awake and active; ¼ rate asleep; exercise burns it faster | food-seeking, standing near the food spot |
| Affection | petting, play, making peace after a scare | slow decay; visible poop; ignoring him; **being scared (shake)** | distance kept from centre, purr strength, heart frequency, tap responsiveness |
| Energy | his sleep (sun schedule) | play, exercise, being kept awake | sluggish vs snappy animations, loaf frequency |
| Exercise | tilt-walking distance, jumps, play sessions | daily reset | spry gait vs extra loafing |

Petting has diminishing returns per session — spam doesn't max a cat.

## HUD (no menu)

Four always-visible icons in the sky — world objects are touchable, and so
is the HUD. No navigation state, no open/close.

- **Top-left cluster**: yarn ball (Play), fish (Feed), three hearts (Status).
  **Top-right**: battery (existing).
- Every icon shows something at a glance and does something when tapped:
  - **Fish** → a bowl drops into the world; he notices, trots over, eats
    (groom/slurp reuse). One bowl at a time; an untouched bowl despawns.
  - **Yarn ball** → 60 s play session: jumps and bounds score double
    exercise and extra hearts.
  - **Hearts** → show his *worst* current need in six half-heart steps
    (min of the four stats) — low hearts always mean something specific
    needs doing. Tap for the full-screen status page: four stat bars +
    battery + poop count, tap to exit.
  - **Battery** → the existing full-screen gauge.
- Quiet HUD: icons render dim so the park stays scenic; an icon brightens
  as an invitation when its stat genuinely wants attention (fish glows when
  hungry, ball when under-exercised).
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
  arch-and-hiss — but now he then **bolts off-screen** and hides at a spot
  in the world, and the scare **costs affection**. Sound does not summon a
  scared cat. While he hides, swiping pans the camera through the looping
  world; find him tucked at his hiding spot and tap or pet him to make
  peace (small affection recovery). The camera then eases back to following
  him and normal life resumes.

While he is off-screen, petting/tap-on-cat/leap zones are inert; the swipe
camera exists only in the hiding state.

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
