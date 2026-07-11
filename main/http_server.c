#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"

#include "board.h"
#include "state.h"
#include "power.h"
#include "routines.h"
#include "timing.h"
#include "ipc.h"

#define TAG "http"
#define MAX_BODY 512
#define JSON_BUF_SIZE 1024
#define RESP_BUF_SIZE 64
#define HTTP_PORT 80
#define MAX_URI_HANDLERS 40
#define FW_VER "1.0.0"
#define LED_MODE_MASK_ALL 0x0F
#define BOOL_STR(b) ((b) ? "true" : "false")

static void get_active_countdown(uint32_t *rem, uint32_t *total) {
    *rem = 0; *total = 0;
    time_t now = time(NULL);
    routine_entry_t rts[ROUTINES_MAX];
    int n = routine_get_all(rts, ROUTINES_MAX);
    for (int i = 0; i < n; i++) {
        if (rts[i].type == RT_COUNTDOWN && rts[i].enabled) {
            if (rts[i].target_epoch > (uint32_t)now) {
                *rem = rts[i].target_epoch - (uint32_t)now;
                *total = rts[i].duration_s;
            }
            break;
        }
    }
}

static int send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
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

static cJSON *parse_json_body(httpd_req_t *req, const char *endpoint) {
    char *body = read_body(req);
    if (!body) { httpd_resp_send_500(req); return NULL; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "cJSON_Parse failed in %s", endpoint);
        httpd_resp_send_500(req);
        return NULL;
    }
    return root;
}

/* Submit an actuator command and block until the actuator task acks.
 * The command is copied into the queue, so `m` may be a local. */
static esp_err_t submit_actuator(actuator_msg_t *m) {
    SemaphoreHandle_t ack = xSemaphoreCreateBinary();
    if (!ack) return ESP_FAIL;
    m->ack = ack;
    if (xQueueSend(g_actuator_q, m, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(ack);
        m->ack = NULL;
        return ESP_FAIL;
    }
    if (xSemaphoreTake(ack, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(ack);
        m->ack = NULL;
        return ESP_FAIL;
    }
    vSemaphoreDelete(ack);
    m->ack = NULL;
    return ESP_OK;
}

/* Submit a routines command and block until the routines task acks. */
static esp_err_t submit_routine(routines_msg_t *m) {
    SemaphoreHandle_t ack = xSemaphoreCreateBinary();
    if (!ack) return ESP_FAIL;
    m->ack = ack;
    if (routines_submit(m) != ESP_OK) {
        vSemaphoreDelete(ack);
        m->ack = NULL;
        return ESP_FAIL;
    }
    if (xSemaphoreTake(ack, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(ack);
        m->ack = NULL;
        return ESP_FAIL;
    }
    vSemaphoreDelete(ack);
    m->ack = NULL;
    return ESP_OK;
}

static esp_err_t get_state(httpd_req_t *req) {
    time_t now = time(NULL);
    uint32_t rem = 0, total = 0;
    get_active_countdown(&rem, &total);
    int acode = state_compute_acode();
    unsigned long uptime = (unsigned long)(esp_timer_get_time() / 1000000ULL);
    char buf[JSON_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf),
        "{\"relay\":%s,\"led\":%s,\"timer\":%s,\"timer_rem\":%lu,"
        "\"timer_total\":%lu,\"time\":%s,\"boot\":%d,\"led_mode\":%d,"
        "\"acode\":%d,\"uptime\":%lu,"
        "\"circulate\":%s,\"iching\":%s}",
        BOOL_STR(relay_get()),
        BOOL_STR(led_get()),
        BOOL_STR(routine_is_active(RT_COUNTDOWN)),
        (unsigned long)rem,
        (unsigned long)total,
        BOOL_STR(now > TIME_VALID_THRESHOLD),
        relay_get_boot_behavior(),
        led_get_mode(),
        acode,
        uptime,
        BOOL_STR(routine_is_active(RT_CIRCULATE)),
        BOOL_STR(routine_is_active(RT_ICHING)));
    return send_json(req, buf);
}

static esp_err_t post_toggle(httpd_req_t *req) {
    actuator_msg_t m = { .type = CMD_TOGGLE };
    submit_actuator(&m);
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"relay\":%s,\"acode\":%d}", BOOL_STR(relay_get()), acode);
    return send_json(req, buf);
}

static esp_err_t get_check(httpd_req_t *req) {
    char *q = strchr(req->uri, '?');
    if (!q) return get_state(req);
    char aval[16];
    if (httpd_query_key_value(q + 1, "a", aval, sizeof(aval)) != ESP_OK)
        return get_state(req);
    int client_acode = (int)strtol(aval, NULL, 10);
    int server_acode = state_compute_acode();
    if (client_acode == server_acode)
        return send_json(req, "{\"ok\":true}");
    return get_state(req);
}

static esp_err_t get_info(httpd_req_t *req) {
    time_t now = time(NULL);
    uint32_t rem = 0, total = 0;
    get_active_countdown(&rem, &total);
    int acode = state_compute_acode();
    unsigned long uptime = (unsigned long)(esp_timer_get_time() / 1000000ULL);
    unsigned long heap = (unsigned long)esp_get_free_heap_size();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char ssid_buf[36] = "";
    int8_t rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(ssid_buf, ap.ssid, sizeof(ap.ssid));
        ssid_buf[sizeof(ap.ssid) - 1] = '\0';
        rssi = ap.rssi;
    }

    tcpip_adapter_ip_info_t ip;
    char ip_str[16] = "";
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK) {
        strncpy(ip_str, ip4addr_ntoa(&ip.ip), sizeof(ip_str) - 1);
    }

    char buf[JSON_BUF_SIZE];
    snprintf(buf, sizeof(buf),
        "{\"mac\":\"" MACSTR "\","
        "\"relay\":%s,\"led\":%s,\"timer\":%s,"
        "\"timer_rem\":%lu,\"timer_total\":%lu,"
        "\"time\":%s,\"boot\":%d,\"led_mode\":%d,"
        "\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
        "\"acode\":%d,\"uptime\":%lu,\"heap\":%lu,\"fw_ver\":\"" FW_VER "\","
        "\"tz\":\"%s\","
        "\"circulate\":%s,\"iching\":%s}",
        MAC2STR(mac),
        BOOL_STR(relay_get()),
        BOOL_STR(led_get()),
        BOOL_STR(routine_is_active(RT_COUNTDOWN)),
        (unsigned long)rem,
        (unsigned long)total,
        BOOL_STR(now > TIME_VALID_THRESHOLD),
        relay_get_boot_behavior(),
        led_get_mode(),
        ssid_buf, rssi, ip_str,
        acode,
        uptime, heap,
        timing_get_timezone(),
        BOOL_STR(routine_is_active(RT_CIRCULATE)),
        BOOL_STR(routine_is_active(RT_ICHING)));
    return send_json(req, buf);
}

