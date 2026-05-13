#include "azure_iot_manager.h"
#include "cellular_manager.h"
#include "web_server_manager.h"
#include "SD_MMC.h"
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <ArduinoJson.h>

// ─── Upload state (GUI-driven) ──────────────────────────────────────────────
volatile UploadState uploadState     = UPLOAD_IDLE;
char                 uploadFilename[32] = {};
uint8_t              uploadClientNum    = 0;

volatile bool telemetryRequested  = false;
uint8_t      telemetryClientNum   = 0;

// ─── Device twin state ──────────────────────────────────────────────────────
bool twinReceived      = false;
char twinResolution[8] = "HD";
long twinPeriod        = 5;
char twinSdWrite[4]    = "on";

static char g_twin_response[4096] = {};
static volatile bool g_twin_response_ready = false;
static bool g_desired_updated = false;
static int  twinRid = 0;

// ─── MQTT state ─────────────────────────────────────────────────────────────
static char     mqttPassword[400]  = {};
static uint32_t sasExpiry          = 0;
static bool     mqttConnected      = false;

// ─── Upload cooldown ────────────────────────────────────────────────────────
static unsigned long g_uploadCooldownUntilMs = 0;
static uint32_t      g_uploadCooldownSecs    = 300;
static const uint32_t COOLDOWN_MIN_SECS      = 300;
static const uint32_t COOLDOWN_MAX_SECS      = 1800;

static bool uploadInCooldown() {
    if (g_uploadCooldownUntilMs == 0) return false;
    return (long)(g_uploadCooldownUntilMs - millis()) > 0;
}

static void bumpUploadCooldown() {
    g_uploadCooldownUntilMs = millis() + (unsigned long)g_uploadCooldownSecs * 1000UL;
    Serial.printf("[UPLOAD] Cooldown %lus before next attempt\n",
                  (unsigned long)g_uploadCooldownSecs);
    uint32_t next = g_uploadCooldownSecs * 2;
    if (next > COOLDOWN_MAX_SECS) next = COOLDOWN_MAX_SECS;
    g_uploadCooldownSecs = next;
}

static void resetUploadCooldown() {
    g_uploadCooldownSecs    = COOLDOWN_MIN_SECS;
    g_uploadCooldownUntilMs = 0;
}

#define MQTT_USERNAME IOT_HUB_HOSTNAME "/" DEVICE_ID "/?api-version=2021-04-12"
#define D2C_TOPIC     "devices/" DEVICE_ID "/messages/events/"

// ─── SAS token renew ────────────────────────────────────────────────────────
static bool sasNeedsRenew() {
    if (sasExpiry == 0) return true;
    uint32_t now = (uint32_t)time(NULL);
    if (now < 1577836800UL) return true;
    return (now > sasExpiry - (SAS_TOKEN_VALID_S / 5));
}

// ─── SAS token (mbedTLS HMAC-SHA256) ────────────────────────────────────────
bool generateSASToken(char* out, size_t outSize, uint32_t durationSecs) {
    char resourceUri[128];
    snprintf(resourceUri, sizeof(resourceUri), "%s/devices/%s", IOT_HUB_HOSTNAME, DEVICE_ID);
    char encodedUri[256] = {};
    for (const char* p = resourceUri; *p; p++) {
        if      (*p == '/') { strcat(encodedUri, "%2F"); }
        else if (*p == ':') { strcat(encodedUri, "%3A"); }
        else                { char t[2] = {*p, '\0'}; strcat(encodedUri, t); }
    }
    uint32_t expiry = (uint32_t)time(NULL) + durationSecs;
    char sts[300];
    snprintf(sts, sizeof(sts), "%s\n%lu", encodedUri, (unsigned long)expiry);
    uint8_t key[32]; size_t keyLen;
    if (mbedtls_base64_decode(key, sizeof(key), &keyLen,
                              (const uint8_t*)DEVICE_KEY, strlen(DEVICE_KEY)) != 0) return false;
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, keyLen);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)sts, strlen(sts));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    char b64[64]; size_t b64len;
    if (mbedtls_base64_encode((uint8_t*)b64, sizeof(b64), &b64len, hmac, 32) != 0) return false;
    b64[b64len] = '\0';
    char encSig[128] = {};
    for (size_t i = 0; i < b64len; i++) {
        if      (b64[i] == '+') { strcat(encSig, "%2B"); }
        else if (b64[i] == '/') { strcat(encSig, "%2F"); }
        else if (b64[i] == '=') { strcat(encSig, "%3D"); }
        else { char t[2] = {b64[i], '\0'}; strcat(encSig, t); }
    }
    snprintf(out, outSize, "SharedAccessSignature sr=%s&sig=%s&se=%lu",
             encodedUri, encSig, (unsigned long)expiry);
    return true;
}

// ═══ Raw AT helpers ═══════════════════════════════════════════════════════════

// Forward declaration
static void mqttRxFeed(char c);

