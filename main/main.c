/* aircover - ESP32-C3 HomeKit air-duct controller
 * --------------------------------------------------
 * Pin map (ESP32-C3 SuperMini / dev-kit):
 *
 *   RIGHT motor (ULN2003 inputs) -- IN1=GPIO0   IN2=GPIO1  IN3=GPIO3   IN4=GPIO10
 *   LEFT  motor (ULN2003 inputs) -- IN1=GPIO4   IN2=GPIO5  IN3=GPIO6   IN4=GPIO7
 *   BOOT button (on-board)       -- GPIO9     (long-press 10s -> factory reset)
 *   Status LED (on-board WS2812) -- GPIO8     (single RGB pixel)
 *
 *   ULN2003 + 28BYJ-48 power : 5V (USB 5V is fine; do NOT drive motors from 3V3).
 *   Common ground between ESP32-C3 and the ULN2003 boards is mandatory. */

#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "homekit_service.h"
#include "led_indicator.h"
#include "stepper.h"
#include "webui.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* ---------- pin map ----------------------------------------------------- */
#define PIN_RIGHT_1   0
#define PIN_RIGHT_2   1
#define PIN_RIGHT_3   3
#define PIN_RIGHT_4  10

#define PIN_LEFT_1    4
#define PIN_LEFT_2    5
#define PIN_LEFT_3    6
#define PIN_LEFT_4    7

#define PIN_BOOT_BTN  9
#define PIN_RGB_LED   8

#define BOOT_HOLD_MS_FOR_FACTORY_RESET   10000
#define WIFI_STA_TIMEOUT_MS              60000
#define SOFTAP_SSID                      "AirCover-Setup"
#define MDNS_HOSTNAME                    "aircover"

/* ---------- BOOT button watchdog --------------------------------------- */

static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    int  held_ms = 0;
    bool was_low = false;
    for (;;) {
        bool pressed = (gpio_get_level(PIN_BOOT_BTN) == 0);
        if (pressed) {
            if (!was_low) {
                ESP_LOGI(TAG, "BOOT pressed (hold %d s for factory reset)",
                         BOOT_HOLD_MS_FOR_FACTORY_RESET / 1000);
                was_low = true;
                held_ms = 0;
            } else {
                held_ms += 100;
                if (held_ms >= BOOT_HOLD_MS_FOR_FACTORY_RESET) {
                    ESP_LOGW(TAG, "FACTORY RESET via long-press!");
                    app_settings_factory_reset();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        } else {
            if (was_low) ESP_LOGI(TAG, "BOOT released after %d ms", held_ms);
            was_low = false;
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------- stepper hooks ---------------------------------------------- */

static void on_motion_start(void)
{
    led_indicator_set_moving(true);
}

static void on_motion_done(int32_t final_position)
{
    ESP_LOGI(TAG, "motion done at step %d -> persisting + notifying HK",
             (int)final_position);
    led_indicator_set_moving(false);
    app_settings_set_last_position(final_position);
    homekit_service_publish_current(final_position, false);
}

/* ---------- mDNS ------------------------------------------------------- */

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_instance_name_set("AirCover Control");

    /* Advertise the HTTP control panel with a couple of TXT records so the
     * service is easy to discover (e.g. Bonjour Browser shows them). */
    mdns_txt_item_t txt[] = {
        { .key = "path",    .value = "/" },
        { .key = "version", .value = "1.0.0" },
    };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return;
    }

    /* Get our IP so the log line is unambiguous if .local doesn't resolve
     * on the user's OS (e.g. older Windows without Bonjour). */
    esp_netif_ip_info_t ip = {0};
    if (wifi_manager_get_sta_netif() &&
        esp_netif_get_ip_info(wifi_manager_get_sta_netif(), &ip) == ESP_OK) {
        ESP_LOGI(TAG, "mDNS up: http://%s.local  (or  http://" IPSTR ")",
                 MDNS_HOSTNAME, IP2STR(&ip.ip));
    } else {
        ESP_LOGI(TAG, "mDNS up: http://%s.local", MDNS_HOSTNAME);
    }
}

/* ----------------------------------------------------------------------- */

void app_main(void)
{
    /* NVS bootstrap. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* WS2812 indicator FIRST so we have visual feedback throughout boot. */
    led_indicator_init(PIN_RGB_LED);
    led_indicator_set_wifi(LED_WIFI_BOOT);

    /* Settings -> stepper -> button watchdog (always-on). */
    ESP_ERROR_CHECK(app_settings_init());
    const app_settings_t *cfg = app_settings_get();

    const stepper_pins_t pins = {
        .pin_left  = { PIN_LEFT_1,  PIN_LEFT_2,  PIN_LEFT_3,  PIN_LEFT_4  },
        .pin_right = { PIN_RIGHT_1, PIN_RIGHT_2, PIN_RIGHT_3, PIN_RIGHT_4 },
    };
    ESP_ERROR_CHECK(stepper_init(&pins, cfg->last_position));
    stepper_set_period(cfg->step_period_us);
    stepper_set_hold(cfg->hold_when_stopped);
    stepper_register_start_cb(on_motion_start);
    stepper_register_done_cb (on_motion_done);

    xTaskCreate(boot_button_task, "boot_btn", 2560, NULL, 5, NULL);

    /* WiFi (blocking until STA up or SoftAP fallback engaged). */
    led_indicator_set_wifi(LED_WIFI_CONNECTING);
    wifi_manager_start(WIFI_STA_TIMEOUT_MS, SOFTAP_SSID);

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "in SoftAP setup mode: join '%s' then open http://192.168.4.1",
                 SOFTAP_SSID);
        led_indicator_set_wifi(LED_WIFI_SOFTAP);
        return;
    }

    /* STA up: start the rest of the stack. */
    led_indicator_set_wifi(LED_WIFI_CONNECTED);
    led_indicator_set_paired(false);     /* HomeKit will update once paired */

    start_mdns();
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(homekit_service_start());

    ESP_LOGI(TAG, "boot complete; HomeKit setup code: %s", cfg->hap_setup_code);
}
