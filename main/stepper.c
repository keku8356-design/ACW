#include "stepper.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "stepper";

/* 8-phase half-step sequence for 28BYJ-48. Each row is the state of IN1..IN4.
 * Going up the table = "forward"; going down = "reverse". The direction
 * mapping to "open/close" is configurable at the application layer -- here
 * we just say positive deltas use forward(). */
static const uint8_t HALF_STEP_SEQ[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

static stepper_pins_t   s_pins;
static esp_timer_handle_t s_timer;

/* All position state is read/written under s_lock to keep API thread-safe. */
static SemaphoreHandle_t s_lock;
static int32_t  s_current;
static int32_t  s_target;
static int      s_phase;            /* 0..7 in HALF_STEP_SEQ */
static uint32_t s_period_us = 1500;
static bool     s_hold      = false;
static bool     s_running   = false;
static stepper_done_cb_t s_done_cb = NULL;

/* --- low-level GPIO helpers ----------------------------------------------- */

static void write_phase(const int32_t pin[4], const uint8_t pat[4])
{
    for (int i = 0; i < 4; ++i) gpio_set_level(pin[i], pat[i]);
}

static void deenergize(void)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    write_phase(s_pins.pin_a, zero);
    write_phase(s_pins.pin_b, zero);
}

/* Apply the current phase to both motors. Both move in the same direction
 * (i.e. both spool in or both spool out together). If one of your motors
 * is mirrored mechanically, swap any two of its IN pins in wiring. */
static void apply_phase(void)
{
    const uint8_t *pat = HALF_STEP_SEQ[s_phase];
    write_phase(s_pins.pin_a, pat);
    write_phase(s_pins.pin_b, pat);
}

/* --- timer-driven stepping ------------------------------------------------- */

static void timer_cb(void *arg)
{
    bool finished = false;
    int32_t finished_pos = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_current == s_target) {
        if (!s_hold) deenergize();
        esp_timer_stop(s_timer);
        s_running    = false;
        finished     = true;
        finished_pos = s_current;
    } else {
        int dir = (s_target > s_current) ? +1 : -1;
        s_phase = (s_phase + dir + 8) & 7;
        apply_phase();
        s_current += dir;
    }

    xSemaphoreGive(s_lock);

    if (finished && s_done_cb) s_done_cb(finished_pos);
}

/* Start the timer if it isn't already running. Must be called with lock held. */
static void ensure_running_locked(void)
{
    if (s_running) return;
    s_running = true;
    esp_timer_start_periodic(s_timer, s_period_us);
}

/* --- public API ----------------------------------------------------------- */

esp_err_t stepper_init(const stepper_pins_t *pins, int32_t start_position)
{
    s_pins   = *pins;
    s_lock   = xSemaphoreCreateMutex();
    s_current = start_position;
    s_target  = start_position;
    s_phase   = 0;

    uint64_t mask = 0;
    for (int i = 0; i < 4; ++i) {
        mask |= 1ULL << s_pins.pin_a[i];
        mask |= 1ULL << s_pins.pin_b[i];
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    deenergize();

    const esp_timer_create_args_t targs = {
        .callback        = timer_cb,
        .name            = "stepper",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    ESP_LOGI(TAG, "init: pinsA=%d,%d,%d,%d pinsB=%d,%d,%d,%d start=%d",
             (int)pins->pin_a[0], (int)pins->pin_a[1], (int)pins->pin_a[2], (int)pins->pin_a[3],
             (int)pins->pin_b[0], (int)pins->pin_b[1], (int)pins->pin_b[2], (int)pins->pin_b[3],
             (int)start_position);
    return ESP_OK;
}

void stepper_move_to(int32_t target)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target = target;
    if (s_current != s_target) ensure_running_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "move_to: %d (from %d)", (int)target, (int)s_current);
}

void stepper_jog(int32_t delta)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target += delta;
    if (s_current != s_target) ensure_running_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "jog: %+d -> target=%d", (int)delta, (int)s_target);
}

void stepper_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_timer);
    s_running = false;
    s_target  = s_current;
    if (!s_hold) deenergize();
    xSemaphoreGive(s_lock);
}

int32_t stepper_get_position(void) { return s_current; }
int32_t stepper_get_target  (void) { return s_target;  }
bool    stepper_is_moving   (void) { return s_running; }

void stepper_set_position(int32_t pos)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_current = pos;
    s_target  = pos;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_position(teach): %d", (int)pos);
}

void stepper_set_period(uint32_t us)
{
    if (us < 900)   us = 900;
    if (us > 20000) us = 20000;
    s_period_us = us;
    /* If currently moving, restart timer with the new period. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        esp_timer_stop(s_timer);
        esp_timer_start_periodic(s_timer, s_period_us);
    }
    xSemaphoreGive(s_lock);
}

void stepper_set_hold(bool hold)
{
    s_hold = hold;
    if (!hold) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_running) deenergize();
        xSemaphoreGive(s_lock);
    }
}

void stepper_register_done_cb(stepper_done_cb_t cb) { s_done_cb = cb; }
