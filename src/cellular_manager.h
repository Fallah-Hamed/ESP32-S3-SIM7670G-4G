#pragma once

#ifndef TINY_GSM_MODEM_SIM7670G
#define TINY_GSM_MODEM_SIM7670G
#endif
#ifndef TINY_GSM_RX_BUFFER
#define TINY_GSM_RX_BUFFER 4096
#endif

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// SIM7670G Modem — UART1
#define MODEM_BAUDRATE      (460800)
#define MODEM_INIT_BAUDRATE  (115200)
#define MODEM_DTR_PIN       (45)
#define MODEM_TX_PIN        (18)   // ESP32-S3 TX  →  SIM7670G RXD
#define MODEM_RX_PIN        (17)   // SIM7670G TXD →  ESP32-S3 RX
#define BOARD_PWRKEY_PIN    (33)
#define MODEM_RING_PIN      (40)
#define MODEM_RESET_PIN     (-1)
#define MODEM_RESET_LEVEL   LOW
#define GPRS_APN            "CMHK"

struct ModemInfo {
    char manufacturer[40];
    char model[40];
    char firmware[64];
    char imei[24];
    char iccid[24];
    char network[48];
    int  rssi_dbm;
    char ip[20];
};

extern HardwareSerial    modemSerial;
extern TinyGsm           modem;
extern SemaphoreHandle_t modemMutex;
extern ModemInfo         modemInfo;
extern volatile bool     modemReady;
extern volatile bool     gprsReady;

void modemInitTask(void *);
bool modemConnectGPRS();
