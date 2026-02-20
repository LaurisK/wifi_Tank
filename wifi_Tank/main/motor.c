#include "motor.h"

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "motor";

// Camera XCLK occupies LEDC_TIMER_0 / LEDC_CHANNEL_0 — use timer 1 and channels 1-4.
#define MOTOR_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define MOTOR_LEDC_TIMER     LEDC_TIMER_1
#define MOTOR_LEDC_RES       LEDC_TIMER_10_BIT
#define MOTOR_LEDC_FREQ_HZ   20000U   // 20 kHz — above audible range
#define MOTOR_MAX_DUTY       (1U << 10) // 1024 = 100 %

// Fixed channel assignment: [left_rpwm, left_lpwm, right_rpwm, right_lpwm]
static const ledc_channel_t CHANNELS[4] = {
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
};

typedef struct {
    ledc_channel_t rpwm_ch;
    ledc_channel_t lpwm_ch;
} motor_ch_t;

static struct {
    motor_ch_t motors[2];
    bool initialized;
} s = {0};

// ---------------------------------------------------------------------------

int MotorInit(const motor_cfg_t *cfg)
{
    if (!cfg) {
        ESP_LOGE(TAG, "null config");
        return -1;
    }

    ledc_timer_config_t timer = {
        .speed_mode      = MOTOR_LEDC_MODE,
        .duty_resolution = MOTOR_LEDC_RES,
        .timer_num       = MOTOR_LEDC_TIMER,
        .freq_hz         = MOTOR_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed");
        return -1;
    }

    int gpios[4] = {
        cfg->left.gpio_rpwm,
        cfg->left.gpio_lpwm,
        cfg->right.gpio_rpwm,
        cfg->right.gpio_lpwm,
    };

    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = gpios[i],
            .speed_mode = MOTOR_LEDC_MODE,
            .channel    = CHANNELS[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = MOTOR_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        if (ledc_channel_config(&ch) != ESP_OK) {
            ESP_LOGE(TAG, "LEDC channel %d config failed (gpio %d)", i, gpios[i]);
            return -1;
        }
    }

    s.motors[MOTOR_LEFT]  = (motor_ch_t){ CHANNELS[0], CHANNELS[1] };
    s.motors[MOTOR_RIGHT] = (motor_ch_t){ CHANNELS[2], CHANNELS[3] };
    s.initialized = true;

    ESP_LOGI(TAG, "init ok — left(rpwm=%d lpwm=%d) right(rpwm=%d lpwm=%d)",
             cfg->left.gpio_rpwm,  cfg->left.gpio_lpwm,
             cfg->right.gpio_rpwm, cfg->right.gpio_lpwm);
    return 0;
}

// ---------------------------------------------------------------------------

int MotorSet(motor_id_t motor, motor_dir_t dir, uint8_t percent)
{
    if (!s.initialized) {
        ESP_LOGE(TAG, "not initialized");
        return -1;
    }
    if ((int)motor > MOTOR_RIGHT) {
        ESP_LOGE(TAG, "invalid motor %d", motor);
        return -1;
    }
    if (percent > 100) percent = 100;

    uint32_t duty = (MOTOR_MAX_DUTY * percent) / 100;

    uint32_t rpwm_duty = 0;
    uint32_t lpwm_duty = 0;
    switch (dir) {
        case MOTOR_FWD:  rpwm_duty = duty; break;
        case MOTOR_REV:  lpwm_duty = duty; break;
        case MOTOR_STOP: break;
        default:
            ESP_LOGE(TAG, "invalid dir %d", dir);
            return -1;
    }

    const motor_ch_t *m = &s.motors[motor];
    ledc_set_duty(MOTOR_LEDC_MODE, m->rpwm_ch, rpwm_duty);
    ledc_update_duty(MOTOR_LEDC_MODE, m->rpwm_ch);
    ledc_set_duty(MOTOR_LEDC_MODE, m->lpwm_ch, lpwm_duty);
    ledc_update_duty(MOTOR_LEDC_MODE, m->lpwm_ch);

    ESP_LOGD(TAG, "motor=%d dir=%d %u%% (duty=%lu/%u)",
             motor, dir, percent, (unsigned long)duty, MOTOR_MAX_DUTY);
    return 0;
}

// ---------------------------------------------------------------------------

int MotorStop(motor_id_t motor)
{
    return MotorSet(motor, MOTOR_STOP, 0);
}

void MotorStopAll(void)
{
    MotorStop(MOTOR_LEFT);
    MotorStop(MOTOR_RIGHT);
}
