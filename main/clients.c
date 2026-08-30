#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "clients.h"

static client_entry_t s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_mux = NULL;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void clients_init(void) {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
}

void client_seen(const char *id) {
    if (!id || !*id) return;
    uint32_t t = now_ms();
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest = t;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].active &&
            strncmp(s_clients[i].id, id, CLIENT_ID_MAX_LEN - 1) == 0) {
            s_clients[i].last_seen_ms = t;
            if (s_mux) xSemaphoreGive(s_mux);
            return;
        }
        if (!s_clients[i].active && free_idx < 0) free_idx = i;
        if (s_clients[i].active && s_clients[i].last_seen_ms < oldest) {
            oldest = s_clients[i].last_seen_ms;
            oldest_idx = i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    strncpy(s_clients[idx].id, id, CLIENT_ID_MAX_LEN - 1);
    s_clients[idx].id[CLIENT_ID_MAX_LEN - 1] = '\0';
    s_clients[idx].last_seen_ms = t;
    s_clients[idx].active = true;
    if (s_mux) xSemaphoreGive(s_mux);
}

unsigned client_count_active(void) {
    uint32_t t = now_ms();
    unsigned n = 0;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].active &&
            (t - s_clients[i].last_seen_ms) < CLIENT_ACTIVE_WINDOW_MS) {
            n++;
        }
    }
    if (s_mux) xSemaphoreGive(s_mux);
    return n;
}

/* Pull the client_id query param out of a request URI. */
void client_extract_id(httpd_req_t *req, char *out, size_t len) {
    out[0] = '\0';
    if (!req || !req->uri) return;
    const char *q = strchr(req->uri, '?');
    if (!q) return;
    q++;
    const size_t klen = 9; /* strlen("client_id") */
    while (*q) {
        if (strncmp(q, "client_id", klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            size_t n = 0;
            while (*q && *q != '&' && n + 1 < len) out[n++] = *q++;
            out[n] = '\0';
            return;
        }
        q = strchr(q, '&');
        if (!q) return;
        q++;
    }
}