static esp_err_t post_boot(httpd_req_t *req) {
    cJSON *root = parse_json_body(req, "/boot");
    if (!root) return ESP_FAIL;

    int mode = -1;
    cJSON *m_item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsNumber(m_item)) mode = m_item->valueint;

    cJSON_Delete(root);

    if (mode < 0 || mode > RELAY_BOOT_AUTO) { httpd_resp_send_500(req); return ESP_FAIL; }
    submit_actuator(&(actuator_msg_t){ .type = CMD_SET_BOOT, .boot_mode = (uint8_t)mode });
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"acode\":%d}", acode);
    return send_json(req, buf);
}

static esp_err_t post_led(httpd_req_t *req) {
    cJSON *root = parse_json_body(req, "/led");
    if (!root) return ESP_FAIL;

    int mode = -1;
    cJSON *m_item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsNumber(m_item)) mode = m_item->valueint;

    cJSON_Delete(root);

    if (mode < 0 || (uint8_t)mode > LED_MODE_MASK_ALL) { httpd_resp_send_500(req); return ESP_FAIL; }
    submit_actuator(&(actuator_msg_t){ .type = CMD_SET_LED, .led_mode = (uint8_t)mode });
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"acode\":%d}", acode);
    return send_json(req, buf);
}

static esp_err_t post_tz(httpd_req_t *req) {
    cJSON *root = parse_json_body(req, "/tz");
    if (!root) return ESP_FAIL;

    cJSON *tz_item = cJSON_GetObjectItem(root, "tz");
    const char *tz = cJSON_IsString(tz_item) ? tz_item->valuestring : NULL;
    cJSON_Delete(root);

    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (tz) e = timing_set_timezone(tz);
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"acode\":%d}", BOOL_STR(e == ESP_OK), acode);
    return send_json(req, buf);
}

