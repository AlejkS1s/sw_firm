#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <stdbool.h>
#include <string.h>

#include "esp_http_server.h"
#include "cJSON.h"
#include "board.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Shared constants & sizing — every handler module includes this instead of
 * each picking its own buffer sizes / error strings.
 * ══════════════════════════════════════════════════════════════════════════ */
#define MAX_BODY       512
#define JSON_BUF_SIZE  1024
#define RESP_BUF_SIZE  64

/* Query-string helper: returns a pointer to the '?' in req->uri, or NULL
 * if there is no query string.  Used by dispatch() in http_server.c and by
 * get_routines/delete_routine in http_handlers_routines.c. */
static inline const char *uri_query_start(httpd_req_t *req) {
    return strchr(req->uri, '?');
}

/* End a chunked response.  Repeated pattern in chunked-stream endpoints. */
static inline esp_err_t send_chunk_end(httpd_req_t *req) {
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Repeated error-message literals consolidated here so every handler speaks
 * the same language and a reword touches exactly one place. */
#define ERR_BAD_JSON          "malformed JSON body"
#define ERR_ROUTINE_NOT_FOUND "routine id not found"

/* Boolean-to-string: used by state_build_json() and all HTTP handlers.
 * Lives here (the HTTP utility header consumed by state.c + http_handlers_*.c)
 * rather than board.h, because board.c does not use it — see the cross-layer
 * dependency rule in AGENTS.md. */
#define BOOL_STR(b)     ((b) ? "true" : "false")

/* ══════════════════════════════════════════════════════════════════════════
 * Standard error envelope: { "error": { "code": "...", "message": "..." } }
 * with the HTTP status line set to match. Every handler that rejects a
 * request uses this instead of inventing its own shape.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    E_BAD_JSON,     /* 400 — body isn't valid JSON */
    E_INVALID_ARG,  /* 400 — valid JSON, invalid/missing field */
    E_NOT_FOUND,    /* 404 — referenced resource (e.g. routine id) doesn't exist */
    E_CONFLICT,     /* 409 — valid request, rejected due to current device state */
    E_INTERNAL,     /* 500 — queue/storage/serialization failure, not the caller's fault */
} api_err_t;

/* Sets the CORS allow-origin header. Every response path must include this
 * — send_json / send_error / send_ok call it internally; non-JSON responses
 * (304, 503, SSE, chunked arrays, OPTIONS) call it directly. */
void set_cors(httpd_req_t *req);

esp_err_t send_error(httpd_req_t *req, api_err_t code, const char *msg);

/* Sends a pre-built JSON string with the standard headers (Content-Type,
 * CORS). Callers own the buffer and its lifetime. */
esp_err_t send_json(httpd_req_t *req, const char *json);

/* Conflict guard for operations that must not run while auto-off is armed.
 * Used by timer-post and routines-post in http_handlers_control.c and
 * http_handlers_routines.c.  Sends the error response if armed, always
 * returns ESP_FAIL on conflict so the caller's `if (conflict != ESP_OK)`
 * catches it regardless of what send_error returns. */
static inline esp_err_t check_auto_off_conflict(httpd_req_t *req) {
    if (relay_auto_off_is_armed()) {
        send_error(req, E_CONFLICT, "auto-off is armed; clear it before this operation");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Convenience: sends {"ok":true}. Replaces the ad-hoc send_json call that
 * appears in 10+ handlers. */
esp_err_t send_ok(httpd_req_t *req);

/* Convenience JSON field extractors — wrap cJSON_GetObjectItem + type check
 * in a single call. Returns `def` when the key is missing or wrong type. */
int         json_get_int(const cJSON *root, const char *key, int def);
bool        json_get_bool(const cJSON *root, const char *key, bool def);
const char *json_get_string(const cJSON *root, const char *key, const char *def);

/* Reads and JSON-parses the request body (up to MAX_BODY bytes). Returns
 * NULL on any failure (oversized body, recv error, malformed JSON) — the
 * caller's only job on NULL is to respond with E_BAD_JSON. Caller owns the
 * returned cJSON* and must cJSON_Delete() it. The endpoint name for error
 * logging is taken from req->uri, eliminating a duplicate string literal. */
cJSON *parse_json_body(httpd_req_t *req);

#endif /* HTTP_UTIL_H */
