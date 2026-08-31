#include <string.h>
#include <errno.h>
#include <time.h>
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sse.h"
#include "clients.h"
#include "power.h"
#include "state.h"
#include "nvs_store.h"
#include "http_util.h"
#include "timing.h"

#define TAG "sse"
/* Raw send() timeout for SSE frames. Short by design: the control task
 * (priority 5) calls this — a slow/dead client must never pin it. */
#define SSE_SEND_TIMEOUT_MS   300
#define SSE_HEARTBEAT_DIVIDER 30   /* control-task ticks (500 ms) → 15 s heartbeat */
#define SSE_STALE_MS        45000
#define SSE_MAX_CLIENTS     2

typedef struct {
    int            fd;
    httpd_handle_t handle;
    bool           active;
    char           client_id[SSE_CLIENT_ID_MAX_LEN];
    uint32_t       last_activity_ms;
} sse_client_t;

static sse_client_t s_clients[SSE_MAX_CLIENTS];
static volatile bool s_push_pending = false;
static volatile bool s_hb_pending   = false;
static state_snapshot_t s_last_snapshot;
static bool s_has_snapshot = false;

static unsigned s_unique_fds = 0;

static bool s_sse_enabled = true;

/* Helper: true if slot j is the only active slot referencing slot i's fd. */
static bool sse_is_last_ref(int i) {
    int j = i ^ 1;
    return !s_clients[j].active || s_clients[j].fd != s_clients[i].fd;
}

static void sse_deactivate_slot(int i) {
    if (!s_clients[i].active) return;
    ESP_LOGI(TAG, "deactivate slot=%d fd=%d id=%s", i, s_clients[i].fd, s_clients[i].client_id);
    bool last = sse_is_last_ref(i);
    sse_mark_fd(s_clients[i].fd, false);
    s_clients[i].active = false;
    s_clients[i].client_id[0] = '\0';
    if (last) {
        power_sse_client_disconnected();
        if (s_unique_fds > 0) s_unique_fds--;
    }
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void sse_init(void) {
    state_register_on_change(sse_notify);
    uint8_t v = 1;
    if (nvs_store_get_u8(NVS_NS_SSE, NVS_KEY_SSE_ENABLED, &v) == ESP_OK)
        s_sse_enabled = (v != 0);
}

void sse_set_enabled(bool enabled) {
    if (enabled == s_sse_enabled) return;
    s_sse_enabled = enabled;
    nvs_store_set_u8(NVS_NS_SSE, NVS_KEY_SSE_ENABLED, enabled ? 1 : 0);
    ESP_LOGI(TAG, "SSE %s", enabled ? "enabled" : "disabled");
    if (!enabled)
        sse_close_all();
    notify_bump_state();
}

bool sse_is_enabled(void) { return s_sse_enabled; }

void sse_close_all(void) {
    for (int i = 0; i < SSE_MAX_CLIENTS; i++)
        sse_deactivate_slot(i);
}

int sse_client_count(void) {
    int n = 0;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++)
        if (s_clients[i].active) n++;
    return n;
}

/* ── SSE fd registry ─────────────────────────────────────────────────────
 * The ESP-IDF httpd is single-threaded: its main loop calls select() and
 * then httpd_sess_process() for every fd that select() reports as
 * readable. For a long-lived SSE stream where the browser only reads
 * (never writes), lwIP can still mark the socket readable (e.g. for
 * disconnect detection), which would make the httpd call
 * httpd_sess_process -> httpd_parse_req -> read_block -> recv() with the
 * SO_RCVTIMEO timeout. On the single-core ESP8266, that recv() blocks
 * the httpd task for 2s while lwIP cannot push any other socket's data
 * to the radio — no other client can connect and no heartbeat is sent.
 *
 * The SSE module marks its fds here when the SSE handler is registered.
 * The httpd loop calls sse_fd_is_active(fd) before processing a session
 * and skips fds that are SSE streams. The entry is cleared when the
 * SSE slot is deactivated. The lookup is O(N) where N=SSE_MAX_CLIENTS=2,
 * which is trivial. */
static int sse_fds[SSE_MAX_CLIENTS] = { -1, -1 };
/* Guard against re-entrancy: if s_seen_fds[] contains fd X, skip X even if
 * not yet in sse_fds[]. The ESP-IDF httpd sometimes iterates the session
 * table and returns the same fd twice in one loop (a bug in sess_iterate
 * when a new session is added mid-iteration). Without this guard, the
 * second visit processes the same fd before sse_register() has run. */
static int s_seen_fds[SSE_MAX_CLIENTS] = { -1, -1 };
static int s_seen_count = 0;

void sse_mark_fd(int fd, bool active) {
    if (active) {
        for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
            if (sse_fds[i] == fd) return;
        }
        for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
            if (sse_fds[i] < 0) { sse_fds[i] = fd; return; }
        }
    } else {
        for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
            if (sse_fds[i] == fd) { sse_fds[i] = -1; return; }
        }
    }
}

bool sse_fd_is_active(int fd) {
    for (int i = 0; i < SSE_MAX_CLIENTS; i++)
        if (sse_fds[i] == fd) return true;
    return false;
}