static int mqttAT(const char* cmd, const char* urc,
                  uint32_t timeoutMs = 3000, String* matchLine = nullptr) {
    while (modemSerial.available()) {
        mqttRxFeed(modemSerial.read());
    }
    if (cmd && cmd[0]) {
        modemSerial.print("AT"); modemSerial.print(cmd); modemSerial.print("\r\n");
    }
    unsigned long t0 = millis();
    String acc = "";
    while (millis() - t0 < timeoutMs) {
        while (modemSerial.available()) {
            char c = modemSerial.read();
            mqttRxFeed(c);
            acc += c;
            if (c == '\n') {
                acc.trim();
                if (urc && acc.indexOf(urc) >= 0) {
                    if (matchLine) *matchLine = acc; return 1;
                }
                if (acc.startsWith("ERROR") || acc.startsWith("+CME ERROR") ||
                    acc.startsWith("+CMS ERROR")) return -1;
                if (acc == "OK") { if (!urc) return 2; }
                acc = "";
            }
        }
        delay(1);
    }
    return 0;
}

static bool mqttATPrompt(const char* cmd, const char* data, size_t dataLen,
                         uint32_t timeoutMs = 8000) {
    while (modemSerial.available()) {
        mqttRxFeed(modemSerial.read());
    }
    modemSerial.print("AT"); modemSerial.print(cmd); modemSerial.print("\r\n");
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (modemSerial.available()) {
            char c = modemSerial.read();
            if (c == '>') {
                delay(10);
                modemSerial.write((const uint8_t*)data, dataLen);
                t0 = millis(); String acc = "";
                while (millis() - t0 < timeoutMs) {
                    while (modemSerial.available()) {
                        char r = modemSerial.read();
                        mqttRxFeed(r);
                        acc += r;
                        if (r == '\n') { acc.trim();
                            if (acc == "OK") return true;
                            if (acc.startsWith("ERROR")) return false;
                            acc = "";
                        }
                    }
                    delay(1);
                }
                return false;
            }
            mqttRxFeed(c);
        }
        delay(1);
    }
    return false;
}

// ═══ MQTT via raw AT+CMQTT* ═══════════════════════════════════════════════════

bool azure_mqtt_init() {
    return azure_mqtt_connect();
}

bool azure_mqtt_connect() {
    if (mqttConnected && !sasNeedsRenew()) return true;

    if (mqttConnected) {
        mqttAT("+CMQTTDISC=0,120", "+CMQTTDISC: 0,", 5000);
        mqttConnected = false;
    }

    if (!generateSASToken(mqttPassword, sizeof(mqttPassword), SAS_TOKEN_VALID_S)) {
        Serial.println("[MQTT] SAS token failed"); return false;
    }
    sasExpiry = (uint32_t)time(NULL) + SAS_TOKEN_VALID_S;

    // Tear down any leftover MQTT state
    mqttAT("+CMQTTDISC=0,120", nullptr, 3000);
    mqttAT("+CMQTTDISC=1,120", nullptr, 3000);
    mqttAT("+CMQTTREL=0",      nullptr, 2000);
    mqttAT("+CMQTTREL=1",      nullptr, 2000);
    mqttAT("+CMQTTSTOP", "+CMQTTSTOP: 0", 5000);
    delay(50);

    // Start MQTT service
    int r = mqttAT("+CMQTTSTART", "+CMQTTSTART: 0", 15000);
    if (r != 1) { Serial.println("[MQTT] CMQTTSTART failed"); return false; }

    // Re-assert ATE0: the modem has been observed to revert to ATE1 after
    // CMQTT lifecycle events, which corrupts later binary HTTPDATA responses.
    mqttAT("E0", nullptr, 1000);

    // SSL config for Azure IoT Hub
    mqttAT("+CSSLCFG=\"sslversion\",0,3", nullptr, 3000);
    mqttAT("+CSSLCFG=\"authmode\",0,0",   nullptr, 3000);
    mqttAT("+CSSLCFG=\"enableSNI\",0,1",  nullptr, 3000);

    // Acquire client — server_type=1 is required for Azure IoT Hub
    mqttAT("+CMQTTREL=0", nullptr, 2000);
    r = mqttAT("+CMQTTACCQ=0,\"" DEVICE_ID "\",1", nullptr, 5000);
    if (r <= 0) { Serial.println("[MQTT] ACCQ failed"); return false; }

    mqttAT("+CMQTTSSLCFG=0,0",        nullptr, 3000);
    mqttAT("+CMQTTCFG=\"version\",0,4", nullptr, 3000);

    // Connect to Azure IoT Hub
    char connCmd[700];
    snprintf(connCmd, sizeof(connCmd),
             "+CMQTTCONNECT=0,\"tcp://" IOT_HUB_HOSTNAME ":%d\",60,0"
             ",\"" MQTT_USERNAME "\",\"%s\"",
             MQTT_PORT, mqttPassword);

    String urcLine;
    r = mqttAT(connCmd, "+CMQTTCONNECT:", 60000, &urcLine);
    if (r != 1) { Serial.println("[MQTT] CONNECT timeout"); return false; }

    int comma = urcLine.indexOf(',');
    int connResult = (comma >= 0) ? urcLine.substring(comma + 1).toInt() : -1;
    if (connResult != 0) {
        Serial.printf("[MQTT] CONNECT code %d\n", connResult); return false;
    }

    // Subscribe to twin topics
    {
        const char* t0 = "$iothub/twin/res/#";
        char sc[48];
        snprintf(sc, sizeof(sc), "+CMQTTSUB=0,%u,1", (unsigned)strlen(t0));
        if (!mqttATPrompt(sc, t0, strlen(t0))) {
            Serial.println("[MQTT] Subscribe twin/res FAILED");
        }
    }
    {
        const char* t1 = "$iothub/twin/PATCH/properties/desired/#";
        char sc[48];
        snprintf(sc, sizeof(sc), "+CMQTTSUB=0,%u,1", (unsigned)strlen(t1));
        if (!mqttATPrompt(sc, t1, strlen(t1))) {
            Serial.println("[MQTT] Subscribe twin/PATCH FAILED");
        }
    }

    mqttConnected = true;
    Serial.println("[MQTT] Connected to Azure IoT Hub (cellular)");
    return true;
}

