#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "trice.h"
#include "system.h"
#include "stream.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"

#define WIFI_SSID "Namai"
#define WIFI_PASS "Slaptazodis123"
#define WEB_SERVER_PORT 80

static const char *TAG = "wifi_Tank";

// Application-level throughput monitoring
typedef struct {
    uint32_t total_rx_bytes;
    uint32_t total_tx_bytes;
    uint32_t last_rx_bytes;
    uint32_t last_tx_bytes;
    uint32_t rx_throughput_kbps;
    uint32_t tx_throughput_kbps;
} app_throughput_t;

app_throughput_t app_throughput = {0};  // Made non-static for access from other modules

// Public functions to update throughput counters
void app_throughput_add_rx(uint32_t bytes) {
    app_throughput.total_rx_bytes += bytes;
}

void app_throughput_add_tx(uint32_t bytes) {
    app_throughput.total_tx_bytes += bytes;
}

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

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

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=== WIFI CONNECTED ===");
        ESP_LOGI(TAG, "Device IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "Web server available at: http://" IPSTR ":%d", IP2STR(&event->ip_info.ip), WEB_SERVER_PORT);
        ESP_LOGI(TAG, "========================");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Performance optimizations: Set WiFi bandwidth to 40MHz for higher throughput
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_IF_WIFI_STA, WIFI_BW_HT40));

    // Enable 802.11n for best performance
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

    // Disable WiFi power save mode for maximum performance
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &root);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void print_network_scan_tips(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== NETWORK SCANNING TIPS ===");
    ESP_LOGI(TAG, "To find your ESP32 device on the network:");
    ESP_LOGI(TAG, "1. Use 'nmap -sn 192.168.1.0/24' (adjust subnet)");
    ESP_LOGI(TAG, "2. Check your router's connected devices list");
    ESP_LOGI(TAG, "3. Use 'ping 192.168.1.X' to test connectivity");
    ESP_LOGI(TAG, "4. Access http://[IP_ADDRESS] in browser to test web server");
    ESP_LOGI(TAG, "5. For Windows: 'arp -a' shows ARP table");
    ESP_LOGI(TAG, "===============================");
}

static void throughput_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Application throughput monitoring started");

    while (1) {
        // Calculate throughput in kbps (kilobits per second) over 1 second
        uint32_t rx_bytes_diff = app_throughput.total_rx_bytes - app_throughput.last_rx_bytes;
        uint32_t tx_bytes_diff = app_throughput.total_tx_bytes - app_throughput.last_tx_bytes;

        app_throughput.rx_throughput_kbps = (rx_bytes_diff * 8) / 1000;  // Convert to kbps
        app_throughput.tx_throughput_kbps = (tx_bytes_diff * 8) / 1000;  // Convert to kbps

        // Log throughput every second (only if there's activity)
        if (rx_bytes_diff > 0 || tx_bytes_diff > 0) {
            ESP_LOGI(TAG, "Throughput - RX: %lu kbps (%.2f Mbps) | TX: %lu kbps (%.2f Mbps) | Total: RX %.2f MB / TX %.2f MB",
                     app_throughput.rx_throughput_kbps,
                     app_throughput.rx_throughput_kbps / 1000.0,
                     app_throughput.tx_throughput_kbps,
                     app_throughput.tx_throughput_kbps / 1000.0,
                     app_throughput.total_rx_bytes / (1024.0 * 1024.0),
                     app_throughput.total_tx_bytes / (1024.0 * 1024.0));
        }

        // Update last values
        app_throughput.last_rx_bytes = app_throughput.total_rx_bytes;
        app_throughput.last_tx_bytes = app_throughput.total_tx_bytes;

        // Wait 1 second before next measurement
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    // Initialize Trice as early as possible
    TriceInit();

    ESP_LOGI(TAG, "Starting wifi_Tank application");

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

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
    }

    // Start application throughput monitoring task
    xTaskCreate(throughput_monitor_task, "throughput_mon", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Application throughput monitoring enabled");
}