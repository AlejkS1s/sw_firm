#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/ip4_addr.h"

#include "diagnostics.h"
#include "power.h"
#include "sse.h"
#include "state.h"
#include "nvs_store.h"
#include "timing.h"

#include "http_handlers.h"
#include "http_util.h"

#define TAG "http_state"

#define DEVICE_ID_MIN_LEN 7   /* 6 hex chars + NUL */


/* ── Device ID ─────────────────────────────────────────────────────────────
 * Derives a short alphanumeric device ID from the STA MAC. The ID is
 * stable across reboots and factory resets (MAC is immutable). Format:
 * last 3 bytes of MAC as uppercase hex, e.g. "A1B2C3".
 * The buffer must be at least DEVICE_ID_MIN_LEN (7) bytes.
 * ──────────────────────────────────────────────────────────────────────── */
static void device_id(char *buf, size_t buflen) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, buflen, "%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}


/* ══════════════════════════════════════════════════════════════════════════
 * GET /api/v1/state — ETag-validated snapshot of control/observable state.
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t get_state(httpd_req_t *req) {
    uint32_t hash = state_get_hash();
    char etag[12];
    snprintf(etag, sizeof(etag), "\"%08x\"", (unsigned)hash);
    httpd_resp_set_hdr(req, "ETag", etag);

    char inm[12] = {0};
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK) {
        const char *p = (inm[0] == '"') ? inm + 1 : inm;
        uint32_t client_hash = (uint32_t)strtoul(p, NULL, 16);
        if (client_hash == hash) {
            set_cors(req);
            httpd_resp_set_status(req, "304 Not Modified");
            ESP_LOGD(TAG, "GET state -> 304 (etag=%08x)", (unsigned)hash);
            return httpd_resp_send(req, NULL, 0);
        }
    }

    char buf[JSON_BUF_SIZE];
    size_t n = state_build_json(buf, sizeof(buf));
    if (n == 0) return send_error(req, E_INTERNAL, "state serialization failed");
    ESP_LOGD(TAG, "GET state -> 200 (etag=%08x, %u bytes)", (unsigned)hash, (unsigned)n);
    return send_json(req, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * GET /api/v1/state/stream — Server-Sent Events push.
 *
 * The SSE handler sends an initial snapshot and returns immediately (the
 * httpd is single-threaded — blocking it blocks all other connections). A
 * separate SSE module (sse.c) keeps the client socket fd and uses
 * httpd_queue_work() to push state updates into the httpd task's select()
 * loop, where they are sent via raw send() as chunked-encoding chunks.
 * The response is never finalized (no NULL chunk), so the TCP connection
 * stays open for future pushes. On client disconnect, the httpd detects
 * the closed socket via its select() loop and cleans up the session;
 * sse_push_work() detects the failure and marks the client inactive.
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t get_state_stream(httpd_req_t *req) {
    if (!sse_register(req)) {
        ESP_LOGW(TAG, "SSE registration rejected (all slots full)");
        httpd_resp_set_status(req, "503 Service Unavailable");
        set_cors(req);
        return httpd_resp_send(req, NULL, 0);
    }

    ESP_LOGI(TAG, "SSE stream opened");
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    set_cors(req);

    state_snapshot_t snap;
    state_snapshot_build(&snap);

    char buf[JSON_BUF_SIZE + 64];
    int off = snprintf(buf, sizeof(buf), "id: %lu\nevent: state\ndata: ",
                        (unsigned long)g_state_version);
    if (off > 0 && (size_t)off < sizeof(buf)) {
        size_t slen = state_build_ssedata(buf + off, sizeof(buf) - off, NULL, &snap);
        if (slen > 0) {
            off += (int)slen;
            int n = snprintf(buf + off, sizeof(buf) - off, "\n\n");
            if (n > 0) off += n;
            httpd_resp_send_chunk(req, buf, off);
        }
    }

    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GET /api/v1/info — device identity & network metadata only. Everything
 * control-related lives in /state; duplicating it here was the original
 * design's redundant surface.
 * ══════════════════════════════════════════════════════════════════════════ */
