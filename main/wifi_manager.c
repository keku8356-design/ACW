#include "wifi_manager.h"

#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_settings.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED   BIT0
#define BIT_FAILED      BIT1

static EventGroupHandle_t   s_events;
static wifi_manager_state_t s_state    = WIFI_MODE_NONE;
static esp_netif_t         *s_sta_netif = NULL;
static esp_netif_t         *s_ap_netif  = NULL;
static httpd_handle_t       s_setup_httpd = NULL;
static int                  s_retry = 0;
static const int            MAX_RETRY = 5;

/* --- Captive-portal-style HTML, fully self-contained (no internet needed) - */
static const char SETUP_PAGE[] =
"<!doctype html><html lang=en><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AirCover Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0a0a0a;color:#e8b860;font-family:ui-monospace,Menlo,Consolas,monospace;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".card{width:100%;max-width:420px;border:1px solid #e8b860;padding:24px;background:#111}"
"h1{font-size:14px;letter-spacing:.3em;border-bottom:1px solid #e8b860;padding-bottom:12px;margin-bottom:20px}"
".lbl{display:block;font-size:11px;letter-spacing:.2em;margin:18px 0 6px;color:#a07a3c}"
"input,select{width:100%;background:#000;border:1px solid #444;color:#e8b860;"
"padding:10px;font-family:inherit;font-size:14px}"
"input:focus,select:focus{outline:0;border-color:#e8b860}"
"button{width:100%;background:#e8b860;color:#000;border:0;padding:12px;margin-top:24px;"
"font-family:inherit;font-weight:700;letter-spacing:.2em;cursor:pointer}"
"button:hover{background:#f5c97a}"
".note{font-size:11px;color:#666;margin-top:16px;line-height:1.6}"
".net{font-size:13px;padding:8px 10px;border:1px solid #333;cursor:pointer;margin-top:6px}"
".net:hover{border-color:#e8b860}"
".bars{float:right;color:#888}"
"</style>"
"<div class=card>"
"<h1>AIRCOVER &middot; WIFI SETUP</h1>"
"<div id=scan></div>"
"<label class=lbl>SSID</label>"
"<input id=ssid maxlength=32 autocomplete=off>"
"<label class=lbl>PASSWORD</label>"
"<input id=pass type=password maxlength=64 autocomplete=off>"
"<button onclick=save()>SAVE &amp; REBOOT</button>"
"<div class=note id=msg>The device will reboot and try to join the network.<br>"
"If it succeeds, find it on your home network at <b>aircover.local</b>.</div>"
"</div>"
"<script>"
"async function scan(){"
"  try{let r=await fetch('/scan');let j=await r.json();"
"    let h='<label class=\"lbl\">DETECTED NETWORKS</label>';"
"    j.forEach(n=>{h+=`<div class=net onclick=\"document.getElementById('ssid').value='${n.s.replace(/'/g,\"\\\\'\")}'\">${n.s}<span class=bars>${'|'.repeat(Math.max(1,Math.round((n.r+90)/10)))}</span></div>`;});"
"    document.getElementById('scan').innerHTML=h;"
"  }catch(e){}"
"}"
"async function save(){"
"  let s=document.getElementById('ssid').value.trim();"
"  if(!s){document.getElementById('msg').innerHTML='SSID required.';return;}"
"  let p=document.getElementById('pass').value;"
"  document.getElementById('msg').innerHTML='Saving&hellip;';"
"  let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p})});"
"  if(r.ok){document.getElementById('msg').innerHTML='Saved. Rebooting&hellip;';}"
"  else{document.getElementById('msg').innerHTML='Error saving.';}"
"}"
"scan();"
"</script></html>";

/* ------------------------------------------------------------------------ */

static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) {
            ++s_retry;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

/* ---- SoftAP HTTP handlers ---------------------------------------------- */

static void deferred_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, SETUP_PAGE, sizeof(SETUP_PAGE) - 1);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    wifi_scan_config_t sc = {0};
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 16) n = 16;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    esp_wifi_scan_get_ap_records(&n, recs);

    /* Build a small JSON array. */
    char *buf = malloc(2048);
    int   off = 0;
    off += snprintf(buf + off, 2048 - off, "[");
    for (int i = 0; i < n; ++i) {
        off += snprintf(buf + off, 2048 - off,
                        "%s{\"s\":\"%s\",\"r\":%d}",
                        i ? "," : "",
                        (char*)recs[i].ssid,
                        recs[i].rssi);
        if (off > 1900) break;
    }
    off += snprintf(buf + off, 2048 - off, "]");
    free(recs);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, buf, off);
    free(buf);
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *r)
{
    char body[256] = {0};
    int  len = httpd_req_recv(r, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    /* Tiny inline parser: look for "ssid":"..." and "pass":"..." */
    char ssid[33] = {0};
    char pass[65] = {0};
    char *p;
    if ((p = strstr(body, "\"ssid\""))) {
        p = strchr(p, ':'); if (p) p = strchr(p, '"');
        if (p) { ++p; char *q = strchr(p, '"');
            if (q && q - p < (int)sizeof(ssid)) { memcpy(ssid, p, q - p); ssid[q - p] = 0; } }
    }
    if ((p = strstr(body, "\"pass\""))) {
        p = strchr(p, ':'); if (p) p = strchr(p, '"');
        if (p) { ++p; char *q = strchr(p, '"');
            if (q && q - p < (int)sizeof(pass)) { memcpy(pass, p, q - p); pass[q - p] = 0; } }
    }
    if (!ssid[0]) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saving new wifi: ssid='%s'", ssid);
    app_settings_set_wifi(ssid, pass);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "ok");
    /* Reboot a short moment later so the response can be flushed. */
    xTaskCreate(deferred_reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void start_softap(const char *ap_ssid)
{
    ESP_LOGW(TAG, "starting SoftAP setup mode: '%s'", ap_ssid);
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t apc = {0};
    strncpy((char*)apc.ap.ssid, ap_ssid, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len      = strlen(ap_ssid);
    apc.ap.channel       = 1;
    apc.ap.max_connection = 4;
    apc.ap.authmode      = WIFI_AUTH_OPEN;

    /* Need APSTA so we can scan visible networks for the dropdown. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    httpd_start(&s_setup_httpd, &cfg);
    httpd_uri_t u_root = {.uri="/",     .method=HTTP_GET,  .handler=h_root};
    httpd_uri_t u_scan = {.uri="/scan", .method=HTTP_GET,  .handler=h_scan};
    httpd_uri_t u_save = {.uri="/save", .method=HTTP_POST, .handler=h_save};
    httpd_register_uri_handler(s_setup_httpd, &u_root);
    httpd_register_uri_handler(s_setup_httpd, &u_scan);
    httpd_register_uri_handler(s_setup_httpd, &u_save);

    s_state = WIFI_MODE_SOFTAP_SETUP;
    ESP_LOGI(TAG, "setup portal up: connect to '%s' then open http://192.168.4.1", ap_ssid);
}

/* ------------------------------------------------------------------------ */

esp_err_t wifi_manager_start(uint32_t sta_timeout_ms, const char *ap_ssid)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const app_settings_t *cfg = app_settings_get();
    if (cfg->wifi_configured && cfg->wifi_ssid[0]) {
        ESP_LOGI(TAG, "connecting to stored SSID '%s'", cfg->wifi_ssid);
        wifi_config_t stac = {0};
        strncpy((char*)stac.sta.ssid,     cfg->wifi_ssid, sizeof(stac.sta.ssid));
        strncpy((char*)stac.sta.password, cfg->wifi_pass, sizeof(stac.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &stac));
        ESP_ERROR_CHECK(esp_wifi_start());

        s_state = WIFI_MODE_STA_CONNECTING;
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(sta_timeout_ms));
        if (bits & BIT_CONNECTED) {
            s_state = WIFI_MODE_STA_CONNECTED;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "STA connect timed out, falling back to SoftAP");
        esp_wifi_stop();
    }

    start_softap(ap_ssid);
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)   { return s_state; }
bool wifi_manager_is_connected(void) { return s_state == WIFI_MODE_STA_CONNECTED; }
esp_netif_t *wifi_manager_get_sta_netif(void) { return s_sta_netif; }
