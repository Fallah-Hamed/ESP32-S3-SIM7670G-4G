#include "sd_card_manager.h"
#include "SD_MMC.h"

bool sdOK = false;

void initSDCard() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_DAT0_PIN);
    if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_HIGHSPEED)) {
        sdOK = true;
        if (!SD_MMC.exists("/photos")) SD_MMC.mkdir("/photos");
        Serial.printf("SD OK (1-bit 40MHz, %.0f MB)\n", SD_MMC.totalBytes() / 1048576.0);
    } else {
        Serial.println("SD not found");
    }
}

int nextPhotoIdx() {
    if (!sdOK) return -1;
    if (!SD_MMC.exists("/photos")) SD_MMC.mkdir("/photos");
    int mx = -1;
    File d = SD_MMC.open("/photos");
    if (!d) return 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (f.isDirectory()) continue;
        String n = String(f.name());
        int sl = n.lastIndexOf('/');
        if (sl >= 0) n = n.substring(sl + 1);
        int us = n.lastIndexOf('_'), dot = n.lastIndexOf('.');
        if (us >= 0 && dot > us) {
            int v = n.substring(us + 1, dot).toInt();
            if (v > mx) mx = v;
        }
    }
    d.close();
    return mx + 1;
}

int photoCount() {
    if (!sdOK || !SD_MMC.exists("/photos")) return 0;
    File d = SD_MMC.open("/photos");
    if (!d) return 0;
    int n = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile())
        if (!f.isDirectory()) n++;
    d.close();
    return n;
}

bool ensureDir(const char* path) {
    if (!sdOK) return false;
    if (SD_MMC.exists(path)) return true;
    return SD_MMC.mkdir(path);
}

bool saveFile(const char* path, const uint8_t* buf, size_t len) {
    if (!sdOK) return false;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write(buf, len);
    f.close();
    return written == len;
}
