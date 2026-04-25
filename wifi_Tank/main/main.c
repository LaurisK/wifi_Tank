#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "trice.h"
#include "system.h"
#include "stream.h"
#include "overlay.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"
#include "provisioning.h"
#include "mdns.h"
#include "motor.h"
#include "control.h"
#include "ota.h"
#include "params.h"
#include "esp_camera.h"           // Camera sensor access for AEC/AGC/temp telemetry

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "driver/temperature_sensor.h"
static temperature_sensor_handle_t s_temp_sensor = NULL;
#else
// ROM temperature sensor — only available on original ESP32.
extern uint8_t temprature_sens_read(void);
#endif

#define WEB_SERVER_PORT 80

static const char *TAG = "wifi_Tank";

// Throughput measurement via netif linkoutput/input hooks.
// The standard lwIP MIB2 counters (ifinoctets/ifoutoctets) are only updated
// for the loopback path, not for the WiFi STA interface. Hooking the netif
// function pointers directly is the only way to count all WiFi bytes.

static volatile uint32_t s_netif_tx_bytes = 0;
static volatile uint32_t s_netif_rx_bytes = 0;

// Original function pointers — set once when the hook is installed.
static netif_linkoutput_fn s_orig_linkoutput = NULL;
static netif_input_fn      s_orig_input      = NULL;

static err_t throughput_tx_hook(struct netif *netif, struct pbuf *p)
{
    s_netif_tx_bytes += p->tot_len;
    return s_orig_linkoutput(netif, p);
}

static err_t throughput_rx_hook(struct pbuf *p, struct netif *inp)
{
    s_netif_rx_bytes += p->tot_len;
    return s_orig_input(p, inp);
}

static struct {
    uint32_t last_rx_bytes;
    uint32_t last_tx_bytes;
    uint32_t rx_throughput_kbps;
    uint32_t tx_throughput_kbps;
} app_throughput = {0};


static esp_err_t root_get_handler(httpd_req_t *req) {
    const char *resp = "hello world";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

// Wildcard CORS preflight handler — responds to OPTIONS /* so browsers can send
// cross-origin requests (including Chrome's Private Network Access preflight).
static esp_err_t cors_preflight_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",          "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",         "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",         "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age",               "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t uri_cors_preflight = {
    .uri     = "/*",
    .method  = HTTP_OPTIONS,
    .handler = cors_preflight_handler,
};

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.stack_size       = 8192;
    config.max_uri_handlers = 20;               // default 8 is too small
    config.uri_match_fn     = httpd_uri_match_wildcard; // needed for OPTIONS /*

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &uri_cors_preflight);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void mdns_start(void) {
    char hostname[64];
    ParamsGetDeviceName(hostname, sizeof(hostname));

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Tank Robot");

    // Advertise each service so clients can discover them
    mdns_service_add("Tank Web",    "_http",  "_tcp", WEB_SERVER_PORT, NULL, 0);
    mdns_service_add("Tank Stream", "_http",  "_tcp", 81,              NULL, 0);
    mdns_service_add("Tank Ctrl",   "_tank",  "_tcp", 8080,            NULL, 0);

    ESP_LOGI(TAG, "mDNS started — device reachable as http://%s.local", hostname);
}

void print_network_scan_tips(void) {
    char hostname[64];
    ParamsGetDeviceName(hostname, sizeof(hostname));
    ESP_LOGI(TAG, "Device reachable at http://%s.local", hostname);
}

static void throughput_monitor_task(void *pvParameters) {
    // Install hooks on the WiFi STA netif's linkoutput (TX) and input (RX).
    // Done here because this task is created after WiFi connects, so the
    // netif is guaranteed to be up by the time the task first runs.
    esp_netif_t *netif_h = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif_h) {
        struct netif *lif = (struct netif *)esp_netif_get_netif_impl(netif_h);
        if (lif) {
            s_orig_linkoutput  = lif->linkoutput;
            lif->linkoutput    = throughput_tx_hook;
            s_orig_input       = lif->input;
            lif->input         = throughput_rx_hook;
            ESP_LOGI(TAG, "Throughput hooks installed on WiFi STA netif");
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t rx = s_netif_rx_bytes;
        uint32_t tx = s_netif_tx_bytes;

        uint32_t rx_diff = rx - app_throughput.last_rx_bytes;
        uint32_t tx_diff = tx - app_throughput.last_tx_bytes;

        app_throughput.rx_throughput_kbps = (rx_diff * 8) / 1000;
        app_throughput.tx_throughput_kbps = (tx_diff * 8) / 1000;
        app_throughput.last_rx_bytes = rx;
        app_throughput.last_tx_bytes = tx;

        if (rx_diff > 0 || tx_diff > 0) {
            ESP_LOGI(TAG, "Throughput — RX: %lu kbps | TX: %lu kbps",
                     app_throughput.rx_throughput_kbps,
                     app_throughput.tx_throughput_kbps);
        }
    }
}

