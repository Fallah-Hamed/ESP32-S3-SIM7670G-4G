#pragma once

#include <WiFi.h>

#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

void initWiFi();
bool isWiFiConnected();
