#include "webui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_settings.h"
#include "stepper.h"
#include "webui_assets.h"

static const char *TAG = "webui";

#define MAX_WS_CLIENTS  4
#define LOG_RING_SZ     128
#define LOG_LINE_MAX    192

/* ---- WebSocket client tracking ---------------------------------------- */
typedef struct {
    httpd_handle_t handle;
    int            fd;
    bool           used;
} ws_client_t;

static ws_client_t       s_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_clients_lock;
static httpd_handle_t    s_httpd;
static bool              s_hk_paired = false;

/* ---- Log capture ------------------------------------------------------ */
static vprintf_like_t  s_old_vprintf;
static SemaphoreHandle_t s_log_lock;
static SemaphoreHandle_t s_log_sem;
static char            s_log_ring[LOG_RING_SZ][LOG_LINE_MAX];
static int             s_log_head = 0;
static int             s_log_tail = 0;
static __thread bool   s_log_reenter = false;  /* re-entry guard */

static void push_log_line(const char *line, int len)
{
    if (!s_log_lock || !s_log_sem) return;
    if (xSemaphoreTake(s_log_lock, 0) != pdTRUE) return;  /* never block the logger */
    int next = (s_log_head + 1) % LOG_RING_SZ;
    if (next == s_log_tail) s_log_tail = (s_log_tail + 1) % LOG_RING_SZ; /* drop oldest */
    int copy = (len < LOG_LINE_MAX - 1) ? len : LOG_LINE_MAX - 1;
    /* strip trailing newline so the WS frame doesn't carry it */
    while (copy > 0 && (line[copy - 1] == '\n' || line[copy - 1] == '\r')) --copy;
    memcpy(s_log_ring[s_log_head], line, copy);
    s_log_ring[s_log_head][copy] = '\0';
    s_log_head = next;
    xSemaphoreGive(s_log_lock);
    xSemaphoreGive(s_log_sem);
}

static int log_vprintf_hook(const char *fmt, va_list ap)
{
    /* Always pass through to the original console sink. */
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_old_vprintf ? s_old_vprintf(fmt, ap) : vprintf(fmt, ap);

    /* Avoid recursion if anything inside the forwarder logs. */
    if (!s_log_reenter) {
        s_log_reenter = true;
        char line[LOG_LINE_MAX];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        if (n > 0) push_log_line(line, n);
        s_log_reenter = false;
    }
    va_end(ap2);
    return r;
}

static void broadcast_ws(const char *data, size_t len)
{
    if (!s_clients_lock) return;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
        if (!s_clients[i].used) continue;
        httpd_ws_frame_t f = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)data,
            .len     = len,
            .final   = true,
        };
        if (httpd_ws_send_frame_async(s_clients[i].handle, s_clients[i].fd, &f) != ESP_OK) {
            s_clients[i].used = false;
        }
    }
    xSemaphoreGive(s_clients_lock);
}

static void log_forwarder_task(void *arg)
{
    char line[LOG_LINE_MAX];
    for (;;) {
        xSemaphoreTake(s_log_sem, portMAX_DELAY);
        /* Drain available */
        for (;;) {
            xSemaphoreTake(s_log_lock, portMAX_DELAY);
            if (s_log_tail == s_log_head) {
                xSemaphoreGive(s_log_lock);
                break;
            }
            strncpy(line, s_log_ring[s_log_tail], sizeof(line) - 1);
            line[sizeof(line) - 1] = 0;
            s_log_tail = (s_log_tail + 1) % LOG_RING_SZ;
            xSemaphoreGive(s_log_lock);
            broadcast_ws(line, strlen(line));
        }
    }
}

/* ---- Tiny JSON helpers (manual parsing of trusted local input) -------- */

static const char *find_key(const char *body, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(body, pat);
}

static int json_get_int(const char *body, const char *key, int dflt)
{
    const char *p = find_key(body, key);
    if (!p) return dflt;
    p = strchr(p, ':');
    if (!p) return dflt;
    return (int)strtol(p + 1, NULL, 10);
}

