#pragma once

#include <Adafruit_NeoPixel.h>

// Onboard WS2812B RGB LED (single pixel)
#define LED_PIN 38

enum LedMode {
    LED_BOOTING,
    LED_WIFI_CONNECTING,
    LED_MQTT_CONNECTING,
    LED_CONNECTED,
    LED_CAPTURING,
    LED_UPLOADING,
    LED_ERROR
};

extern Adafruit_NeoPixel pixel;

void initLED();
void setLedMode(LedMode mode);
void ledUpdate();
