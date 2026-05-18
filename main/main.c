/* aircover - ESP32-C3 HomeKit air-duct controller
 * --------------------------------------------------
 * Wiring (ESP32-C3 SuperMini / dev-kit):
 *
 *   Motor A (ULN2003 inputs)  -- IN1=GPIO0  IN2=GPIO1  IN3=GPIO3  IN4=GPIO4
 *   Motor B (ULN2003 inputs)  -- IN1=GPIO5  IN2=GPIO6  IN3=GPIO7  IN4=GPIO10
 *   BOOT button (on-board)    -- GPIO9 (active low; long-press 10s -> factory reset)
 *   Status LED (on-board)     -- GPIO8 (active low on most C-3 boards)
 *
 *   ULN2003 + 28BYJ-48 power : 5V (USB 5V is fine; do NOT drive motors from 3V3).
 *   Common ground between ESP32-C3 and the ULN2003 boards is mandatory.
 *
 * State machine on boot:
 *   1. Read NVS settings.
 *   2. Try to join stored WiFi (60s).  On success -> STA mode -> HomeKit + Web UI.
 *      On failure -> SoftAP "AirCover-Setup" + captive config page on 192.168.4.1.
 *   3. From either mode, holding BOOT for >=10s wipes everything (factory reset). */

#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "homekit_service.h"
#include "stepper.h"
#include "webui.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* ---------- pin map ----------------------------------------------------- */
#define PIN_MOTOR_A1   0
#define PIN_MOTOR_A2   1
#define PIN_MOTOR_A3   3
#define PIN_MOTOR_A4   4
#define PIN_MOTOR_B1   5
#define PIN_MOTOR_B2   6
#define PIN_MOTOR_B3   7
#define PIN_MOTOR_B4  10
#define PIN_BOOT_BTN   9
#define PIN_STATUS_LED 8

#define BOOT_HOLD_MS_FOR_FACTORY_RESET   10000
#define WIFI_STA_TIMEOUT_MS              60000
#define SOFTAP_SSID                      "AirCover-Setup"
#define MDNS_HOSTNAME                    "aircover"

/* ---------- LED helpers ------------------------------------------------- */
static void led_set(bool on)
{
    /* on-board LED is usually active-low */
    gpio_set_level(PIN_STATUS_LED, on ? 0 : 1);
}

static void led_blink_task(void *arg)
{
    int period_ms = (int)(intptr_t)arg;
    bool s = false;
    for (;;) {
        led_set(s = !s);
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

/* ---------- BOOT button watchdog --------------------------------------- */
static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    int held_ms = 0;
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
                    /* fast blink as a hint */
                    led_set(true);
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

/* ---------- stepper done callback -------------------------------------- */
static void on_motion_done(int32_t final_position)
{
    ESP_LOGI(TAG, "motion done at step %d -> persisting + notifying HK", (int)final_position);
    app_settings_set_last_position(final_position);
    homekit_service_publish_current(final_position, false);
}

/* ---------- mDNS ------------------------------------------------------- */
static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("AirCover Control");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS up: http://%s.local", MDNS_HOSTNAME);
}

/* ----------------------------------------------------------------------- */

void app_main(void)
{
    /* --- bring up the basics --- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* status LED early */
    gpio_config_t lcfg = {
        .pin_bit_mask = 1ULL << PIN_STATUS_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&lcfg);
    led_set(true);  /* solid-on while booting */

    /* settings -> stepper -> button watchdog (always-on) */
    ESP_ERROR_CHECK(app_settings_init());
    const app_settings_t *cfg = app_settings_get();

    const stepper_pins_t pins = {
        .pin_a = { PIN_MOTOR_A1, PIN_MOTOR_A2, PIN_MOTOR_A3, PIN_MOTOR_A4 },
        .pin_b = { PIN_MOTOR_B1, PIN_MOTOR_B2, PIN_MOTOR_B3, PIN_MOTOR_B4 },
    };
    ESP_ERROR_CHECK(stepper_init(&pins, cfg->last_position));
    stepper_set_period(cfg->step_period_us);
    stepper_set_hold(cfg->hold_when_stopped);
    stepper_register_done_cb(on_motion_done);

    xTaskCreate(boot_button_task, "boot_btn", 2560, NULL, 5, NULL);

    /* --- WiFi (blocking until STA up or SoftAP fallback engaged) --- */
    /* slow blink during connect */
    TaskHandle_t blink_h = NULL;
    xTaskCreate(led_blink_task, "led_blink", 1536, (void*)(intptr_t)400, 3, &blink_h);

    wifi_manager_start(WIFI_STA_TIMEOUT_MS, SOFTAP_SSID);

    if (blink_h) vTaskDelete(blink_h);

    if (!wifi_manager_is_connected()) {
        /* SoftAP mode is now running with its own captive page (port 80).
         * We don't bring up HomeKit or the main UI here. Just blink fast and
         * wait for the user to submit creds via the setup portal. */
        ESP_LOGW(TAG, "in SoftAP setup mode: join '%s' then open http://192.168.4.1",
                 SOFTAP_SSID);
        xTaskCreate(led_blink_task, "led_setup", 1536, (void*)(intptr_t)150, 3, NULL);
        return;
    }

    /* --- STA up: start the rest of the stack --- */
    start_mdns();
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(homekit_service_start());

    led_set(false);  /* idle */
    ESP_LOGI(TAG, "boot complete; HomeKit setup code: %s", cfg->hap_setup_code);
    ESP_LOGI(TAG, "control panel: http://%s.local", MDNS_HOSTNAME);
}
