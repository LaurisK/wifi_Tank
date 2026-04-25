#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Register parameters HTTP endpoints on the given server (port 80).
//
//   GET  /params/networks          → JSON list of stored networks + current index
//   POST /params/networks/add      → add new network   (body: ssid=…&pass=…)
//   POST /params/networks/update   → update password   (body: idx=0&pass=…)
//   POST /params/networks/delete   → delete by index   (body: idx=0)
//
//   GET  /params/ctrl              → {"ramp_rate": N}
//   POST /params/ctrl              → set ramp_rate=N (1–500 %/s), persists in NVS
//
//   GET  /params/device            → {"name": "tank_xxxx"}
//   POST /params/device            → set name=… (max 32 chars), persists in NVS,
//                                    updates mDNS hostname immediately
//
// ParamsGetDeviceName: returns the active device name (NVS value or CRC16 default).
// buf must be at least 33 bytes.
esp_err_t ParamsGetDeviceName(char *buf, size_t bufsz);

esp_err_t ParamsInit(httpd_handle_t server);
