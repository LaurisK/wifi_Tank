#include "provisioning.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG              "provisioning"
#define NVS_CFG_NS       "tank_cfg"   /* network credentials list */
#define NVS_BOOT_NS      "tank_boot"  /* one-time boot flags (never erased) */
#define NVS_KEY_NET_COUNT "net_count"
#define NVS_KEY_SEEDED    "seeded"
#define SOFTAP_SSID      "Tank-Setup"
#define SOFTAP_CHANNEL   1
#define SOFTAP_MAX_CONN  4
#define PER_NET_RETRIES  3

/* Default networks written to NVS on first boot only */
static const struct { const char *ssid; const char *pass; } s_defaults[] = {
    { "Namai",         "Slaptazodis123"   },
    { "#Telia-BCBEFE", "fM3udPwhvw91N1ds" },
};
#define DEFAULT_NET_COUNT  (int)(sizeof(s_defaults) / sizeof(s_defaults[0]))

/* Embedded HTML from provisioning.html (populated by CMake EMBED_TXTFILES) */
extern const uint8_t provisioning_html_start[] asm("_binary_provisioning_html_start");
extern const uint8_t provisioning_html_end[]   asm("_binary_provisioning_html_end");

static EventGroupHandle_t s_wifi_event_group;
static int s_current_net_idx     = 0;
static int s_current_net_retries = 0;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

void ProvisioningClearCredentials(void) {
    /* Only erase the credentials namespace — the boot namespace (seeded flag)
       is intentionally preserved so defaults are never re-seeded. */
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Credential list erased from NVS");
    }
}

static uint8_t nvs_get_net_count(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t count = 0;
    nvs_get_u8(h, NVS_KEY_NET_COUNT, &count);
    nvs_close(h);
    return count;
}

static esp_err_t nvs_read_network(int idx, char *ssid, size_t ssid_sz,
                                  char *pass, size_t pass_sz) {
    char key[16];
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    snprintf(key, sizeof(key), "ssid_%d", idx);
    esp_err_t err = nvs_get_str(h, key, ssid, &ssid_sz);
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), "pass_%d", idx);
        err = nvs_get_str(h, key, pass, &pass_sz);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_add_network(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t count = 0;
    nvs_get_u8(h, NVS_KEY_NET_COUNT, &count);
    char key[16];
    snprintf(key, sizeof(key), "ssid_%d", count);
    err = nvs_set_str(h, key, ssid);
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), "pass_%d", count);
        err = nvs_set_str(h, key, pass);
    }
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_NET_COUNT, count + 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Added network[%d] SSID='%s'", count, ssid);
    return err;
}

static bool nvs_is_seeded(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_BOOT_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t val = 0;
    nvs_get_u8(h, NVS_KEY_SEEDED, &val);
    nvs_close(h);
    return val != 0;
}

