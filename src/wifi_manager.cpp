#include "wifi_manager.h"
#include "rgb_led_manager.h"

static bool wifiConnected = false;

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    setLedMode(LED_WIFI_CONNECTING);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
        ledUpdate();
        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.print("[WIFI] Connected — IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WIFI] Connection FAILED");
    }
}

bool isWiFiConnected() {
    return wifiConnected && WiFi.status() == WL_CONNECTED;
}
