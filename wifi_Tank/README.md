# WiFi Tank

ESP-IDF project for the AI-Thinker ESP32-CAM (OV3660 sensor) that streams live MJPEG video over WiFi with a real-time WebSocket overlay system.

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
idf.py set-target esp32
idf.py reconfigure    # downloads espressif__esp32-camera and espressif__esp_jpeg
```

## Build and Flash

```bash
cd wifi_Tank

# Build
idf.py build

# Flash and open serial monitor (adjust port as needed)
idf.py -p /dev/ttyUSB0 flash monitor
```

The serial monitor shows startup logs including the obtained IP address. Press `Ctrl+]` to exit the monitor.

To find the device on your network after boot:

```bash
nmap -sn 192.168.1.0/24   # adjust subnet to match your router
```

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

## Capabilities

### Network Services

| Port | Protocol | Description |
|------|----------|-------------|
| 80   | HTTP     | Main web server |
| 81   | HTTP / WebSocket | Video stream + overlay |
| 8080 | TCP (raw) | System control channel |

### Video Stream — port 81

Open the stream in any MJPEG-capable client (browser, VLC, ffplay):

```
http://<device-ip>:81/stream
```

- Format: MJPEG multipart HTTP stream
- Resolution: 1280×720 (HD)
- JPEG quality: 12 (higher = lower quality; range 0–63)
- Up to 13 concurrent clients
- FPS is tracked and exposed via the overlay WebSocket

A plain info page is served at `http://<device-ip>:81/`.

### Overlay WebSocket — port 81

Connect to `ws://<device-ip>:81/ws` to receive real-time overlay data as JSON. The server pushes an update every 2 seconds containing:

- **Text elements** — up to 10 items, each with position, colour, size, and content (e.g. current FPS)
- **Shape elements** — up to 20 shapes: lines, rectangles, circles, triangles

See `overlay_demo.html` in the project root for a ready-made client that renders the overlay on top of the video stream.

Up to 8 WebSocket clients can be connected simultaneously.

### Main HTTP Server — port 80

```
GET http://<device-ip>/    →  "hello world"
```

Placeholder endpoint; intended for future control UI.

### TCP System Server — port 8080

Raw TCP socket server for system-level commands and monitoring. Supports multiple concurrent clients with keep-alive (5 s idle, 3 retries).

### Throughput Monitoring

The firmware logs application-level RX/TX throughput to the serial monitor once per second (only when there is activity):

```
Throughput - RX: 450 kbps (0.45 Mbps) | TX: 3200 kbps (3.20 Mbps) | Total: RX 0.12 MB / TX 0.85 MB
```

## Hardware

- **MCU**: ESP32 (AI-Thinker ESP32-CAM form factor)
- **Camera**: OV3660
- **Flash**: 2 MB app partition (see `partitions.csv`)
- **XCLK**: 20 MHz on GPIO 0
- **Camera data bus**: GPIOs 5, 18, 19, 21, 34, 35, 36, 39
- **VSYNC / HREF / PCLK**: GPIOs 25, 23, 22
- **I²C (SIOD/SIOC)**: GPIOs 26, 27
- **PWDN**: GPIO 32

## Project Structure

```
wifi_Tank/
├── main/
│   ├── main.c          # Entry point, web server, throughput monitor
│   ├── provisioning.c  # SoftAP setup, NVS credential management
│   ├── stream.c        # Camera init, MJPEG HTTP server (port 81)
│   ├── overlay.c       # WebSocket overlay system
│   └── system.c        # TCP server (port 8080)
├── provisioning.html   # WiFi setup page (embedded into firmware)
├── overlay_demo.html   # Browser overlay demo
├── partitions.csv      # Custom partition table
├── sdkconfig           # ESP-IDF project configuration
└── Submodules/trice/   # Trice logging library
```
