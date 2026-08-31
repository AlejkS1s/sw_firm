#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "countdown.h"
#include "power.h"
#include "routines.h"
#include "sse.h"
#include "timing.h"

#include "http_handlers.h"
#include "http_util.h"

#define TAG "http_ctrl"



/* ══════════════════════════════════════════════════════════════════════════
 * POST /api/v1/relay  { "action": "on" | "off" | "toggle" }
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t post_relay(httpd_req_t *req) {
    int64_t t0 = esp_timer_get_time();
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    const char *action = json_get_string(root, "action", "");
    esp_err_t e = ESP_FAIL;
    if (strcmp(action, "on") == 0)          { relay_set(true); e = ESP_OK; }
    else if (strcmp(action, "off") == 0)    { relay_set(false); e = ESP_OK; }
    else if (strcmp(action, "toggle") == 0) { relay_toggle(); e = ESP_OK; }
    else { cJSON_Delete(root); return send_error(req, E_INVALID_ARG, "action must be one of on|off|toggle"); }
    cJSON_Delete(root);

    int64_t dt_us = esp_timer_get_time() - t0;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "relay %s FAILED (%s, %lu us)", action, esp_err_to_name(e), (unsigned long)dt_us);
        return send_error(req, E_INTERNAL, "relay command failed");
    }

    ESP_LOGI(TAG, "relay %s -> %s (%lu us)", action, BOOL_STR(relay_get()), (unsigned long)dt_us);
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"relay\":%s}", BOOL_STR(relay_get()));
    return send_json(req, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUT /api/v1/config/boot | led | timezone | power-save | auto-off
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t put_boot(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    int mode = json_get_int(root, "mode", -1);
    cJSON_Delete(root);
    if (mode < 0 || mode > RELAY_BOOT_AUTO)
        return send_error(req, E_INVALID_ARG, "mode out of range");
    relay_set_boot_behavior((uint8_t)mode);
    ESP_LOGI(TAG, "boot mode -> %d", mode);
    return send_ok(req);
}

esp_err_t put_led(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    int mode = json_get_int(root, "mode", -1);
    cJSON_Delete(root);
    if (mode < 0 || mode > LED_MODE_MAX)
        return send_error(req, E_INVALID_ARG, "mode out of range");
    led_set_mode((uint8_t)mode);
    ESP_LOGI(TAG, "led mode -> 0x%02x", mode);
    return send_ok(req);
}

esp_err_t put_tz(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    const char *tz = json_get_string(root, "tz", NULL);
    esp_err_t e = tz ? timing_set_timezone(tz) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (e != ESP_OK) return send_error(req, E_INVALID_ARG, "invalid timezone string");
    ESP_LOGI(TAG, "timezone -> %s", tz);
    return send_ok(req);
}

/* Toggling WiFi power-save is a config change, not an "action" — it has a
 * persisted, queryable current value (see state.c's "power_save" field),
 * same as boot mode or LED mode. That's why it's PUT under /config rather
 * than a POST verb. */
esp_err_t put_power_save(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    bool disabled = json_get_bool(root, "disabled", false);
    bool has_field = cJSON_GetObjectItem(root, "disabled") != NULL;
    cJSON_Delete(root);

    if (!has_field) return send_error(req, E_INVALID_ARG, "disabled (bool) is required");
    power_set_save_disabled(disabled);
    ESP_LOGI(TAG, "power-save -> %s", disabled ? "disabled" : "enabled");
    return send_ok(req);
}

/* Auto-off used to be POST (arm) + DELETE (disarm) at /api/v1/auto-off.
 * Folded into a single PUT under /config: it has exactly the same shape
 * as every other config resource here — "what should this be set to" —
 * and enabled:false disarming it is more consistent than a separate verb
 * and path for the same resource. */
esp_err_t put_auto_off(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    bool enabled = json_get_bool(root, "enabled", false);
    uint8_t h = (uint8_t)json_get_int(root, "h", 0);
    uint8_t m = (uint8_t)json_get_int(root, "m", 0);
    uint8_t s = (uint8_t)json_get_int(root, "s", 0);
    cJSON_Delete(root);

    if (!enabled) {
        relay_auto_off_clear();
        ESP_LOGI(TAG, "auto-off cleared");
        return send_ok(req);
    }

    uint32_t total_s = HMS_TO_SEC(h, m, s);
    if (total_s == 0 || total_s > MAX_DURATION_S)
        return send_error(req, E_INVALID_ARG, "duration must be between 1 second and 24 hours");
    /* Symmetric to check_auto_off_conflict(): reject arming auto-off while
     * any routine is enabled. The safety auto-off must never fight an
     * active schedule/circulate. */
    if (routine_any_enabled()) {
        ESP_LOGW(TAG, "auto-off arm rejected: routine enabled");
        return send_error(req, E_CONFLICT, "a routine is currently active; disable it before arming auto-off");
    }
    if (!relay_set_auto_off(h, m, s))
        return send_error(req, E_CONFLICT, "a timer or routine is currently active");
    ESP_LOGI(TAG, "auto-off set %02d:%02d:%02d", h, m, s);
    return send_ok(req);
}

/* ══════════════════════════════════════════════════════════════════════════
 * POST/DELETE /api/v1/timer
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t post_timer(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    uint32_t dur = (uint32_t)json_get_int(root, "dur", 0);
    bool relay_on = json_get_bool(root, "on", false);
    cJSON_Delete(root);

    if (dur == 0 || dur > MAX_DURATION_S)
        return send_error(req, E_INVALID_ARG, "dur must be between 1 and 86400 seconds");
    esp_err_t conflict = check_auto_off_conflict(req);
    if (conflict != ESP_OK) return ESP_OK;   /* response already sent by send_error() */

    countdown_set(dur, relay_on);
    ESP_LOGI(TAG, "timer set %lus -> %s", (unsigned long)dur, RELAY_STR(relay_on));
    return send_ok(req);
}

esp_err_t delete_timer(httpd_req_t *req) {
    countdown_cancel();
    ESP_LOGI(TAG, "timer cancelled");
    return send_ok(req);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUT /api/v1/config/sse  { "enabled": true | false }
 *
 * Toggles the entire SSE subsystem. When disabled, all existing SSE
 * connections are closed immediately and new registrations are rejected
 * with 503. The setting is persisted to NVS and survives reboots.
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t put_sse(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    bool enabled = json_get_bool(root, "enabled", true);
    bool has_field = cJSON_GetObjectItem(root, "enabled") != NULL;
    cJSON_Delete(root);

    if (!has_field) return send_error(req, E_INVALID_ARG, "enabled (bool) is required");
    sse_set_enabled(enabled);
    ESP_LOGI(TAG, "sse -> %s", enabled ? "enabled" : "disabled");
    return send_ok(req);
}
