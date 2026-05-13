#include "camera_manager.h"
#include "sd_card_manager.h"
#include "web_server_manager.h"
#include "SD_MMC.h"

QueueHandle_t    frameQ        = nullptr;
SemaphoreHandle_t camMutex      = nullptr;
framesize_t       pendingSize   = FRAMESIZE_SVGA;
bool              pendingChange = false;
char              curSizeName[8] = "SVGA";
bool              snapRequested = false;
uint8_t           snapClientNum = 0;

framesize_t sizeEnum(const char *s) {
    if (!strcmp(s, "QVGA"))  return FRAMESIZE_QVGA;
    if (!strcmp(s, "VGA"))   return FRAMESIZE_VGA;
    if (!strcmp(s, "XGA"))   return FRAMESIZE_XGA;
    if (!strcmp(s, "HD"))    return FRAMESIZE_HD;
    if (!strcmp(s, "SXGA"))  return FRAMESIZE_SXGA;
    if (!strcmp(s, "UXGA"))  return FRAMESIZE_UXGA;
    if (!strcmp(s, "FHD"))   return FRAMESIZE_FHD;
    if (!strcmp(s, "QXGA"))  return FRAMESIZE_QXGA;
    if (!strcmp(s, "QSXGA")) return FRAMESIZE_QSXGA;
    return FRAMESIZE_SVGA;
}

void doSnapshot(uint8_t clientNum) {
    if (!sdOK) {
        webServerSendTXT(clientNum,
            "{\"type\":\"snapshot_err\",\"msg\":\"SD not available\"}");
        return;
    }
    int idx = nextPhotoIdx();
    if (idx < 0 || idx > 999) {
        webServerSendTXT(clientNum,
            "{\"type\":\"snapshot_err\",\"msg\":\"Photo limit reached\"}");
        return;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        webServerSendTXT(clientNum,
            "{\"type\":\"snapshot_err\",\"msg\":\"Camera capture failed\"}");
        return;
    }
    char path[32];
    snprintf(path, sizeof(path), "/photos/photo_%03d.jpg", idx);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        esp_camera_fb_return(fb);
        webServerSendTXT(clientNum,
            "{\"type\":\"snapshot_err\",\"msg\":\"File create failed\"}");
        return;
    }
    f.write(fb->buf, fb->len);
    f.close();
    esp_camera_fb_return(fb);
    Serial.printf("Snapshot: %s (%u bytes)\n", path, (unsigned)fb->len);
    char resp[80];
    snprintf(resp, sizeof(resp),
             "{\"type\":\"snapshot_ok\",\"file\":\"photo_%03d.jpg\"}", idx);
    webServerSendTXT(clientNum, resp);
}

void camTask(void *) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;) {
        if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(10))) {
            if (pendingChange) {
                framesize_t sz = pendingSize;
                pendingChange  = false;
                xSemaphoreGive(camMutex);
                sensor_t *s = esp_camera_sensor_get();
                if (s) {
                    s->set_framesize(s, sz);
                    for (int i = 0; i < 3; i++) {
                        camera_fb_t *f = esp_camera_fb_get();
                        if (f) esp_camera_fb_return(f);
                    }
                    Serial.printf("Framesize → %s\n", curSizeName);
                }
            } else {
                xSemaphoreGive(camMutex);
            }
        }

        if (uxQueueSpacesAvailable(frameQ) > 0) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                if (xQueueSendToBack(frameQ, &fb, 0) != pdTRUE)
                    esp_camera_fb_return(fb);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vTaskDelay(1);
    }
}

bool initCamera(framesize_t startRes) {
    camera_config_t cam{};
    cam.ledc_channel = LEDC_CHANNEL_0;
    cam.ledc_timer   = LEDC_TIMER_0;
    cam.pin_d0       = CAM_PIN_D0;  cam.pin_d1 = CAM_PIN_D1;
    cam.pin_d2       = CAM_PIN_D2;  cam.pin_d3 = CAM_PIN_D3;
    cam.pin_d4       = CAM_PIN_D4;  cam.pin_d5 = CAM_PIN_D5;
    cam.pin_d6       = CAM_PIN_D6;  cam.pin_d7 = CAM_PIN_D7;
    cam.pin_xclk     = CAM_PIN_XCLK;
    cam.pin_pclk     = CAM_PIN_PCLK;
    cam.pin_vsync    = CAM_PIN_VSYNC;
    cam.pin_href     = CAM_PIN_HREF;
    cam.pin_sccb_sda = CAM_PIN_SIOD;
    cam.pin_sccb_scl = CAM_PIN_SIOC;
    cam.pin_pwdn     = CAM_PIN_PWDN;
    cam.pin_reset    = CAM_PIN_RESET;
    cam.xclk_freq_hz = CAM_XCLK_FREQ;
    cam.pixel_format = PIXFORMAT_JPEG;
    cam.grab_mode    = CAMERA_GRAB_LATEST;

    if (psramFound()) {
        cam.frame_size   = startRes;
        cam.jpeg_quality = 10;
        cam.fb_count     = 4;
        cam.fb_location  = CAMERA_FB_IN_PSRAM;
        Serial.printf("PSRAM: buffers x4, starting at res=%d\n", startRes);
    } else {
        cam.frame_size   = FRAMESIZE_VGA;
        cam.jpeg_quality = 15;
        cam.fb_count     = 1;
        cam.fb_location  = CAMERA_FB_IN_DRAM;
        Serial.println("No PSRAM: VGA, 1 buffer");
    }

    if (esp_camera_init(&cam) != ESP_OK) {
        Serial.println("Camera init failed");
        return false;
    }
    Serial.println("Camera OK");

    frameQ   = xQueueCreate(1, sizeof(camera_fb_t *));
    camMutex = xSemaphoreCreateMutex();

    return true;
}

camera_fb_t* captureFrame() {
    return esp_camera_fb_get();
}
