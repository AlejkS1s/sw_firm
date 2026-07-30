#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_util.h"

#define TAG "http_util"

static const struct { const char *status; const char *name; } ERR_TABLE[] = {
    [E_BAD_JSON]    = { "400 Bad Request",          "bad_json" },
    [E_INVALID_ARG] = { "400 Bad Request",          "invalid_arg" },
    [E_NOT_FOUND]   = { "404 Not Found",             "not_found" },
    [E_CONFLICT]    = { "409 Conflict",              "conflict" },
    [E_INTERNAL]    = { "500 Internal Server Error", "internal" },
};

void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

esp_err_t send_error(httpd_req_t *req, api_err_t code, const char *msg) {
    httpd_resp_set_status(req, ERR_TABLE[code].status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    set_cors(req);
    char buf[192];
    int n = snprintf(buf, sizeof(buf), "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                      ERR_TABLE[code].name, msg);
    return httpd_resp_send(req, buf, n);
}

esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    set_cors(req);
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t send_ok(httpd_req_t *req) {
    return send_json(req, "{\"ok\":true}");
}

int json_get_int(const cJSON *root, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

bool json_get_bool(const cJSON *root, const char *key, bool def) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsBool(item)) ? cJSON_IsTrue(item) : def;
}

const char *json_get_string(const cJSON *root, const char *key, const char *def) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : def;
}

static char *read_body(httpd_req_t *req) {
    int len = req->content_len;
    if (len <= 0 || len > MAX_BODY) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) { free(buf); return NULL; }
        total += r;
    }
    buf[len] = '\0';
    return buf;
}

cJSON *parse_json_body(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) return NULL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) ESP_LOGE(TAG, "cJSON_Parse failed in %s", req->uri);
    return root;
}