static esp_err_t post_on(httpd_req_t *req) {
    submit_actuator(&(actuator_msg_t){ .type = CMD_TURN_ON });
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"relay\":%s,\"acode\":%d}", BOOL_STR(relay_get()), acode);
    return send_json(req, buf);
}

static esp_err_t post_off(httpd_req_t *req) {
    submit_actuator(&(actuator_msg_t){ .type = CMD_TURN_OFF });
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"relay\":%s,\"acode\":%d}", BOOL_STR(relay_get()), acode);
    return send_json(req, buf);
}

static esp_err_t get_routines(httpd_req_t *req) {
    routine_entry_t entries[ROUTINES_MAX];
    int n = routine_get_all(entries, ROUTINES_MAX);
    char buf[JSON_BUF_SIZE];
    size_t pos = snprintf(buf, sizeof(buf), "{\"routines\":[");
    for (int i = 0; i < n; i++) {
        if (pos >= sizeof(buf)) break;
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        if (pos >= sizeof(buf)) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"i\":%d,\"t\":%d,\"h\":%d,\"m\":%d,\"eh\":%d,\"em\":%d,"
            "\"ion\":%u,\"ioff\":%u,\"dur\":%lu,\"d\":%d,"
            "\"on\":%s,\"e\":%s}",
            entries[i].id, entries[i].type,
            entries[i].hour, entries[i].minute,
            entries[i].end_hour, entries[i].end_minute,
            entries[i].interval_on, entries[i].interval_off,
            (unsigned long)entries[i].duration_s,
            entries[i].days,
            BOOL_STR(entries[i].relay_on),
            BOOL_STR(entries[i].enabled));
    }
    if (pos < sizeof(buf))
        snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return send_json(req, buf);
}

static esp_err_t get_rout_ids(httpd_req_t *req) {
    int ids[ROUTINES_MAX];
    int n = routine_get_ids(ids, ROUTINES_MAX);
    char buf[128];
    size_t pos = snprintf(buf, sizeof(buf), "{\"ids\":[");
    for (int i = 0; i < n; i++) {
        if (pos >= sizeof(buf)) break;
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        if (pos >= sizeof(buf)) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", ids[i]);
    }
    if (pos < sizeof(buf))
        snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return send_json(req, buf);
}

static esp_err_t get_rout_by_id(httpd_req_t *req) {
    char *q = strchr(req->uri, '?');
    if (!q) return get_routines(req);
    char aval[16];
    if (httpd_query_key_value(q + 1, "id", aval, sizeof(aval)) != ESP_OK)
        return get_routines(req);
    int id = (int)strtol(aval, NULL, 10);
    routine_entry_t e;
    if (!routine_get_by_id((uint8_t)id, &e))
        return httpd_resp_send_404(req);
    char buf[JSON_BUF_SIZE];
    snprintf(buf, sizeof(buf),
        "{\"i\":%d,\"t\":%d,\"h\":%d,\"m\":%d,\"eh\":%d,\"em\":%d,"
        "\"ion\":%u,\"ioff\":%u,\"dur\":%lu,\"d\":%d,"
        "\"on\":%s,\"e\":%s}",
        e.id, e.type, e.hour, e.minute,
        e.end_hour, e.end_minute,
        e.interval_on, e.interval_off,
        (unsigned long)e.duration_s, e.days,
        BOOL_STR(e.relay_on), BOOL_STR(e.enabled));
    return send_json(req, buf);
}

