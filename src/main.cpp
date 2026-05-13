#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>

#include "camera_manager.h"
#include "wifi_manager.h"
#include "sd_card_manager.h"
#include "rgb_led_manager.h"
#include "web_server_manager.h"
#include "cellular_manager.h"
#include "azure_iot_manager.h"
#include "timesync.h"

static char camRes[8] = "SVGA";

void setup() {
    Serial.begin(115200);

    // 1. LED boot indicator
    initLED();
    setLedMode(LED_BOOTING);
    ledUpdate();

    // 2. SD card
    initSDCard();

    // 3. WiFi — local web server only (no NTP, no Azure)
    setLedMode(LED_WIFI_CONNECTING);
    initWiFi();

    // 4. Modem init on Core 1 (runs concurrently)
    xTaskCreatePinnedToCore(modemInitTask, "mdm_init", 8192, nullptr, 1, nullptr, 1);

    // Wait for modem to be ready
    unsigned long t0 = millis();
    while (!modemReady && millis() - t0 < 30000) {
        ledUpdate(); delay(50);
    }
    if (!modemReady) {
        Serial.println("[SETUP] Modem not ready — halt");
        for (;;) { setLedMode(LED_ERROR); ledUpdate(); delay(200); }
    }

    // 5. GPRS attach — cellular internet
    setLedMode(LED_MQTT_CONNECTING);
    ledUpdate();
    if (!modemConnectGPRS()) {
        Serial.println("[SETUP] GPRS failed — halt");
        for (;;) { setLedMode(LED_ERROR); ledUpdate(); delay(200); }
    }

    // 6. Time sync via cellular (network time + HTTPS fallback)
    timesync_init();
    // Set Hong Kong timezone for local time display
    setenv("TZ", "CST-8", 1); tzset();

    // 7. Azure MQTT over cellular (raw AT+CMQTT*)
    if (!azure_mqtt_init()) {
        Serial.println("[SETUP] MQTT init failed — will retry on demand");
    } else {
        Serial.println("[SETUP] MQTT connected");
    }

    // 8. Request device twin, wait for response
    if (requestDeviceTwin()) {
        t0 = millis();
        while (!twinReceived && millis() - t0 < 10000) {
            azure_handle();
            ledUpdate();
            delay(50);
        }
    }

    // Parse initial twin GET response for resolution
    char tmpRes[8] = {};
    long tmpPeriod = 0;
    if (parseInitialTwin(tmpRes, sizeof(tmpRes), &tmpPeriod)) {
        if (strlen(tmpRes) > 0) strncpy(camRes, tmpRes, sizeof(camRes) - 1);
        if (tmpPeriod > 0)      twinPeriod = tmpPeriod;
    }
    Serial.printf("[SETUP] Resolution=%s  Period=%lds\n", camRes, twinPeriod);

    // 9. Camera with twin-configured resolution
    framesize_t fs = sizeEnum(camRes);
    if (!initCamera(fs)) {
        Serial.println("[SETUP] Camera init FAILED — halt");
        for (;;) { setLedMode(LED_ERROR); ledUpdate(); delay(200); }
    }

    // 10. Web server (HTTP:80 + WS:81) for local GUI
    initWebServer();

    // 11. Camera capture task on Core 0 (feeds MJPEG stream)
    xTaskCreatePinnedToCore(camTask, "cam", 4096, nullptr, 1, nullptr, 0);

    // 12. Report initial twin state
    reportDeviceTwin(camRes, twinPeriod, 0, twinSdWrite);

    setLedMode(LED_CONNECTED);
    Serial.print("[SETUP] Ready — Web GUI at http://");
    Serial.println(WiFi.localIP());
}

void loop() {
    ledUpdate();

    // Deferred snapshot — triggered by WebSocket "snapshot" command
    if (snapRequested) {
        snapRequested = false;
        camera_fb_t *qfb = nullptr;
        while (xQueueReceive(frameQ, &qfb, 0) == pdTRUE && qfb) {
            esp_camera_fb_return(qfb);
            qfb = nullptr;
        }
        doSnapshot(snapClientNum);
    }

    // Blocking Azure upload via cellular — stream pauses, resumes when done
    if (uploadState == UPLOAD_REQUESTED) {
        uploadState = UPLOAD_IN_PROGRESS;
        performUpload();
        uploadState = UPLOAD_IDLE;
        return;
    }

    // Telemetry send via cellular MQTT
    if (telemetryRequested) {
        telemetryRequested = false;
        uint8_t tc = telemetryClientNum;
        Serial.println("[TEL] Telemetry requested by client");

        if (!gprsReady) {
            Serial.println("[TEL] Cellular not connected");
            webServerSendTXT(tc,
                "{\"type\":\"upload_error\",\"msg\":\"Cellular not connected\"}");
        } else {
            webServerSendTXT(tc,
                "{\"type\":\"upload_progress\",\"msg\":\"Sending telemetry via cellular...\"}");

            // Read RSSI under mutex, release immediately
            int rssi = 0;
            if (xSemaphoreTake(modemMutex, pdMS_TO_TICKS(100))) {
                rssi = modemInfo.rssi_dbm;
                xSemaphoreGive(modemMutex);
            }

            char telJson[256];
            snprintf(telJson, sizeof(telJson),
                     "{\"device\":\"%s\",\"signal_dbm\":%d,\"uptime_ms\":%lu}",
                     DEVICE_ID, rssi, (unsigned long)millis());

            Serial.printf("[TEL] Sending: %s\n", telJson);
            if (azure_publish_telemetry(telJson)) {
                Serial.println("[TEL] OK");
                webServerSendTXT(tc,
                    "{\"type\":\"upload_done\",\"msg\":\"Telemetry sent via cellular\"}");
            } else {
                Serial.println("[TEL] MQTT publish failed");
                webServerSendTXT(tc,
                    "{\"type\":\"upload_error\",\"msg\":\"Telemetry send failed\"}");
            }
        }
        return;
    }

    // Process MQTT URCs (twin PATCH, C2D messages)
    azure_handle();

    // Check for device twin desired property updates
    char newRes[8] = {};
    long newPeriod = 0;
    if (checkTwinUpdate(newRes, sizeof(newRes), &newPeriod)) {
        if (strlen(newRes) > 0 && strcmp(newRes, camRes) != 0) {
            strncpy(camRes, newRes, sizeof(camRes) - 1);
            Serial.printf("[LOOP] Twin resolution -> %s\n", camRes);
            if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(1000))) {
                framesize_t fs = sizeEnum(camRes);
                pendingSize = fs;
                pendingChange = true;
                strncpy(curSizeName, camRes, sizeof(curSizeName) - 1);
                xSemaphoreGive(camMutex);
            }
        }
        if (newPeriod > 0 && newPeriod != twinPeriod) {
            twinPeriod = newPeriod;
            Serial.printf("[LOOP] Twin period -> %ld\n", twinPeriod);
        }
    }

    // Retry time sync if needed
    timesync_check_resync();

    handleClients();
    delay(1);
}
