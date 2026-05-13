#include "azure_iot_manager.h"
#include "cellular_manager.h"
#include "web_server_manager.h"
#include "sd_card_manager.h"
#include "SD_MMC.h"
#include "mbedtls/base64.h"
#include <mbedtls/md.h>
#include <ArduinoJson.h>

// ─── MQTT persistent state ──────────────────────────────────────────────────
static char     mqttPassword[400]  = {};
static uint32_t sasExpiry          = 0;
static bool     mqttConnected      = false;
static bool     mqttInitialized    = false;

// ─── GUI upload state (definitions for extern declarations in header) ───
volatile UploadState uploadState = UPLOAD_IDLE;
char                 uploadFilename[32] = {};
uint8_t              uploadClientNum    = 0;

volatile bool telemetryRequested = false;
uint8_t      telemetryClientNum  = 0;

#define MQTT_USERNAME_FMT IOT_HUB_HOSTNAME "/" DEVICE_ID "/?api-version=2021-04-12"
#define D2C_TOPIC         "devices/" DEVICE_ID "/messages/events/"

// ─── SAS token renew helper ─────────────────────────────────────────────────
static bool sasNeedsRenew() {
    if (sasExpiry == 0) return true;
    uint32_t now = (uint32_t)time(NULL);
    if (now < 1577836800UL) return true;
    return (now > sasExpiry - (SAS_TOKEN_VALID_S / 5));
}

// ─── Azure SAS token (mbedTLS HMAC-SHA256) ──────────────────────────────────
bool generateSASToken(char* out, size_t outSize, uint32_t durationSecs) {
    if (time(NULL) < 1577836800UL) {
        Serial.println("[SAS] System time not set");
    }
    char resourceUri[128];
    snprintf(resourceUri, sizeof(resourceUri), "%s/devices/%s",
             IOT_HUB_HOSTNAME, DEVICE_ID);
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
                              (const uint8_t*)DEVICE_KEY, strlen(DEVICE_KEY)) != 0)
        return false;
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, keyLen);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)sts, strlen(sts));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    char b64[64]; size_t b64len;
    if (mbedtls_base64_encode((uint8_t*)b64, sizeof(b64), &b64len, hmac, 32) != 0)
        return false;
    b64[b64len] = '\0';
    char encSig[128] = {};
    for (size_t i = 0; i < b64len; i++) {
        if      (b64[i] == '+') { strcat(encSig, "%2B"); }
        else if (b64[i] == '/') { strcat(encSig, "%2F"); }
        else if (b64[i] == '=') { strcat(encSig, "%3D"); }
        else { char t[2] = {b64[i], '\0'}; strcat(encSig, t); }
    }
    snprintf(out, outSize,
             "SharedAccessSignature sr=%s&sig=%s&se=%lu",
             encodedUri, encSig, (unsigned long)expiry);
    return true;
}

// ═══ AT command helpers ═════════════════════════════════════════════════════

static int mqttAT(const char* cmd, const char* urc,
                  uint32_t timeoutMs = 3000, String* matchLine = nullptr) {
    while (modemSerial.available()) modemSerial.read();
    if (cmd && cmd[0]) {
        modemSerial.print("AT"); modemSerial.print(cmd); modemSerial.print("\r\n");
    }
    unsigned long t0 = millis();
    String acc = "";
    while (millis() - t0 < timeoutMs) {
        while (modemSerial.available()) {
            char c = modemSerial.read(); acc += c;
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
    while (modemSerial.available()) modemSerial.read();
    modemSerial.print("AT"); modemSerial.print(cmd); modemSerial.print("\r\n");
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (modemSerial.available() && modemSerial.read() == '>') {
            delay(10);
            modemSerial.write((const uint8_t*)data, dataLen);
            t0 = millis(); String acc = "";
            while (millis() - t0 < timeoutMs) {
                while (modemSerial.available()) {
                    char c = modemSerial.read(); acc += c;
                    if (c == '\n') {
                        acc.trim();
                        if (acc == "OK") return true;
                        if (acc.startsWith("ERROR")) return false;
                        acc = "";
                    }
                }
                delay(1);
            }
            return false;
        }
        delay(1);
    }
    return false;
}

// Wait for substring in serial stream (for DOWNLOAD prompt with no trailing \n)
static bool atWaitSubstr(const char* substr, uint32_t timeoutMs) {
    size_t slen = strlen(substr);
    String acc = "";
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (modemSerial.available()) {
            char c = modemSerial.read(); acc += c;
            if (acc.indexOf(substr) >= 0) return true;
            if (acc.indexOf("ERROR") >= 0 || acc.indexOf("+CME ERROR") >= 0) return false;
            if (acc.length() > 80) acc = acc.substring(acc.length() - 40);
        }
        delay(1);
    }
    return false;
}

