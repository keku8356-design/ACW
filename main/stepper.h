#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Half-step driver for two 28BYJ-48 stepper motors via ULN2003 boards.
 *
 * Each motor maintains its own phase and step counter so it can be jogged
 * independently of the other (useful for trimming when one rope is slightly
 * longer than the other). Synchronous operations move both motors in step.
 *
 * Position semantics:
 *   - 0  = "fully closed" (calibrated zero)
 *   - positive = "more open"
 *   - LEFT and RIGHT track independently
 *   - For HomeKit purposes the LEFT motor's position is treated as the
 *     "primary" position (returned by the no-arg getters below).
 *
 * Stepping is timer-driven (esp_timer), so callers never block. */

typedef enum {
    MOTOR_BOTH  = 0,
    MOTOR_LEFT  = 1,
    MOTOR_RIGHT = 2,
} motor_id_t;

typedef struct {
    int32_t pin_left[4];    /* IN1..IN4 of left motor (ULN2003 inputs) */
    int32_t pin_right[4];   /* IN1..IN4 of right motor */
} stepper_pins_t;

typedef void (*stepper_done_cb_t)(int32_t final_position);
typedef void (*stepper_start_cb_t)(void);

esp_err_t stepper_init(const stepper_pins_t *pins, int32_t start_position);

/* Synchronous absolute move: both motors aim for `target`. */
void stepper_move_to(int32_t target);

/* Synchronous relative jog (kept for back-compat; same as jog_motor(BOTH,delta)). */
void stepper_jog(int32_t delta);

/* Relative jog for the selected motor(s). */
void stepper_jog_motor(motor_id_t which, int32_t delta);

/* Halt motion at the current position (all motors). */
void stepper_stop(void);

/* No-arg variants return LEFT (primary). */
int32_t stepper_get_position(void);
int32_t stepper_get_target(void);
bool    stepper_is_moving(void);

int32_t stepper_get_motor_position(motor_id_t which);
int32_t stepper_get_motor_target  (motor_id_t which);

/* Teach: forcibly declare the selected motor(s)' current position to be `pos`. */
void stepper_set_position(motor_id_t which, int32_t pos);

void stepper_set_period(uint32_t us);
void stepper_set_hold  (bool hold);

/* Position quantisation. When `steps` > 0, stepper_move_to() targets are
 * rounded to the nearest multiple. Pass 0 to disable. Has no effect on
 * stepper_jog_motor() so manual trimming stays at single-step precision. */
void stepper_set_detent(int32_t steps);

void stepper_register_done_cb (stepper_done_cb_t  cb);
void stepper_register_start_cb(stepper_start_cb_t cb);

#ifdef __cplusplus
}
#endif
