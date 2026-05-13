#include "rgb_led_manager.h"
#include <Arduino.h>
#include <math.h>

Adafruit_NeoPixel pixel(1, LED_PIN, NEO_RGB + NEO_KHZ800);

static LedMode      currentMode = LED_BOOTING;
static unsigned long modeStart   = 0;
static int          animPhase    = 0;
static unsigned long lastUpdate  = 0;

void initLED() {
    pixel.begin();
    pixel.setPixelColor(0, pixel.Color(0, 0, 32));
    pixel.show();
    modeStart = millis();
}

void setLedMode(LedMode mode) {
    if (currentMode != mode) {
        currentMode = mode;
        modeStart   = millis();
        animPhase   = 0;
    }
}

void ledUpdate() {
    unsigned long now = millis();
    if (now - lastUpdate < 30) return;
    lastUpdate = now;

    switch (currentMode) {
    case LED_BOOTING:
        pixel.setPixelColor(0, pixel.Color(0, 0, 32));
        break;

    case LED_WIFI_CONNECTING:
        if ((now / 500) % 2 == 0)
            pixel.setPixelColor(0, pixel.Color(32, 24, 0));
        else
            pixel.setPixelColor(0, 0);
        break;

    case LED_MQTT_CONNECTING:
        pixel.setPixelColor(0, pixel.Color(0, 32, 32));
        break;

    case LED_CONNECTED: {
        animPhase = (animPhase + 1) % 256;
        float s = sinf(animPhase * 2.0f * 3.14159f / 256.0f);
        uint8_t g = (uint8_t)((s + 1.0f) * 12.0f);
        pixel.setPixelColor(0, pixel.Color(0, g, 0));
        break;
    }

    case LED_CAPTURING:
        pixel.setPixelColor(0, pixel.Color(64, 64, 64));
        if (now - modeStart > 80)
            setLedMode(LED_CONNECTED);
        break;

    case LED_UPLOADING:
        pixel.setPixelColor(0, pixel.Color(64, 0, 64));
        break;

    case LED_ERROR:
        if ((now / 200) % 2 == 0)
            pixel.setPixelColor(0, pixel.Color(64, 0, 0));
        else
            pixel.setPixelColor(0, 0);
        break;
    }
    pixel.show();
}
