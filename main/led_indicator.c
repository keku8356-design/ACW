#include "led_indicator.h"
 
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
 
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
 
static const char *TAG = "led";
 
/* --- Timing -----------------------------------------------------------
 * Render at 40Hz (25ms tick). At 20Hz the human eye picks up the steps
 * between frames when the colour is changing slowly; 40Hz hides them.
 * Going higher (e.g. 60Hz) gives diminishing returns and eats more CPU. */
#define TICK_MS                 25
#define TICK_HZ                 80
#define READY_VISIBLE_TICKS     200              /* 5s of full green   */
#define READY_FADE_TICKS        10               /* 1s fade-to-black   */
#define TRANSITION_TICKS        20               /* 1s colour blend    */
#define BLUE_PULSE_TICKS        60               /* 1.5s pulse cycle   */
#define YELLOW_BREATHE_TICKS    80               /* 2s breath cycle    */
#define CONNECTING_BLINK_TICKS  24               /* 0.6s wifi blink    */
#define SOFTAP_BLINK_TICKS      10               /* 0.25s softap blink */
#define MOVING_LATCH_MS         1600             /* ≥ one pulse period */
 
typedef struct { uint8_t r, g, b; } rgb_t;
 
/* All COL_xxx values are in *perceptual* (linear-light) space. They get
 * gamma-encoded at the end of render() right before being shipped to the
 * WS2812. This is the standard trick to make low-end brightness look
 * smooth instead of stepping noticeably between values like 3-2-1-0. */
static const rgb_t COL_OFF       = {0,   0,   0  };
static const rgb_t COL_BOOT      = {40,  40,  40 };  /* dim white          */
static const rgb_t COL_CONN      = {0,   0,   140};  /* connecting blink   */
static const rgb_t COL_SOFTAP    = {200, 80,  0  };  /* softap (orange)    */
static const rgb_t COL_YELLOW_P  = {150, 150, 0  };  /* unpaired (peak)    */
static const rgb_t COL_BLUE_P    = {0,   0,   230};  /* moving (peak)      */
static const rgb_t COL_READY     = {0,   100, 0  };  /* paired ready (dim) */
 
typedef struct {
    led_wifi_phase_t wifi;
    bool             paired;
    bool             moving;
} led_state_t;
 
static led_strip_handle_t        s_strip      = NULL;
static volatile led_wifi_phase_t s_wifi       = LED_WIFI_BOOT;
static volatile bool             s_paired     = false;
/* "moving" gets double-tracked: the explicit flag plus a wall-clock latch.
 * The latch lets us notice motor jogs that are shorter than one 25ms tick. */
static volatile bool             s_currently_moving      = false;
static volatile uint32_t         s_moving_latch_until_ms = 0;
 
static uint8_t s_gamma     [256];
static uint8_t s_pulse_lut [BLUE_PULSE_TICKS];      /* peak at i=0, trough at i=period/2 */
static uint8_t s_breath_lut[YELLOW_BREATHE_TICKS];  /* peak at i=0, trough at i=period/2 */
 
/* Animation/transition state machine. */
static led_state_t s_active             = { LED_WIFI_BOOT, false, false };
static int         s_active_enter_tick  = 0;
static int         s_transition_start   = -1;        /* -1 = no transition  */
static rgb_t       s_transition_from    = {0, 0, 0};
static rgb_t       s_displayed          = {0, 0, 0}; /* last frame as written, pre-gamma */
 
/* ---- helpers ---- */
 
static inline rgb_t apply_gamma(rgb_t c) {
    return (rgb_t){ s_gamma[c.r], s_gamma[c.g], s_gamma[c.b] };
}
 
static rgb_t scale(rgb_t c, int num, int den) {
    if (den == 0) return COL_OFF;
    return (rgb_t){
        (uint8_t)((int)c.r * num / den),
        (uint8_t)((int)c.g * num / den),
        (uint8_t)((int)c.b * num / den),
    };
}
 
static rgb_t lerp(rgb_t a, rgb_t b, int num, int den) {
    if (num <= 0) return a;
    if (num >= den) return b;
    return (rgb_t){
        (uint8_t)((int)a.r + ((int)b.r - (int)a.r) * num / den),
        (uint8_t)((int)a.g + ((int)b.g - (int)a.g) * num / den),
        (uint8_t)((int)a.b + ((int)b.b - (int)a.b) * num / den),
    };
}
 
static bool state_equal(led_state_t a, led_state_t b) {
    return a.wifi == b.wifi && a.paired == b.paired && a.moving == b.moving;
}
 
static led_state_t current_state(void) {
    uint32_t now_ms  = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool     latched = ((int32_t)(s_moving_latch_until_ms - now_ms)) > 0;
    bool     moving  = s_currently_moving || latched;
    return (led_state_t){ s_wifi, s_paired, moving };
}
 
/* Pre-gamma "natural" colour of the given state at a given tick.
 * For the animated states (moving / unpaired-breathing / ready-fade)
 * the timing is relative to s_active_enter_tick so that whenever the
 * state is freshly entered the animation starts at a known phase. */