/* ══════════════════════════════════════════════════════════════════════════
 * GET /api/v1/system — aggregated device identity + system diagnostics,
 * combining info and diagnostics into a single response. Replaces the
 * original /api/v1/cache and the old /api/v1/system (diagnostics only).
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t get_system(httpd_req_t *req) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char dev_id[DEVICE_ID_MIN_LEN];
    device_id(dev_id, sizeof(dev_id));

    char ssid_buf[36] = "";
    int8_t rssi = 0;
    uint8_t channel = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(ssid_buf, ap.ssid, sizeof(ap.ssid));
        ssid_buf[sizeof(ap.ssid) - 1] = '\0';
        rssi = ap.rssi;
        channel = ap.primary;
    }

    tcpip_adapter_ip_info_t ip;
    char ip_str[16] = "";
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK)
        strncpy(ip_str, ip4addr_ntoa(&ip.ip), sizeof(ip_str) - 1);

    /* Build system-specific identity fields first, then splice the
     * diagnostics JSON (rreason, boots, uptm, heap, cores, tsync, etc.)
     * into the same response via diagnostics_build_json(). Both use the
     * same field names ("rreason", "boots", ...), so no renaming needed. */
    char diag[JSON_BUF_SIZE];
    size_t diag_len = diagnostics_build_json(diag, sizeof(diag));
    if (diag_len == 0)
        return send_error(req, E_INTERNAL, "diagnostics failed");

    char buf[JSON_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"id\":\"%s\","
        "\"mac\":\"" MACSTR "\","
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"ch\":%d,"
        "\"ip\":\"%s\","
        "\"fwver\":\"" FW_VER "\","
        "\"fwbld\":\"" FW_BUILD "\","
        "\"tz\":\"%s\","
        "%.*s"
        "}",
        dev_id, MAC2STR(mac), ssid_buf, rssi, channel, ip_str,
        timing_get_timezone(),
        (int)(diag_len - 2), diag + 1);  /* strip outer { } */

    if (n < 0 || (size_t)n >= sizeof(buf))
        return send_error(req, E_INTERNAL, "system serialization failed");
    return send_json(req, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * POST /api/v1/system/reset — remote soft reboot.
 *
 * Responds first, then reboots ~300ms later via a one-shot esp_timer so
 * the HTTP response actually reaches the client before the connection
 * dies. Calling esp_restart() synchronously inside the handler would race
 * the response against the reboot and usually lose.
 * ══════════════════════════════════════════════════════════════════════════ */

static bool s_reset_scheduled = false;
static esp_timer_handle_t s_reset_timer;

static void reset_timer_cb(void *arg) {
    (void)arg;
    s_reset_scheduled = false;
    esp_restart();
}

esp_err_t post_system_reset(httpd_req_t *req) {
    if (!s_reset_scheduled) {
        s_reset_scheduled = true;
        esp_err_t e = timer_create_and_start(&reset_timer_cb, "sys_reset",
                                              &s_reset_timer, 300000, false);
        if (e != ESP_OK) esp_restart();
        ESP_LOGW(TAG, "Reboot requested via API, rebooting in 300ms");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"rebooting\"}");
}

/* ══════════════════════════════════════════════════════════════════════════
 * POST /api/v1/system/factory-reset — wipe all NVS namespaces and reboot.
 *
 * Erases every persisted configuration (relay, boot mode, LED mode, auto-off,
 * WiFi credentials, routines, countdown, time, diagnostics, power settings)
 * then re-uses the same deferred-reboot pattern as post_system_reset so the
 * HTTP response reaches the client before the connection dies.
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t post_system_factory_reset(httpd_req_t *req) {
    nvs_store_erase_all(NVS_NS_RELAY);
    nvs_store_erase_all(NVS_NS_LED);
    nvs_store_erase_all(NVS_NS_WIFI_CREDS);
    nvs_store_erase_all(NVS_NS_ROUTINES);
    nvs_store_erase_all(NVS_NS_COUNTDOWN);
    nvs_store_erase_all(NVS_NS_TIME);
    nvs_store_erase_all(NVS_NS_DIAG);
    nvs_store_erase_all(NVS_NS_POWER);
    nvs_store_erase_all(NVS_NS_SSE);
    if (!s_reset_scheduled) {
        s_reset_scheduled = true;
        esp_err_t e = timer_create_and_start(&reset_timer_cb, "sys_fct_rs",
                                              &s_reset_timer, 300000, false);
        if (e != ESP_OK) esp_restart();
        ESP_LOGW(TAG, "Factory reset requested via API, rebooting in 300ms");
    }
    return send_json(req, "{\"ok\":true,\"message\":\"factory reset, rebooting\"}");
}

esp_err_t get_ping(httpd_req_t *req) {
    return send_ok(req);
}
