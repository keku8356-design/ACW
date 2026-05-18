#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HomeKit accessory (Window Covering service).
 * Must be called after WiFi is up and after stepper_init(). */
esp_err_t homekit_service_start(void);

/* Called by application (or by stepper done-callback) to refresh the
 * Current Position / Position State characteristics. The position is in
 * raw stepper steps; this function maps it to the 0..100 HK percent. */
void homekit_service_publish_current(int32_t step_position, bool moving);

#ifdef __cplusplus
}
#endif