static void overlay_demo_task(void *pvParameters) {
    ESP_LOGI(TAG, "Overlay demo task started");

    // Wait for everything to initialise
    vTaskDelay(pdMS_TO_TICKS(5000));

    int  last_motor_l  = 0;
    int  last_motor_r  = 0;
    TickType_t last_send_tick = xTaskGetTickCount();
    uint32_t counter = 0;

    while (1) {
        int cur_l, cur_r;
        ControlGetMotorState(&cur_l, &cur_r);

        bool motor_changed = (cur_l != last_motor_l || cur_r != last_motor_r);
        bool heartbeat_due = (xTaskGetTickCount() - last_send_tick) >= pdMS_TO_TICKS(2000);

        if ((motor_changed || heartbeat_due) && OverlayGetClientCount() > 0) {
            overlay_data_t overlay;
            OverlayCreateSampleData(&overlay);

            overlay.has_motors = true;
            overlay.motors.l   = (int8_t)cur_l;
            overlay.motors.r   = (int8_t)cur_r;

            // ── Telemetry ──────────────────────────────────────────────────
            overlay.has_telemetry = true;
            overlay.telemetry.fps = StreamGetFps();

            // WiFi RSSI + channel
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                overlay.telemetry.rssi        = ap_info.rssi;
                overlay.telemetry.wifi_channel = ap_info.primary;
            } else {
                overlay.telemetry.rssi        = -100;
                overlay.telemetry.wifi_channel = 0;
            }

            // Throughput (measured by throughput_monitor_task, updated every 1 s)
            overlay.telemetry.tx_kbps = app_throughput.tx_throughput_kbps;
            overlay.telemetry.rx_kbps = app_throughput.rx_throughput_kbps;

            // Internal heap (PSRAM excluded — it's the scarce resource)
            overlay.telemetry.int_heap_kb =
                (uint32_t)(esp_get_free_internal_heap_size() / 1024);

            // MCU die temperature
#ifdef CONFIG_IDF_TARGET_ESP32S3
            if (s_temp_sensor) {
                float t;
                if (temperature_sensor_get_celsius(s_temp_sensor, &t) == ESP_OK)
                    overlay.telemetry.mcu_temp_c = t;
            }
#else
            // ROM temperature approximation: accuracy ≈ ±5°C, reflects self-heated die.
            overlay.telemetry.mcu_temp_c =
                (temprature_sens_read() - 32) / 1.8f;
#endif

            // Camera sensor status (AEC = exposure, AGC = gain) + die temperature.
            //
            // OV3660 temperature sensor (datasheet §7.30, registers 0x6700–0x6721):
            //   0x6700 = 0x05  written by driver reset(): enable + free-running mode
            //   0x6701 = 25    low-temp alarm at 25/2 = 12.5 °C
            //   0x6702 = 253   high-temp alarm 1 at 253/2 = 126.5 °C (disabled)
            //   0x6703 = 209   high-temp alarm 2 at 209/2 = 104.5 °C (disabled)
            //   0x6719        "TPM_CTRL19 Bit[7:0]: Measured temperature" (read-only)
            //
            // Conversion: T(°C) = raw / 2.0   (0.5 °C per LSB)
            // All alarm thresholds lie above the 85 °C operating max → effectively
            // disabled, consistent with the 0xFF values at 0x6704/0x6705.
            sensor_t *cam = esp_camera_sensor_get();
            float cam_temp_c = -127.0f;
            if (cam) {
                overlay.telemetry.cam_aec  = cam->status.aec_value;
                overlay.telemetry.cam_gain = cam->status.agc_gain;

                if (cam->get_reg) {
                    int raw = cam->get_reg(cam, 0x6719, 0xFF);
                    if (raw > 0 && raw < 255) {
                        cam_temp_c = (float)raw / 2.0f;
                    }
                }
            }
            overlay.telemetry.cam_temp_c = cam_temp_c;

            // Uptime
            overlay.telemetry.uptime_s =
                (uint32_t)(esp_timer_get_time() / 1000000ULL);
            // ──────────────────────────────────────────────────────────────

            int sent = OverlaySendUpdate(&overlay);
            if (sent > 0) {
                ESP_LOGI(TAG, "Sent overlay update #%lu to %d clients", counter, sent);
                counter++;
            }

            last_motor_l   = cur_l;
            last_motor_r   = cur_r;
            last_send_tick = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    // Initialize Trice as early as possible
    TriceInit();

    ESP_LOGI(TAG, "Starting wifi_Tank application");

#ifdef CONFIG_IDF_TARGET_ESP32S3
    // GPIO14,21,41,42 — free after camera(4-18,38), LEDs(2,48), SD(39,40)
    motor_cfg_t motor_cfg = {
        .left  = { .gpio_rpwm = 14, .gpio_lpwm = 21 },
        .right = { .gpio_rpwm = 41, .gpio_lpwm = 42 },
    };
#else
    // Initialise motor drivers (GPIO12-15, BTS7960B EN tied to 5 V)
    motor_cfg_t motor_cfg = {
        .left  = { .gpio_rpwm = 13, .gpio_lpwm = 14 },
        .right = { .gpio_rpwm = 15, .gpio_lpwm = 12 },
    };
#endif
    if (MotorInit(&motor_cfg) != 0) {
        ESP_LOGW(TAG, "Motor init failed");
    }

#ifdef CONFIG_IDF_TARGET_ESP32S3
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&temp_cfg, &s_temp_sensor) == ESP_OK) {
        temperature_sensor_enable(s_temp_sensor);
    }