// ═══ Cellular MQTT via AT+CMQTT* ═══════════════════════════════════════════

static void mqtt_callback(const char* topic, const uint8_t* payload, uint32_t len) {
    Serial.printf("[MQTT] topic=%s\n", topic);
}

bool azure_mqtt_init() {
    if (mqttInitialized) return true;
    if (!generateSASToken(mqttPassword, sizeof(mqttPassword), SAS_TOKEN_VALID_S)) {
        Serial.println("[MQTT] SAS failed"); return false;
    }
    sasExpiry = (uint32_t)time(NULL) + SAS_TOKEN_VALID_S;

    if (!modemConnectGPRS()) { Serial.println("[MQTT] GPRS failed"); return false; }

    modem.mqtt_set_callback(mqtt_callback);
    modem.mqtt_set_rx_buffer_size(2048);

    // TinyGSM doesn't set SNI — required by Azure IoT Hub
    modem.sendAT("+CSSLCFG=\"enableSNI\",0,1");
    modem.waitResponse();

    if (!modem.mqtt_connect(0, IOT_HUB_HOSTNAME, MQTT_PORT,
                            MQTT_USERNAME_FMT, mqttPassword, DEVICE_ID, 60)) {
        Serial.println("[MQTT] Connect failed"); return false;
    }
    mqttConnected = true;
    mqttInitialized = true;

    Serial.println("[MQTT] Connected (cellular)");
    return true;
}

bool azure_mqtt_connect() {
    if (mqttConnected && !sasNeedsRenew()) return true;
    if (sasNeedsRenew()) {
        if (!mqttInitialized) return azure_mqtt_init();
        modem.mqtt_disconnect(0);
        mqttConnected = false; mqttInitialized = false;
        return azure_mqtt_init();
    }
    return mqttConnected;
}

bool azure_publish_telemetry(const char* payload) {
    if (!azure_mqtt_connect()) return false;
    return modem.mqtt_publish(0, D2C_TOPIC, payload);
}

void azure_handle() {
    if (mqttConnected) modem.mqtt_handle();
}

