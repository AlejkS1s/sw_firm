#pragma once
#include "ipc.h"

void wifi_init(void);
void wifi_reconnect(void);

/* Net task: starts NTP on WiFi connect, processes reconnect requests. */
void net_task(void *arg);

/* Request a WiFi re-provision (clears creds, enters SmartConfig). Safe to
 * call from any context — posts to the net queue. */
void net_reconnect_request(void);
