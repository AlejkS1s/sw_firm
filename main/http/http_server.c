#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "power.h"
#include "sse.h"
#include "http_handlers.h"
#include "http_util.h"
#include "clients.h"

#define TAG "http_routes"
#define HTTP_PORT 80
#define MAX_URI_HANDLERS 40
/* Below the control task (routines_task, prio 5): GPIO/scheduling outrank
 * HTTP under load. esp_timer daemon sits at 2. */
#define HTTPD_TASK_PRIO 3
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Diagnostic heartbeat — fires every 30s on the httpd task to surface
 * silent hangs (where the device stops responding but no error is logged).
 * Logs a simple mark so serial gaps >30s between "dispatch" lines = hang.
 * Also logs WiFi RSSI so correlation between weak signal and hangs is visible. */
static void heartbeat_cb(void *arg) {
    (void)arg;
    int8_t rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;
    ESP_LOGW(TAG, "HB: alive rssi=%ddb sse=%d heap=%lu/%lu",
             (int)rssi, sse_client_count(),
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
}

/* ══════════════════════════════════════════════════════════════════════════
 * Route table — the entire URI surface in one place. This is the single
 * source of truth for both the handler dispatch table AND CORS preflight
 * registration (any non-GET entry automatically gets an OPTIONS handler).
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} route_t;

static const route_t s_routes[] = {
    {"/api/v1/state",             HTTP_GET,    get_state},
    {"/api/v1/state/stream",      HTTP_GET,    get_state_stream},
    {"/api/v1/system",            HTTP_GET,    get_system},
    {"/api/v1/system/reset",      HTTP_POST,   post_system_reset},
    {"/api/v1/system/factory-reset", HTTP_POST, post_system_factory_reset},
    {"/api/v1/ping",              HTTP_GET,    get_ping},
    {"/api/v1/id",                HTTP_GET,    get_id},
    {"/api/v1/relay",             HTTP_POST,   post_relay},
    {"/api/v1/config/boot",       HTTP_PUT,    put_boot},
    {"/api/v1/config/led",        HTTP_PUT,    put_led},
    {"/api/v1/config/timezone",   HTTP_PUT,    put_tz},
    {"/api/v1/config/power-save", HTTP_PUT,    put_power_save},
    {"/api/v1/config/auto-off",   HTTP_PUT,    put_auto_off},
    {"/api/v1/config/sse",        HTTP_PUT,    put_sse},
    {"/api/v1/timer",             HTTP_POST,   post_timer},
    {"/api/v1/timer",             HTTP_DELETE, delete_timer},
    {"/api/v1/routines",          HTTP_GET,    get_routines},
    {"/api/v1/routines",          HTTP_POST,   post_routines},
    {"/api/v1/routines",          HTTP_PUT,    put_routine},
    {"/api/v1/routines",          HTTP_DELETE, delete_routine},
};

/* ══════════════════════════════════════════════════════════════════════════
 * Dispatcher — one httpd_uri_t handler is registered per route, all
 * pointing here; this looks the actual URI (minus query string) + method
 * up in the table and forwards to the real handler.
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *method_name(httpd_method_t m) {
    switch (m) {
    case HTTP_GET:     return "GET";
    case HTTP_POST:    return "POST";
    case HTTP_PUT:     return "PUT";
    case HTTP_DELETE:  return "DELETE";
    case HTTP_OPTIONS: return "OPTIONS";
    default:           return "?";
    }
}

static esp_err_t dispatch(httpd_req_t *req) {
    if (req->method == HTTP_OPTIONS) {
        set_cors(req);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, If-None-Match");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    power_notify_activity();

    /* Record the client by ID so /system reports a correct connection count
     * even for REST-only clients (not just open SSE sockets). */
    char _cid[CLIENT_ID_MAX_LEN];
    client_extract_id(req, _cid, sizeof(_cid));
    if (_cid[0]) client_seen(_cid);

    const char *q = uri_query_start(req);
    size_t plen = q ? (size_t)(q - req->uri) : strlen(req->uri);

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    const char *matched = NULL;

    ESP_LOGI(TAG, "[DBG] dispatch IN: %s %s heap=%lu",
             method_name(req->method), req->uri,
             (unsigned long)esp_get_free_heap_size());

    for (int i = 0; i < (int)ARRAY_LEN(s_routes); i++) {
        if (req->method == s_routes[i].method &&
            plen == strlen(s_routes[i].uri) &&
            memcmp(req->uri, s_routes[i].uri, plen) == 0) {
            matched = s_routes[i].uri;
            ret = s_routes[i].handler(req);
            break;
        }
    }

    int64_t dt_us = esp_timer_get_time() - t0;
    if (matched) {
        ESP_LOGI(TAG, "%s %s -> %s (%lu us) [heap=%lu]",
                 method_name(req->method), matched,
                 esp_err_to_name(ret), (unsigned long)dt_us,
                 (unsigned long)esp_get_free_heap_size());
    } else {
        ESP_LOGW(TAG, "%s %s -> 404 (%lu us)",
                 method_name(req->method), req->uri, (unsigned long)dt_us);
        ret = send_error(req, E_NOT_FOUND, "no such endpoint");
    }
    ESP_LOGI(TAG, "[DBG] dispatch OUT: %s %s heap=%lu",
             method_name(req->method), matched ? matched : req->uri,
             (unsigned long)esp_get_free_heap_size());
    return ret;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Bootstrap
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t http_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_uri_handlers = MAX_URI_HANDLERS;
    /* NOTE: verify against CONFIG_LWIP_MAX_SOCKETS in your sdkconfig — mDNS
     * and SNTP also draw from the same socket pool on ESP8266. Lower this
     * if httpd_start() logs socket-allocation failures. */
    cfg.max_open_sockets = 7;
    /* Single-core ESP8266: the httpd task shares one core with lwIP/WiFi.
     * A 10s send/recv timeout means a slow `send()` blocks the httpd task
     * for up to 10s while lwIP cannot push ANY other socket's data to the
     * radio. On a weak WiFi link this manifests as: dispatch logs the
     * request + response, the handler returns, but the response bytes
     * never reach the client (browser times out). 2s is plenty for a
     * <1KB JSON response on a healthy link; if a send blocks for 2s the
     * link is broken and the client should retry on a fresh socket. */
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    cfg.task_priority     = HTTPD_TASK_PRIO;
    /* If every socket is a held-open SSE stream, an incoming REST command
     * must still be able to get through — reclaim the oldest connection
     * rather than reject the new one. */
    cfg.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP on port %d", cfg.server_port);
    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Diagnostic heartbeat — 30s periodic log so silent httpd hangs are
     * visible. ESP_LOGW to land in any default-level boot log. */
    esp_timer_handle_t hb_timer;
    esp_timer_create_args_t hb_args = {
        .callback = &heartbeat_cb,
        .name     = "http_hb",
    };
    if (esp_timer_create(&hb_args, &hb_timer) == ESP_OK)
        esp_timer_start_periodic(hb_timer, 30000000ULL);   /* 30 s */

    esp_err_t result = ESP_OK;
    for (int i = 0; i < (int)ARRAY_LEN(s_routes); i++) {
        httpd_uri_t u = { .uri = s_routes[i].uri, .method = s_routes[i].method, .handler = dispatch };
        esp_err_t e = httpd_register_uri_handler(server, &u);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s %s %s",
                     s_routes[i].uri, s_routes[i].method == HTTP_GET ? "GET" : "POST",
                     esp_err_to_name(e));
            result = e;
        }
    }

    /* CORS preflight (OPTIONS) for EVERY route, including GET-only endpoints.
     * Chrome's Private Network Access requires a successful preflight before a
     * file:// (or other cross-origin) page may talk to a private IP such as
     * 192.168.x.x; without an OPTIONS handler on GET routes the device answers
     * 405 and the browser blocks every request. dispatch() already returns the
     * correct CORS + Access-Control-Allow-Private-Network headers. Deduplication
     * relies on same-URI routes being adjacent in s_routes[] — keep them grouped. */
    const char *last = NULL;
    for (int i = 0; i < (int)ARRAY_LEN(s_routes); i++) {
        if (last && strcmp(s_routes[i].uri, last) == 0) continue;
        httpd_uri_t u = { .uri = s_routes[i].uri, .method = HTTP_OPTIONS, .handler = dispatch };
        esp_err_t e = httpd_register_uri_handler(server, &u);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reg OPTIONS %s: %s", s_routes[i].uri, esp_err_to_name(e));
            result = e;
        }
        last = s_routes[i].uri;
    }
    return result;
}