/* Called from the SSE handler BEFORE sse_register() to pre-mark the fd so
 * that a re-entrant visit to the same fd (from the ESP-IDF httpd bug where
 * sess_iterate returns the same fd twice in one loop) is also skipped. */
void sse_mark_seen(int fd) {
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_seen_fds[i] == fd) return;
    }
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_seen_fds[i] < 0) { s_seen_fds[i] = fd; return; }
    }
}

void sse_clear_seen(int fd) {
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_seen_fds[i] == fd) { s_seen_fds[i] = -1; return; }
    }
}

bool sse_fd_is_seen(int fd) {
    for (int i = 0; i < SSE_MAX_CLIENTS; i++)
        if (s_seen_fds[i] == fd) return true;
    return false;
}

/* ── Heartbeat ────────────────────────────────────────────────────────── */

/* Runs on the httpd task (via httpd_queue_work) — NOT the control task.
 * Evicts stale clients and pushes a heartbeat frame to live connections.
 * All socket I/O (send/recv) happens here so a slow/dead client can never
 * stall the control task. Mirrors sse_push_work(). */
static void sse_heartbeat_work(void *arg) {
    (void)arg;

    uint32_t now = now_ms();
    time_t t = time(NULL);

    /* Build combined chunk once: [hex-len\r\n][SSE event][\r\n] */
    char buf[128];
    int datalen = snprintf(buf + 12, sizeof(buf) - 12,
        "id: %lu\nevent: heartbeat\ndata: %ld\n\n",
        (unsigned long)g_state_version, (long)t);
    if (datalen < 0 || (size_t)(datalen + 12 + 2) > sizeof(buf)) { s_hb_pending = false; return; }
    int hdrlen = snprintf(buf, 12, "%x\r\n", datalen);
    if (hdrlen < 0 || hdrlen >= 12) { s_hb_pending = false; return; }
    int gap = 12 - hdrlen;
    if (gap > 0)
        memmove(buf + hdrlen, buf + 12, (size_t)datalen);
    int total = hdrlen + datalen;
    buf[total] = '\r';
    buf[total + 1] = '\n';

    /* Single pass: evict stale clients, send heartbeat to survivors. */
    int last_fd = -1;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) continue;
        client_seen(s_clients[i].client_id);   /* keep registry fresh for SSE-only clients */
        if (now - s_clients[i].last_activity_ms > SSE_STALE_MS) {
            ESP_LOGW(TAG, "evicting stale slot=%d fd=%d", i, s_clients[i].fd);
            sse_deactivate_slot(i);
            continue;
        }
        if (s_clients[i].fd == last_fd) continue;
        last_fd = s_clients[i].fd;

        int ret = send(s_clients[i].fd, buf, total + 2, 0);
        if (ret == total + 2) {
            s_clients[i].last_activity_ms = now;
        } else {
            ESP_LOGW(TAG, "hb send failed fd=%d ret=%d errno=%d", s_clients[i].fd, ret, errno);
            sse_deactivate_slot(i);
        }
    }

    s_hb_pending = false;
}

/* Control-task tick (500 ms cadence): every SSE_HEARTBEAT_DIVIDER ticks,
 * queue the heartbeat to the httpd task. Returns immediately — no socket
 * I/O on the control task. */
void sse_heartbeat_tick(void) {
    static int divider = 0;
    if (++divider < SSE_HEARTBEAT_DIVIDER) return;
    divider = 0;

    if (s_unique_fds == 0 || !s_sse_enabled) return;
    if (s_hb_pending) return;
    s_hb_pending = true;

    httpd_handle_t handle = NULL;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_clients[i].active) { handle = s_clients[i].handle; break; }
    }
    if (!handle) { s_hb_pending = false; return; }

    if (httpd_queue_work(handle, sse_heartbeat_work, NULL) != ESP_OK)
        s_hb_pending = false;
}

/* ── Registration ─────────────────────────────────────────────────────── */

static bool get_query_param(const char *uri, const char *key, char *out, size_t outlen) {
    if (!uri) return false;
    const char *q = strchr(uri, '?');
    if (!q) return false;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            size_t n = 0;
            while (*q && *q != '&' && n + 1 < outlen)
                out[n++] = *q++;
            out[n] = '\0';
            return n > 0;
        }
        q = strchr(q, '&');
        if (!q) return false;
        q++;
    }
    return false;
}

