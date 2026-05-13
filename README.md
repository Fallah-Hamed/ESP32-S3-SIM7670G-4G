# ESP32-S3-SIM7670G-4G Example Firmware

Example firmware for the **Waveshare ESP32-S3-SIM7670G-4G v2.0** board — a
dual-core ESP32-S3 with a SIM7670G 4G LTE Cat.1 modem, OV5640 camera, WS2812B
RGB LED, and SD card slot.

## Configuration

All user-specific settings are in the header files under `src/`.

### WiFi

Edit [`src/wifi_manager.h`](src/wifi_manager.h):

```cpp
#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
```

WiFi is used **only** for the local web GUI — the board connects to your
existing WiFi network and serves the dashboard over HTTP.

### Cellular modem

Edit [`src/cellular_manager.h`](src/cellular_manager.h):

```cpp
#define GPRS_APN "CMHK"
```

Change the APN to match your SIM card provider.

### Azure IoT Hub

Edit [`src/azure_iot_manager.h`](src/azure_iot_manager.h):

```cpp
#define IOT_HUB_HOSTNAME  "your-iothub.azure-devices.net"
#define DEVICE_ID         "your-device-id"
#define DEVICE_KEY        "your-base64-device-key"
```

The device key is the base64-encoded primary key from your Azure IoT Hub device
registration. The firmware generates SAS tokens on-device using mbedTLS
HMAC-SHA256.

All Azure IoT Hub traffic (MQTT telemetry, blob uploads, device twin) goes
through the cellular modem — not WiFi.

## Firmware Features

The firmware is organized into modular components under [`src/`](src/):

| Module | Description |
|--------|-------------|
| `camera_manager` | OV5640 camera init, MJPEG frame capture (FreeRTOS task on Core 0) |
| `wifi_manager` | WiFi STA connection for the local web server |
| `cellular_manager` | SIM7670G modem init, GPRS attach, network info queries |
| `azure_iot_manager` | Cellular Azure IoT Hub: SAS token auth, raw AT+CMQTT\* MQTT, raw AT+HTTP\* blob upload, device twin |
| `web_server_manager` | HTTP server on port 80 and WebSocket server on port 81, with an embedded HTML/JS dashboard |
| `sd_card_manager` | SD card (SDMMC 1-bit) for photo storage and retrieval |
| `rgb_led_manager` | WS2812B RGB LED with mode-based animations |
| `timesync` | System time from cellular network (NITZ) with HTTPS API fallback |

## Web GUI

The built-in web dashboard is served at `http://<esp32-ip>` and communicates
via WebSocket on port 81. No internet connection is needed — the GUI runs
entirely on the local WiFi network.

### Live camera panel
- MJPEG live stream from the OV5640 camera
- Resolution selector (QVGA through QSXGA, 5 MP)
- Snapshot button — saves the current frame to the SD card

### Camera image settings panel
Real-time adjustment of sensor parameters with immediate visual feedback:
- JPEG quality, brightness, contrast, saturation, sharpness
- Auto-exposure level compensation
- Special effects (negative, grayscale, sepia, color tints)
- White balance mode (auto, sunny, cloudy, office, home)
- Gain ceiling (2× through 128×)

### Camera processing panel
Toggle switches for sensor processing features:
- H-Mirror and V-Flip
- Auto White Balance, Auto Gain Control, Auto Exposure Control
- AWB Gain, AEC2 (advanced exposure)
- DCW (downsize), Black/White Pixel Correction
- Raw Gamma correction, Lens Correction

### RGB LED control
- Color picker with brightness slider
- On/Off toggle with instant feedback

### Azure cloud upload panel
- Browse and select photos stored on the SD card
- Upload to Azure Blob Storage via the cellular modem
- Send device-to-cloud telemetry (signal strength, uptime)

### Modem info panel
- Manufacturer, model, firmware revision, IMEI, ICCID
- Network type (LTE/HSDPA/3G/GSM), signal strength (dBm)
- IP address and GPRS connection status
- Refresh button for live updates

## Hardware

- **Board:** Waveshare ESP32-S3-SIM7670G-4G v2.0
- **MCU:** ESP32-S3 dual-core Xtensa LX7 @ 240 MHz
- **Modem:** SIMCom SIM7670G 4G LTE Cat.1
- **Camera:** OV5640 (24-pin FPC, 5 MP)
- **Storage:** microSD slot (SDMMC 1-bit)
- **LED:** WS2812B addressable RGB (GPIO38)

## How It Works

### Connectivity

```
WiFi (STA) ──► Local web server (HTTP :80 + WebSocket :81)
                 └── MJPEG stream, camera controls, GUI

Cellular ────► Azure IoT Hub
  (SIM7670G)    ├── MQTT D2C telemetry (raw AT+CMQTT*)
                └── Blob upload (raw AT+HTTP* PUT)
```

WiFi is used exclusively for the local dashboard. All cloud communication
goes through the cellular modem — the ESP32's WiFi never touches Azure.

## License

MIT — use freely for your own projects.
