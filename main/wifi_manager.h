#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MODE_NONE,
    WIFI_MODE_STA_CONNECTING,
    WIFI_MODE_STA_CONNECTED,
    WIFI_MODE_SOFTAP_SETUP,
} wifi_manager_state_t;

/* Initialise netif/event loop and start trying to connect to stored WiFi.
 *
 * If credentials exist and connect within `sta_timeout_ms`, the state goes
 * to STA_CONNECTED.  Otherwise the manager flips to SOFTAP_SETUP, broadcasting
 * an open AP named `ap_ssid` and serving a tiny config page on 192.168.4.1
 * that lets the user enter new credentials.  Once new credentials are
 * submitted the manager persists them and reboots the device. */
esp_err_t wifi_manager_start(uint32_t sta_timeout_ms, const char *ap_ssid);

wifi_manager_state_t wifi_manager_get_state(void);
bool wifi_manager_is_connected(void);
esp_netif_t *wifi_manager_get_sta_netif(void);

#ifdef __cplusplus
}
#endif