static void nvs_seed_defaults(void) {
    for (int i = 0; i < DEFAULT_NET_COUNT; i++)
        nvs_add_network(s_defaults[i].ssid, s_defaults[i].pass);

    nvs_handle_t h;
    if (nvs_open(NVS_BOOT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_SEEDED, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Seeded %d default networks to NVS", DEFAULT_NET_COUNT);
}

// ---------------------------------------------------------------------------
// URL decode
// ---------------------------------------------------------------------------

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_max) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out < dst_max - 1; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            int hi = hex_val(src[i+1]);
            int lo = hex_val(src[i+2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

// ---------------------------------------------------------------------------
// POST body field extraction
// ---------------------------------------------------------------------------

static void extract_field(const char *body, const char *key,
                           char *dst, size_t dst_max) {
    dst[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            char raw[256] = {0};
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, p, vlen);
            raw[vlen] = '\0';
            url_decode(dst, raw, dst_max);
            return;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
}

// ---------------------------------------------------------------------------
// HTTP handlers (SoftAP provisioning server)
// ---------------------------------------------------------------------------

static esp_err_t handle_get_html(httpd_req_t *req) {
    size_t len = provisioning_html_end - provisioning_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)provisioning_html_start, (ssize_t)len);
    return ESP_OK;
}

static esp_err_t handle_post_configure(httpd_req_t *req) {
    char body[512] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';
    ESP_LOGI(TAG, "POST /configure body: %s", body);

    char ssid[64] = {0};
    char pass[64] = {0};
    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_add_network(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save network: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Network saved: SSID='%s', rebooting...", ssid);

    const char *resp =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>body{background:#1a1a2e;color:#e0e0e0;font-family:Arial,sans-serif;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh;}"
        ".msg{text-align:center;}</style></head>"
        "<body><div class='msg'><h2 style='color:#e94560'>Saved!</h2>"
        "<p>Rebooting and connecting to your network...</p></div></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static void start_provisioning_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning HTTP server");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handle_get_html,
        .user_ctx = NULL
    };
    static const httpd_uri_t uri_setup = {
        .uri      = "/setup",
        .method   = HTTP_GET,
        .handler  = handle_get_html,
        .user_ctx = NULL
    };
    static const httpd_uri_t uri_configure = {
        .uri      = "/configure",
        .method   = HTTP_POST,
        .handler  = handle_post_configure,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_setup);
    httpd_register_uri_handler(server, &uri_configure);

    ESP_LOGI(TAG, "Provisioning HTTP server started on port 80");
}

// ---------------------------------------------------------------------------
// SoftAP mode
// ---------------------------------------------------------------------------

static void start_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = SOFTAP_SSID,
            .ssid_len       = strlen(SOFTAP_SSID),
            .channel        = SOFTAP_CHANNEL,
            .password       = "",
            .max_connection = SOFTAP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID='%s', open, IP=192.168.4.1", SOFTAP_SSID);
}

// ---------------------------------------------------------------------------
// STA multi-network mode
// ---------------------------------------------------------------------------

static void connect_to_network(int idx) {
    char ssid[64] = {0};
    char pass[64] = {0};
    if (nvs_read_network(idx, ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read network[%d] from NVS", idx);
        ProvisioningClearCredentials();
        esp_restart();
        return;
    }
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = strlen(pass) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable  = true;
    sta_cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_wifi_connect();
    ESP_LOGI(TAG, "Connecting to network[%d] SSID='%s'", idx, ssid);
}

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_current_net_idx     = 0;
        s_current_net_retries = 0;
        connect_to_network(0);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_current_net_retries++;
        ESP_LOGW(TAG, "STA disconnected, net[%d] attempt %d/%d",
                 s_current_net_idx, s_current_net_retries, PER_NET_RETRIES);
        if (s_current_net_retries >= PER_NET_RETRIES) {
            s_current_net_retries = 0;
            s_current_net_idx++;
            uint8_t count = nvs_get_net_count();
            if (s_current_net_idx >= (int)count) {
                ESP_LOGE(TAG, "All %d known networks exhausted, clearing and rebooting to SoftAP", count);
                ProvisioningClearCredentials();
                esp_restart();
                return;
            }
            ESP_LOGI(TAG, "Trying next network[%d]", s_current_net_idx);
        }
        connect_to_network(s_current_net_idx);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "=== WIFI CONNECTED (network[%d]) ===", s_current_net_idx);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "====================================");
        s_current_net_retries = 0;
        xEventGroupSetBits(s_wifi_event_group, BIT0);
    }
}

static int start_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &sta_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &sta_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Performance settings applied before start */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Actual SSID/pass are set inside WIFI_EVENT_STA_START → connect_to_network(0) */
    ESP_ERROR_CHECK(esp_wifi_start());
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int ProvisioningStart(EventGroupHandle_t *wifi_event_group_out) {
    s_wifi_event_group = xEventGroupCreate();
    *wifi_event_group_out = s_wifi_event_group;

    if (!nvs_is_seeded()) {
        ESP_LOGI(TAG, "First boot: seeding default networks to NVS");
        nvs_seed_defaults();
    }

    uint8_t count = nvs_get_net_count();
    if (count > 0) {
        ESP_LOGI(TAG, "%d known network(s) in NVS, starting STA mode", count);
        return start_sta();
    }

    ESP_LOGI(TAG, "No known networks, starting SoftAP provisioning mode");
    start_softap();
    start_provisioning_server();

    /* Block forever — reboot happens inside handle_post_configure after save */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
