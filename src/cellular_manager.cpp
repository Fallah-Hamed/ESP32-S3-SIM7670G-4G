#include "cellular_manager.h"

HardwareSerial    modemSerial(1);
TinyGsm           modem(modemSerial);
SemaphoreHandle_t modemMutex  = nullptr;
ModemInfo         modemInfo   = {};
volatile bool     modemReady  = false;
volatile bool     gprsReady   = false;

static bool modemWaitAT(uint32_t ms = 15000) {
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        if (modem.testAT(500)) return true;
    }
    return false;
}

static void modemInit() {
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, HIGH);

    modemSerial.begin(MODEM_INIT_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    Serial.println("[MDM] Starting SIM7670G...");

    if (!modem.testAT(2000)) {
        pinMode(BOARD_PWRKEY_PIN, OUTPUT);
        digitalWrite(BOARD_PWRKEY_PIN, HIGH);
        delay(100);
        digitalWrite(BOARD_PWRKEY_PIN, LOW);
        delay(1200);
        digitalWrite(BOARD_PWRKEY_PIN, HIGH);
        Serial.println("[MDM] Power-on pulse sent, waiting for boot (~15 s)...");
        if (!modemWaitAT(30000)) {
            Serial.println("[MDM] ERROR: modem did not respond after power-on");
            return;
        }
    }

    modem.init();

    modem.sendAT(GF("I"));
    String prodInfo;
    modem.waitResponse(5000, prodInfo);

    // Parse ATI response: Manufacturer, Model, Revision, IMEI
    String mfr, mdl, fw, imei;
    int manuIdx = prodInfo.indexOf("Manufacturer:");
    int modelIdx = prodInfo.indexOf("Model:");
    int revIdx = prodInfo.indexOf("Revision:");
    int imeiIdx = prodInfo.indexOf("IMEI:");
    if (manuIdx >= 0 && modelIdx > manuIdx)
        mfr = prodInfo.substring(manuIdx + 13, modelIdx);
    if (modelIdx >= 0 && revIdx > modelIdx)
        mdl = prodInfo.substring(modelIdx + 6, revIdx);
    if (revIdx >= 0 && imeiIdx > revIdx)
        fw = prodInfo.substring(revIdx + 9, imeiIdx);
    if (imeiIdx >= 0) {
        int okIdx = prodInfo.indexOf("OK", imeiIdx);
        imei = prodInfo.substring(imeiIdx + 5, (okIdx > imeiIdx ? okIdx : prodInfo.length()));
    }
    mfr.trim(); mdl.trim(); fw.trim(); imei.trim();

    String ccid = modem.getSimCCID();

    int yr, mo, dy, hr, mn, sc; float tz;
    if (modem.getNetworkTime(&yr, &mo, &dy, &hr, &mn, &sc, &tz)) {
        struct tm t = {};
        t.tm_year = yr - 1900; t.tm_mon = mo - 1; t.tm_mday = dy;
        t.tm_hour = hr;        t.tm_min  = mn;     t.tm_sec  = sc;
        // AT+CCLK? on SIM7670G returns UTC — mktime() treats struct as UTC since
        // ESP32's system TZ is UTC; do NOT subtract tz offset (that would go behind UTC).
        time_t utc = mktime(&t);
        struct timeval tv = { .tv_sec = utc };
        settimeofday(&tv, NULL);
        Serial.printf("[MDM] Network time (UTC): %04d-%02d-%02d %02d:%02d:%02d (local tz=%.1f h)\n",
                      yr, mo, dy, hr, mn, sc, tz);
    }

    if (xSemaphoreTake(modemMutex, pdMS_TO_TICKS(1000))) {
        mfr .toCharArray(modemInfo.manufacturer, sizeof(modemInfo.manufacturer));
        mdl .toCharArray(modemInfo.model,        sizeof(modemInfo.model));
        fw  .toCharArray(modemInfo.firmware,     sizeof(modemInfo.firmware));
        imei.toCharArray(modemInfo.imei,         sizeof(modemInfo.imei));
        ccid.toCharArray(modemInfo.iccid,        sizeof(modemInfo.iccid));
        modemReady = true;
        xSemaphoreGive(modemMutex);
    }
    Serial.printf("[MDM] Ready — %s %s  IMEI:%s\n",
                  modemInfo.manufacturer, modemInfo.model, modemInfo.imei);

    // Switch to high-speed baud rate for faster uploads
    if (MODEM_BAUDRATE != MODEM_INIT_BAUDRATE) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "+IPR=%d", MODEM_BAUDRATE);
        modem.sendAT(cmd);
        if (modem.waitResponse(3000) == 1) {
            delay(50);
            modemSerial.updateBaudRate(MODEM_BAUDRATE);
            Serial.printf("[MDM] Baud switched to %d\n", MODEM_BAUDRATE);
            if (!modem.testAT(2000)) {
                Serial.println("[MDM] WARNING: no response at high baud — staying at 115200");
                modemSerial.updateBaudRate(MODEM_INIT_BAUDRATE);
            }
        } else {
            Serial.printf("[MDM] AT+IPR=%d failed — staying at %d\n",
                         MODEM_BAUDRATE, MODEM_INIT_BAUDRATE);
        }
    }
}

