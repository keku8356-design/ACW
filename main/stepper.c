#include "stepper.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "stepper";

/* 8-phase half-step sequence for 28BYJ-48. */
static const uint8_t HALF_STEP_SEQ[8][4] = {
    {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0},
    {0, 0, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 1},
};

typedef struct {
    int32_t pin[4];
    int     phase;        /* 0..7 in HALF_STEP_SEQ */
    int32_t current;
    int32_t target;
} motor_t;

#define M_LEFT   0
#define M_RIGHT  1

static motor_t           s_motor[2];
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
static bool      s_running   = false;
static uint32_t  s_period_us = 1500;
static bool      s_hold      = false;
static stepper_done_cb_t  s_done_cb  = NULL;
static stepper_start_cb_t s_start_cb = NULL;

/* --- low-level helpers --------------------------------------------------- */

static void write_phase(const int32_t pin[4], const uint8_t pat[4])
{
    for (int i = 0; i < 4; ++i) gpio_set_level(pin[i], pat[i]);
}

static void deenergize_motor(int m)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    write_phase(s_motor[m].pin, zero);
}

static void deenergize_all(void)
{
    deenergize_motor(M_LEFT);
    deenergize_motor(M_RIGHT);
}

static void ensure_running_locked(void)
{
    if (s_running) return;
    s_running = true;
    if (s_start_cb) s_start_cb();
    esp_timer_start_periodic(s_timer, s_period_us);
}

/* --- timer-driven stepping ---------------------------------------------- */

static void timer_cb(void *arg)
{
    bool    finished = false;
    int32_t finished_pos = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool any_motion = false;
    for (int m = 0; m < 2; ++m) {
        if (s_motor[m].current == s_motor[m].target) continue;
        int dir = (s_motor[m].target > s_motor[m].current) ? +1 : -1;
        s_motor[m].phase = (s_motor[m].phase + dir + 8) & 7;
        write_phase(s_motor[m].pin, HALF_STEP_SEQ[s_motor[m].phase]);
        s_motor[m].current += dir;
        any_motion = true;
    }

    if (!any_motion) {
        esp_timer_stop(s_timer);
        s_running = false;
        if (!s_hold) deenergize_all();
        finished     = true;
        finished_pos = s_motor[M_LEFT].current;
    }

    xSemaphoreGive(s_lock);

    /* Fire callback outside the lock. */
    if (finished && s_done_cb) s_done_cb(finished_pos);
}

/* --- public API ---------------------------------------------------------- */

esp_err_t stepper_init(const stepper_pins_t *pins, int32_t start_position)
{
    s_lock = xSemaphoreCreateMutex();
    memcpy(s_motor[M_LEFT].pin,  pins->pin_left,  sizeof(pins->pin_left));
    memcpy(s_motor[M_RIGHT].pin, pins->pin_right, sizeof(pins->pin_right));
    for (int m = 0; m < 2; ++m) {
        s_motor[m].phase   = 0;
        s_motor[m].current = start_position;
        s_motor[m].target  = start_position;
    }

    uint64_t mask = 0;
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < 4; ++i)
            mask |= 1ULL << s_motor[m].pin[i];
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    deenergize_all();

    const esp_timer_create_args_t targs = {
        .callback        = timer_cb,
        .name            = "stepper",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    ESP_LOGI(TAG, "init: L=%d,%d,%d,%d  R=%d,%d,%d,%d  start=%d",
             (int)pins->pin_left[0],  (int)pins->pin_left[1],
             (int)pins->pin_left[2],  (int)pins->pin_left[3],
             (int)pins->pin_right[0], (int)pins->pin_right[1],
             (int)pins->pin_right[2], (int)pins->pin_right[3],
             (int)start_position);
    return ESP_OK;
}

void stepper_move_to(int32_t target)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_motor[M_LEFT ].target = target;
    s_motor[M_RIGHT].target = target;
    if (s_motor[M_LEFT ].current != target ||
        s_motor[M_RIGHT].current != target) {
        ensure_running_locked();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "move_to %d (L=%d R=%d)",
             (int)target,
             (int)s_motor[M_LEFT].current,
             (int)s_motor[M_RIGHT].current);
}

void stepper_jog_motor(motor_id_t which, int32_t delta)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (which == MOTOR_BOTH || which == MOTOR_LEFT)  s_motor[M_LEFT ].target += delta;
    if (which == MOTOR_BOTH || which == MOTOR_RIGHT) s_motor[M_RIGHT].target += delta;
    if (s_motor[M_LEFT ].current != s_motor[M_LEFT ].target ||
        s_motor[M_RIGHT].current != s_motor[M_RIGHT].target) {
        ensure_running_locked();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "jog %s %+d -> L_t=%d R_t=%d",
             (which == MOTOR_LEFT)  ? "LEFT"  :
             (which == MOTOR_RIGHT) ? "RIGHT" : "BOTH",
             (int)delta,
             (int)s_motor[M_LEFT].target,
             (int)s_motor[M_RIGHT].target);
}

void stepper_jog(int32_t delta) { stepper_jog_motor(MOTOR_BOTH, delta); }

void stepper_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_timer);
    s_running = false;
    s_motor[M_LEFT ].target = s_motor[M_LEFT ].current;
    s_motor[M_RIGHT].target = s_motor[M_RIGHT].current;
    if (!s_hold) deenergize_all();
    xSemaphoreGive(s_lock);
}

int32_t stepper_get_position(void) { return s_motor[M_LEFT].current; }
int32_t stepper_get_target  (void) { return s_motor[M_LEFT].target;  }
bool    stepper_is_moving   (void) { return s_running; }

int32_t stepper_get_motor_position(motor_id_t w)
{
    if (w == MOTOR_RIGHT) return s_motor[M_RIGHT].current;
    return s_motor[M_LEFT].current;                      /* BOTH / LEFT */
}
int32_t stepper_get_motor_target(motor_id_t w)
{
    if (w == MOTOR_RIGHT) return s_motor[M_RIGHT].target;
    return s_motor[M_LEFT].target;
}

void stepper_set_position(motor_id_t which, int32_t pos)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (which == MOTOR_BOTH || which == MOTOR_LEFT) {
        s_motor[M_LEFT].current = pos;
        s_motor[M_LEFT].target  = pos;
    }
    if (which == MOTOR_BOTH || which == MOTOR_RIGHT) {
        s_motor[M_RIGHT].current = pos;
        s_motor[M_RIGHT].target  = pos;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_position %s = %d",
             (which == MOTOR_LEFT)  ? "LEFT"  :
             (which == MOTOR_RIGHT) ? "RIGHT" : "BOTH",
             (int)pos);
}

void stepper_set_period(uint32_t us)
{
    if (us < 900)   us = 900;
    if (us > 20000) us = 20000;
    s_period_us = us;
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
        if (!s_running) deenergize_all();
        xSemaphoreGive(s_lock);
    }
}

void stepper_register_done_cb (stepper_done_cb_t  cb) { s_done_cb  = cb; }
void stepper_register_start_cb(stepper_start_cb_t cb) { s_start_cb = cb; }
