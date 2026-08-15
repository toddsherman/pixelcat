#pragma once

// Idle power management: after POWER_IDLE_S with no movement, touch, or
// button activity, the panel sleeps and the ESP32 light-sleeps, polling the
// PWR button (I2C expander) every 300 ms and waking instantly on BOOT
// (GPIO0). Wake resumes via a clean reboot.

#include <stdbool.h>

void power_note_activity(void);

// Call every loop tick; does not return if the device goes to sleep.
void power_idle_check(void);

// Expire the idle timer immediately (the next idle check sleeps, battery
// permitting) — used when a proactive audition times out.
void power_sleep_now(void);

// True once if this boot is a proactive wake the sleep loop fired; returns
// the enticement arm and time period chosen at fire time.
bool power_take_proactive(int *arm, int *period);