bool modemConnectGPRS() {
    if (!modemReady) return false;

    // Already connected check — use IP address instead of isGprsConnected()
    // because SIM7672 uses AT+NETOPEN which doesn't set the old GPRS flag.
    if (gprsReady) {
        String ip = modem.getLocalIP();
        if (ip.length() > 0 && ip != "0.0.0.0") {
            int rssi = modem.getSignalQuality();
            modemInfo.rssi_dbm = (rssi == 99 || rssi == 0) ? 0 : (-113 + 2 * rssi);
            ip.toCharArray(modemInfo.ip, sizeof(modemInfo.ip));
            return true;
        }
        gprsReady = false;  // IP lost — reconnect
    }

    Serial.printf("[MDM] Connecting GPRS, APN=%s...\n", GPRS_APN);

    // Wait for network registration
    RegStatus rs = REG_UNREGISTERED;
    unsigned long t0 = millis();
    while (millis() - t0 < 60000) {
        rs = modem.getRegistrationStatus();
        if (rs == REG_OK_HOME || rs == REG_OK_ROAMING) break;
        delay(1000); Serial.print(".");
    }
    Serial.println();
    if (rs != REG_OK_HOME && rs != REG_OK_ROAMING) {
        Serial.println("[MDM] Network registration failed"); return false;
    }

    // Detect network type
    String cops;
    modem.sendAT("+COPS?");
    modem.waitResponse(3000, cops);
    {
        int lc  = cops.lastIndexOf(',');
        int act = (lc > 0) ? cops.substring(lc + 1).toInt() : -1;
        const char* nt = (act == 7) ? "LTE Cat.1" :
                         (act == 6) ? "HSDPA" :
                         (act == 2) ? "UTRAN/3G" :
                         (act == 0) ? "GSM" : "Unknown";
        if (xSemaphoreTake(modemMutex, pdMS_TO_TICKS(100))) {
            snprintf(modemInfo.network, sizeof(modemInfo.network), "%s", nt);
            xSemaphoreGive(modemMutex);
        }
    }

    // SIM7672 network activation: AT+CGDCONT + AT+NETOPEN
    if (!modem.setNetworkAPN(GPRS_APN)) {
        Serial.println("[MDM] setNetworkAPN failed"); return false;
    }
    if (!modem.setNetworkActive(GPRS_APN, false)) {
        Serial.println("[MDM] setNetworkActive failed"); return false;
    }

    // Give the network stack time to obtain an IP
    delay(5000);

    String ip = modem.getLocalIP();
    if (ip.length() == 0 || ip == "0.0.0.0") {
        Serial.println("[MDM] No IP after NETOPEN"); return false;
    }

    gprsReady = true;
    int rssi = modem.getSignalQuality();
    modemInfo.rssi_dbm = (rssi == 99 || rssi == 0) ? 0 : (-113 + 2 * rssi);
    ip.toCharArray(modemInfo.ip, sizeof(modemInfo.ip));
    Serial.printf("[MDM] GPRS OK — IP:%s  RSSI:%d dBm\n",
                  modemInfo.ip, modemInfo.rssi_dbm);
    return true;
}

void modemInitTask(void *) {
    modemMutex = xSemaphoreCreateMutex();
    modemInit();
    vTaskDelete(nullptr);
}
