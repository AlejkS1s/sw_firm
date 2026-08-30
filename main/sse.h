#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#define SSE_CLIENT_ID_MAX_LEN 24

void sse_init(void);
bool sse_register(httpd_req_t *req);
void sse_notify(void);

/* Control-task tick (500 ms cadence): heartbeat push + stale-client eviction.
 * Runs at high priority, hence the short SSE send timeout. */
void sse_heartbeat_tick(void);

/* SSE feature control — enable/disable the entire SSE subsystem.
 * When disabled, existing connections are closed immediately and new
 * registrations are rejected with 503. Persisted to NVS. */
void sse_set_enabled(bool enabled);
bool sse_is_enabled(void);
void sse_close_all(void);