// ─── HTTPS via AT+HTTP* modem commands ───────────────────────────────────────
static int httpsReq(const char* host, const char* method, const char* path,
                    const char* extraHdrs, const char* body, size_t bodyLen,
                    char* respBuf, size_t respBufSz) {
    modem.sendAT(GF("+CSSLCFG=\"sslversion\",0,4"));
    modem.waitResponse(3000);
    modem.sendAT(GF("+CSSLCFG=\"authmode\",0,0"));
    modem.waitResponse(3000);

    modem.sendAT(GF("+HTTPTERM"));
    modem.waitResponse(2000);
    modem.sendAT(GF("+HTTPINIT"));
    if (modem.waitResponse(5000) != 1) {
        Serial.println("[HTTP] HTTPINIT failed"); return -1;
    }
    modem.sendAT(GF("+HTTPPARA=\"CID\",1"));
    modem.waitResponse(3000);
    modem.sendAT(GF("+HTTPPARA=\"SSLCFG\",0"));
    modem.waitResponse(3000);

    modemSerial.print("AT+HTTPPARA=\"URL\",\"https://");
    modemSerial.print(host); modemSerial.print(path);
    modemSerial.print("\"\r\n");
    modem.waitResponse(5000);

    if (body && bodyLen > 0) {
        modem.sendAT(GF("+HTTPPARA=\"CONTENT\",\"application/json\""));
        modem.waitResponse(3000);
    }

    if (extraHdrs) {
        const char* p = strstr(extraHdrs, "Authorization: ");
        if (p) {
            const char* end = strstr(p, "\r\n");
            char authLine[380];
            size_t alen = end ? (size_t)(end - p) : strlen(p);
            if (alen >= sizeof(authLine)) alen = sizeof(authLine) - 1;
            strncpy(authLine, p, alen); authLine[alen] = '\0';
            modemSerial.print("AT+HTTPPARA=\"USERDATA\",\"");
            modemSerial.print(authLine);
            modemSerial.print("\\r\\n\"\r\n");
            modem.waitResponse(5000);
        }
    }

    int statusCode = -1, dataLen = 0;

    modemSerial.print("AT+HTTPPOST=60000,");
    modemSerial.print((unsigned)bodyLen);
    modemSerial.print("\r\n");
    if (modem.waitResponse(10000, GF("CONNECT")) != 1) {
        Serial.println("[HTTP] No CONNECT for POST");
        modem.sendAT(GF("+HTTPTERM")); modem.waitResponse(2000);
        return -1;
    }
    if (body && bodyLen > 0)
        modemSerial.write((const uint8_t*)body, bodyLen);

    String dummy;
    modem.waitResponse(60000, dummy, GF("+HTTPPOST:"));
    String line = modemSerial.readStringUntil('\n');
    line.trim();
    modem.waitResponse(3000);
    {
        int c1 = line.indexOf(','), c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
        if (c1 >= 0) {
            statusCode = line.substring(c1 + 1, c2 > 0 ? c2 : line.length()).toInt();
            if (c2 > 0) dataLen = line.substring(c2 + 1).toInt();
        }
    }
    Serial.printf("[HTTP] POST → %d  dataLen=%d\n", statusCode, dataLen);

    if (respBuf && respBufSz > 1 && dataLen > 0) {
        int rlen = (int)(respBufSz - 1) < dataLen ? (int)(respBufSz - 1) : dataLen;
        modemSerial.print("AT+HTTPREAD=0,");
        modemSerial.print(rlen);
        modemSerial.print("\r\n");
        String dummy2;
        if (modem.waitResponse(10000, dummy2, GF("+HTTPREAD:")) == 1) {
            modemSerial.readStringUntil('\n');
            int pos = 0;
            unsigned long t0 = millis();
            while (pos < rlen && millis() - t0 < 10000) {
                if (modemSerial.available()) {
                    respBuf[pos++] = (char)modemSerial.read();
                    t0 = millis();
                }
            }
            respBuf[pos] = '\0';
            modem.waitResponse(5000);
        }
    }

    modem.sendAT(GF("+HTTPTERM"));
    modem.waitResponse(3000);
    return statusCode;
}

