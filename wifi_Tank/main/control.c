#include "control.h"
#include "motor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

#include <string.h>
#include <math.h>

static const char *TAG = "control";

// ---------------------------------------------------------------------------
// Ramp configuration
// ---------------------------------------------------------------------------

#define RAMP_DEFAULT_RATE_PCT_S   50   // %/s — default acceleration/deceleration
#define RAMP_TICK_MS              20   // ramp task period (50 Hz)

#define NVS_NAMESPACE  "ctrl"
#define NVS_KEY_RAMP   "ramp_rate"

// ---------------------------------------------------------------------------
// Shared state (written by WS handler, read by ramp task)
// Simple int reads/writes are atomic on ESP32, volatile is sufficient.
// ---------------------------------------------------------------------------

static volatile int s_target_l   = 0;
static volatile int s_target_r   = 0;
static volatile int s_ramp_rate  = RAMP_DEFAULT_RATE_PCT_S;

// Actual values currently applied to hardware — only written by ramp task.
static int s_motor_l = 0;
static int s_motor_r = 0;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static void nvs_save_ramp_rate(int rate)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_RAMP, (int32_t)rate);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int nvs_load_ramp_rate(void)
{
    nvs_handle_t h;
    int32_t val = RAMP_DEFAULT_RATE_PCT_S;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_RAMP, &val);
        nvs_close(h);
    }
    return (int)val;
}

// ---------------------------------------------------------------------------
// Motor application (hardware layer)
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

// ---------------------------------------------------------------------------
// Ramp task — runs at RAMP_TICK_MS interval, steps current toward target
// ---------------------------------------------------------------------------

static void ramp_task(void *arg)
{
    float cur_l = 0.0f;
    float cur_r = 0.0f;
    const float tick_s = RAMP_TICK_MS / 1000.0f;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RAMP_TICK_MS));

        int tgt_l = s_target_l;
        int tgt_r = s_target_r;
        float step = (float)s_ramp_rate * tick_s;

        float diff_l = (float)tgt_l - cur_l;
        if (fabsf(diff_l) <= step) {
            cur_l = (float)tgt_l;
        } else {
            cur_l += (diff_l > 0.0f ? step : -step);
        }

        float diff_r = (float)tgt_r - cur_r;
        if (fabsf(diff_r) <= step) {
            cur_r = (float)tgt_r;
        } else {
            cur_r += (diff_r > 0.0f ? step : -step);
        }

        apply_motor_values((int)roundf(cur_l), (int)roundf(cur_r));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ControlGetMotorState(int *l, int *r)
{
    if (l) *l = s_motor_l;
    if (r) *r = s_motor_r;
}

void ControlSetRampRate(int rate)
{
    if (rate <   1) rate =   1;
    if (rate > 500) rate = 500;
    s_ramp_rate = rate;
    nvs_save_ramp_rate(rate);
    ESP_LOGI(TAG, "ramp rate set to %d %%/s", rate);
}

int ControlGetRampRate(void)
{
    return s_ramp_rate;
}

// ---------------------------------------------------------------------------
// WebSocket handler — sets targets only; ramp task drives hardware
// ---------------------------------------------------------------------------

static esp_err_t ctrl_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "client connected fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv header failed (%s) — zeroing targets", esp_err_to_name(ret));
        s_target_l = 0;
        s_target_r = 0;
        return ret;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "client closed — zeroing targets");
        s_target_l = 0;
        s_target_r = 0;
        return ESP_OK;
    }

    if (pkt.type == HTTPD_WS_TYPE_PING) {
        pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &pkt);
    }

    if (pkt.len == 0 || pkt.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv payload failed (%s) — zeroing targets", esp_err_to_name(ret));
        s_target_l = 0;
        s_target_r = 0;
        free(buf);
        return ret;
    }


    cJSON *root = cJSON_Parse((char *)buf);
    if (root) {
        cJSON *lj = cJSON_GetObjectItem(root, "L");
        cJSON *rj = cJSON_GetObjectItem(root, "R");
        if (cJSON_IsNumber(lj) && cJSON_IsNumber(rj)) {
            int new_l = (int)lj->valuedouble;
            int new_r = (int)rj->valuedouble;
            if (new_l >  100) new_l =  100;
            if (new_l < -100) new_l = -100;
            if (new_r >  100) new_r =  100;
            if (new_r < -100) new_r = -100;
            s_target_l = new_l;
            s_target_r = new_r;
        } else {
            ESP_LOGW(TAG, "unexpected frame: %s", buf);
        }
        cJSON_Delete(root);
    }

    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

int ControlInit(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "null server handle");
        return -1;
    }

    s_ramp_rate = nvs_load_ramp_rate();
    ESP_LOGI(TAG, "ramp rate loaded: %d %%/s", s_ramp_rate);

    xTaskCreate(ramp_task, "ctrl_ramp", 2048, NULL, 5, NULL);

    httpd_uri_t uri = {
        .uri                      = "/ctrl",
        .method                   = HTTP_GET,
        .handler                  = ctrl_ws_handler,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        .handle_ws_control_frames = true,
    };

    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register /ctrl WebSocket");
        return -1;
    }

    ESP_LOGI(TAG, "init ok — ws://<host>/ctrl");
    return 0;
}