bool azure_publish_telemetry(const char* payload) {
    if (!mqttConnected) { if (!azure_mqtt_connect()) return false; }

    size_t topicLen   = strlen(D2C_TOPIC);
    size_t payloadLen = strlen(payload);
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "+CMQTTTOPIC=0,%u", (unsigned)topicLen);
    if (!mqttATPrompt(cmd, D2C_TOPIC, topicLen)) {
        mqttConnected = false;
        if (!azure_mqtt_connect()) return false;
        if (!mqttATPrompt(cmd, D2C_TOPIC, topicLen)) return false;
    }
    snprintf(cmd, sizeof(cmd), "+CMQTTPAYLOAD=0,%u", (unsigned)payloadLen);
    if (!mqttATPrompt(cmd, payload, payloadLen)) return false;

    String urcLine;
    int r = mqttAT("+CMQTTPUB=0,1,60,0", "+CMQTTPUB:", 70000, &urcLine);
    if (r != 1) return false;
    int c1 = urcLine.indexOf(',');
    return (c1 >= 0) && (urcLine.substring(c1 + 1).toInt() == 0);
}

// Shared MQTT-rx state machine — processes +CMQTTRXSTART/TOPIC/PAYLOAD/END URCs
static void mqttRxFeed(char c) {
    static enum { S_IDLE, S_WAIT_TOPIC, S_WAIT_PAYLOAD } state = S_IDLE;
    static String rxTopic, rxPayload;
    static int rxTopicRem = 0, rxPayloadRem = 0;
    static String lineBuf;

    if (c == '\n') {
        lineBuf.trim();
        if (lineBuf.length() == 0) { lineBuf = ""; return; }

        if (lineBuf.startsWith("+CMQTTRXSTART:")) {
            int c1 = lineBuf.indexOf(',');
            int c2 = lineBuf.indexOf(',', c1 + 1);
            if (c1 >= 0 && c2 > c1) {
                rxTopicRem   = lineBuf.substring(c1 + 1, c2).toInt();
                rxPayloadRem = lineBuf.substring(c2 + 1).toInt();
                rxTopic   = "";
                rxPayload = "";
                state = S_WAIT_TOPIC;
            }
        } else if (state == S_WAIT_TOPIC && lineBuf.startsWith("+CMQTTRXTOPIC:")) {
            int c1 = lineBuf.indexOf(',');
            if (c1 >= 0) {
                int subLen = lineBuf.substring(c1 + 1).toInt();
                if (subLen > 0 && subLen <= rxTopicRem) {
                    char buf[subLen + 1];
                    unsigned long t0 = millis();
                    while (modemSerial.available() < subLen && millis() - t0 < 2000) delay(1);
                    size_t n = modemSerial.readBytes(buf, subLen);
                    buf[n] = '\0';
                    rxTopic.concat(buf, n);
                    rxTopicRem -= n;
                }
                if (rxTopicRem <= 0) state = S_WAIT_PAYLOAD;
            }
        } else if (state == S_WAIT_PAYLOAD && lineBuf.startsWith("+CMQTTRXPAYLOAD:")) {
            int c1 = lineBuf.indexOf(',');
            if (c1 >= 0) {
                int subLen = lineBuf.substring(c1 + 1).toInt();
                if (subLen > 0 && subLen <= rxPayloadRem) {
                    char buf[subLen + 1];
                    unsigned long t0 = millis();
                    while (modemSerial.available() < subLen && millis() - t0 < 2000) delay(1);
                    size_t n = modemSerial.readBytes(buf, subLen);
                    buf[n] = '\0';
                    rxPayload.concat(buf, n);
                    rxPayloadRem -= n;
                }
            }
        } else if (lineBuf.startsWith("+CMQTTRXEND:")) {

            if (rxTopic.indexOf("twin/res/") >= 0) {
                rxPayload.toCharArray(g_twin_response, sizeof(g_twin_response));
                g_twin_response_ready = true;
                twinReceived = true;
                Serial.println("[TWIN] Dispatched as GET response");
            } else if (rxTopic.indexOf("twin/PATCH/properties/desired") >= 0) {
                JsonDocument doc;
                if (deserializeJson(doc, rxPayload) == DeserializationError::Ok) {
                    const char* res = doc["resolution"];
                    if (res && strlen(res) > 0) {
                        strncpy(twinResolution, res, sizeof(twinResolution) - 1);
                    }
                    long p = doc["period"];
                    if (p > 0) twinPeriod = p;
                    const char* sd = doc["sd_write"];
                    if (sd && (strcmp(sd, "on") == 0 || strcmp(sd, "off") == 0))
                        strncpy(twinSdWrite, sd, sizeof(twinSdWrite) - 1);
                    g_desired_updated = true;
                    Serial.printf("[TWIN] Desired update: res=%s period=%ld sd_write=%s\n",
                                  twinResolution, twinPeriod, twinSdWrite);
                }
            }
            state = S_IDLE;
        }
        lineBuf = "";
    } else if (c != '\r') {
        lineBuf += c;
    }
}

