#include "app_settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG       = "settings";
static const char *NVS_NS    = "aircover";
static const char *KEY_BLOB  = "cfg_v1";  /* whole struct stored as a blob */

static app_settings_t s_cfg;

static void load_defaults(app_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->wifi_configured  = false;
    c->full_open_steps  = 12288;          /* 3 full revolutions in half-step */
    c->last_position    = 0;
    c->step_period_us   = 1500;           /* ~667 half-steps/s, comfortable */
    c->hold_when_stopped = false;
    /* "111-22-333" is the canonical Apple HomeKit example setup code.
     * It is fine for development; the user can change it via Web UI later. */
    strcpy(c->hap_setup_code, "111-22-333");
    strcpy(c->accessory_name, "AirCover");
    c->detent_steps = 0;                 /* off by default */
    c->invert_direction = false;
}

esp_err_t app_settings_init(void)
{
    /* nvs_flash_init is performed once in main() before this is called. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS namespace yet, using defaults");
        load_defaults(&s_cfg);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    /* Forward-compatible NVS read:
     *
     *   1. Query the stored blob length.
     *   2. Pre-fill s_cfg with defaults so any field added in a newer firmware
     *      version that isn't present in the saved blob keeps its default.
     *   3. Copy at most sizeof(s_cfg) bytes from NVS over the top.
     *
     * This means we can append fields to app_settings_t in new firmware and
     * old NVS blobs still load cleanly across an OTA update. Two rules:
     *   - Never reorder existing fields, only append.
     *   - load_defaults() must give every new field a sensible default. */
    size_t stored_sz = 0;
    err = nvs_get_blob(h, KEY_BLOB, NULL, &stored_sz);
    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGW(TAG, "NVS blob missing (err=0x%x), using defaults", err);
        load_defaults(&s_cfg);
        return ESP_OK;
    }

    load_defaults(&s_cfg);
    size_t to_read = stored_sz < sizeof(s_cfg) ? stored_sz : sizeof(s_cfg);
    err = nvs_get_blob(h, KEY_BLOB, &s_cfg, &to_read);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS read failed (err=0x%x), using defaults", err);
        load_defaults(&s_cfg);
        return ESP_OK;
    }

    if (stored_sz < sizeof(s_cfg)) {
        ESP_LOGW(TAG, "NVS blob smaller than struct (%u < %u), new fields kept at defaults",
                 (unsigned)stored_sz, (unsigned)sizeof(s_cfg));
        /* Persist immediately so the on-disk blob matches the new layout. */
        app_settings_save();
    } else if (stored_sz > sizeof(s_cfg)) {
        ESP_LOGW(TAG, "NVS blob larger than struct (%u > %u, downgrade?), tail ignored",
                 (unsigned)stored_sz, (unsigned)sizeof(s_cfg));
    }

    ESP_LOGI(TAG, "loaded: wifi=%s full_open=%d period=%uus hold=%d detent=%d invert=%d",
             s_cfg.wifi_configured ? "yes" : "no",
             (int)s_cfg.full_open_steps,
             (unsigned)s_cfg.step_period_us,
             (int)s_cfg.hold_when_stopped,
             (int)s_cfg.detent_steps,
             (int)s_cfg.invert_direction);
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_cfg;
}

esp_err_t app_settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, KEY_BLOB, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_settings_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset!");
    /* Erase the entire default namespace too (HomeKit pairings, etc). */
    nvs_flash_erase();
    nvs_flash_init();
    load_defaults(&s_cfg);
    return app_settings_save();
}

esp_err_t app_settings_set_wifi(const char *ssid, const char *pass)
{
    strncpy(s_cfg.wifi_ssid, ssid ? ssid : "", sizeof(s_cfg.wifi_ssid) - 1);
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';
    strncpy(s_cfg.wifi_pass, pass ? pass : "", sizeof(s_cfg.wifi_pass) - 1);
    s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    s_cfg.wifi_configured = s_cfg.wifi_ssid[0] != '\0';
    return app_settings_save();
}

esp_err_t app_settings_set_full_open(int32_t steps)
{
    if (steps < 100) steps = 100;   /* sanity */
    s_cfg.full_open_steps = steps;
    return app_settings_save();
}

esp_err_t app_settings_set_last_position(int32_t pos)
{
    s_cfg.last_position = pos;
    return app_settings_save();
}

esp_err_t app_settings_set_step_period(uint32_t us)
{
    if (us < 900)   us = 900;       /* 28BYJ-48 won't keep up below ~1ms */
    if (us > 20000) us = 20000;
    s_cfg.step_period_us = us;
    return app_settings_save();
}

esp_err_t app_settings_set_hold(bool hold)
{
    s_cfg.hold_when_stopped = hold;
    return app_settings_save();
}

esp_err_t app_settings_set_name(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    strncpy(s_cfg.accessory_name, name, sizeof(s_cfg.accessory_name) - 1);
    s_cfg.accessory_name[sizeof(s_cfg.accessory_name) - 1] = '\0';
    return app_settings_save();
}

esp_err_t app_settings_set_setup_code(const char *code)
{
    if (!code || strlen(code) != 10) return ESP_ERR_INVALID_ARG;  /* XXX-XX-XXX */
    strncpy(s_cfg.hap_setup_code, code, sizeof(s_cfg.hap_setup_code) - 1);
    s_cfg.hap_setup_code[sizeof(s_cfg.hap_setup_code) - 1] = '\0';
    return app_settings_save();
}

esp_err_t app_settings_set_detent(int32_t steps)
{
    if (steps < 0)      steps = 0;
    /* Cap at half the full travel so at least 2 detents exist when enabled. */
    if (steps > s_cfg.full_open_steps / 2) steps = s_cfg.full_open_steps / 2;
    s_cfg.detent_steps = steps;
    return app_settings_save();
}

esp_err_t app_settings_set_invert(bool invert)
{
    s_cfg.invert_direction = invert;
    return app_settings_save();
}
