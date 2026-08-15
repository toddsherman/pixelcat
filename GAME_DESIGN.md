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
| Affection | petting, play | slow decay; visible poop; ignoring him | distance kept from centre, purr strength, heart frequency, tap responsiveness |
| Energy | his sleep (sun schedule) | play, exercise, being kept awake | sluggish vs snappy animations, loaf frequency |
| Exercise | tilt-walking distance, jumps, play sessions | daily reset | spry gait vs extra loafing |

Petting has diminishing returns per session — spam doesn't max a cat.

## Menu (two buttons)

- **Left button**: open menu / cycle highlight (wraps). **Right button**: select.
- Items as a pixel-icon strip along the bottom: **Feed · Play · Status · Close**.
- Auto-closes after 8 s without a press. A press that wakes the device is
  consumed — it never doubles as a menu press.
- **Feed**: food appears in the world; he trots to it and eats (groom/slurp
  reuse). **Play**: 60 s where jumps and bounds score double exercise and
  extra hearts. **Status**: full-screen stat bars + battery, tap to exit.
- PWR's 6-second hardware power-off is untouchable; menu uses short presses.
- Physical left/right mapping confirmed empirically at first flash (one-line
  swap).

## Poop

Some hours after eating, one appears somewhere in the walked world and simply
sits there. Tap to clean (poof + swipe sound). Each visible poop costs a
little affection per hour — he has standards. New art: one small sprite
(ASCII, in-repo).

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
2. Menu, feeding, poop, status screen.
3. Behaviour consequences (aloofness, purr scaling, food-seeking).
4. Meow synth + schedule/bandit models + proactive wake, activating as the
   models mature.

Each phase is flashed and lived with before the next.
