#pragma once
#include <stdbool.h>

void power_init(void);
void power_notify_activity(void);
void power_process(void);

/* Returns the configured idle timeout in seconds for the actuator task. */
uint32_t power_idle_timeout_ms(void);

void power_sse_client_connected(void);
void power_sse_client_disconnected(void);

/* User-facing power-save override. When disabled (true), the device stays
 * in WIFI_PS_NONE permanently — trades power for low latency. */
bool power_save_is_disabled(void);
void power_set_save_disabled(bool disabled);