static void set_send_timeout(int fd) {
    struct timeval tv = { .tv_sec = 0, .tv_usec = SSE_SEND_TIMEOUT_MS * USEC_PER_MSEC };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Activate slot i with the given fd and client_id. Handles power and fd
 * tracking — called once a target slot is determined. */
static bool sse_activate_slot(int i, int fd, httpd_handle_t handle,
                               const char *client_id, uint32_t now) {
    bool is_new_fd = true;
    if (s_clients[i].active && s_clients[i].fd == fd)
        is_new_fd = false;
    else if (s_clients[i ^ 1].active && s_clients[i ^ 1].fd == fd)
        is_new_fd = false;

    s_clients[i].fd        = fd;
    s_clients[i].handle    = handle;
    s_clients[i].active    = true;
    s_clients[i].last_activity_ms = now;
    strncpy(s_clients[i].client_id, client_id, SSE_CLIENT_ID_MAX_LEN - 1);
    s_clients[i].client_id[SSE_CLIENT_ID_MAX_LEN - 1] = '\0';

    set_send_timeout(fd);
    if (is_new_fd) {
        s_unique_fds++;
        power_sse_client_connected();
    }
    return true;
}

bool sse_register(httpd_req_t *req) {
    if (!s_sse_enabled) {
        ESP_LOGW(TAG, "registration rejected: SSE disabled");
        return false;
    }

    int fd = httpd_req_to_sockfd(req);
    httpd_handle_t handle = req->handle;
    char client_id[SSE_CLIENT_ID_MAX_LEN] = "";
    get_query_param(req->uri, "client_id", client_id, sizeof(client_id));

    /* Mark this fd as an SSE stream so the httpd loop knows to skip it on
     * future select() wakeups. Without this, every time select() reports
     * the fd as readable (e.g. lwIP marks idle sockets readable for
     * disconnect detection), httpd_sess_process is called, which calls
     * httpd_parse_req -> read_block -> httpd_recv_with_opt, which BLOCKS
     * for SO_RCVTIMEO (2s) waiting for the next HTTP request. On a
     * single-core ESP8266 that 2s blocks the httpd task completely and
     * no other client can connect, no SSE heartbeat is sent, and the
     * control task is starved. sse_fd_is_sse() tells the httpd loop to
     * ignore this fd in the read_set processing. */
    sse_mark_fd(fd, true);

    /* Single scan: find replace slot, free slot, or candidate for eviction. */
    int replace_idx = -1, free_idx = -1;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd)
            replace_idx = i;
        else if (!s_clients[i].active)
            free_idx = i;
    }

    if (replace_idx >= 0) {
        ESP_LOGI(TAG, "replacing existing slot=%d fd=%d", replace_idx, fd);
        return sse_activate_slot(replace_idx, fd, handle, client_id, now_ms());
    }

    if (free_idx >= 0) {
        ESP_LOGI(TAG, "client registered slot=%d fd=%d id=%s", free_idx, fd, client_id);
        return sse_activate_slot(free_idx, fd, handle, client_id, now_ms());
    }

    /* Both slots full — probe each for stale disconnect. */
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        char c;
        if (recv(s_clients[i].fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {
            sse_deactivate_slot(i);
            ESP_LOGI(TAG, "client registered (evict) slot=%d fd=%d id=%s", i, fd, client_id);
            return sse_activate_slot(i, fd, handle, client_id, now_ms());
        }
    }

    ESP_LOGW(TAG, "registration rejected: all %d slots full, fd=%d", SSE_MAX_CLIENTS, fd);
    return false;
}

/* ── Push ─────────────────────────────────────────────────────────────── */

static void sse_push_work(void *arg) {
    (void)arg;

    state_snapshot_t cur = *state_get_snapshot();

    char buf[JSON_BUF_SIZE + 112];
    int off = 12 + snprintf(buf + 12, sizeof(buf) - 12,
        "id: %lu\nevent: state\ndata: ", (unsigned long)g_state_version);
    if (off < 12 || (size_t)off >= sizeof(buf)) { s_push_pending = false; return; }

    size_t slen = state_build_ssedata(buf + off, sizeof(buf) - off,
                                       s_has_snapshot ? &s_last_snapshot : NULL, &cur);
    if (slen == 0) { s_push_pending = false; return; }
    s_last_snapshot = cur;
    s_has_snapshot = true;
    off += (int)slen;

    int n = snprintf(buf + off, sizeof(buf) - off, "\n\n");
    if (n < 0) { s_push_pending = false; return; }
    off += n;

    int datalen = off - 12;
    int hdrlen = snprintf(buf, 12, "%x\r\n", datalen);
    if (hdrlen < 0 || hdrlen >= 12) { s_push_pending = false; return; }

    int gap = 12 - hdrlen;
    if (gap > 0)
        memmove(buf + hdrlen, buf + 12, (size_t)(off - 12));
    off = hdrlen + datalen;
    buf[off] = '\r';
    buf[off + 1] = '\n';

    uint32_t now = now_ms();
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) continue;
        int ret = send(s_clients[i].fd, buf, off + 2, 0);
        if (ret == off + 2) {
            s_clients[i].last_activity_ms = now;
        } else {
            ESP_LOGW(TAG, "push failed slot=%d fd=%d, disconnecting", i, s_clients[i].fd);
            sse_deactivate_slot(i);
        }
    }

    s_push_pending = false;
}

void sse_notify(void) {
    if (s_push_pending) { return; }
    s_push_pending = true;

    httpd_handle_t handle = NULL;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_clients[i].active) { handle = s_clients[i].handle; break; }
    }
    if (!handle) { s_push_pending = false; return; }

    if (httpd_queue_work(handle, sse_push_work, NULL) != ESP_OK)
        s_push_pending = false;
}