void azure_handle() {
    if (!mqttConnected) return;
    while (modemSerial.available()) {
        mqttRxFeed(modemSerial.read());
    }
}

// ═══ Device twin via raw MQTT ══════════════════════════════════════════════════

bool requestDeviceTwin() {
    if (!azure_mqtt_connect()) return false;
    g_twin_response_ready = false;
    twinRid++;
    char topic[64];
    snprintf(topic, sizeof(topic), "$iothub/twin/GET/?$rid=%d", twinRid);
    size_t topicLen = strlen(topic);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "+CMQTTTOPIC=0,%u", (unsigned)topicLen);
    if (!mqttATPrompt(cmd, topic, topicLen)) return false;
    // SIM7670G requires minimum 1-byte payload — send a space
    snprintf(cmd, sizeof(cmd), "+CMQTTPAYLOAD=0,1");
    if (!mqttATPrompt(cmd, " ", 1)) return false;
    String urcLine;
    int r = mqttAT("+CMQTTPUB=0,1,60,0", "+CMQTTPUB:", 70000, &urcLine);
    return (r == 1);
}

bool checkTwinUpdate(char* resolution, size_t resSize, long* period) {
    if (g_desired_updated) {
        g_desired_updated = false;
        if (strlen(twinResolution) > 0) {
            strncpy(resolution, twinResolution, resSize - 1);
            resolution[resSize - 1] = '\0';
        }
        if (twinPeriod > 0) *period = twinPeriod;
        return true;
    }
    return false;
}

bool parseInitialTwin(char* resolution, size_t resSize, long* period) {
    if (!g_twin_response_ready) return false;
    g_twin_response_ready = false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, g_twin_response);
    if (err) { Serial.printf("[TWIN] Parse err: %s\n", err.c_str()); return false; }
    const char* res = doc["desired"]["resolution"];
    if (res && strlen(res) > 0) { strncpy(resolution, res, resSize - 1); resolution[resSize - 1] = '\0'; }
    long p = doc["desired"]["period"];
    if (p > 0) *period = p;
    const char* sd = doc["desired"]["sd_write"];
    if (sd && (strcmp(sd, "on") == 0 || strcmp(sd, "off") == 0))
        strncpy(twinSdWrite, sd, sizeof(twinSdWrite) - 1);
    if (res && strlen(res) > 0) strncpy(twinResolution, res, sizeof(twinResolution) - 1);
    if (p > 0) twinPeriod = p;
    Serial.printf("[TWIN] Parsed: res=%s period=%ld sd_write=%s\n", resolution, *period, twinSdWrite);
    return true;
}

bool reportDeviceTwin(const char* resolution, long period, int temp, const char* sdWrite) {
    if (!azure_mqtt_connect()) return false;
    twinRid++;
    char topic[96];
    snprintf(topic, sizeof(topic), "$iothub/twin/PATCH/properties/reported/?$rid=%d", twinRid);
    char payload[200];
    snprintf(payload, sizeof(payload),
             "{\"resolution\":\"%s\",\"period\":%ld,\"temp\":%d,\"sd_write\":\"%s\"}",
             resolution, period, temp, sdWrite);

    size_t topicLen = strlen(topic), payloadLen = strlen(payload);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "+CMQTTTOPIC=0,%u", (unsigned)topicLen);
    if (!mqttATPrompt(cmd, topic, topicLen)) return false;
    snprintf(cmd, sizeof(cmd), "+CMQTTPAYLOAD=0,%u", (unsigned)payloadLen);
    if (!mqttATPrompt(cmd, payload, payloadLen)) return false;
    String urcLine;
    int r = mqttAT("+CMQTTPUB=0,1,60,0", "+CMQTTPUB:", 70000, &urcLine);
    int c1 = (r == 1) ? urcLine.indexOf(',') : -1;
    bool ok = (c1 >= 0) && (urcLine.substring(c1 + 1).toInt() == 0);
    Serial.printf("[TWIN] Reported %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// ═══ Block-blob upload via raw AT+HTTP* ════════════════════════════════════════
static void httpFlush() {
    while (modemSerial.available()) modemSerial.read();
}

static bool httpWaitSubstr(const char* target, uint32_t ms) {
    String acc;
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        while (modemSerial.available()) {
            acc += (char)modemSerial.read();
            if (acc.indexOf(target) >= 0) return true;
            if (acc.length() > 512) acc = acc.substring(256);
        }
        delay(1);
    }
    return false;
}

