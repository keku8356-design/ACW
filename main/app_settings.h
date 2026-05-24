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

    /* --- Detent / position snap ---
     * Quantises stepper_move_to() targets to multiples of this many half-steps,
     * so that motion-stop positions land on a finite set of "azimuths" and
     * small per-motor errors don't accumulate over time. 0 = disabled.
     *
     * Examples for the default full_open_steps=12288 (3 revs):
     *   detent_steps=256  →  48 detents over the whole travel  (16 per rev)
     *   detent_steps=128  →  96 detents (32 per rev, fine)
     *   detent_steps=512  →  24 detents (8 per rev, coarse)
     *
     * Note: this field must stay at the END of the struct so that future
     * additions don't break the NVS migration in app_settings_init(). */
    int32_t detent_steps;

    /* Reverse the physical rotation direction of both motors without
     * touching logical position semantics. Use when the mechanism is
     * installed such that "open" and "close" come out swapped. Toggle
     * from the Web UI; no recalibration needed. */
    bool invert_direction;
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
esp_err_t app_settings_set_detent(int32_t steps);
esp_err_t app_settings_set_invert(bool invert);

#ifdef __cplusplus
}
#endif
