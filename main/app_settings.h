#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All persistent settings the app cares about, kept in one struct so we
 * can pass it around easily. Mirrors what is written to NVS. */
typedef struct {
    /* --- WiFi credentials (used by wifi_manager) --- */
    char wifi_ssid[33];      /* up to 32 chars + NUL */
    char wifi_pass[65];      /* up to 64 chars + NUL */
    bool wifi_configured;    /* true once user has provided credentials */

    /* --- Stepper calibration --- */
    int32_t full_open_steps; /* steps from fully-closed (0) to fully-open (default 12288 = 3 turns) */
    int32_t last_position;   /* last known position, saved on every move-complete */

    /* --- Motion parameters --- */
    uint32_t step_period_us; /* microseconds per half-step (default 1500us) */
    bool hold_when_stopped;  /* keep coils energised when idle? default false */

    /* --- HomeKit --- */
    char hap_setup_code[11]; /* "XXX-XX-XXX\0" */
    char accessory_name[33]; /* default "AirCover" */
} app_settings_t;

/* Load settings from NVS into the in-memory copy; fills defaults if missing. */
esp_err_t app_settings_init(void);

/* Get pointer to the (read-only) live settings. */
const app_settings_t *app_settings_get(void);

/* Save the entire struct to NVS. Call after mutating fields via the helpers below. */
esp_err_t app_settings_save(void);

/* Reset everything to defaults and persist (used by factory-reset). */
esp_err_t app_settings_factory_reset(void);

/* Convenience setters that also persist. They take a copy of the input. */
esp_err_t app_settings_set_wifi(const char *ssid, const char *pass);
esp_err_t app_settings_set_full_open(int32_t steps);
esp_err_t app_settings_set_last_position(int32_t pos);
esp_err_t app_settings_set_step_period(uint32_t us);
esp_err_t app_settings_set_hold(bool hold);
esp_err_t app_settings_set_name(const char *name);
esp_err_t app_settings_set_setup_code(const char *code);

#ifdef __cplusplus
}
#endif
