#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Half-step driver for two 28BYJ-48 stepper motors via ULN2003 boards.
 *
 * Both motors are commanded with the same half-step pattern in lock-step,
 * so they advance/retreat together.  Position is stored as a signed step
 * count where 0 = "fully closed" (the calibrated zero) and positive values
 * correspond to "more open".
 *
 * Stepping is timer-driven (esp_timer), so callers never block. */

typedef struct {
    int32_t pin_a[4];   /* IN1..IN4 of motor A (ULN2003 inputs) */
    int32_t pin_b[4];   /* IN1..IN4 of motor B */
} stepper_pins_t;

/* Callback fired (from the timer task) when current_position reaches target. */
typedef void (*stepper_done_cb_t)(int32_t final_position);

/* Initialise GPIOs and timer. Restores `start_position` as the assumed
 * current position (e.g. last value persisted to NVS). */
esp_err_t stepper_init(const stepper_pins_t *pins, int32_t start_position);

/* Begin moving toward target. Returns immediately. */
void stepper_move_to(int32_t target);

/* Relative jog. */
void stepper_jog(int32_t delta);

/* Halt motion at the current position. */
void stepper_stop(void);

int32_t stepper_get_position(void);
int32_t stepper_get_target(void);
bool    stepper_is_moving(void);

/* Teach: forcibly declare the current position to be `pos` (used during
 * calibration -- the user has manually jogged to a known endpoint). */
void stepper_set_position(int32_t pos);

/* Change the half-step period (us). Takes effect on the next move. */
void stepper_set_period(uint32_t us);

/* Whether coils stay energised after motion ends (true = lock, false = release). */
void stepper_set_hold(bool hold);

/* Registers a single callback invoked when a move completes. */
void stepper_register_done_cb(stepper_done_cb_t cb);

#ifdef __cplusplus
}
#endif
