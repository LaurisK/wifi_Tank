#include "control.h"
#include "motor.h"

#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "control";

static int s_motor_l = 0;
static int s_motor_r = 0;

// ---------------------------------------------------------------------------
// Motor value application
// Protocol: {"L": <-100..100>, "R": <-100..100>}
//   positive = forward, negative = reverse, 0 = stop
// ---------------------------------------------------------------------------

static void apply_motor_values(int l, int r)
{
    if (l >  100) l =  100;
    if (l < -100) l = -100;
    if (r >  100) r =  100;
    if (r < -100) r = -100;

    if (l > 0)       MotorSet(MOTOR_LEFT, MOTOR_FWD, (uint8_t) l);
    else if (l < 0)  MotorSet(MOTOR_LEFT, MOTOR_REV, (uint8_t)-l);
    else             MotorStop(MOTOR_LEFT);

    if (r > 0)       MotorSet(MOTOR_RIGHT, MOTOR_FWD, (uint8_t) r);
    else if (r < 0)  MotorSet(MOTOR_RIGHT, MOTOR_REV, (uint8_t)-r);
    else             MotorStop(MOTOR_RIGHT);

    s_motor_l = l;
    s_motor_r = r;
}

void ControlGetMotorState(int *l, int *r)
{
    if (l) *l = s_motor_l;
    if (r) *r = s_motor_r;
}

// ---------------------------------------------------------------------------
// WebSocket handler
// ---------------------------------------------------------------------------

static esp_err_t ctrl_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Initial WebSocket handshake
        ESP_LOGI(TAG, "client connected fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Receive frame header first (len=0 probes the frame size)
    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv header failed (%s) — stopping motors", esp_err_to_name(ret));
        MotorStopAll();
        return ret;
    }

    // Handle control frames without payload
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "client closed — stopping motors");
        MotorStopAll();
        return ESP_OK;
    }

    if (pkt.type == HTTPD_WS_TYPE_PING) {
        pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &pkt);
    }

    if (pkt.len == 0 || pkt.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    // Receive payload
    uint8_t *buf = calloc(1, pkt.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv payload failed (%s) — stopping motors", esp_err_to_name(ret));
        MotorStopAll();
        free(buf);
        return ret;
    }

    // Parse {"L": <-100..100>, "R": <-100..100>}
    cJSON *root = cJSON_Parse((char *)buf);
    if (root) {
        cJSON *lj = cJSON_GetObjectItem(root, "L");
        cJSON *rj = cJSON_GetObjectItem(root, "R");
        if (cJSON_IsNumber(lj) && cJSON_IsNumber(rj)) {
            apply_motor_values((int)lj->valuedouble, (int)rj->valuedouble);
        } else {
            ESP_LOGW(TAG, "unexpected frame: %s", buf);
        }
        cJSON_Delete(root);
    }

    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------

int ControlInit(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "null server handle");
        return -1;
    }

    httpd_uri_t uri = {
        .uri                     = "/ctrl",
        .method                  = HTTP_GET,
        .handler                 = ctrl_ws_handler,
        .user_ctx                = NULL,
        .is_websocket            = true,
        .handle_ws_control_frames = true,
    };

    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register /ctrl WebSocket");
        return -1;
    }

    ESP_LOGI(TAG, "init ok — ws://<host>/ctrl");
    return 0;
}
