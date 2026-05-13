#include "timesync.h"
#include "cellular_manager.h"
#include <ArduinoJson.h>

static bool time_initialized = false;

bool timesync_is_initialized() {
    return time_initialized;
}

// ── Cellular network time (AT+CCLK? / NITZ) ─────────────────────────────────
static bool sync_cellular_network_time() {
    for (int retries = 5; retries > 0; retries--) {
        int yr, mo, dy, hr, mn, sc; float tz;
        if (modem.getNetworkTime(&yr, &mo, &dy, &hr, &mn, &sc, &tz)) {
            struct tm tm = {};
            tm.tm_year = yr - 1900;
            tm.tm_mon  = mo - 1;
            tm.tm_mday = dy;
            tm.tm_hour = hr;
            tm.tm_min  = mn;
            tm.tm_sec  = sc;
            tm.tm_isdst = -1;
            time_t ts = mktime(&tm);

            struct timeval now = { .tv_sec = ts };
            settimeofday(&now, NULL);
            time_initialized = true;
            Serial.printf("Time set from cellular: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                          yr, mo, dy, hr, mn, sc);
            return true;
        }
        Serial.printf("Cell time failed, retries left: %d\n", retries - 1);
        delay(2000);
    }
    return false;
}

// ── Extract unix timestamp from HTTPS API response ──────────────────────────
static time_t extract_timestamp(const String& response, int apiIndex) {
    JsonDocument doc;
    switch (apiIndex) {
    case 1: // timeapi.io
        if (deserializeJson(doc, response) == DeserializationError::Ok)
            if (doc["unix_timestamp"].is<time_t>())
                return doc["unix_timestamp"].as<time_t>();
        break;
    case 2: case 3: // aisenseapi.com / gettimeapi.dev
        if (deserializeJson(doc, response) == DeserializationError::Ok)
            if (doc["timestamp"].is<time_t>())
                return doc["timestamp"].as<time_t>();
        break;
    case 4: { // 1.1.1.1/cdn-cgi/trace
        int tsIdx = response.indexOf("ts=");
        if (tsIdx >= 0) {
            int start = tsIdx + 3;
            int end = response.indexOf('\n', start);
            if (end < 0) end = response.length();
            String tsStr = response.substring(start, end);
            int dot = tsStr.indexOf('.');
            if (dot > 0) tsStr = tsStr.substring(0, dot);
            return (time_t)tsStr.toInt();
        }
        break;
    }
    }
    return 0;
}

// ── HTTPS GET via modem ─────────────────────────────────────────────────────
static time_t fetch_time_api(const char* url, int apiIndex) {
    Serial.printf("Trying %s\n", url);

    if (!modem.https_begin()) { Serial.println("  https_begin failed"); return 0; }
    if (!modem.https_set_url(url)) { Serial.println("  set_url failed"); modem.https_end(); return 0; }

    int httpCode = modem.https_get();
    if (httpCode != 200) {
        Serial.printf("  HTTP %d\n", httpCode);
        modem.https_end();
        return 0;
    }
    String response = modem.https_body();
    modem.https_end();
    Serial.printf("  %u bytes\n", (unsigned)response.length());

    time_t ts = extract_timestamp(response, apiIndex);
    if (ts > UNIX_TIME_NOV_13_2017) {
        Serial.printf("  Timestamp: %lu\n", (unsigned long)ts);
        return ts;
    }
    Serial.println("  Failed to extract timestamp");
    return 0;
}

// ── Apply timestamp ─────────────────────────────────────────────────────────
static bool apply_timestamp(time_t ts) {
    struct timeval now = { .tv_sec = ts };
    settimeofday(&now, NULL);
    struct tm* t = gmtime(&ts);
    Serial.printf("Time set via HTTPS: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                  t->tm_hour, t->tm_min, t->tm_sec);
    time_initialized = true;
    return true;
}

// ── Entry point: cellular time first, then HTTPS APIs ───────────────────────
bool timesync_init() {
    Serial.println("Time sync: trying cellular network time...");

    if (sync_cellular_network_time()) {
        Serial.println("Time synced via cellular network.");
        return true;
    }

    Serial.println("Cellular time failed. Trying HTTPS APIs via modem...");

    struct { const char* url; int index; } apis[] = {
        {TIME_API_1, 1},
        {TIME_API_2, 2},
        {TIME_API_3, 3},
        {TIME_API_4, 4},
    };

    for (int i = 0; i < 4; i++) {
        for (int retry = 0; retry < TIME_API_RETRY_COUNT; retry++) {
            if (retry > 0) delay(TIME_API_RETRY_DELAY);
            time_t ts = fetch_time_api(apis[i].url, apis[i].index);
            if (ts > UNIX_TIME_NOV_13_2017) return apply_timestamp(ts);
        }
    }

    Serial.println("All time sources failed.");
    time_initialized = false;
    return false;
}

bool timesync_ensure_synced() {
    if (time_initialized && time(NULL) > UNIX_TIME_NOV_13_2017) return true;
    Serial.println("Time invalid, re-syncing...");
    return timesync_init();
}

void timesync_check_resync() {
    if (time_initialized) return;
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt < 40000) return;
    lastAttempt = millis();
    Serial.println("Retrying time sync...");
    timesync_init();
}
