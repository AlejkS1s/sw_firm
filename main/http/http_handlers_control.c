#include <string.h>

#include "cJSON.h"

#include "board.h"
#include "countdown.h"
#include "power.h"
#include "timing.h"

#include "http_handlers.h"
#include "http_util.h"



/* ══════════════════════════════════════════════════════════════════════════
 * POST /api/v1/relay  { "action": "on" | "off" | "toggle" }
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t post_relay(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    const char *action = json_get_string(root, "action", "");
    esp_err_t e = ESP_FAIL;
    if (strcmp(action, "on") == 0)          e = relay_set_sync(true);
    else if (strcmp(action, "off") == 0)    e = relay_set_sync(false);
    else if (strcmp(action, "toggle") == 0) e = relay_toggle_sync();
    else { cJSON_Delete(root); return send_error(req, E_INVALID_ARG, "action must be one of on|off|toggle"); }
    cJSON_Delete(root);

    if (e != ESP_OK) return send_error(req, E_INTERNAL, "relay command failed");

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
    return send_ok(req);
}

esp_err_t put_tz(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);
    const char *tz = json_get_string(root, "tz", NULL);
    esp_err_t e = tz ? timing_set_timezone(tz) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (e != ESP_OK) return send_error(req, E_INVALID_ARG, "invalid timezone string");
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
        return send_ok(req);
    }

    uint32_t total_s = HMS_TO_SEC(h, m, s);
    if (total_s == 0 || total_s > MAX_DURATION_S)
        return send_error(req, E_INVALID_ARG, "duration must be between 1 second and 24 hours");
    if (!relay_set_auto_off(h, m, s))
        return send_error(req, E_CONFLICT, "a timer or routine is currently active");
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
    if (conflict != ESP_OK) return conflict;

    countdown_set(dur, relay_on);
    return send_ok(req);
}

esp_err_t delete_timer(httpd_req_t *req) {
    countdown_cancel();
    return send_ok(req);
}