static bool httpWaitLine(const char* want, uint32_t ms, String* out = nullptr) {
    String line;
    bool exactOK = (strcmp(want, "OK") == 0);
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        while (modemSerial.available()) {
            char c = modemSerial.read();
            if (c == '\r') continue;
            if (c == '\n') {
                if (line.length() > 0) {
                    bool hit = exactOK ? (line == "OK") : (line.indexOf(want) >= 0);
                    if (hit) { if (out) *out = line; return true; }
                    if (line == "ERROR" || line.startsWith("+CME ERROR")) {
                        Serial.printf("[HTTP] err: %s\n", line.c_str());
                        return false;
                    }
                }
                line = "";
            } else {
                line += c;
            }
        }
        delay(1);
    }
    return false;
}

static bool httpWaitOKNoise(uint32_t ms) {
    String acc;
    acc.reserve(256);
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        while (modemSerial.available()) {
            char c = modemSerial.read();
            acc += c;
            if (acc.endsWith("\r\nOK\r\n"))    return true;
            if (acc.endsWith("\r\nERROR\r\n")) { Serial.println("[HTTP] err: ERROR"); return false; }
            if (acc.length() > 4096) acc.remove(0, acc.length() - 256);
        }
        delay(1);
    }
    return false;
}

static const char* httpStatusLabel(int sc) {
    switch (sc) {
        case 701: return " (unknown)";
        case 702: return " (conn timeout)";
        case 703: return " (DNS err)";
        case 704: return " (conn failed)";
        case 705: return " (SSL err)";
        case 706: return " (socket send/recv fail)";
        case 707: return " (server closed)";
        default:  return "";
    }
}

// Single HTTPS PUT using raw AT+HTTP* commands. Uses SSL context 1.
static bool rawHttpPut(const char* blobHost, const char* path,
                       const uint8_t* body, size_t bodyLen,
                       const char* contentType, const char* blobType) {
    // Force echo off — ATE1 leaks corrupt binary HTTPDATA
    modemSerial.print("ATE0\r\n");
    delay(20);
    httpFlush();

    // Reset any prior HTTP session
    modemSerial.print("AT+HTTPTERM\r\n");
    delay(100);
    httpFlush();

    // Open new HTTP session
    modemSerial.print("AT+HTTPINIT\r\n");
    if (!httpWaitLine("OK", 10000)) {
        Serial.println("[RAW] HTTPINIT fail"); return false;
    }
    httpFlush();

    // Configure SSL context 1 (separate from MQTT's context 0)
    modemSerial.print("AT+CSSLCFG=\"sslversion\",1,4\r\n");
    httpWaitLine("OK", 3000); httpFlush();
    modemSerial.print("AT+CSSLCFG=\"authmode\",1,0\r\n");
    httpWaitLine("OK", 3000); httpFlush();
    modemSerial.print("AT+CSSLCFG=\"enableSNI\",1,1\r\n");
    httpWaitLine("OK", 3000); httpFlush();

    // Bind HTTP session to SSL context 1
    modemSerial.print("AT+HTTPPARA=\"SSLCFG\",\"1\"\r\n");
    if (!httpWaitLine("OK", 3000)) Serial.println("[RAW] SSLCFG bind fail (may be ok)");
    httpFlush();

    // Set request URL
    {
        String urlCmd = String("AT+HTTPPARA=\"URL\",\"https://") + blobHost + path + "\"\r\n";
        Serial.printf("[RAW] URL len=%u\n", (unsigned)(urlCmd.length() - 19));
        modemSerial.print(urlCmd);
        if (!httpWaitLine("OK", 5000)) {
            Serial.println("[RAW] URL fail");
            modemSerial.print("AT+HTTPTERM\r\n"); delay(200); return false;
        }
        httpFlush();
    }

    // Content-Type header
    {
        String ctCmd = String("AT+HTTPPARA=\"CONTENT\",\"") + contentType + "\"\r\n";
        modemSerial.print(ctCmd);
        httpWaitLine("OK", 3000); httpFlush();
    }

    // x-ms-blob-type custom header
    if (blobType) {
        String hdCmd = String("AT+HTTPPARA=\"USERDATA\",\"x-ms-blob-type: ") + blobType + "\"\r\n";
        modemSerial.print(hdCmd);
        if (!httpWaitLine("OK", 3000)) Serial.println("[RAW] USERDATA fail");
        httpFlush();
    }

    // Feed payload via AT+HTTPDATA
    {
        char dataCmd[64];
        snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%u,10000\r\n", (unsigned)bodyLen);
        modemSerial.print(dataCmd);
        if (!httpWaitSubstr("DOWNLOAD", 15000)) {
            Serial.println("[RAW] DOWNLOAD fail");
            modemSerial.print("AT+HTTPTERM\r\n"); delay(200); return false;
        }
        const size_t CHUNK = 1024;
        for (size_t pos = 0; pos < bodyLen; pos += CHUNK) {
            size_t n = (bodyLen - pos < CHUNK) ? (bodyLen - pos) : CHUNK;
            modemSerial.write(body + pos, n);
            modemSerial.flush();
            delay(5);
        }
        Serial.printf("[RAW] Data sent (%u B)\n", (unsigned)bodyLen);
        if (!httpWaitOKNoise(30000)) {
            Serial.println("[RAW] Data ack fail");
            modemSerial.print("AT+HTTPTERM\r\n"); delay(200); return false;
        }
        httpFlush();
    }

    // Execute PUT and wait for +HTTPACTION: URC
    modemSerial.print("AT+HTTPACTION=4\r\n");
    String actionUrc;
    if (!httpWaitLine("+HTTPACTION:", 120000, &actionUrc)) {
        Serial.println("[RAW] HTTPACTION timeout/fail");
        modemSerial.print("AT+HTTPTERM\r\n"); delay(300); httpFlush();
        return false;
    }
    Serial.printf("[RAW] %s\n", actionUrc.c_str());

    // Close HTTP session
    modemSerial.print("AT+HTTPTERM\r\n");
    delay(100); httpFlush();

    // Parse HTTP status from "+HTTPACTION: 4,<code>,<len>"
    int c1 = actionUrc.indexOf(',');
    if (c1 < 0) return false;
    int c2 = actionUrc.indexOf(',', c1 + 1);
    int sc = (c2 > c1) ? actionUrc.substring(c1 + 1, c2).toInt()
                       : actionUrc.substring(c1 + 1).toInt();
    Serial.printf("[RAW] PUT %u B -> HTTP %d%s\n", (unsigned)bodyLen, sc, httpStatusLabel(sc));
    return (sc == 201);
}

