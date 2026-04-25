# WiFi Tank

ESP-IDF project for a WiFi-controlled tank with live MJPEG video streaming and a real-time WebSocket overlay system. Supports two hardware targets:

- **AI-Thinker ESP32-CAM** (`esp32`) — original build, OV3660 sensor, 4 MB flash
- **ESP32-S3-CAM** (`esp32s3`) — S3 WROOM variant, 8 MB flash, octal PSRAM

## Environment Setup

### Prerequisites

Install ESP-IDF **v5.5.1** by following the [official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/get-started/index.html). The quick path on Linux/macOS:

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.5.1
./install.sh esp32
. ./export.sh          # add this line to ~/.bashrc / ~/.zshrc for convenience
```

### Clone and initialise submodules

```bash
git clone <repo-url> Tank
cd Tank
git submodule update --init --recursive   # pulls the Trice logging library
```

### Install managed components

```bash
cd wifi_Tank
idf.py set-target esp32       # or esp32s3 for the S3-CAM build
idf.py reconfigure            # downloads espressif__esp32-camera and espressif__esp_jpeg
```

## Build and Flash

Both targets share the same source tree. Use separate build directories to keep both artifacts without reconfiguring.

```bash
cd wifi_Tank

# --- AI-Thinker ESP32-CAM ---
idf.py -B build_esp32 set-target esp32 build
idf.py -B build_esp32 -p /dev/ttyUSB0 flash monitor

# --- ESP32-S3-CAM ---
idf.py -B build_esp32s3 set-target esp32s3 build
idf.py -B build_esp32s3 -p /dev/ttyUSB0 flash monitor
```

Target-specific defaults (flash size, PSRAM mode) are picked up automatically from `sdkconfig.defaults.<target>` during `set-target`.

The serial monitor shows startup logs including the obtained IP address. Press `Ctrl+]` to exit the monitor.

Once connected to WiFi, the device advertises itself via **mDNS** as `tank.local`. No IP lookup needed — just open:

```
http://tank.local
```

Works on Linux (requires `avahi-daemon`, running by default on most distros), Windows 10+, and Android (Chrome/Firefox resolve `.local` natively).

The IP address is still printed to the serial monitor on connect as a fallback.

## WiFi Provisioning

On **first boot** (no credentials stored) the device starts a SoftAP named **Tank-Setup** (open, no password).

1. Connect your phone or laptop to `Tank-Setup`.
2. Open `http://192.168.4.1` in a browser — the provisioning page loads.
3. Enter an SSID and password, then submit.
4. The device saves the credentials to NVS flash, reboots, and connects to your network.

Two default networks (`Namai`, `#Telia-BCBEFE`) are seeded into NVS on the very first boot and tried automatically if present. Provisioning via the web UI adds further networks to the list. Stored credentials survive reflashing (NVS partition is not erased by `idf.py flash`). To clear them:

```bash
idf.py -p /dev/ttyUSB0 erase-flash   # wipes everything including NVS
# or call ProvisioningClearCredentials() from code and reboot
```

## OTA Firmware Update

Once the device is on WiFi, all future firmware updates can be done over the network — no USB cable required.

> **First flash after enabling OTA partitions**: If you have just switched from the old single `factory` partition layout to the new dual OTA layout (i.e. you changed `partitions.csv`), you must flash once via USB to write the new partition table. After that, all further updates go OTA.
>
> ```bash
> idf.py -p /dev/ttyUSB0 flash
> ```

### Step-by-step OTA upload

**1. Build the new firmware**

```bash
idf.py build
# Produces build/wifi_Tank.bin (~1.1 MB)
```

**2. (Optional) Check what version is currently running**

```bash
curl http://tank.local/version
# {"version":"1.0","project":"wifi_Tank","date":"Feb 20 2026","time":"16:00:00","partition":"ota_0"}
```

**3. Upload the binary**

```bash
curl -X POST http://tank.local/ota \
     --data-binary @build/wifi_Tank.bin
```

Progress is logged to the serial monitor in 64 KB increments. A successful upload returns:

