#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// PCF85063 real-time clock. At init, if the RTC has lost time (oscillator
// stopped) or is behind this firmware's build timestamp, it is set to the
// build timestamp — builds happen moments before flashing, so the clock is
// effectively set from the host machine at flash time and then free-runs.

esp_err_t pcf_init(i2c_master_bus_handle_t bus);

// Local time as minutes since midnight (0..1439), or -1 if the RTC is absent.
int pcf_minutes_of_day(void);

// Civil date. Returns false if the RTC is absent.
bool pcf_date(int *year, int *mon, int *day);

// Set the clock to a civil local time (used by the NTP sync).
esp_err_t pcf_set_civil(int year, int mon, int day, int hour, int min, int sec);
