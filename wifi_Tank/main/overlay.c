/*! \file overlay.c
\brief Video overlay management implementation with WebSocket support
*******************************************************************************/

#include "overlay.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "OVERLAY";

// WebSocket client tracking
#define MAX_WS_CLIENTS 8

typedef struct {
    int fd;
    bool connected;
} ws_client_t;

// Overlay state
static struct {
    httpd_handle_t server;
    ws_client_t clients[MAX_WS_CLIENTS];
    int client_count;
    bool initialized;
} overlay_state = {
    .server = NULL,
    .client_count = 0,
    .initialized = false
};

/**
 * @brief Convert overlay data to JSON string
 */
static char* overlay_to_json(const overlay_data_t *overlay) {
    if (overlay == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON root object");
        return NULL;
    }

    // Add text array
    cJSON *text_array = cJSON_CreateArray();
    for (int i = 0; i < overlay->text_count && i < OVERLAY_MAX_TEXT; i++) {
        cJSON *text_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(text_obj, "content", overlay->texts[i].content);
        cJSON_AddNumberToObject(text_obj, "x", overlay->texts[i].x);
        cJSON_AddNumberToObject(text_obj, "y", overlay->texts[i].y);
        cJSON_AddStringToObject(text_obj, "color", overlay->texts[i].color);
        cJSON_AddNumberToObject(text_obj, "size", overlay->texts[i].size);
        cJSON_AddItemToArray(text_array, text_obj);
    }
    cJSON_AddItemToObject(root, "text", text_array);

    // Add shapes array
    cJSON *shapes_array = cJSON_CreateArray();
    for (int i = 0; i < overlay->shape_count && i < OVERLAY_MAX_SHAPES; i++) {
        cJSON *shape_obj = cJSON_CreateObject();

        const overlay_shape_t *shape = &overlay->shapes[i];

        // Add type
        const char *type_str = "line";
        if (shape->type == OVERLAY_SHAPE_RECT) type_str = "rect";
        else if (shape->type == OVERLAY_SHAPE_CIRCLE) type_str = "circle";
        cJSON_AddStringToObject(shape_obj, "type", type_str);

        // Add coordinates based on type
        if (shape->type == OVERLAY_SHAPE_LINE) {
            cJSON_AddNumberToObject(shape_obj, "x1", shape->x1);
            cJSON_AddNumberToObject(shape_obj, "y1", shape->y1);
            cJSON_AddNumberToObject(shape_obj, "x2", shape->x2);
            cJSON_AddNumberToObject(shape_obj, "y2", shape->y2);
            cJSON_AddNumberToObject(shape_obj, "width", shape->width);
        } else if (shape->type == OVERLAY_SHAPE_RECT) {
            cJSON_AddNumberToObject(shape_obj, "x", shape->x1);
            cJSON_AddNumberToObject(shape_obj, "y", shape->y1);
            cJSON_AddNumberToObject(shape_obj, "w", shape->x2);
            cJSON_AddNumberToObject(shape_obj, "h", shape->y2);
            cJSON_AddBoolToObject(shape_obj, "fill", shape->fill);
        } else if (shape->type == OVERLAY_SHAPE_CIRCLE) {
            cJSON_AddNumberToObject(shape_obj, "x", shape->x1);
            cJSON_AddNumberToObject(shape_obj, "y", shape->y1);
            cJSON_AddNumberToObject(shape_obj, "r", shape->radius);
            cJSON_AddBoolToObject(shape_obj, "fill", shape->fill);
        }

        cJSON_AddStringToObject(shape_obj, "color", shape->color);
        cJSON_AddItemToArray(shapes_array, shape_obj);
    }
    cJSON_AddItemToObject(root, "shapes", shapes_array);

    // Convert to string
    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_string;
}

