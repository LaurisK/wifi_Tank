#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    MOTOR_LEFT  = 0,
    MOTOR_RIGHT = 1,
} motor_id_t;

typedef enum {
    MOTOR_FWD  = 0,  // RPWM active, LPWM = 0
    MOTOR_REV  = 1,  // LPWM active, RPWM = 0
    MOTOR_STOP = 2,  // Both = 0
} motor_dir_t;

typedef struct {
    int gpio_rpwm;  // Forward PWM pin (BTS7960B RPWM)
    int gpio_lpwm;  // Reverse PWM pin (BTS7960B LPWM)
} motor_pin_t;

typedef struct {
    motor_pin_t left;   // Left track
    motor_pin_t right;  // Right track
} motor_cfg_t;

/**
 * Initialise both motor drivers.
 * Configures LEDC timer and four PWM channels.
 * BTS7960B R_EN/L_EN must be tied to 5 V externally.
 *
 * @param cfg  Pin assignments for left and right motors.
 * @return     0 on success, -1 on failure.
 */
int MotorInit(const motor_cfg_t *cfg);

/**
 * Set a motor direction and speed.
 *
 * @param motor    MOTOR_LEFT or MOTOR_RIGHT.
 * @param dir      MOTOR_FWD, MOTOR_REV, or MOTOR_STOP.
 * @param percent  Duty cycle 0–100 %. Clamped to 100 if exceeded.
 * @return         0 on success, -1 on failure.
 */
int MotorSet(motor_id_t motor, motor_dir_t dir, uint8_t percent);

/**
 * Stop a single motor (equivalent to MotorSet(…, MOTOR_STOP, 0)).
 */
int MotorStop(motor_id_t motor);

/**
 * Stop both motors immediately.
 */
void MotorStopAll(void);

#ifdef __cplusplus
}
#endif
