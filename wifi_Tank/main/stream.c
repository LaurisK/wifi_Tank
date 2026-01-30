/*! \file stream.c
\brief Video streaming implementation for ESP32-CAM with OV3660
*******************************************************************************/

#include "stream.h"
#include "overlay.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "STREAM";

// Camera pin definitions for AI-Thinker ESP32-CAM
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1  // Software reset
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0      5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

// Stream configuration
#define STREAM_BOUNDARY "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY
#define STREAM_PART_BOUNDARY "\r\n--" STREAM_BOUNDARY "\r\n"
#define STREAM_PART_HEADER "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

// Stream state
static struct {
    httpd_handle_t server;
    uint16_t port;
    bool camera_initialized;
    bool streaming;
    int client_count;
    uint32_t frame_count;
    uint32_t last_frame_time;
} stream_state = {
    .server = NULL,
    .port = 0,
    .camera_initialized = false,
    .streaming = false,
    .client_count = 0,
    .frame_count = 0,
    .last_frame_time = 0
};

/**
 * @brief Initialize the camera
 */
static int camera_init(void) {
    ESP_LOGI(TAG, "Initializing camera for AI-Thinker ESP32-CAM with OV3660");

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,           // 20MHz
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,     // JPEG for streaming
        .frame_size = FRAMESIZE_HD,         // 1280x720
        .jpeg_quality = 12,                 // 0-63, lower = higher quality
        .fb_count = 2,                      // Double buffering
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY // Grab next frame when buffer is empty
    };

    // Initialize camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return -1;
    }

    // Get camera sensor
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return -1;
    }

    // Initial sensor configuration
    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_special_effect(s, 0); // 0 to 6 (0 - No Effect, 1 - Negative, 2 - Grayscale, etc.)
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
    s->set_wb_mode(s, 0);        // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 0);           // 0 = disable , 1 = enable
    s->set_ae_level(s, 0);       // -2 to 2
    s->set_aec_value(s, 300);    // 0 to 1200
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
    s->set_bpc(s, 0);            // 0 = disable , 1 = enable
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable

    ESP_LOGI(TAG, "Camera initialized successfully");
    ESP_LOGI(TAG, "Camera sensor: PID=0x%02x VER=0x%02x MIDL=0x%02x MIDH=0x%02x",
             s->id.PID, s->id.VER, s->id.MIDL, s->id.MIDH);

    stream_state.camera_initialized = true;
    return 0;
}

/**
 * @brief HTTP handler for MJPEG stream
 */
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    ESP_LOGI(TAG, "Stream client connected from %s",
             req->sess_ctx ? (char*)req->sess_ctx : "unknown");

    stream_state.client_count++;

    // Set HTTP response headers
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        stream_state.client_count--;
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Stream loop
    while (true) {
        // Capture frame
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;

        // Send MIME boundary
        res = httpd_resp_send_chunk(req, STREAM_PART_BOUNDARY, strlen(STREAM_PART_BOUNDARY));
        if (res != ESP_OK) {
            break;
        }

        // Send JPEG content-type and length
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HEADER, _jpg_buf_len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            break;
        }

        // Send actual JPEG data
        res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        if (res != ESP_OK) {
            break;
        }

        // Return framebuffer
        esp_camera_fb_return(fb);
        fb = NULL;

        // Update stats
        stream_state.frame_count++;
        stream_state.last_frame_time = xTaskGetTickCount();

        // Thermal management: Add 100ms delay between frames (~10 fps max)
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Cleanup
    if (fb) {
        esp_camera_fb_return(fb);
    }

    stream_state.client_count--;
    ESP_LOGI(TAG, "Stream client disconnected");

    return res;
}

/**
 * @brief HTTP handler for stream info/status page
 */
static esp_err_t stream_info_handler(httpd_req_t *req) {
    char response[512];
    sensor_t *s = esp_camera_sensor_get();

    snprintf(response, sizeof(response),
             "<!DOCTYPE html><html><head><title>ESP32-CAM Stream</title></head>"
             "<body><h1>ESP32-CAM Video Stream</h1>"
             "<p>Camera: %s (PID:0x%02x VER:0x%02x)</p>"
             "<p>Resolution: %s</p>"
             "<p>Clients: %d</p>"
             "<p>Frames: %"PRIu32"</p>"
             "<p><a href=\"/stream\">View Stream</a></p>"
             "<img src=\"/stream\" width=\"640\" height=\"480\">"
             "</body></html>",
             s ? "OV3660" : "Unknown",
             s ? s->id.PID : 0,
             s ? s->id.VER : 0,
             "VGA (640x480)",
             stream_state.client_count,
             stream_state.frame_count);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, response, strlen(response));
}

int StreamInit(uint16_t stream_port) {
    ESP_LOGI(TAG, "Initializing video stream module");

    if (stream_port == 0) {
        ESP_LOGI(TAG, "Stream disabled (port = 0)");
        return 0;
    }

    // Initialize camera
    if (camera_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize camera");
        return -1;
    }

    // Create HTTP server for streaming
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = stream_port;
    config.ctrl_port = stream_port + 1;
    config.max_open_sockets = 13;  // Increased from 7 for more concurrent clients
    config.lru_purge_enable = true;
    config.send_wait_timeout = 10;  // Add send timeout
    config.recv_wait_timeout = 10;  // Add receive timeout
    config.backlog_conn = 5;  // Add connection backlog

    ESP_LOGI(TAG, "Starting stream server on port %d", stream_port);

    esp_err_t err = httpd_start(&stream_state.server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start stream server: %s", esp_err_to_name(err));
        return -1;
    }

    // Register URI handlers
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_state.server, &stream_uri);

    httpd_uri_t info_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = stream_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_state.server, &info_uri);

    stream_state.port = stream_port;

    ESP_LOGI(TAG, "Stream server started successfully");
    ESP_LOGI(TAG, "Stream available at: http://[ESP32-IP]:%d/stream", stream_port);
    ESP_LOGI(TAG, "Info page at: http://[ESP32-IP]:%d/", stream_port);

    // Initialize overlay WebSocket system
    if (OverlayInit(stream_state.server) == 0) {
        ESP_LOGI(TAG, "Overlay WebSocket initialized at: ws://[ESP32-IP]:%d/ws", stream_port);
    } else {
        ESP_LOGW(TAG, "Failed to initialize overlay WebSocket");
    }

    return 0;
}

int StreamStart(void) {
    if (!stream_state.camera_initialized) {
        ESP_LOGE(TAG, "Camera not initialized");
        return -1;
    }

    stream_state.streaming = true;
    ESP_LOGI(TAG, "Video streaming started");
    return 0;
}

void StreamStop(void) {
    stream_state.streaming = false;
    ESP_LOGI(TAG, "Video streaming stopped");
}

bool StreamIsActive(void) {
    return stream_state.streaming && stream_state.client_count > 0;
}

int StreamGetClientCount(void) {
    return stream_state.client_count;
}

float StreamGetFps(void) {
    // Simple FPS calculation based on frame count and time
    // This is a rough estimate
    if (stream_state.frame_count == 0) {
        return 0.0f;
    }

    uint32_t elapsed_ms = (xTaskGetTickCount() - stream_state.last_frame_time) * portTICK_PERIOD_MS;
    if (elapsed_ms == 0) {
        return 0.0f;
    }

    return 1000.0f / elapsed_ms;
}

void* StreamGetServerHandle(void) {
    return stream_state.server;
}
