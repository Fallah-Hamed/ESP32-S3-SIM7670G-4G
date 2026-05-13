#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// Camera pin definitions (24-pin FPC connector)
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    39
#define CAM_PIN_SIOD    15  // I2C SDA
#define CAM_PIN_SIOC    16  // I2C SCL
#define CAM_PIN_D0       7  // Y2
#define CAM_PIN_D1       8  // Y3
#define CAM_PIN_D2       9  // Y4
#define CAM_PIN_D3      10  // Y5
#define CAM_PIN_D4      11  // Y6
#define CAM_PIN_D5      12  // Y7
#define CAM_PIN_D6      13  // Y8
#define CAM_PIN_D7      14  // Y9
#define CAM_PIN_VSYNC   42
#define CAM_PIN_HREF    41
#define CAM_PIN_PCLK    46

#define CAM_XCLK_FREQ   20000000

extern QueueHandle_t    frameQ;
extern SemaphoreHandle_t camMutex;
extern framesize_t       pendingSize;
extern bool              pendingChange;
extern char              curSizeName[8];
extern bool              snapRequested;
extern uint8_t           snapClientNum;

bool       initCamera(framesize_t startRes);
framesize_t sizeEnum(const char *s);
void       doSnapshot(uint8_t clientNum);
camera_fb_t* captureFrame();
void         camTask(void *);
