#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Background task: joins Wi-Fi (credentials from wifi_secrets.h), syncs time
// over NTP, writes Pacific local time into the PCF85063, then shuts Wi-Fi
// down again. Fails soft at every step — with no network the RTC free-runs.

esp_err_t wifi_time_start(void);

// True once any NTP sync has landed this boot — from then on the system
// clock is trustworthy for elapsed-time math.
bool wifi_time_synced(void);