// ─── HTTPS POST via TinyGSM modem ────────────────────────────────────────────
static int modemHttpsPost(const char* url, const char* sasTok,
                           const char* body, String& respBody) {
    if (!modem.https_begin()) { Serial.println("[HTTPS] begin fail"); return -1; }
    if (!modem.https_set_url(url)) { modem.https_end(); Serial.println("[HTTPS] set_url fail"); return -1; }
    modem.https_set_content_type("application/json");
    modem.https_add_header("Authorization", sasTok);
    int sc = modem.https_post(body, strlen(body));
    respBody = modem.https_body();
    modem.https_end();
    return sc;
}

// ═══ Blob upload from frame buffer (headless / periodic capture) ═══════════════

bool uploadFrameBuffer(const uint8_t* data, size_t len, const char* blobName) {
    if (!gprsReady) { Serial.println("[UPLOAD] GPRS not ready"); return false; }

    if (uploadInCooldown()) {
        unsigned long remainSec = (g_uploadCooldownUntilMs - millis()) / 1000UL;
        Serial.printf("[UPLOAD] Skip: cooldown %lus left\n", remainSec);
        return false;
    }

    // Step 1: Get blob SAS URI from IoT Hub
    char sasTok[400];
    if (!generateSASToken(sasTok, sizeof(sasTok), 300)) {
        Serial.println("[UPLOAD] SAS token failed"); return false;
    }

    char reqBody[128];
    snprintf(reqBody, sizeof(reqBody), "{\"blobName\":\"%s/%s\"}", BLOB_DIRECTORY, blobName);
    char apiPath[96];
    snprintf(apiPath, sizeof(apiPath), "/devices/%s/files?api-version=2020-03-13", DEVICE_ID);
    String hubUrl = String("https://") + IOT_HUB_HOSTNAME + apiPath;

    String respBody;
    int sc = modemHttpsPost(hubUrl.c_str(), sasTok, reqBody, respBody);
    Serial.printf("[HUB] SAS request -> HTTP %d\n", sc);
    if (sc != 200 && sc != 201) {
        if (respBody.length() > 0) Serial.printf("[HUB] SAS body: %s\n", respBody.c_str());
        bumpUploadCooldown();
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, respBody) != DeserializationError::Ok) {
        Serial.println("[UPLOAD] JSON parse failed"); bumpUploadCooldown(); return false;
    }
    const char* hn  = doc["hostName"];
    const char* cn  = doc["containerName"];
    const char* bn  = doc["blobName"];
    const char* st  = doc["sasToken"];
    const char* cid = doc["correlationId"];
    if (!hn || !cn || !bn || !st || !cid) {
        Serial.println("[UPLOAD] Incomplete SAS response"); bumpUploadCooldown(); return false;
    }

    char blobHost[128], corrId[256];
    strncpy(blobHost, hn,  sizeof(blobHost) - 1); blobHost[sizeof(blobHost) - 1] = '\0';
    strncpy(corrId,   cid, sizeof(corrId)   - 1); corrId[sizeof(corrId)   - 1]   = '\0';
    const char* sasQuery = (st[0] == '?') ? (st + 1) : st;

    // Step 2: Single PUT
    Serial.printf("[UPLOAD] %u B -> single PUT\n", (unsigned)len);

    char blobPath[768];
    snprintf(blobPath, sizeof(blobPath), "/%s/%s?%s", cn, bn, sasQuery);

    bool ok = rawHttpPut(blobHost, blobPath, data, len,
                         "application/octet-stream", "BlockBlob");
    Serial.printf("[BLOB] Single PUT -> %s\n", ok ? "HTTP 201" : "FAIL");

    bool mqttWasStopped = false;

    if (!ok) {
        // Stop MQTT to free modem SSL resources, then retry
        Serial.println("[UPLOAD] Pausing MQTT for retry...");
        mqttAT("+CMQTTDISC=0,120", nullptr, 5000);
        mqttAT("+CMQTTSTOP", "+CMQTTSTOP: 0", 5000);
        mqttConnected = false;
        mqttWasStopped = true;
        delay(200);

        ok = rawHttpPut(blobHost, blobPath, data, len,
                        "application/octet-stream", "BlockBlob");
        Serial.printf("[BLOB] Retry 1 -> %s\n", ok ? "HTTP 201" : "FAIL");

        if (!ok) {
            delay(2000);
            ok = rawHttpPut(blobHost, blobPath, data, len,
                            "application/octet-stream", "BlockBlob");
            Serial.printf("[BLOB] Retry 2 -> %s\n", ok ? "HTTP 201" : "FAIL");
        }
    }

    // Step 3: Notify IoT Hub
    generateSASToken(sasTok, sizeof(sasTok), 300);
    {
        char nBody[512];
        snprintf(nBody, sizeof(nBody),
                 "{\"correlationId\":\"%s\",\"isSuccess\":%s,\"statusCode\":%d,"
                 "\"statusDescription\":\"%s\"}",
                 corrId, ok ? "true" : "false", ok ? 200 : 500,
                 ok ? "OK" : "DeviceUploadFailed");
        String nUrl = String("https://") + IOT_HUB_HOSTNAME
                      + "/devices/" DEVICE_ID "/files/notifications?api-version=2020-03-13";
        Serial.printf("[HUB] Notify attempt (isSuccess=%s)\n", ok ? "true" : "false");
        modemHttpsPost(nUrl.c_str(), sasTok, nBody, respBody);
    }

    if (mqttWasStopped) {
        Serial.println("[UPLOAD] Reconnecting MQTT...");
        azure_mqtt_connect();
        delay(200);
        azure_handle();
    }

    // Sync twin via GET after upload
    if (requestDeviceTwin()) {
        unsigned long t0 = millis();
        while (!g_twin_response_ready && millis() - t0 < 5000) {
            azure_handle();
            delay(10);
        }
        if (g_twin_response_ready) {
            char tmpRes[8] = {};
            long tmpPeriod = 0;
            if (parseInitialTwin(tmpRes, sizeof(tmpRes), &tmpPeriod)) {
                if (strlen(tmpRes) > 0) strncpy(twinResolution, tmpRes, sizeof(twinResolution) - 1);
                if (tmpPeriod > 0) twinPeriod = tmpPeriod;
                g_desired_updated = true;
                Serial.printf("[TWIN] Synced via GET: res=%s period=%ld\n",
                              twinResolution, twinPeriod);
            }
        }
    }

    Serial.printf("[UPLOAD] %s (%u B)\n", ok ? "OK" : "FAILED", (unsigned)len);
    if (ok) resetUploadCooldown();
    else    bumpUploadCooldown();
    return ok;
}

