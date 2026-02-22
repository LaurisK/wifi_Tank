#pragma once

#include "esp_http_server.h"

// Register parameters HTTP endpoints on the given server (port 80).
//
//   GET  /params/networks          → JSON list of stored networks + current index
//   POST /params/networks/add      → add new network   (body: ssid=…&pass=…)
//   POST /params/networks/update   → update password   (body: idx=0&pass=…)
//   POST /params/networks/delete   → delete by index   (body: idx=0)
//
esp_err_t ParamsInit(httpd_handle_t server);