// File-streaming HTTPS PUT via AT+HTTPPUT transparent mode.
static int httpsUploadFile(const char* host, const char* pathAndQuery,
                           const char* sdPath) {
    File f = SD_MMC.open(sdPath, FILE_READ);
    if (!f) { Serial.printf("[BLOB] Cannot open %s\n", sdPath); return -1; }
    size_t fsz = f.size();
    Serial.printf("[BLOB] Uploading %s (%u bytes) → %s\n", sdPath, (unsigned)fsz, host);

    modem.sendAT(GF("+CSSLCFG=\"sslversion\",0,4"));
    modem.waitResponse(3000);
    modem.sendAT(GF("+CSSLCFG=\"authmode\",0,0"));
    modem.waitResponse(3000);

    modem.sendAT(GF("+HTTPTERM"));
    modem.waitResponse(2000);
    modem.sendAT(GF("+HTTPINIT"));
    if (modem.waitResponse(5000) != 1) {
        f.close(); Serial.println("[BLOB] HTTPINIT failed"); return -1;
    }
    modem.sendAT(GF("+HTTPPARA=\"CID\",1"));
    modem.waitResponse(3000);
    modem.sendAT(GF("+HTTPPARA=\"SSLCFG\",0"));
    modem.waitResponse(3000);

    modemSerial.print("AT+HTTPPARA=\"URL\",\"https://");
    modemSerial.print(host); modemSerial.print(pathAndQuery);
    modemSerial.print("\"\r\n");
    modem.waitResponse(5000);

    modem.sendAT(GF("+HTTPPARA=\"CONTENT\",\"image/jpeg\""));
    modem.waitResponse(3000);
    modem.sendAT(GF("+HTTPPARA=\"USERDATA\",\"x-ms-blob-type: BlockBlob\\r\\n\""));
    modem.waitResponse(3000);

    modemSerial.print("AT+HTTPPUT=120000,");
    modemSerial.print((unsigned)fsz);
    modemSerial.print("\r\n");
    if (modem.waitResponse(15000, GF("CONNECT")) != 1) {
        f.close(); Serial.println("[BLOB] No CONNECT for PUT");
        modem.sendAT(GF("+HTTPTERM")); modem.waitResponse(2000);
        return -1;
    }

    uint8_t buf[512]; size_t sent = 0;
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        modemSerial.write(buf, n);
        sent += n;
    }
    f.close();
    Serial.printf("[BLOB] Sent %u / %u bytes\n", (unsigned)sent, (unsigned)fsz);

    String dummy;
    modem.waitResponse(120000, dummy, GF("+HTTPPUT:"));
    String line = modemSerial.readStringUntil('\n');
    line.trim();
    modem.waitResponse(5000);

    int statusCode = -1;
    {
        int c1 = line.indexOf(','), c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
        if (c1 >= 0)
            statusCode = line.substring(c1 + 1, c2 > 0 ? c2 : line.length()).toInt();
    }
    modem.sendAT(GF("+HTTPTERM"));
    modem.waitResponse(3000);
    Serial.printf("[BLOB] Response: HTTP %d\n", statusCode);
    return statusCode;
}

// ═══ HTTPS REST API via TinyGSM ════════════════════════════════════════════

static bool getBlobSASUri(const char* sasTok, const char* blobName,
                          char* corrId, char* blobHost, char* blobPathQ) {
    char authHdr[380];
    snprintf(authHdr, sizeof(authHdr),
             "Authorization: %s\r\nContent-Type: application/json\r\n", sasTok);

    char body[96];
    snprintf(body, sizeof(body), "{\"blobName\":\"%s/%s\"}", BLOB_DIRECTORY, blobName);
    char path[96];
    snprintf(path, sizeof(path), "/devices/%s/files?api-version=2020-03-13", DEVICE_ID);

    static char respBuf[1024];
    respBuf[0] = '\0';
    int sc = httpsReq(IOT_HUB_HOSTNAME, "POST", path,
                      authHdr, body, strlen(body), respBuf, sizeof(respBuf));
    Serial.printf("[HUB] SAS request → HTTP %d\n%s\n", sc, respBuf);
    if (sc != 200 && sc != 201) return false;

    JsonDocument doc;
    if (deserializeJson(doc, respBuf) != DeserializationError::Ok) return false;

    const char* cid = doc["correlationId"];
    const char* hn  = doc["hostName"];
    const char* cn  = doc["containerName"];
    const char* bn  = doc["blobName"];
    const char* st  = doc["sasToken"];
    if (!cid || !hn || !cn || !bn || !st) return false;
    strncpy(corrId,   cid, 255); corrId[255]  = '\0';
    strncpy(blobHost, hn,  127); blobHost[127] = '\0';
    snprintf(blobPathQ, 768, "/%s/%s%s", cn, bn, st);
    return true;
}

