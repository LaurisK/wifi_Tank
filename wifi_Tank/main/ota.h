#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

// Register OTA endpoints on the given HTTP server.
// POST /ota   — upload new firmware binary; reboots on success
// GET  /version — returns running firmware metadata as JSON
esp_err_t OtaInit(httpd_handle_t server);
