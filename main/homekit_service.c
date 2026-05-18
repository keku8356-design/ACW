#include "homekit_service.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"

#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"

#include "app_settings.h"
#include "stepper.h"

static const char *TAG = "homekit";

#define POS_STATE_DECREASING 0
#define POS_STATE_INCREASING 1
#define POS_STATE_STOPPED    2

static hap_acc_t  *s_acc      = NULL;
static hap_serv_t *s_wc       = NULL;
static hap_char_t *s_c_current = NULL;
static hap_char_t *s_c_target  = NULL;
static hap_char_t *s_c_state   = NULL;

/* Convert raw step count to HomeKit percent (0..100). */
static uint8_t steps_to_percent(int32_t steps)
{
    int32_t full = app_settings_get()->full_open_steps;
    if (full <= 0) return 0;
    if (steps < 0) steps = 0;
    if (steps > full) steps = full;
    return (uint8_t)((steps * 100 + full / 2) / full);
}

/* Convert HomeKit percent to step count using current calibration. */
static int32_t percent_to_steps(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    int32_t full = app_settings_get()->full_open_steps;
    return (int32_t)((int64_t)full * percent / 100);
}

/* --- HAP callbacks ------------------------------------------------------- */

static int identify(hap_acc_t *acc)
{
    /* Wiggle motors a little so the user can tell which accessory is which. */
    int32_t cur = stepper_get_position();
    stepper_jog(+200);
    /* No blocking here: stepper is async. The user will see/hear it. */
    (void)cur;
    return HAP_SUCCESS;
}

static int wc_write(hap_write_data_t write_data[], int count,
                    void *serv_priv, void *write_priv)
{
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; ++i) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_POSITION)) {
            int pct = w->val.i;
            int32_t target_steps = percent_to_steps(pct);
            int32_t current = stepper_get_position();
            ESP_LOGI(TAG, "HK target=%d%% -> %d steps (from %d)",
                     pct, (int)target_steps, (int)current);

            /* Update Position State immediately so the Home app shows motion. */
            hap_val_t state = { .i = (target_steps == current) ? POS_STATE_STOPPED
                                  : (target_steps  > current ? POS_STATE_INCREASING
                                                             : POS_STATE_DECREASING) };
            hap_char_update_val(s_c_state, &state);

            stepper_move_to(target_steps);
            hap_char_update_val(w->hc, &w->val);  /* echo target */
            *(w->status) = HAP_STATUS_SUCCESS;
        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
        }
    }
    return ret;
}

/* --- Public --------------------------------------------------------------- */

void homekit_service_publish_current(int32_t steps, bool moving)
{
    if (!s_c_current) return;
    hap_val_t cur = { .i = steps_to_percent(steps) };
    hap_char_update_val(s_c_current, &cur);

    hap_val_t st;
    if (moving) {
        int32_t target = stepper_get_target();
        st.i = (target > steps) ? POS_STATE_INCREASING
             : (target < steps ? POS_STATE_DECREASING : POS_STATE_STOPPED);
    } else {
        st.i = POS_STATE_STOPPED;
    }
    hap_char_update_val(s_c_state, &st);
}

esp_err_t homekit_service_start(void)
{
    hap_init(HAP_TRANSPORT_WIFI);

    const app_settings_t *cfg = app_settings_get();

    /* Use the MAC tail to derive a unique-ish serial. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[16];
    snprintf(serial, sizeof(serial), "AC-%02X%02X%02X", mac[3], mac[4], mac[5]);

    hap_acc_cfg_t acc_cfg = {
        .name             = (char*)cfg->accessory_name,
        .model            = "AirCoverV1",
        .manufacturer     = "DIY",
        .serial_num       = serial,
        .fw_rev           = "1.0.0",
        .hw_rev           = "1.0",
        .pv               = "1.1.0",
        .cid              = HAP_CID_WINDOW_COVERING,
        .identify_routine = identify,
    };
    s_acc = hap_acc_create(&acc_cfg);

    uint8_t pct_now = steps_to_percent(stepper_get_position());
    s_wc = hap_serv_window_covering_create(pct_now, pct_now, POS_STATE_STOPPED);

    /* Pull the three required characteristics back out so we can update them. */
    s_c_current = hap_serv_get_char_by_uuid(s_wc, HAP_CHAR_UUID_CURRENT_POSITION);
    s_c_target  = hap_serv_get_char_by_uuid(s_wc, HAP_CHAR_UUID_TARGET_POSITION);
    s_c_state   = hap_serv_get_char_by_uuid(s_wc, HAP_CHAR_UUID_POSITION_STATE);

    hap_serv_add_char(s_wc, hap_char_name_create((char*)cfg->accessory_name));
    hap_serv_set_write_cb(s_wc, wc_write);

    hap_acc_add_serv(s_acc, s_wc);
    hap_add_accessory(s_acc);

    hap_set_setup_code(cfg->hap_setup_code);
    hap_set_setup_id("ACV1");

    int rc = hap_start();
    ESP_LOGI(TAG, "HAP started (rc=%d) name='%s' code='%s'",
             rc, cfg->accessory_name, cfg->hap_setup_code);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
