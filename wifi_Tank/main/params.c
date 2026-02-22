#include "params.h"
#include "provisioning.h"
#include "control.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "params";

// ---------------------------------------------------------------------------
// URL-encoded body parser (avoids CORS pre-flight for POST requests)
// ---------------------------------------------------------------------------

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_field(const char *body, const char *key,
                        char *dst, size_t dsz) {
    dst[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            size_t out = 0;
            for (size_t i = 0; i < vlen && out < dsz - 1; i++) {
                char c = p[i];
                if (c == '+') {
                    dst[out++] = ' ';
                } else if (c == '%' && i + 2 < vlen) {
                    int hi = hex_val(p[i + 1]);
                    int lo = hex_val(p[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        dst[out++] = (char)((hi << 4) | lo);
                        i += 2;
                    } else {
                        dst[out++] = c;
                    }
                } else {
                    dst[out++] = c;
                }
            }
            dst[out] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static int parse_int_field(const char *body, const char *key, int def) {
    char buf[12];
    parse_field(body, key, buf, sizeof(buf));
    return buf[0] ? atoi(buf) : def;
}

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t bufsz) {
    int len = httpd_req_recv(req, buf, (int)(bufsz - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /params/networks
// ---------------------------------------------------------------------------

static esp_err_t handle_get_networks(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    int count   = (int)ProvisioningGetNetCount();
    int current = ProvisioningGetCurrentNetIdx();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "current", current);
    cJSON *nets = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        char ssid[64] = {0}, pass[64] = {0};
        if (ProvisioningReadNetwork(i, ssid, sizeof(ssid),
                                        pass, sizeof(pass)) == ESP_OK) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", ssid);
            cJSON_AddStringToObject(net, "pass", pass);
            cJSON_AddItemToArray(nets, net);
        }
    }
    cJSON_AddItemToObject(root, "networks", nets);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_err_t ret = ESP_OK;
    if (body) {
        ret = httpd_resp_send(req, body, (ssize_t)strlen(body));
        free(body);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        ret = ESP_FAIL;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// POST /params/networks/add   body: ssid=…&pass=…
// ---------------------------------------------------------------------------

static esp_err_t handle_add_network(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    char body[512];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    char ssid[64], pass[64];
    parse_field(body, "ssid", ssid, sizeof(ssid));
    parse_field(body, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    esp_err_t err = ProvisioningAddNetwork(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Add network failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /params/networks/update   body: idx=0&pass=…
// ---------------------------------------------------------------------------

static esp_err_t handle_update_network(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    char body[512];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    int  idx  = parse_int_field(body, "idx", -1);
    char pass[64];
    parse_field(body, "pass", pass, sizeof(pass));

    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "idx required");
        return ESP_FAIL;
    }

    esp_err_t err = ProvisioningUpdateNetwork(idx, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Update network[%d] failed: %s", idx, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /params/networks/delete   body: idx=0
// ---------------------------------------------------------------------------

static esp_err_t handle_delete_network(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    char body[128];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    int idx = parse_int_field(body, "idx", -1);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "idx required");
        return ESP_FAIL;
    }

    esp_err_t err = ProvisioningDeleteNetwork(idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Delete network[%d] failed: %s", idx, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS delete failed");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /params/ctrl   → {"ramp_rate": N}
// ---------------------------------------------------------------------------

static esp_err_t handle_get_ctrl(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    char buf[32];
    snprintf(buf, sizeof(buf), "{\"ramp_rate\":%d}", ControlGetRampRate());
    httpd_resp_send(req, buf, (ssize_t)strlen(buf));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /params/ctrl   body: ramp_rate=N
// ---------------------------------------------------------------------------

static esp_err_t handle_post_ctrl(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);

    char body[64];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    int rate = parse_int_field(body, "ramp_rate", -1);
    if (rate < 1 || rate > 500) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ramp_rate must be 1-500");
        return ESP_FAIL;
    }

    ControlSetRampRate(rate);
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static const httpd_uri_t uri_get_networks = {
    .uri     = "/params/networks",
    .method  = HTTP_GET,
    .handler = handle_get_networks,
};
static const httpd_uri_t uri_add_network = {
    .uri     = "/params/networks/add",
    .method  = HTTP_POST,
    .handler = handle_add_network,
};
static const httpd_uri_t uri_update_network = {
    .uri     = "/params/networks/update",
    .method  = HTTP_POST,
    .handler = handle_update_network,
};
static const httpd_uri_t uri_delete_network = {
    .uri     = "/params/networks/delete",
    .method  = HTTP_POST,
    .handler = handle_delete_network,
};
static const httpd_uri_t uri_get_ctrl = {
    .uri     = "/params/ctrl",
    .method  = HTTP_GET,
    .handler = handle_get_ctrl,
};
static const httpd_uri_t uri_post_ctrl = {
    .uri     = "/params/ctrl",
    .method  = HTTP_POST,
    .handler = handle_post_ctrl,
};

esp_err_t ParamsInit(httpd_handle_t server) {
    httpd_register_uri_handler(server, &uri_get_networks);
    httpd_register_uri_handler(server, &uri_add_network);
    httpd_register_uri_handler(server, &uri_update_network);
    httpd_register_uri_handler(server, &uri_delete_network);
    httpd_register_uri_handler(server, &uri_get_ctrl);
    httpd_register_uri_handler(server, &uri_post_ctrl);
    ESP_LOGI(TAG, "Params endpoints registered");
    return ESP_OK;
}