// ═══ Blocking upload from SD card (GUI-driven) ═════════════════════════════════

void performUpload() {
    uint8_t cl = uploadClientNum;
    char    fn[32];
    strncpy(fn, uploadFilename, sizeof(fn) - 1); fn[sizeof(fn) - 1] = '\0';

    if (!gprsReady) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"Cellular not connected\"}");
        return;
    }

    if (uploadInCooldown()) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"Upload cooling down — try again later\"}");
        return;
    }

    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Generating Azure auth token...\"}");

    char sasTok[400];
    if (!generateSASToken(sasTok, sizeof(sasTok), 300)) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"SAS token generation failed\"}");
        return;
    }

    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Requesting upload URL from IoT Hub via cellular...\"}");

    // Step 1: Get blob SAS URI
    char reqBody[128];
    snprintf(reqBody, sizeof(reqBody), "{\"blobName\":\"%s/%s\"}", BLOB_DIRECTORY, fn);
    char apiPath[96];
    snprintf(apiPath, sizeof(apiPath), "/devices/%s/files?api-version=2020-03-13", DEVICE_ID);
    String hubUrl = String("https://") + IOT_HUB_HOSTNAME + apiPath;

    String respBody;
    int sc = modemHttpsPost(hubUrl.c_str(), sasTok, reqBody, respBody);
    Serial.printf("[HUB] SAS request -> HTTP %d\n", sc);
    if (sc != 200 && sc != 201) {
        if (respBody.length() > 0) Serial.printf("[HUB] SAS body: %s\n", respBody.c_str());
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"IoT Hub did not provide upload URL\"}");
        bumpUploadCooldown();
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, respBody) != DeserializationError::Ok) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"Bad JSON from IoT Hub\"}");
        bumpUploadCooldown();
        return;
    }
    const char* hn  = doc["hostName"];
    const char* cn  = doc["containerName"];
    const char* bn  = doc["blobName"];
    const char* st  = doc["sasToken"];
    const char* cid = doc["correlationId"];
    if (!hn || !cn || !bn || !st || !cid) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"Incomplete SAS response\"}");
        bumpUploadCooldown();
        return;
    }

    char blobHost[128], corrId[256];
    strncpy(blobHost, hn,  sizeof(blobHost) - 1); blobHost[sizeof(blobHost) - 1] = '\0';
    strncpy(corrId,   cid, sizeof(corrId)   - 1); corrId[sizeof(corrId)   - 1]   = '\0';
    const char* sasQuery = (st[0] == '?') ? (st + 1) : st;

    // Step 2: Read file from SD and upload
    bool ok = false;
    char sdPath[48];
    snprintf(sdPath, sizeof(sdPath), "/photos/%s", fn);

    File f = SD_MMC.open(sdPath, FILE_READ);
    if (!f) {
        Serial.printf("[UPLOAD] Cannot open %s\n", sdPath);
        goto upload_cleanup;
    }
    {
        size_t fileSize = f.size();
        if (fileSize == 0) {
            f.close();
            Serial.println("[UPLOAD] File is empty");
            goto upload_cleanup;
        }

        uint8_t* fileBuf = (uint8_t*)malloc(fileSize);
        if (!fileBuf) {
            f.close();
            Serial.printf("[UPLOAD] Failed to allocate %u bytes\n", (unsigned)fileSize);
            webServerSendTXT(cl,
                "{\"type\":\"upload_error\",\"msg\":\"Out of memory\"}");
            goto upload_cleanup;
        }

        size_t bytesRead = f.read(fileBuf, fileSize);
        f.close();

        if (bytesRead != fileSize) {
            Serial.printf("[UPLOAD] SD read error: got %u of %u\n",
                          (unsigned)bytesRead, (unsigned)fileSize);
            free(fileBuf);
            goto upload_cleanup;
        }

        char msg[80];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"upload_progress\",\"msg\":\"Uploading %u bytes via cellular...\"}",
                 (unsigned)fileSize);
        webServerSendTXT(cl, msg);

        char blobPath[768];
        snprintf(blobPath, sizeof(blobPath), "/%s/%s?%s", cn, bn, sasQuery);

        ok = rawHttpPut(blobHost, blobPath, fileBuf, fileSize,
                        "application/octet-stream", "BlockBlob");

        bool mqttWasStopped = false;
        if (!ok) {
            mqttAT("+CMQTTDISC=0,120", nullptr, 5000);
            mqttAT("+CMQTTSTOP", "+CMQTTSTOP: 0", 5000);
            mqttConnected = false;
            mqttWasStopped = true;
            delay(200);

            ok = rawHttpPut(blobHost, blobPath, fileBuf, fileSize,
                            "application/octet-stream", "BlockBlob");
            if (!ok) {
                delay(2000);
                ok = rawHttpPut(blobHost, blobPath, fileBuf, fileSize,
                                "application/octet-stream", "BlockBlob");
            }
        }

        if (mqttWasStopped) {
            azure_mqtt_connect();
            delay(200);
            azure_handle();
        }

        free(fileBuf);
    }

