#include "ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE 1024

static void reboot_after_ota(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    vTaskDelete(NULL);
}

static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    set_cors(req);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA update partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA target: '%s' at 0x%08lx (%lu bytes available)",
             update_partition->label, update_partition->address, update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    int content_len = req->content_len;
    int total = 0;

    ESP_LOGI(TAG, "Receiving OTA image: %d bytes", content_len);

    while (total < content_len) {
        int remaining = content_len - total;
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "Receive error: %d", received);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        total += received;
        if (total % (64 * 1024) == 0 || total == content_len) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%.1f%%)",
                     total, content_len, (total * 100.0f) / content_len);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes). Rebooting...", total);

    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"ok\",\"message\":\"OTA complete, rebooting\"}";
    httpd_resp_send(req, resp, strlen(resp));

    xTaskCreate(reboot_after_ota, "ota_reboot", 1024, NULL, 4, NULL);
    return ESP_OK;
}

static esp_err_t version_get_handler(httpd_req_t *req) {
    set_cors(req);
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"project\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"partition\":\"%s\"}",
             desc->version, desc->project_name, desc->date, desc->time,
             running ? running->label : "unknown");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static const httpd_uri_t uri_ota = {
    .uri     = "/ota",
    .method  = HTTP_POST,
    .handler = ota_post_handler,
};

static const httpd_uri_t uri_version = {
    .uri     = "/version",
    .method  = HTTP_GET,
    .handler = version_get_handler,
};

esp_err_t OtaInit(httpd_handle_t server) {
    httpd_register_uri_handler(server, &uri_ota);
    httpd_register_uri_handler(server, &uri_version);
    ESP_LOGI(TAG, "OTA endpoint registered: POST /ota");
    ESP_LOGI(TAG, "Version endpoint registered: GET /version");
    return ESP_OK;
}
