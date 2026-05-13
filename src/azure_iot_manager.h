#pragma once

#include <Arduino.h>

#define IOT_HUB_HOSTNAME  "your-iothub.azure-devices.net"
#define DEVICE_ID         "your-device-id"
#define DEVICE_KEY        "your-base64-device-key"
#define BLOB_DIRECTORY    "Photos"

#define MQTT_PORT          8883
#define SAS_TOKEN_VALID_S  604800   // 1 week

// ─── Upload state (GUI-driven) ──────────────────────────────────────────────
enum UploadState : uint8_t { UPLOAD_IDLE, UPLOAD_REQUESTED, UPLOAD_IN_PROGRESS };

extern volatile UploadState uploadState;
extern char                 uploadFilename[32];
extern uint8_t              uploadClientNum;

extern volatile bool telemetryRequested;
extern uint8_t      telemetryClientNum;

// ─── Device twin state ──────────────────────────────────────────────────────
extern bool twinReceived;
extern char twinResolution[8];
extern long twinPeriod;
extern char twinSdWrite[4];

// SAS token generation (mbedTLS HMAC-SHA256)
bool generateSASToken(char* out, size_t outSize, uint32_t durationSecs);

// Cellular MQTT (raw AT+CMQTT*)
bool azure_mqtt_init();
bool azure_mqtt_connect();
bool azure_publish_telemetry(const char* payload);
void azure_handle();

// Device twin (via MQTT)
bool requestDeviceTwin();
bool checkTwinUpdate(char* resolution, size_t resSize, long* period);
bool parseInitialTwin(char* resolution, size_t resSize, long* period);
bool reportDeviceTwin(const char* resolution, long period, int temp, const char* sdWrite);

// Blob upload — from frame buffer (headless) or from SD card (GUI)
bool uploadFrameBuffer(const uint8_t* data, size_t len, const char* blobName);
void performUpload();