static esp_err_t post_routines(httpd_req_t *req) {
    cJSON *root = parse_json_body(req, "/routines");
    if (!root) return ESP_FAIL;

    routine_entry_t entry = {0};

    cJSON *item;
    item = cJSON_GetObjectItem(root, "t");
    if (cJSON_IsNumber(item)) entry.type = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "h");
    if (cJSON_IsNumber(item)) entry.hour = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "m");
    if (cJSON_IsNumber(item)) entry.minute = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "eh");
    if (cJSON_IsNumber(item)) entry.end_hour = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "em");
    if (cJSON_IsNumber(item)) entry.end_minute = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "ion");
    if (cJSON_IsNumber(item)) entry.interval_on = (uint16_t)item->valueint;
    item = cJSON_GetObjectItem(root, "ioff");
    if (cJSON_IsNumber(item)) entry.interval_off = (uint16_t)item->valueint;
    item = cJSON_GetObjectItem(root, "dur");
    if (cJSON_IsNumber(item)) entry.duration_s = (uint32_t)item->valuedouble;
    item = cJSON_GetObjectItem(root, "d");
    if (cJSON_IsNumber(item)) entry.days = (uint8_t)item->valueint & 0x7F;
    item = cJSON_GetObjectItem(root, "on");
    if (cJSON_IsBool(item)) entry.relay_on = cJSON_IsTrue(item);

    cJSON_Delete(root);

    if (entry.type > RT_ICHING) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (entry.type == RT_CIRCULATE && (entry.interval_on == 0 || entry.interval_off == 0)) {
        httpd_resp_send_500(req); return ESP_FAIL;
    }
    if (entry.type == RT_COUNTDOWN && entry.duration_s == 0) {
        httpd_resp_send_500(req); return ESP_FAIL;
    }

    routines_msg_t m = { .type = RCMD_ADD, .entry = entry };
    esp_err_t e = submit_routine(&m);
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"acode\":%d}", BOOL_STR(e == ESP_OK), acode);
    return send_json(req, buf);
}

static esp_err_t post_routines_remove(httpd_req_t *req) {
    cJSON *root = parse_json_body(req, "/routines/remove");
    if (!root) return ESP_FAIL;

    int id = -1;
    cJSON *i_item = cJSON_GetObjectItem(root, "i");
    if (cJSON_IsNumber(i_item)) id = i_item->valueint;

    cJSON_Delete(root);

    if (id < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    routines_msg_t m = { .type = RCMD_REMOVE, .id = (uint8_t)id };
    submit_routine(&m);
    int acode = state_compute_acode();
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"acode\":%d}", acode);
    return send_json(req, buf);
}

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} route_t;

static const route_t s_routes[] = {
    {"/state", HTTP_GET, get_state},
    {"/check", HTTP_GET, get_check},
    {"/info", HTTP_GET, get_info},
    {"/routines", HTTP_GET, get_routines},
    {"/rout_ids", HTTP_GET, get_rout_ids},
    {"/rout", HTTP_GET, get_rout_by_id},
    {"/on", HTTP_POST, post_on},
    {"/off", HTTP_POST, post_off},
    {"/toggle", HTTP_POST, post_toggle},
    {"/routines", HTTP_POST, post_routines},
    {"/routines/remove", HTTP_POST, post_routines_remove},
    {"/boot", HTTP_POST, post_boot},
    {"/led", HTTP_POST, post_led},
    {"/tz", HTTP_POST, post_tz},
};

static esp_err_t handler(httpd_req_t *req) {
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    power_notify_activity();
    char *q = strchr(req->uri, '?');
    size_t plen = q ? (size_t)(q - req->uri) : strlen(req->uri);
    for (int i = 0; i < (int)LWIP_ARRAYSIZE(s_routes); i++) {
        if (req->method == s_routes[i].method &&
            plen == strlen(s_routes[i].uri) &&
            memcmp(req->uri, s_routes[i].uri, plen) == 0)
            return s_routes[i].handler(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

esp_err_t http_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_uri_handlers = MAX_URI_HANDLERS;
    int n_routes = LWIP_ARRAYSIZE(s_routes);

    ESP_LOGI(TAG, "Starting HTTP on port %d", cfg.server_port);
    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < n_routes; i++) {
        httpd_uri_t u = { .uri = s_routes[i].uri, .method = s_routes[i].method,
                          .handler = handler };
        esp_err_t e = httpd_register_uri_handler(server, &u);
        if (e != ESP_OK)
            ESP_LOGE(TAG, "Failed to register %s: %s", s_routes[i].uri, esp_err_to_name(e));
    }
    // OPTIONS for CORS preflight on POST endpoints
    static const char *cors_uris[] = {"/routines", "/routines/remove", "/boot", "/led", "/tz"};
    for (int i = 0; i < (int)LWIP_ARRAYSIZE(cors_uris); i++) {
        httpd_uri_t u = { .uri = cors_uris[i], .method = HTTP_OPTIONS, .handler = handler };
        esp_err_t e = httpd_register_uri_handler(server, &u);
        if (e != ESP_OK)
            ESP_LOGE(TAG, "Failed to reg OPTIONS %s: %s", cors_uris[i], esp_err_to_name(e));
    }
    return ESP_OK;
}