static rgb_t natural_color(led_state_t st, int tick) {
    int rel = tick - s_active_enter_tick;
    if (rel < 0) rel = 0;
 
    if (st.moving) {
        uint8_t v = s_pulse_lut[rel % BLUE_PULSE_TICKS];
        return scale(COL_BLUE_P, v, 255);
    }
    switch (st.wifi) {
    case LED_WIFI_BOOT:
        return COL_BOOT;
    case LED_WIFI_CONNECTING: {
        bool on = (tick % CONNECTING_BLINK_TICKS) < (CONNECTING_BLINK_TICKS/2);
        return on ? COL_CONN : COL_OFF;
    }
    case LED_WIFI_SOFTAP: {
        bool on = (tick % SOFTAP_BLINK_TICKS) < (SOFTAP_BLINK_TICKS/2);
        return on ? COL_SOFTAP : COL_OFF;
    }
    case LED_WIFI_CONNECTED:
        if (!st.paired) {
            uint8_t v = s_breath_lut[rel % YELLOW_BREATHE_TICKS];
            return scale(COL_YELLOW_P, v, 255);
        }
        /* paired + idle = "ready": 5s full green, 1s fade-out, then off. */
        if (rel < READY_VISIBLE_TICKS) return COL_READY;
        if (rel < READY_VISIBLE_TICKS + READY_FADE_TICKS) {
            int remaining = (READY_VISIBLE_TICKS + READY_FADE_TICKS) - rel;
            return scale(COL_READY, remaining, READY_FADE_TICKS);
        }
        return COL_OFF;
    }
    return COL_OFF;
}
 
static void write_strip(rgb_t c) {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
    led_strip_refresh(s_strip);
}
 
/* The main per-tick state machine. */
static void render(int tick) {
    led_state_t target = current_state();
 
    /* Detect a logical state change and start a colour blend.
     * Special case: when leaving the "moving" state, wait until the next
     * blue-pulse peak so the pulse appears to complete before the blend
     * begins. The from-colour is forced to peak blue so the transition
     * actually starts at full brightness no matter what s_displayed was. */
    if (!state_equal(target, s_active)) {
        bool defer   = s_active.moving && !target.moving;
        bool at_peak = ((tick - s_active_enter_tick) % BLUE_PULSE_TICKS) == 0;
        if (!defer || at_peak) {
            s_transition_from   = defer ? COL_BLUE_P : s_displayed;
            s_transition_start  = tick;
            s_active            = target;
            s_active_enter_tick = tick;
        }
        /* else: keep rendering with the old s_active (still pulsing). */
    }
 
    rgb_t natural = natural_color(s_active, tick);
 
    rgb_t shown = natural;
    if (s_transition_start >= 0) {
        int elapsed = tick - s_transition_start;
        if (elapsed >= TRANSITION_TICKS) {
            s_transition_start = -1;
        } else {
            shown = lerp(s_transition_from, natural, elapsed, TRANSITION_TICKS);
        }
    }
 
    s_displayed = shown;
    write_strip(apply_gamma(shown));
}
 
static void led_task(void *arg) {
    int t = 0;
    for (;;) {
        render(t);
        t++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}
 
esp_err_t led_indicator_init(int gpio) {
    /* Gamma 2.2 = sRGB-ish encoding. The eye perceives this as a roughly
     * linear ramp in brightness. Higher gamma (2.4, 2.6) compresses the
     * low end more and is sometimes better for "fade to black" smoothness;
     * lower (1.8) lifts the low end. Tunable if the user wants. */
    for (int i = 0; i < 256; i++) {
        s_gamma[i] = (uint8_t)(pow(i / 255.0, 2.6) * 255.0 + 0.5);
    }
    /* Pulse: (1 + cos(2π·t/T))/2  → peak at t=0, trough at t=T/2.
     * Cosine is C¹-continuous so there's no perceptible "corner" at the
     * peak/trough like a triangle wave has. */
    for (int i = 0; i < BLUE_PULSE_TICKS; i++) {
        double v = (1.0 + cos(2.0 * M_PI * i / BLUE_PULSE_TICKS)) / 2.0;
        s_pulse_lut[i] = (uint8_t)(v * 255.0 + 0.5);
    }
    /* Breath: same shape, just a different period. Starting at peak makes
     * the transition into "unpaired" look continuous from the previous colour. */
    for (int i = 0; i < YELLOW_BREATHE_TICKS; i++) {
        double v = (1.0 + cos(2.0 * M_PI * i / YELLOW_BREATHE_TICKS)) / 2.0;
        s_breath_lut[i] = (uint8_t)(v * 255.0 + 0.5);
    }
 
    led_strip_config_t cfg = {
        .strip_gpio_num   = gpio,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed on GPIO %d: %s", gpio, esp_err_to_name(err));
        return err;
    }
    write_strip(COL_OFF);
    xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "WS2812 on GPIO %d (40Hz, gamma 2.2, 1s blends)", gpio);
    return ESP_OK;
}
 
void led_indicator_set_wifi(led_wifi_phase_t p) { s_wifi   = p;      }
void led_indicator_set_paired(bool paired)       { s_paired = paired; }
 
void led_indicator_set_moving(bool moving) {
    s_currently_moving = moving;
    if (moving) {
        /* Latch "moving" for at least one full pulse so brief jogs
         * (<25ms render tick) still flash blue. */
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        s_moving_latch_until_ms = now_ms + MOVING_LATCH_MS;
    }
}