```json
{"status":"ok","message":"OTA complete, rebooting"}
```

The device reboots automatically ~500 ms after the response is sent.

**4. Verify the update**

Wait a few seconds for the reboot, then confirm the new firmware is running:

```bash
curl http://tank.local/version
# partition should now show "ota_1" (or back to "ota_0" on the next update)
```

### How it works

- Flash has two equal app partitions: `ota_0` (offset `0x20000`) and `ota_1` (offset `0x210000`), each 1984 KB.
- The `otadata` partition (8 KB at `0x10000`) records which slot to boot.
- On each successful OTA the device writes to the **inactive** slot and marks it as the new boot target. Slots alternate: `ota_0 → ota_1 → ota_0 → …`
- WiFi credentials (NVS partition) are in a completely separate region and are **never touched** by an OTA update.
- There is no authentication on the `/ota` endpoint — use only on a trusted network.

## Capabilities

### Network Services

| Port | Protocol | URL | Description |
|------|----------|-----|-------------|
| 80   | HTTP     | `http://tank.local` | Main web server, OTA |
| 80   | WebSocket | `ws://tank.local/ws` | Overlay data |
| 80   | WebSocket | `ws://tank.local/ctrl` | Motor control |
| 81   | HTTP     | `http://tank.local:81` | MJPEG video stream |
| 8080 | TCP (raw) | `tank.local:8080` | System control channel |

### Video Stream — port 81

Open the stream in any MJPEG-capable client (browser, VLC, ffplay):

```
http://tank.local:81/stream
```

- Format: MJPEG multipart HTTP stream
- Resolution: 1280×720 (HD)
- JPEG quality: 12 (higher = lower quality; range 0–63)
- Up to 13 concurrent clients
- FPS is tracked and exposed via the overlay WebSocket

A plain info page is served at `http://tank.local:81/`.

### Overlay WebSocket — port 81

Connect to `ws://tank.local:81/ws` to receive real-time overlay data as JSON. The server pushes an update every 2 seconds containing:

- **Text elements** — up to 10 items, each with position, colour, size, and content (e.g. current FPS)
- **Shape elements** — up to 20 shapes: lines, rectangles, circles, triangles

See `overlay_demo.html` in the project root for a ready-made client that renders the overlay on top of the video stream.

Up to 8 WebSocket clients can be connected simultaneously.

### Main HTTP Server — port 80

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Placeholder ("hello world") |
| `GET` | `/version` | Firmware version and running OTA partition as JSON |
| `POST` | `/ota` | Upload a new firmware binary; reboots on success |

### TCP System Server — port 8080

Raw TCP socket server for system-level commands and monitoring. Supports multiple concurrent clients with keep-alive (5 s idle, 3 retries).

### Throughput Monitoring

The firmware logs application-level RX/TX throughput to the serial monitor once per second (only when there is activity):

```
Throughput - RX: 450 kbps (0.45 Mbps) | TX: 3200 kbps (3.20 Mbps) | Total: RX 0.12 MB / TX 0.85 MB
```

## Hardware

Two boards are supported. The active target is selected at build time via `idf.py set-target`.

---

### AI-Thinker ESP32-CAM (`esp32`)

**MCU**: ESP32 · **Camera**: OV3660 · **Flash**: 4 MB quad PSRAM

#### Camera (OV3660 — DVP parallel interface)

| Signal | GPIO | Notes |
|--------|------|-------|
| PWDN | 32 | Power-down, active-high |
| RESET | — | Software reset (not wired) |
| XCLK | 0 | 20 MHz master clock |
| SIOD | 26 | SCCB / I²C data |
| SIOC | 27 | SCCB / I²C clock |
| D0 | 5 | Pixel data bit 0 |
| D1 | 18 | Pixel data bit 1 |
| D2 | 19 | Pixel data bit 2 |
| D3 | 21 | Pixel data bit 3 |
| D4 | 36 | Pixel data bit 4 |
| D5 | 39 | Pixel data bit 5 |
| D6 | 34 | Pixel data bit 6 |
| D7 | 35 | Pixel data bit 7 |
| VSYNC | 25 | Frame sync |
| HREF | 23 | Line sync |
| PCLK | 22 | Pixel clock |