upload_cleanup:
    // Step 3: Notify IoT Hub
    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Notifying IoT Hub of upload result...\"}");
    generateSASToken(sasTok, sizeof(sasTok), 300);
    {
        char nBody[512];
        snprintf(nBody, sizeof(nBody),
                 "{\"correlationId\":\"%s\",\"isSuccess\":%s,\"statusCode\":%d,"
                 "\"statusDescription\":\"%s\"}",
                 corrId, ok ? "true" : "false", ok ? 200 : 500,
                 ok ? "OK" : "DeviceUploadFailed");
        String nUrl = String("https://") + IOT_HUB_HOSTNAME
                      + "/devices/" DEVICE_ID "/files/notifications?api-version=2020-03-13";
        respBody = "";
        modemHttpsPost(nUrl.c_str(), sasTok, nBody, respBody);
    }

    if (ok) {
        resetUploadCooldown();
        generateSASToken(sasTok, sizeof(sasTok), 300);
        char telJson[256];
        snprintf(telJson, sizeof(telJson),
                 "{\"device\":\"%s\",\"file\":\"%s\","
                 "\"path\":\"%s/%s\",\"success\":true}",
                 DEVICE_ID, fn, BLOB_DIRECTORY, fn);
        azure_publish_telemetry(telJson);

        char done[120];
        snprintf(done, sizeof(done),
                 "{\"type\":\"upload_done\",\"msg\":\"%s uploaded via cellular\","
                 "\"file\":\"%s\"}", fn, fn);
        webServerSendTXT(cl, done);
    } else {
        bumpUploadCooldown();
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"Upload failed. Check serial monitor.\"}");
    }
}
