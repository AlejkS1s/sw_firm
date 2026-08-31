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
int  sse_client_count(void);  /* active SSE slot count, for diagnostics */

/* Mark/unmark an fd as belonging to an active SSE stream. The httpd loop
 * consults sse_fd_is_active() before processing a session fd to avoid
 * blocking the httpd task on recv() for a long-lived SSE connection.
 * sse_mark_seen() pre-marks an fd before sse_register() to guard against
 * the ESP-IDF httpd bug where sess_iterate returns the same fd twice in
 * one loop iteration. */
void sse_mark_fd(int fd, bool active);
bool sse_fd_is_active(int fd);
void sse_mark_seen(int fd);
void sse_clear_seen(int fd);
bool sse_fd_is_seen(int fd);
