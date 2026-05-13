#pragma once

#include <Arduino.h>

// MicroSD card — SDMMC 1-bit mode
#define SD_CLK_PIN   5
#define SD_CMD_PIN   4
#define SD_DAT0_PIN  6

extern bool sdOK;

void initSDCard();
int nextPhotoIdx();
int photoCount();
bool ensureDir(const char* path);
bool saveFile(const char* path, const uint8_t* buf, size_t len);
