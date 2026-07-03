#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "gpio.h"
#include "timer_sched.h"
#include "lwip/udp.h"

#define TAG "http"
#define MAX_BODY 512



static int send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

static char *read_body(httpd_req_t *req) {
    int len = req->content_len;
    if (len <= 0 || len > MAX_BODY) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int r = httpd_req_recv(req, buf, len);
    if (r != len) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

static int json_int(const char *json, const char *key, int def) {
    char k[32];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(json, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    return atoi(p);
}

static bool json_bool(const char *json, const char *key, bool def) {
    char k[32];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(json, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

static esp_err_t get_state(httpd_req_t *req) {
    char buf[256];
    time_t now = time(NULL);
    bool time_ok = now > 100000;
    int active = timer_sched_is_active() ? 1 : 0;

    snprintf(buf, sizeof(buf),
        "{\"relay\":%s,\"led\":%s,\"timer\":%s,\"timer_rem\":0,\"time\":%s}",
        relay_get() ? "true" : "false",
        led_get() ? "true" : "false",
        active ? "true" : "false",
        time_ok ? "true" : "false");
    return send_json(req, buf);
}

static esp_err_t get_info(httpd_req_t *req) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return send_json(req, buf);
}

static esp_err_t get_schedules(httpd_req_t *req) {
    sched_entry_t entries[SCHED_MAX];
    int n = sched_get_all(entries, SCHED_MAX);
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "{\"s\":[");
    for (int i = 0; i < n; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"i\":%d,\"h\":%d,\"m\":%d,\"o\":%s,\"d\":%d,\"e\":%s}",
            entries[i].id, entries[i].hour, entries[i].minute,
            entries[i].action ? "true" : "false",
            entries[i].days,
            entries[i].enabled ? "true" : "false");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return send_json(req, buf);
}

static esp_err_t post_timer(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int seconds = json_int(body, "s", 0);
    bool action = json_bool(body, "on", true);
    free(body);

    if (seconds <= 0 || seconds > 86400) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    timer_start((uint32_t)seconds, action);
    return get_state(req);
}

static esp_err_t post_timer_cancel(httpd_req_t *req) {
    timer_cancel();
    return get_state(req);
}

static esp_err_t post_schedules(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    sched_entry_t entry = {0};
    entry.hour   = (uint8_t)json_int(body, "h", 0);
    entry.minute = (uint8_t)json_int(body, "m", 0);
    entry.action = json_bool(body, "on", true);
    entry.days   = (uint8_t)json_int(body, "d", 0);
    entry.enabled = true;
    free(body);

    if (entry.hour > 23 || entry.minute > 59) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t e = sched_add(&entry);
    char ack[64];
    snprintf(ack, sizeof(ack),
        "{\"ok\":%s}", e == ESP_OK ? "true" : "false");
    return send_json(req, ack);
}

static esp_err_t post_schedules_remove(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int id = json_int(body, "i", -1);
    free(body);

    if (id < 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    sched_remove((uint8_t)id);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t handler(httpd_req_t *req) {
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (req->method == HTTP_GET) {
        if (strcmp(req->uri, "/state") == 0)     return get_state(req);
        if (strcmp(req->uri, "/info") == 0)      return get_info(req);
        if (strcmp(req->uri, "/schedules") == 0) return get_schedules(req);
    }
    if (req->method == HTTP_GET && strcmp(req->uri, "/udptest") == 0) {
        struct udp_pcb *upcb = udp_new();
        if (upcb) {
            ip_addr_t dst;
            IP4_ADDR(&dst.u_addr.ip4, 192, 168, 100, 143);
            dst.type = IPADDR_TYPE_V4;
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM);
            if (p) {
                memcpy(p->payload, "PONG", 4);
                udp_sendto(upcb, p, &dst, 12345);
                pbuf_free(p);
            }
            udp_remove(upcb);
        }
        return send_json(req, "{\"udp\":1}");
    }
    if (req->method == HTTP_POST) {
        if (strcmp(req->uri, "/on") == 0)             { relay_set(true);  return get_state(req); }
        if (strcmp(req->uri, "/off") == 0)            { relay_set(false); return get_state(req); }
        if (strcmp(req->uri, "/toggle") == 0)         { relay_set(!relay_get()); return get_state(req); }
        if (strcmp(req->uri, "/timer") == 0)          return post_timer(req);
        if (strcmp(req->uri, "/timer/cancel") == 0)   return post_timer_cancel(req);
        if (strcmp(req->uri, "/schedules") == 0)      return post_schedules(req);
        if (strcmp(req->uri, "/schedules/remove") == 0) return post_schedules_remove(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

int http_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;

    ESP_LOGI(TAG, "Starting HTTP on port %d", cfg.server_port);
    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    struct { const char *u; httpd_method_t m; } routes[] = {
        {"/state", HTTP_GET}, {"/info", HTTP_GET}, {"/schedules", HTTP_GET}, {"/udptest", HTTP_GET},
        {"/on", HTTP_POST}, {"/off", HTTP_POST}, {"/toggle", HTTP_POST},
        {"/timer", HTTP_POST}, {"/timer/cancel", HTTP_POST},
        {"/schedules", HTTP_POST}, {"/schedules/remove", HTTP_POST},
        {"/timer", HTTP_OPTIONS},
        {"/schedules", HTTP_OPTIONS}, {"/schedules/remove", HTTP_OPTIONS},
    };
    for (int i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        httpd_uri_t u = { .uri = routes[i].u, .method = routes[i].m,
                          .handler = handler };
        httpd_register_uri_handler(server, &u);
    }
    return ESP_OK;
}