/**
 * @brief WebSocket handler for overlay updates
 */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake initiated");
        return ESP_OK;
    }

    // Handle WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // First call to get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len) {
        // Allocate buffer for payload
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WebSocket payload");
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = buf;

        // Receive the frame payload
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }

        // Handle different frame types
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGI(TAG, "Received WebSocket message: %s", ws_pkt.payload);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            // Respond to ping with pong
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
            httpd_ws_send_frame(req, &ws_pkt);
        }

        free(buf);
    }

    return ESP_OK;
}

/**
 * @brief Async send function for WebSocket
 */
static void ws_async_send(void *arg) {
    httpd_ws_frame_t *ws_pkt = (httpd_ws_frame_t *)arg;
    httpd_handle_t hd = overlay_state.server;

    if (!hd) {
        free(ws_pkt->payload);
        free(ws_pkt);
        return;
    }

    // Send to all connected WebSocket clients
    size_t clients = 0;
    int max_clients = sizeof(overlay_state.clients) / sizeof(overlay_state.clients[0]);

    for (int i = 0; i < max_clients; i++) {
        if (overlay_state.clients[i].connected) {
            int client_fd = overlay_state.clients[i].fd;

            // Check if client is still connected
            httpd_ws_client_info_t client_info = httpd_ws_get_fd_info(hd, client_fd);
            if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) {
                esp_err_t ret = httpd_ws_send_frame_async(hd, client_fd, ws_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send to client fd=%d: %s", client_fd, esp_err_to_name(ret));
                    overlay_state.clients[i].connected = false;
                    overlay_state.client_count--;
                } else {
                    clients++;
                }
            } else {
                // Client disconnected
                overlay_state.clients[i].connected = false;
                overlay_state.client_count--;
            }
        }
    }

    ESP_LOGI(TAG, "Sent overlay update to %d WebSocket clients", clients);

    free(ws_pkt->payload);
    free(ws_pkt);
}

int OverlayInit(httpd_handle_t server) {
    if (server == NULL) {
        ESP_LOGE(TAG, "Invalid server handle");
        return -1;
    }

    ESP_LOGI(TAG, "Initializing overlay WebSocket system");

    overlay_state.server = server;

    // Initialize client tracking
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        overlay_state.clients[i].fd = -1;
        overlay_state.clients[i].connected = false;
    }

    // Register WebSocket handler
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true
    };

    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(ret));
        return -1;
    }

    overlay_state.initialized = true;
    ESP_LOGI(TAG, "Overlay WebSocket initialized on /ws endpoint");

    return 0;
}

int OverlaySendUpdate(const overlay_data_t *overlay) {
    if (!overlay_state.initialized || overlay == NULL) {
        return -1;
    }

    // Convert overlay to JSON
    char *json = overlay_to_json(overlay);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to convert overlay to JSON");
        return -1;
    }

    ESP_LOGD(TAG, "Overlay JSON: %s", json);

    // Create WebSocket frame
    httpd_ws_frame_t *ws_pkt = calloc(1, sizeof(httpd_ws_frame_t));
    if (ws_pkt == NULL) {
        ESP_LOGE(TAG, "Failed to allocate WebSocket frame");
        free(json);
        return -1;
    }

    ws_pkt->payload = (uint8_t *)json;
    ws_pkt->len = strlen(json);
    ws_pkt->type = HTTPD_WS_TYPE_TEXT;

    // Update client list by checking all possible file descriptors
    // This is a simple approach - we'll track clients when they connect
    httpd_handle_t hd = overlay_state.server;
    overlay_state.client_count = 0;

    // Scan for WebSocket clients
    for (int fd = 3; fd < CONFIG_LWIP_MAX_SOCKETS; fd++) {
        httpd_ws_client_info_t client_info = httpd_ws_get_fd_info(hd, fd);
        if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) {
            // Check if this fd is already tracked
            bool found = false;
            for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                if (overlay_state.clients[i].fd == fd) {
                    found = true;
                    overlay_state.clients[i].connected = true;
                    break;
                }
            }

            // Add new client
            if (!found) {
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (!overlay_state.clients[i].connected) {
                        overlay_state.clients[i].fd = fd;
                        overlay_state.clients[i].connected = true;
                        ESP_LOGI(TAG, "New WebSocket client tracked: fd=%d", fd);
                        break;
                    }
                }
            }
            overlay_state.client_count++;
        }
    }

    if (overlay_state.client_count == 0) {
        ESP_LOGW(TAG, "No WebSocket clients connected");
        free(json);
        free(ws_pkt);
        return 0;
    }

    // Queue async send
    ws_async_send(ws_pkt);

    return overlay_state.client_count;
}

