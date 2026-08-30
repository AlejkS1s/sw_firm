#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "board.h"
#include "routines.h"
#include "state.h"
#include "timing.h"

#include "http_handlers.h"
#include "http_util.h"

#define TAG "http_rtn"

static size_t routine_to_json(const routine_entry_t *e, int idx, char *buf, size_t buflen) {
    int n = snprintf(buf, buflen,
        "{\"i\":%d,\"t\":%d,\"h\":%d,\"m\":%d,\"eh\":%d,\"em\":%d,"
        "\"ion\":%u,\"ioff\":%u,\"dur\":%lu,\"d\":%d,"
        "\"ds\":%lu,\"de\":%lu,"
        "\"on\":%s,\"e\":%s}",
        idx, e->type, e->hour, e->minute,
        e->end_hour, e->end_minute,
        e->interval_on, e->interval_off,
        (unsigned long)e->duration_s, e->days,
        (unsigned long)e->date_start, (unsigned long)e->date_end,
        BOOL_STR(e->relay_on), BOOL_STR(e->enabled));
    return snprintf_guard(n, buflen);
}

/* ══════════════════════════════════════════════════════════════════════════
 * GET /api/v1/routines[?id=N]
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t get_routines(httpd_req_t *req) {
    const char *q = uri_query_start(req);
    if (q) {
        char idv[8];
        if (httpd_query_key_value(q + 1, "id", idv, sizeof(idv)) == ESP_OK) {
            int idx = (int)strtol(idv, NULL, 10);
            routine_handle_t h = routine_at(idx);
            if (!h) {
                ESP_LOGW(TAG, "GET routine id=%d not found", idx);
                return send_error(req, E_NOT_FOUND, ERR_ROUTINE_NOT_FOUND);
            }
            ESP_LOGI(TAG, "GET routine id=%d", idx);
            char buf[JSON_BUF_SIZE];
            routine_to_json(h, idx, buf, sizeof(buf));
            return send_json(req, buf);
        }
    }

    ESP_LOGI(TAG, "GET routines (all)");
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    set_cors(req);

    char buf[JSON_BUF_SIZE];
    httpd_resp_send_chunk(req, "[", 1);
    for (int i = 0; i < ROUTINES_MAX; i++) {
        routine_handle_t h = routine_at(i);
        if (!h) continue;
        size_t len = routine_to_json(h, i, buf, sizeof(buf));
        if (i > 0) httpd_resp_send_chunk(req, ",", 1);
        httpd_resp_send_chunk(req, buf, len);
    }
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * POST /api/v1/routines
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t post_routines(httpd_req_t *req) {
    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    routine_entry_t entry = { .in_use = true, .enabled = true, .magic = ROUTINE_BLOB_MAGIC };
    uint8_t type = (uint8_t)json_get_int(root, "t", 0);
    /* Start hour/minute. `h`/`m` are the documented + frontend keys; `sh`/`sm`
     * are accepted as a legacy fallback. */
    entry.hour       = (uint8_t)json_get_int(root, "h", json_get_int(root, "sh", 0));
    entry.minute     = (uint8_t)json_get_int(root, "m", json_get_int(root, "sm", 0));
    entry.end_hour   = (uint8_t)json_get_int(root, "eh", 0);
    entry.end_minute = (uint8_t)json_get_int(root, "em", 0);
    entry.interval_on  = (uint16_t)json_get_int(root, "ion", 0);
    entry.interval_off = (uint16_t)json_get_int(root, "ioff", 0);
    entry.duration_s   = (uint32_t)json_get_int(root, "dur", 0);
    entry.days         = (uint8_t)(json_get_int(root, "d", 0) & DAYS_MASK);
    entry.date_start   = (uint32_t)json_get_int(root, "ds", 0);
    entry.date_end     = (uint32_t)json_get_int(root, "de", 0);
    entry.relay_on     = json_get_bool(root, "on", false);
    cJSON_Delete(root);

    if (type != RT_SCHEDULE && type != RT_CIRCULATE)
        return send_error(req, E_INVALID_ARG, "unknown routine type");

    esp_err_t conflict = check_auto_off_conflict(req);
    if (conflict != ESP_OK) return conflict;

    routine_handle_t h = routine_create(type, &entry);
    if (!h) {
        ESP_LOGW(TAG, "POST routine type=%d FAILED", type);
        return send_error(req, E_CONFLICT, "overlap, invalid config, or storage full");
    }

    int idx = routine_index(h);
    ESP_LOGI(TAG, "POST routine type=%d -> slot=%d", type, idx);
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"i\":%d}", idx);
    return send_json(req, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUT /api/v1/routines?id=N
 * Edit a routine and/or toggle its enabled state. Body fields are overlaid
 * on the routine's current values (all optional), so this single endpoint
 * covers both full edits (send the whole config) and enable/disable-only
 * toggles (send just {"en":bool}).
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t put_routine(httpd_req_t *req) {
    const char *q = uri_query_start(req);
    char idv[8];
    if (!q || httpd_query_key_value(q + 1, "id", idv, sizeof(idv)) != ESP_OK)
        return send_error(req, E_INVALID_ARG, "id query parameter required");
    int idx = (int)strtol(idv, NULL, 10);

    cJSON *root = parse_json_body(req);
    if (!root) return send_error(req, E_BAD_JSON, ERR_BAD_JSON);

    routine_handle_t h = routine_at(idx);
    if (!h) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "PUT routine id=%d not found", idx);
        return send_error(req, E_NOT_FOUND, ERR_ROUTINE_NOT_FOUND);
    }

    /* Candidate = current entry overlaid with provided fields. */
    routine_entry_t entry = *h;
    entry.hour       = (uint8_t)json_get_int(root, "h",  entry.hour);
    entry.minute     = (uint8_t)json_get_int(root, "m",  entry.minute);
    entry.end_hour   = (uint8_t)json_get_int(root, "eh", entry.end_hour);
    entry.end_minute = (uint8_t)json_get_int(root, "em", entry.end_minute);
    entry.interval_on  = (uint16_t)json_get_int(root, "ion", entry.interval_on);
    entry.interval_off = (uint16_t)json_get_int(root, "ioff", entry.interval_off);
    entry.duration_s   = (uint32_t)json_get_int(root, "dur", entry.duration_s);
    entry.days         = (uint8_t)(json_get_int(root, "d", entry.days) & DAYS_MASK);
    entry.date_start   = (uint32_t)json_get_int(root, "ds", entry.date_start);
    entry.date_end     = (uint32_t)json_get_int(root, "de", entry.date_end);
    entry.relay_on     = json_get_bool(root, "on", entry.relay_on);
    entry.enabled      = json_get_bool(root, "en", entry.enabled);
    entry.type         = (uint8_t)json_get_int(root, "t", entry.type);
    cJSON_Delete(root);

    /* Enabling/keeping a routine enabled while auto-off is armed would fight
     * the safety auto-off — reject like POST does. Disabling is always
     * allowed (it frees the conflict). */
    if (entry.enabled) {
        esp_err_t conflict = check_auto_off_conflict(req);
        if (conflict != ESP_OK) return conflict;
    }

    if (routine_update(h, &entry) != ESP_OK) {
        ESP_LOGW(TAG, "PUT routine id=%d type=%d FAILED", idx, entry.type);
        return send_error(req, E_CONFLICT, "invalid config, type change, or overlap");
    }

    ESP_LOGI(TAG, "PUT routine id=%d enabled=%d", idx, entry.enabled);
    return send_ok(req);
}

/* ══════════════════════════════════════════════════════════════════════════
 * DELETE /api/v1/routines?id=N
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t delete_routine(httpd_req_t *req) {
    const char *q = uri_query_start(req);
    char idv[8];
    if (!q || httpd_query_key_value(q + 1, "id", idv, sizeof(idv)) != ESP_OK)
        return send_error(req, E_INVALID_ARG, "id query parameter required");
    int idx = (int)strtol(idv, NULL, 10);

    routine_handle_t h = routine_at(idx);
    if (!h) return send_error(req, E_NOT_FOUND, ERR_ROUTINE_NOT_FOUND);

    if (routine_remove(h) != ESP_OK)
        return send_error(req, E_INTERNAL, "failed to remove routine");
    ESP_LOGI(TAG, "DELETE routine slot=%d", idx);
    return send_ok(req);
}
