#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WS2812 RGB LED state-machine indicator.
 *
 * Three orthogonal signals are tracked. The displayed colour is chosen by
 * priority (moving > wifi state):
 *
 *   wifi=BOOT         -> dim white
 *   wifi=CONNECTING   -> slow blue blink
 *   wifi=SOFTAP       -> fast orange blink
 *   wifi=CONNECTED, paired=false -> yellow breathing
 *   wifi=CONNECTED, paired=true  -> dim green steady
 *   moving=true       -> blue pulse (overrides everything)
 */

typedef enum {
    LED_WIFI_BOOT,
    LED_WIFI_CONNECTING,
    LED_WIFI_SOFTAP,
    LED_WIFI_CONNECTED,
} led_wifi_phase_t;

esp_err_t led_indicator_init(int gpio);
void led_indicator_set_wifi(led_wifi_phase_t phase);
void led_indicator_set_paired(bool paired);
void led_indicator_set_moving(bool moving);

#ifdef __cplusplus
}
#endif
