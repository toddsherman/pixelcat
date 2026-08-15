#pragma once

#include <stdbool.h>

// Getting the park onto the card without ever taking the card out.
//
// The world files are far too big to carry in flash and too tedious to
// shuttle by hand, so the device fetches its own: while Wi-Fi is already up
// for the clock, anything missing or the wrong size is pulled from a plain
// HTTP server on the workshop machine and written straight to the SD card.
// Drop new art there, delete the stale file (or bump it), and the cat
// re-fetches it on the next boot.
//
// Returns true if anything was written — the caller reboots so the loader
// picks up the new park.

bool provision_run(void);
