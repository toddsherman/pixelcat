#pragma once

#include <stdbool.h>
#include <stdint.h>

// The logbook: an append-only record of what actually happened, on the SD
// card, so the learning models can be retuned against real history instead
// of guesswork. Today only their EMA *state* survives; with this, months of
// real sessions can be replayed through a changed model offline.
//
// One line per event, CSV, human-readable and trivially parseable:
//   epoch,event,a,b
// Buffered in RAM and flushed sparingly — an SD write costs power, and this
// runs on a battery.

typedef enum {
    LOG_BOOT = 0,      // a: reset reason, b: seconds the last run lasted
    LOG_SESSION,       // a: bucket, b: 1 if it followed a proactive wake
    LOG_AUDITION,      // a: arm, b: 1 hit / 0 miss
    LOG_WAKE,          // a: bucket, b: arm chosen
    LOG_FULL,          // a gauge reached full. a: ST_* index
    LOG_SCARE,         // a: scare level
    LOG_RECONCILE,     // peace made
    LOG_FEED,          // a bowl finished
    LOG_PLAY,          // a play session ended. a: hits scored
    LOG_SLEEP,         // his sleep gauge completed
    LOG_DAY,           // a: day serial; the streak ledger rolled
} log_event_t;

void logbook_init(void);

// Record an event. Cheap: appends to a small RAM ring, no I/O.
void logbook_add(log_event_t ev, int a, int b);

// Write anything buffered to the card. Called on a slow timer and before
// sleep; safe to call with nothing pending or no card.
void logbook_flush(void);

// Note this boot in the crash log, with why the chip reset. The board has a
// history of vanishing without a word (wedged panels, sleep freezes) — this
// is the first record that survives a power cycle.
void logbook_note_boot(void);

// Throw away the event history — the raw material a retune would replay.
// The boot log survives; it is diagnostics, not training.
void logbook_forget(void);

// Stamp how long this run has lasted, so the next boot can report it even
// if this one ends without warning.
void logbook_mark_uptime(void);