void OverlayCreateSampleData(overlay_data_t *overlay) {
    if (overlay == NULL) {
        return;
    }

    memset(overlay, 0, sizeof(overlay_data_t));

    // Add sample text overlays
    overlay->text_count = 3;

    snprintf(overlay->texts[0].content, OVERLAY_MAX_TEXT_LENGTH, "ESP32 WiFi Tank");
    overlay->texts[0].x = 10;
    overlay->texts[0].y = 30;
    strncpy(overlay->texts[0].color, "white", OVERLAY_MAX_COLOR_LENGTH);
    overlay->texts[0].size = 20;

    snprintf(overlay->texts[1].content, OVERLAY_MAX_TEXT_LENGTH, "Speed: 50%%");
    overlay->texts[1].x = 10;
    overlay->texts[1].y = 60;
    strncpy(overlay->texts[1].color, "lime", OVERLAY_MAX_COLOR_LENGTH);
    overlay->texts[1].size = 16;

    snprintf(overlay->texts[2].content, OVERLAY_MAX_TEXT_LENGTH, "Battery: 85%%");
    overlay->texts[2].x = 10;
    overlay->texts[2].y = 85;
    strncpy(overlay->texts[2].color, "cyan", OVERLAY_MAX_COLOR_LENGTH);
    overlay->texts[2].size = 16;

    // Add sample shapes
    overlay->shape_count = 4;

    // Vertical crosshair line
    overlay->shapes[0].type = OVERLAY_SHAPE_LINE;
    overlay->shapes[0].x1 = 640;
    overlay->shapes[0].y1 = 0;
    overlay->shapes[0].x2 = 640;
    overlay->shapes[0].y2 = 720;
    strncpy(overlay->shapes[0].color, "red", OVERLAY_MAX_COLOR_LENGTH);
    overlay->shapes[0].width = 2;

    // Horizontal crosshair line
    overlay->shapes[1].type = OVERLAY_SHAPE_LINE;
    overlay->shapes[1].x1 = 0;
    overlay->shapes[1].y1 = 360;
    overlay->shapes[1].x2 = 1280;
    overlay->shapes[1].y2 = 360;
    strncpy(overlay->shapes[1].color, "red", OVERLAY_MAX_COLOR_LENGTH);
    overlay->shapes[1].width = 2;

    // Target rectangle
    overlay->shapes[2].type = OVERLAY_SHAPE_RECT;
    overlay->shapes[2].x1 = 500;
    overlay->shapes[2].y1 = 250;
    overlay->shapes[2].x2 = 100;  // width
    overlay->shapes[2].y2 = 80;   // height
    strncpy(overlay->shapes[2].color, "yellow", OVERLAY_MAX_COLOR_LENGTH);
    overlay->shapes[2].fill = false;

    // Status indicator circle
    overlay->shapes[3].type = OVERLAY_SHAPE_CIRCLE;
    overlay->shapes[3].x1 = 1250;
    overlay->shapes[3].y1 = 30;
    overlay->shapes[3].radius = 15;
    strncpy(overlay->shapes[3].color, "lime", OVERLAY_MAX_COLOR_LENGTH);
    overlay->shapes[3].fill = true;
}

int OverlayGetClientCount(void) {
    return overlay_state.client_count;
}