#endif

    ESP_ERROR_CHECK(nvs_flash_init());

    EventGroupHandle_t wifi_event_group;
    ProvisioningStart(&wifi_event_group);

    xEventGroupWaitBits(wifi_event_group, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);

    mdns_start();
    print_network_scan_tips();

    ESP_LOGI(TAG, "WiFi connected, initializing system");

    // Initialize system (creates task and TCP server on port 8080)
    SystemInit(8080);

    // Initialize video stream (camera + HTTP MJPEG server on port 81)
    if (StreamInit(81) == 0) {
        StreamStart();
        ESP_LOGI(TAG, "Video stream initialized on port 81");
    } else {
        ESP_LOGW(TAG, "Failed to initialize video stream");
    }

    ESP_LOGI(TAG, "Starting web server");
    httpd_handle_t server = start_webserver();

    if (server) {
        ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);

        // Initialize overlay WebSocket on the main server (port 80)
        // NOTE: Must NOT be on the stream server (port 81) — the stream handler
        // blocks the httpd task, preventing any WS connections from being processed.
        char hostname[64];
        ParamsGetDeviceName(hostname, sizeof(hostname));

        if (OverlayInit(server) == 0) {
            ESP_LOGI(TAG, "Overlay WebSocket initialized at: ws://%s.local:%d/ws", hostname, WEB_SERVER_PORT);
        } else {
            ESP_LOGW(TAG, "Failed to initialize overlay WebSocket");
        }

        if (ControlInit(server) == 0) {
            ESP_LOGI(TAG, "Control WebSocket initialized at: ws://%s.local:%d/ctrl", hostname, WEB_SERVER_PORT);
        } else {
            ESP_LOGW(TAG, "Failed to initialize control WebSocket");
        }

        OtaInit(server);
        ParamsInit(server);
    }

    // Start application throughput monitoring task
    xTaskCreate(throughput_monitor_task, "throughput_mon", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Application throughput monitoring enabled");

    // Start overlay demo task
    xTaskCreate(overlay_demo_task, "overlay_demo", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Overlay demo task started - will send sample overlays every 2 seconds");
}