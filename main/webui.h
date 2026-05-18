#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the HTTP + WebSocket server on port 80. Should be called after
 * stepper_init + wifi_manager STA-up. */
esp_err_t webui_start(void);

/* Signal whether HomeKit is paired (so the UI badge can reflect it). */
void webui_set_hk_paired(bool paired);

#ifdef __cplusplus
}
#endif