static bool notifyIoTHubComplete(const char* sasTok, const char* corrId, bool success) {
    char authHdr[380];
    snprintf(authHdr, sizeof(authHdr),
             "Authorization: %s\r\nContent-Type: application/json\r\n", sasTok);

    char body[128];
    snprintf(body, sizeof(body),
             "{\"correlationId\":\"%s\",\"isSuccess\":%s,\"statusCode\":%d,"
             "\"statusDescription\":\"OK\"}",
             corrId, success ? "true" : "false", success ? 200 : 500);
    char path[96];
    snprintf(path, sizeof(path),
             "/devices/%s/files/notifications?api-version=2020-03-13", DEVICE_ID);

    int sc = httpsReq(IOT_HUB_HOSTNAME, "POST", path,
                      authHdr, body, strlen(body), nullptr, 0);
    Serial.printf("[HUB] Upload notify → HTTP %d\n", sc);
    return (sc >= 200 && sc < 300);
}

// ─── Chunked blob upload via Azure Put Block + Put Block List ────────────────
// AT+HTTPDATA has a ~10 KB body limit. We split the file into 8 KB blocks,
// upload each with PUT Block, then commit with PUT Block List — all over cellular.

#define BLOB_CHUNK_SIZE 8192

// Encode block number to URL-safe base64 (blockid query param) and plain base64
// (for the XML body). "block000" → base64 "YmxvY2swMDA=".
static void makeBlockId(uint32_t num, char* plainOut, size_t plainSz,
                        char* urlOut, size_t urlSz) {
    char raw[9];
    snprintf(raw, sizeof(raw), "block%03lu", (unsigned long)num);

    uint8_t b64[16] = {};
    size_t  b64len  = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64len, (const uint8_t*)raw, 8);
    b64[b64len] = '\0';

    if (plainOut) { strncpy(plainOut, (char*)b64, plainSz - 1); plainOut[plainSz - 1] = '\0'; }

    if (urlOut) {
        urlOut[0] = '\0';
        for (size_t i = 0; i < b64len; i++) {
            if      (b64[i] == '=') strncat(urlOut, "%3D", urlSz - strlen(urlOut) - 1);
            else if (b64[i] == '+') strncat(urlOut, "%2B", urlSz - strlen(urlOut) - 1);
            else if (b64[i] == '/') strncat(urlOut, "%2F", urlSz - strlen(urlOut) - 1);
            else { char t[2] = {(char)b64[i], '\0'}; strncat(urlOut, t, urlSz - strlen(urlOut) - 1); }
        }
    }
}

// ─── Low-level helpers for prompt-mode AT responses ────────────────────────────
// mqttAT() only checks accumulated strings on '\n', but the SIM7670G HTTPDATA
// prompt is the literal word "DOWNLOAD" with no trailing newline. These helpers
// read character-by-character and match substrings directly.

// Wait for an exact substring anywhere in the incoming stream.
// Returns true if found, false on timeout or ERROR.

// Wait for a line-buffered response: accumulate until '\n', then check.
// Returns: 2 = OK, 1 = matchLine contains the matched URC, -1 = ERROR, 0 = timeout
static int atWaitLine(const char* urc, uint32_t timeoutMs,
                      String* matchLine = nullptr) {
    String acc = "";
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (modemSerial.available()) {
            char c = modemSerial.read();
            acc += c;
            if (c == '\n') {
                acc.trim();
                if (acc.startsWith("ERROR") ||
                    acc.startsWith("+CME ERROR") ||
                    acc.startsWith("+CMS ERROR")) return -1;
                if (urc && acc.indexOf(urc) >= 0) {
                    if (matchLine) *matchLine = acc;
                    return 1;
                }
                if (acc == "OK") {
                    if (!urc) return 2;
                }
                acc = "";
            }
        }
        delay(1);
    }
    return 0;
}

