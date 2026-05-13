#pragma once
#include <Arduino.h>
#include <time.h>

// HTTPS time API URLs in priority order
#define TIME_API_1 "https://timeapi.io/api/v1/time/current/unix"
#define TIME_API_2 "https://aisenseapi.com/services/v1/timestamp"
#define TIME_API_3 "https://gettimeapi.dev/v1/time?timezone=UTC"
#define TIME_API_4 "https://1.1.1.1/cdn-cgi/trace"

#define TIME_API_RETRY_COUNT       10
#define TIME_API_RETRY_DELAY       2000

// Reference epoch — sanity-checks the system clock
#define UNIX_TIME_NOV_13_2017 1510592825

// NTP servers
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define NTP_SERVER_3 "asia.pool.ntp.org"
#define NTP_SERVER_4 "hk.pool.ntp.org"

bool timesync_init();
bool timesync_is_initialized();
bool timesync_ensure_synced();
void timesync_check_resync();