#### Motor driver (BTS7960B — two channels, H-bridge)

LEDC timer 1, channels 1–4, 20 kHz PWM. EN pins tied to 5 V externally.

| Signal | GPIO | Motor |
|--------|------|-------|
| RPWM (forward) | 13 | Left track |
| LPWM (reverse) | 14 | Left track |
| RPWM (forward) | 15 | Right track |
| LPWM (reverse) | 12 | Right track |

#### Reserved / occupied pins summary

Pins in use: 0, 5, 12, 13, 14, 15, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39.

---

### ESP32-S3-CAM (`esp32s3`)

**MCU**: ESP32-S3 · **Camera**: OV3660 (or compatible) · **Flash**: 8 MB octal PSRAM

#### Camera (DVP parallel interface via LCD_CAM peripheral)

| Signal | GPIO | Notes |
|--------|------|-------|
| PWDN | 38 | Power-down, active-high |
| RESET | — | Software reset (not wired) |
| XCLK | 15 | 20 MHz master clock |
| SIOD | 4 | SCCB / I²C data |
| SIOC | 5 | SCCB / I²C clock |
| D0 | 11 | Pixel data bit 0 |
| D1 | 9 | Pixel data bit 1 |
| D2 | 8 | Pixel data bit 2 |
| D3 | 10 | Pixel data bit 3 |
| D4 | 12 | Pixel data bit 4 |
| D5 | 18 | Pixel data bit 5 |
| D6 | 17 | Pixel data bit 6 |
| D7 | 16 | Pixel data bit 7 |
| VSYNC | 6 | Frame sync |
| HREF | 7 | Line sync |
| PCLK | 13 | Pixel clock |

#### On-board LEDs (board hardware, not driven by firmware)

| GPIO | Type | Purpose |
|------|------|---------|
| 2 | Single LED | Blue status indicator |
| 48 | WS2812B | RGB flash LED |

#### SD card (on-board slot)

| GPIO | Signal |
|------|--------|
| 39 | CLK |
| 40 | CMD |

#### Motor driver (BTS7960B — two channels, H-bridge)

LEDC timer 1, channels 1–4, 20 kHz PWM. EN pins tied to 5 V externally.
Pins chosen as the cleanest available after camera (4–18, 38), LEDs (2, 48), SD (39–40), USB (19–20), UART0 (43–44), and strapping pins (0, 45, 46) are excluded.

| Signal | GPIO | Motor |
|--------|------|-------|
| RPWM (forward) | 14 | Left track |
| LPWM (reverse) | 21 | Left track |
| RPWM (forward) | 41 | Right track |
| LPWM (reverse) | 42 | Right track |

#### Reserved / occupied pins summary

Pins in use: 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 48.

Free / available: 1, 3, 19, 20 (USB D±), 22, 23, 24, 25, 26, 27, 43 (UART TX), 44 (UART RX), 45, 46 (strapping), 47.

## Project Structure

```
wifi_Tank/
├── main/
│   ├── main.c                  # Entry point, web server, throughput monitor
│   ├── provisioning.c          # SoftAP setup, NVS credential management
│   ├── stream.c                # Camera init, MJPEG HTTP server (port 81)
│   ├── motor.c / motor.h       # BTS7960B PWM motor driver
│   ├── overlay.c               # WebSocket overlay system
│   ├── system.c                # TCP server (port 8080)
│   └── ota.c                   # OTA firmware update (POST /ota, GET /version)
├── provisioning.html           # WiFi setup page (embedded into firmware)
├── overlay_demo.html           # Browser overlay demo
├── partitions.csv              # Custom partition table (shared by both targets)
├── sdkconfig                   # Active ESP-IDF config (generated, do not edit)
├── sdkconfig.defaults.esp32    # Default config overrides for ESP32 target
├── sdkconfig.defaults.esp32s3  # Default config overrides for ESP32-S3 target
└── Submodules/trice/           # Trice logging library
```