static bool json_get_str(const char *body, const char *key, char *out, size_t outsz)
{
    const char *p = find_key(body, key);
    if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    p = strchr(p, '"'); if (!p) return false;
    ++p;
    const char *q = strchr(p, '"'); if (!q) return false;
    size_t L = (size_t)(q - p);
    if (L >= outsz) L = outsz - 1;
    memcpy(out, p, L); out[L] = 0;
    return true;
}

static esp_err_t read_body(httpd_req_t *r, char *buf, size_t bufsz)
{
    size_t total = r->content_len < bufsz - 1 ? r->content_len : bufsz - 1;
    int got = 0;
    while ((size_t)got < total) {
        int n = httpd_req_recv(r, buf + got, total - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    buf[got] = 0;
    return ESP_OK;
}

/* ---- API handlers ----------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, WEBUI_HTML, sizeof(WEBUI_HTML) - 1);
}

static esp_err_t h_status(httpd_req_t *r)
{
    const app_settings_t *cfg = app_settings_get();
    int32_t pos    = stepper_get_position();          /* primary (left) */
    int32_t tgt    = stepper_get_target();
    int32_t pos_l  = stepper_get_motor_position(MOTOR_LEFT);
    int32_t pos_r  = stepper_get_motor_position(MOTOR_RIGHT);
    int32_t full   = cfg->full_open_steps;
    int     pct    = full > 0 ? (int)((int64_t)pos * 100 / full) : 0;
    int     pct_t  = full > 0 ? (int)((int64_t)tgt * 100 / full) : 0;
    if (pct < 0)   pct = 0;
    if (pct > 100)   pct = 100;
    if (pct_t < 0) pct_t = 0;
    if (pct_t > 100) pct_t = 100;

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"position\":%d,\"target\":%d,\"full_open\":%d,"
        "\"position_left\":%d,\"position_right\":%d,"
        "\"percent\":%d,\"target_percent\":%d,\"moving\":%s,"
        "\"step_period_us\":%u,\"hold_when_stopped\":%s,"
        "\"detent_steps\":%d,"
        "\"hk_paired\":%s,\"name\":\"%s\",\"setup_code\":\"%s\"}",
        (int)pos, (int)tgt, (int)full,
        (int)pos_l, (int)pos_r,
        pct, pct_t,
        stepper_is_moving() ? "true" : "false",
        (unsigned)cfg->step_period_us,
        cfg->hold_when_stopped ? "true" : "false",
        (int)cfg->detent_steps,
        s_hk_paired ? "true" : "false",
        cfg->accessory_name, cfg->hap_setup_code);

    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_jog(httpd_req_t *r)
{
    char body[160];
    if (read_body(r, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(r);
    int  steps = json_get_int(body, "steps", 0);
    char which[16] = {0};
    json_get_str(body, "motor", which, sizeof(which));

    motor_id_t m = MOTOR_BOTH;
    if      (!strcmp(which, "left"))  m = MOTOR_LEFT;
    else if (!strcmp(which, "right")) m = MOTOR_RIGHT;

    if (steps) stepper_jog_motor(m, steps);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_move(httpd_req_t *r)
{
    char body[128];
    if (read_body(r, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(r);
    int pct = json_get_int(body, "percent", -1);
    if (pct < 0 || pct > 100)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "percent 0-100");
    int32_t full = app_settings_get()->full_open_steps;
    stepper_move_to((int32_t)((int64_t)full * pct / 100));
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_stop(httpd_req_t *r)
{
    stepper_stop();
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_calibrate(httpd_req_t *r)
{
    char body[160];
    if (read_body(r, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(r);
    char act[32] = {0};
    char which[16] = {0};
    json_get_str(body, "action", act, sizeof(act));
    json_get_str(body, "motor",  which, sizeof(which));

    motor_id_t m = MOTOR_BOTH;
    if      (!strcmp(which, "left"))  m = MOTOR_LEFT;
    else if (!strcmp(which, "right")) m = MOTOR_RIGHT;

    if (!strcmp(act, "set_zero")) {
        stepper_set_position(m, 0);
        if (m == MOTOR_BOTH) app_settings_set_last_position(0);
        ESP_LOGI(TAG, "calibrated: ZERO (%s)",
                 m == MOTOR_LEFT ? "LEFT" : m == MOTOR_RIGHT ? "RIGHT" : "BOTH");
    } else if (!strcmp(act, "set_open")) {
        /* "100% open" is a single number shared by both motors. We base it on
         * the LEFT motor's current step count (the primary position). After
         * calibration, both motors are claimed to be at full_open. */
        int32_t pos = stepper_get_motor_position(MOTOR_LEFT);
        if (pos < 100) pos = 100;
        app_settings_set_full_open(pos);
        stepper_set_position(MOTOR_BOTH, pos);   /* claim both at full_open */
        app_settings_set_last_position(pos);
        ESP_LOGI(TAG, "calibrated: FULL OPEN = %d steps", (int)pos);
    } else if (!strcmp(act, "sync_here")) {
        /* User claims both ropes are now at the same physical position --
         * snap RIGHT's logical count onto LEFT's so future sync moves stay aligned. */
        int32_t left = stepper_get_motor_position(MOTOR_LEFT);
        stepper_set_position(MOTOR_RIGHT, left);
        ESP_LOGI(TAG, "calibrated: synced RIGHT to LEFT = %d", (int)left);
    } else {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "action must be set_zero|set_open|sync_here");
    }
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_config(httpd_req_t *r)
{
    char body[384];
    if (read_body(r, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(r);

    int period   = json_get_int (body, "step_period_us",    -1);
    int hold     = json_get_int (body, "hold_when_stopped", -1);
    int fopen    = json_get_int (body, "full_open_steps",   -1);
    int detent   = json_get_int (body, "detent_steps",      -1);
    if (period > 0) {
        app_settings_set_step_period(period);
        stepper_set_period(period);
    }
    if (hold >= 0) {
        app_settings_set_hold(hold ? true : false);
        stepper_set_hold(hold ? true : false);
    }
    if (fopen > 0) app_settings_set_full_open(fopen);
    if (detent >= 0) {
        app_settings_set_detent(detent);
        /* Re-read after the setter so we pass the clamped value down. */
        stepper_set_detent(app_settings_get()->detent_steps);
    }

    char nm[33] = {0}, code[11] = {0};
    if (json_get_str(body, "name", nm, sizeof(nm)) && nm[0]) {
        app_settings_set_name(nm);
    }
    if (json_get_str(body, "setup_code", code, sizeof(code)) && code[0]) {
        app_settings_set_setup_code(code);
    }

    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static void deferred_reboot(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_reset(httpd_req_t *r)
{
    ESP_LOGW(TAG, "factory reset requested via API");
    app_settings_factory_reset();
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    xTaskCreate(deferred_reboot, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *r)
{
    ESP_LOGW(TAG, "reboot requested via API");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    xTaskCreate(deferred_reboot, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---- OTA --------------------------------------------------------------
 * Accepts the raw firmware .bin as the POST body. The browser uploads with
 * XMLHttpRequest so it can show an upload-progress bar.
 *
 * Flow: open next OTA partition -> stream-write the body -> verify ->
 *       set boot partition -> reboot. On any error, esp_ota_abort() and
 *       leave the running slot untouched so the device stays bootable.
 *
 * NOTE: there is no authentication here. The Web UI is open on the local
 * network. For DIY home use this is the usual trade-off; if exposing the
 * device beyond the LAN you'll want at least an HTTP basic-auth wrapper. */
static esp_err_t h_ota(httpd_req_t *r)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "no OTA partition available");
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition (still on factory image?)");
    }
    ESP_LOGI(TAG, "OTA target: '%s' @ 0x%" PRIx32 " size=%" PRIu32 " incoming=%d bytes",
             update->label, update->address, update->size, r->content_len);

    if (r->content_len <= 0 || r->content_len > (int)update->size) {
        ESP_LOGE(TAG, "OTA: bad content_len %d", r->content_len);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad content length");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    char buf[1536];
    int  total = 0;
    int  last_logged = -1;
    while (total < r->content_len) {
        int n = httpd_req_recv(r, buf, sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ESP_LOGE(TAG, "OTA recv error at %d/%d", total, r->content_len);
            esp_ota_abort(handle);
            return httpd_resp_send_500(r);
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d: %s", total, esp_err_to_name(err));
            esp_ota_abort(handle);
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       esp_err_to_name(err));
        }
        total += n;
        int pct = (int)((int64_t)total * 100 / r->content_len);
        if (pct / 10 != last_logged) {
            ESP_LOGI(TAG, "OTA: %d%% (%d / %d bytes)", pct, total, r->content_len);
            last_logged = pct / 10;
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "OTA complete (%d bytes); rebooting into '%s'", total, update->label);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    xTaskCreate(deferred_reboot, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---- WebSocket handler ------------------------------------------------ */

static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {
        /* Handshake. Track this fd as a connected client. */
        int fd = httpd_req_to_sockfd(r);
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
            if (!s_clients[i].used) {
                s_clients[i].handle = r->handle;
                s_clients[i].fd     = fd;
                s_clients[i].used   = true;
                ESP_LOGI(TAG, "ws client connected slot=%d fd=%d", i, fd);
                break;
            }
        }
        xSemaphoreGive(s_clients_lock);
        return ESP_OK;
    }

    /* Receive frame (mainly to detect CLOSE). */
    httpd_ws_frame_t f = {0};
    uint8_t buf[64] = {0};
    f.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(r, &f, sizeof(buf) - 1);
    if (err != ESP_OK) return err;
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(r);
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
            if (s_clients[i].used && s_clients[i].fd == fd) s_clients[i].used = false;
        }
        xSemaphoreGive(s_clients_lock);
    }
    return ESP_OK;
}

/* ---- Public ----------------------------------------------------------- */

void webui_set_hk_paired(bool paired) { s_hk_paired = paired; }

esp_err_t webui_start(void)
{
    s_clients_lock = xSemaphoreCreateMutex();
    s_log_lock     = xSemaphoreCreateMutex();
    s_log_sem      = xSemaphoreCreateCounting(LOG_RING_SZ, 0);

    /* Install log hook. */
    s_old_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    /* Start the log -> WS forwarder. */
    xTaskCreate(log_forwarder_task, "log_fwd", 4096, NULL, 4, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 6144;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %d", err);
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/",              .method = HTTP_GET,  .handler = h_index},
        {.uri = "/api/status",    .method = HTTP_GET,  .handler = h_status},
        {.uri = "/api/jog",       .method = HTTP_POST, .handler = h_jog},
        {.uri = "/api/move",      .method = HTTP_POST, .handler = h_move},
        {.uri = "/api/stop",      .method = HTTP_POST, .handler = h_stop},
        {.uri = "/api/calibrate", .method = HTTP_POST, .handler = h_calibrate},
        {.uri = "/api/config",    .method = HTTP_POST, .handler = h_config},
        {.uri = "/api/reset",     .method = HTTP_POST, .handler = h_reset},
        {.uri = "/api/reboot",    .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/ota",       .method = HTTP_POST, .handler = h_ota},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    /* WebSocket route - must set is_websocket = true */
    httpd_uri_t ws = {
        .uri = "/ws/logs", .method = HTTP_GET, .handler = h_ws,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_httpd, &ws);

    ESP_LOGI(TAG, "web UI up on port 80");
    return ESP_OK;
}