// ─── HTTPS PUT via AT+HTTPACTION=4 ─────────────────────────────────────────────
// The SIM7670G modem supports HTTP methods 0-4 (GET/POST/HEAD/DELETE/PUT).
// We drive HTTPINIT → HTTPPARA(URL,CONTENT) → HTTPDATA → HTTPACTION=4 directly
// via raw AT commands, with proper SSL configuration for context 0.
// Per-call HTTPDATA buffer is ~10 KB so blocks stay at BLOB_CHUNK_SIZE (8 KB).
static int httpActionPut(const char* host, const char* path,
                         const char* contentType,
                         const uint8_t* body, size_t bodyLen) {
    // Drain any pending URCs from MQTT or prior HTTP sessions
    while (modemSerial.available()) modemSerial.read();

    // --- 1. Configure SSL context 0 for HTTPS ---
    mqttAT("+CSSLCFG=\"sslversion\",0,3", nullptr, 3000);  // TLS 1.2
    mqttAT("+CSSLCFG=\"authmode\",0,0",   nullptr, 3000);  // no cert verify
    mqttAT("+CSSLCFG=\"enableSNI\",0,1",  nullptr, 3000);  // SNI enabled

    // --- 2. Init HTTP session ---
    mqttAT("+HTTPTERM", nullptr, 2000);
    delay(50);
    int r = mqttAT("+HTTPINIT", nullptr, 5000);
    if (r != 2) {
        Serial.printf("[PUT] HTTPINIT failed r=%d\n", r);
        return -1;
    }

    // --- 3. Bind HTTP session to SSL context 0 ---
    r = mqttAT("+HTTPPARA=\"SSLCFG\",0", nullptr, 3000);
    if (r != 2) {
        Serial.printf("[PUT] HTTPPARA SSLCFG failed r=%d\n", r);
        mqttAT("+HTTPTERM", nullptr, 2000);
        return -1;
    }

    // --- 4. Set URL ---
    {
        String urlCmd = "+HTTPPARA=\"URL\",\"https://";
        urlCmd += host;
        urlCmd += path;
        urlCmd += "\"";
        Serial.printf("[PUT] URL %u chars\n", (unsigned)(strlen(host) + strlen(path) + 8));
        r = mqttAT(urlCmd.c_str(), nullptr, 5000);
        if (r != 2) {
            Serial.printf("[PUT] HTTPPARA URL failed r=%d\n", r);
            mqttAT("+HTTPTERM", nullptr, 2000);
            return -1;
        }
    }

    // --- 5. Set Content-Type ---
    {
        String ctCmd = "+HTTPPARA=\"CONTENT\",\"";
        ctCmd += contentType;
        ctCmd += "\"";
        mqttAT(ctCmd.c_str(), nullptr, 3000);
    }

    // --- 6. Send body via HTTPDATA (prompt-mode: DOWNLOAD → data → OK) ---
    if (bodyLen > 0) {
        // 6a. CRITICAL: disable echo before sending binary data.
        //     With ATE1 (default), the modem echoes every byte we write back on
        //     the RX line. JPEG binary data inevitably contains the byte sequence
        //     "\nERROR" or "\n+CME ERROR", which atWaitLine would catch as a
        //     false AT error. ATE0 turns echo off; ATE1 re-enables it below.
        //     Use mqttAT so we get a clean OK handshake before proceeding.
        mqttAT("E0", nullptr, 3000);

        // 6b. Send the HTTPDATA command
        modemSerial.print("AT+HTTPDATA=");
        modemSerial.print(bodyLen);
        modemSerial.print(",30000\r\n");

        // 6c. Wait for "DOWNLOAD" prompt — no trailing newline on this prompt.
        if (!atWaitSubstr("DOWNLOAD", 10000)) {
            Serial.println("[PUT] HTTPDATA: no DOWNLOAD prompt");
            mqttAT("E1", nullptr, 3000);  // restore echo
            mqttAT("+HTTPTERM", nullptr, 2000);
            return -1;
        }
        delay(10);

        // 6d. Stream the body in 1 KB chunks (UART TX buffer friendly).
        //     With echo off, the RX line stays clean — no echoed binary data.
        const size_t CHUNK = 1024;
        for (size_t off = 0; off < bodyLen; ) {
            size_t n = ((bodyLen - off) < CHUNK) ? (bodyLen - off) : CHUNK;
            modemSerial.write(body + off, n);
            modemSerial.flush();
            off += n;
        }

        // 6e. Wait for OK — with echo off the RX line only has the real
        //     modem response. Use substring match (like TinyGSM's waitResponse)
        //     instead of line-based matching which can miss OK in data mode.
        r = atWaitSubstr("OK", 30000) ? 2 : 0;

        // 6f. Restore echo for subsequent AT commands (HTTPACTION, HTTPTERM)
        mqttAT("E1", nullptr, 3000);

        if (r != 2) {
            Serial.printf("[PUT] HTTPDATA: no OK after %u bytes r=%d\n",
                          (unsigned)bodyLen, r);
            mqttAT("+HTTPTERM", nullptr, 2000);
            return -1;
        }
    }

    // --- 7. Execute PUT — async URC: +HTTPACTION: 4,<status>,<datalen> ---
    String urc;
    r = mqttAT("+HTTPACTION=4", "+HTTPACTION:", 60000, &urc);
    if (r != 1) {
        Serial.printf("[PUT] HTTPACTION=4 r=%d\n", r);
        mqttAT("+HTTPTERM", nullptr, 2000);
        return -1;
    }

    int c1 = urc.indexOf(',');
    int c2 = urc.indexOf(',', c1 + 1);
    int sc = (c1 >= 0 && c2 > c1) ? urc.substring(c1 + 1, c2).toInt() : -1;
    Serial.printf("[PUT] %s → status=%d\n", urc.c_str(), sc);

    mqttAT("+HTTPTERM", nullptr, 3000);
    return sc;
}


