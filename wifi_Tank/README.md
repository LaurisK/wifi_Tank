# ESP32-S3 WiFi Tank Project

ESP-IDF project for ESP32-S3 that connects to WiFi network "Namai" and hosts a simple web server.

## Features
- Connects to WiFi network "Namai" with password "Slaptazodis123"
- Runs HTTP web server on port 80
- Serves "hello world" response at root URL

## Build and Flash

```bash
cd wifi_Tank
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Hardware Requirements
- ESP32-S3 development board
- USB connection for programming/monitoring

## Usage
After flashing, the ESP32-S3 will:
1. Connect to the configured WiFi network
2. Display the obtained IP address
3. Start web server on port 80
4. Respond with "hello world" to HTTP requests at the root URL