#include "led_indicator.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip = NULL;
static volatile led_wifi_phase_t s_wifi   = LED_WIFI_BOOT;
static volatile bool             s_paired = false;
static volatile bool             s_moving = false;

static inline void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void render(int tick_50ms)
{
    /* "Green ready" auto-off: count consecutive ticks spent in the
     * paired/idle/connected state. Anything that breaks that state (motion,
     * disconnect, pairing change) resets the counter, so the LED relights
     * naturally on the next interesting event. 200 ticks * 50ms == 10s. */
    static int ready_ticks = 0;
    const int  READY_VISIBLE_TICKS = 200;

    /* Highest priority: motion overlay (blue pulse). */
    if (s_moving) {
        ready_ticks = 0;
        int t = tick_50ms % 30;
        int p = (t < 15 ? t : 30 - t) * 12;   /* 0..180 triangle */
        set_rgb(0, 0, p);
        return;
    }

    switch (s_wifi) {
    case LED_WIFI_BOOT:
        ready_ticks = 0;
        set_rgb(40, 40, 40);                                       /* dim white */
        break;
    case LED_WIFI_CONNECTING:
        ready_ticks = 0;
        set_rgb(0, 0, (tick_50ms % 20) < 10 ? 80 : 0);             /* slow blue blink */
        break;
    case LED_WIFI_SOFTAP:
        ready_ticks = 0;
        set_rgb((tick_50ms % 6) < 3 ? 140 : 0,
                (tick_50ms % 6) < 3 ?  50 : 0, 0);                 /* fast orange */
        break;
    case LED_WIFI_CONNECTED:
        if (!s_paired) {
            ready_ticks = 0;
            int t = tick_50ms % 100;
            int p = (t < 50 ? t : 100 - t) * 2;                    /* 0..100 triangle */
            set_rgb(p, p, 0);                                      /* yellow breathing */
        } else {
            /* Show green for 10s after entering ready state, then dim out. */
            if (ready_ticks < READY_VISIBLE_TICKS) {
                set_rgb(0, 25, 0);                                 /* dim green */
                ready_ticks++;
            } else {
                set_rgb(0, 0, 0);                                  /* off */
            }
        }
        break;
    }
}

static void led_task(void *arg)
{
    int t = 0;
    for (;;) {
        render(t);
        t = (t + 1) & 0xFFFF;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t led_indicator_init(int gpio)
{
    led_strip_config_t cfg = {
        .strip_gpio_num   = gpio,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,    /* WS2812 default */
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,           /* 10 MHz */
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed on GPIO %d: %s", gpio, esp_err_to_name(err));
        return err;
    }
    set_rgb(0, 0, 0);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "WS2812 indicator on GPIO %d", gpio);
    return ESP_OK;
}

void led_indicator_set_wifi(led_wifi_phase_t p) { s_wifi   = p;     }
void led_indicator_set_paired(bool paired)       { s_paired = paired; }
void led_indicator_set_moving(bool moving)       { s_moving = moving; }