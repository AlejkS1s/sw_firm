#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#define SSE_CLIENT_ID_MAX_LEN 24

void sse_init(void);
bool sse_register(httpd_req_t *req);
void sse_notify(void);
unsigned sse_client_count(void);