// Upload one block: PUT /path?<sas>&comp=block&blockid=<urlEnc>
static int putBlock(const char* blobHost, const char* blobPathQ,
                    const char* blockIdUrl,
                    const uint8_t* data, size_t dataLen) {
    String path = String(blobPathQ) + "&comp=block&blockid=" + blockIdUrl;
    Serial.printf("[BLOB] PUT block (path=%u bytes, data=%u bytes)\n",
                  (unsigned)path.length(), (unsigned)dataLen);
    return httpActionPut(blobHost, path.c_str(), "application/octet-stream", data, dataLen);
}

// Commit all blocks: PUT /path?<sas>&comp=blocklist
static int commitBlockList(const char* blobHost, const char* blobPathQ,
                           uint32_t numBlocks) {
    String xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><BlockList>";
    for (uint32_t i = 0; i < numBlocks; i++) {
        char plain[16];
        makeBlockId(i, plain, sizeof(plain), nullptr, 0);
        xml += "<Latest>"; xml += plain; xml += "</Latest>";
    }
    xml += "</BlockList>";
    String path = String(blobPathQ) + "&comp=blocklist";
    Serial.printf("[BLOB] Commit blocklist (%u bytes)\n", (unsigned)xml.length());
    return httpActionPut(blobHost, path.c_str(), "application/xml",
                         (const uint8_t*)xml.c_str(), xml.length());
}


