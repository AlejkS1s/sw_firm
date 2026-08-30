#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"

/* Client connection registry.
 *
 * Clients are counted by an opaque ID (the web UI's CSWID), not by socket.
 * Every HTTP request and the SSE stream carry ?client_id=<id>; dispatch()
 * records a "seen" event and the SSE heartbeat refreshes it for SSE-only
 * clients. A client counts as active if it was seen within
 * CLIENT_ACTIVE_WINDOW_MS. This replaces the old sse_client_count() which only
 * counted open SSE sockets and therefore reported 0 for a freshly-connected
 * REST client before its SSE stream opened. */

#define CLIENT_ID_MAX_LEN       24   /* matches SSE_CLIENT_ID_MAX_LEN */
#define MAX_CLIENTS             8
#define CLIENT_ACTIVE_WINDOW_MS 45000

typedef struct {
    char     id[CLIENT_ID_MAX_LEN];
    uint32_t last_seen_ms;
    bool     active;
} client_entry_t;

void clients_init(void);
void client_seen(const char *id);
unsigned client_count_active(void);
void client_extract_id(httpd_req_t *req, char *out, size_t len);
