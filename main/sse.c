#include <string.h>
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "sse.h"
#include "power.h"
#include "state.h"
#include "http_util.h"

#define SSE_MAX_CLIENTS 2

typedef struct {
    int            fd;
    httpd_handle_t handle;
    bool           active;
    char           client_id[SSE_CLIENT_ID_MAX_LEN];
} sse_client_t;

static sse_client_t s_clients[SSE_MAX_CLIENTS];
static volatile bool s_push_pending = false;
static state_snapshot_t s_last_snapshot;
static bool s_has_snapshot = false;

void sse_init(void) {
    state_register_on_change(sse_notify);
}

/* Extract a query parameter value from a URI string. Returns true if found. */
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

bool sse_register(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    httpd_handle_t handle = req->handle;

    char client_id[SSE_CLIENT_ID_MAX_LEN] = "";
    get_query_param(req->uri, "client_id", client_id, sizeof(client_id));

    /* First pass: find a free slot. */
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd        = fd;
            s_clients[i].handle    = handle;
            s_clients[i].active    = true;
            strncpy(s_clients[i].client_id, client_id, SSE_CLIENT_ID_MAX_LEN - 1);
            s_clients[i].client_id[SSE_CLIENT_ID_MAX_LEN - 1] = '\0';
            power_sse_client_connected();
            return true;
        }
    }

    /* All slots full — probe each active client's socket to detect stale
     * (disconnected) entries left over from page refresh / reconnect.
     * recv() with MSG_PEEK | MSG_DONTWAIT returns 0 on graceful close
     * (EOF) or -1 on error (socket already reclaimed by httpd). */
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        char c;
        int ret = recv(s_clients[i].fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret <= 0) {
            s_clients[i].fd        = fd;
            s_clients[i].handle    = handle;
            s_clients[i].active    = true;
            strncpy(s_clients[i].client_id, client_id, SSE_CLIENT_ID_MAX_LEN - 1);
            s_clients[i].client_id[SSE_CLIENT_ID_MAX_LEN - 1] = '\0';
            /* power count unchanged — one stale client swapped */
            return true;
        }
    }

    return false;
}

unsigned sse_client_count(void) {
    unsigned count = 0;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++)
        if (s_clients[i].active) count++;
    return count;
}

static void sse_push_work(void *arg) {
    (void)arg;

    state_snapshot_t cur;
    state_snapshot_build(&cur);

    char buf[JSON_BUF_SIZE + 64];
    int off = snprintf(buf, sizeof(buf),
        "id: %lu\nevent: state\ndata: ", (unsigned long)g_state_version);
    if (off < 0 || (size_t)off >= sizeof(buf)) { s_push_pending = false; return; }

    size_t slen = state_build_ssedata(buf + off, sizeof(buf) - off,
                                       s_has_snapshot ? &s_last_snapshot : NULL, &cur);
    s_last_snapshot = cur;
    s_has_snapshot = true;

    if (slen == 0) { s_push_pending = false; return; }
    off += (int)slen;
    int n = snprintf(buf + off, sizeof(buf) - off, "\n\n");
    if (n < 0) { s_push_pending = false; return; }
    off += n;

    char hdr[16];
    int hdr_len = snprintf(hdr, sizeof(hdr), "%x\r\n", off);

    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) continue;
        if (send(s_clients[i].fd, hdr, hdr_len, 0) < 0 ||
            send(s_clients[i].fd, buf, off, 0) < 0 ||
            send(s_clients[i].fd, "\r\n", 2, 0) < 0) {
            s_clients[i].active = false;
            s_clients[i].client_id[0] = '\0';
            power_sse_client_disconnected();
        }
    }
    s_push_pending = false;
}

void sse_notify(void) {
    if (s_push_pending) return;
    s_push_pending = true;

    httpd_handle_t handle = NULL;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_clients[i].active) {
            handle = s_clients[i].handle;
            break;
        }
    }
    if (!handle) {
        s_push_pending = false;
        return;
    }
    esp_err_t e = httpd_queue_work(handle, sse_push_work, NULL);
    if (e != ESP_OK)
        s_push_pending = false;
}