bool uploadFrameBuffer(const uint8_t* data, size_t len, const char* blobName) {
    if (!gprsReady) { Serial.println("[UPLOAD] GPRS not ready"); return false; }

    char sasTok[400];
    if (!generateSASToken(sasTok, sizeof(sasTok), 300)) {
        Serial.println("[UPLOAD] SAS token generation failed");
        return false;
    }

    Serial.println("[UPLOAD] Requesting upload URL from IoT Hub...");
    char corrId[64] = {}, blobHost[128] = {}, blobPathQ[768] = {};
    if (!getBlobSASUri(sasTok, blobName, corrId, blobHost, blobPathQ)) {
        Serial.println("[UPLOAD] IoT Hub did not provide upload URL");
        return false;
    }

    // Upload data in BLOB_CHUNK_SIZE chunks using Put Block
    uint32_t numBlocks = (len + BLOB_CHUNK_SIZE - 1) / BLOB_CHUNK_SIZE;
    Serial.printf("[UPLOAD] Uploading %u bytes in %u blocks\n", (unsigned)len, (unsigned)numBlocks);

    for (uint32_t i = 0; i < numBlocks; i++) {
        char blockIdUrl[32];
        makeBlockId(i, nullptr, 0, blockIdUrl, sizeof(blockIdUrl));
        size_t offset = i * BLOB_CHUNK_SIZE;
        size_t chunkLen = (len - offset < BLOB_CHUNK_SIZE) ? len - offset : BLOB_CHUNK_SIZE;

        Serial.printf("[UPLOAD] Block %u: offset=%u, len=%u\n",
                      (unsigned)i, (unsigned)offset, (unsigned)chunkLen);
        int sc = putBlock(blobHost, blobPathQ, blockIdUrl, data + offset, chunkLen);
        if (sc != 201) {
            Serial.printf("[UPLOAD] Put block %u failed (HTTP %d)\n", (unsigned)i, sc);
            generateSASToken(sasTok, sizeof(sasTok), 300);
            notifyIoTHubComplete(sasTok, corrId, false);
            return false;
        }
    }

    // Commit block list
    Serial.println("[UPLOAD] Committing block list...");
    int sc = commitBlockList(blobHost, blobPathQ, numBlocks);
    if (sc != 201) {
        Serial.printf("[UPLOAD] Commit block list failed (HTTP %d)\n", sc);
        generateSASToken(sasTok, sizeof(sasTok), 300);
        notifyIoTHubComplete(sasTok, corrId, false);
        return false;
    }

    // Notify IoT Hub
    Serial.println("[UPLOAD] Notifying IoT Hub...");
    generateSASToken(sasTok, sizeof(sasTok), 300);
    notifyIoTHubComplete(sasTok, corrId, true);

    // Publish D2C telemetry
    Serial.println("[UPLOAD] Sending D2C telemetry...");
    generateSASToken(sasTok, sizeof(sasTok), 300);
    char telJson[256];
    snprintf(telJson, sizeof(telJson),
             "{\"device\":\"%s\",\"file\":\"%s\",\"success\":true}",
             DEVICE_ID, blobName);
    azure_publish_telemetry(telJson);

    Serial.printf("[UPLOAD] %s uploaded successfully\n", blobName);
    return true;
}

// ═══ GUI-driven upload from SD card ═════════════════════════════════════
void performUpload() {
    uint8_t cl = uploadClientNum;

    if (!gprsReady) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"GPRS not ready\"}");
        return;
    }

    char sasTok[400];
    if (!generateSASToken(sasTok, sizeof(sasTok), 300)) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"SAS token generation failed\"}");
        return;
    }

    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Requesting upload URL from IoT Hub...\"}");
    char corrId[64] = {}, blobHost[128] = {}, blobPathQ[768] = {};
    if (!getBlobSASUri(sasTok, uploadFilename, corrId, blobHost, blobPathQ)) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_error\",\"msg\":\"IoT Hub did not provide upload URL\"}");
        return;
    }

    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Uploading photo to Azure Blob Storage...\"}");
    char sdPath[48];
    snprintf(sdPath, sizeof(sdPath), "/photos/%s", uploadFilename);
    int blobSc = httpsUploadFile(blobHost, blobPathQ, sdPath);
    bool ok = (blobSc == 201);

    webServerSendTXT(cl,
        "{\"type\":\"upload_progress\",\"msg\":\"Notifying IoT Hub of upload result...\"}");
    generateSASToken(sasTok, sizeof(sasTok), 300);
    notifyIoTHubComplete(sasTok, corrId, ok);

    if (ok) {
        webServerSendTXT(cl,
            "{\"type\":\"upload_progress\",\"msg\":\"Sending D2C telemetry...\"}");
        generateSASToken(sasTok, sizeof(sasTok), 300);
        char telJson[256];
        snprintf(telJson, sizeof(telJson),
                 "{\"device\":\"%s\",\"file\":\"%s\",\"success\":true}",
                 DEVICE_ID, uploadFilename);
        azure_publish_telemetry(telJson);

        char done[120];
        snprintf(done, sizeof(done),
                 "{\"type\":\"upload_done\",\"msg\":\"%s uploaded successfully\","
                 "\"file\":\"%s\"}", uploadFilename, uploadFilename);
        webServerSendTXT(cl, done);
    } else {
        char err[96];
        snprintf(err, sizeof(err),
                 "{\"type\":\"upload_error\",\"msg\":\"Blob PUT failed (HTTP %d)\"}",
                 blobSc);
        webServerSendTXT(cl, err);
    }
}
