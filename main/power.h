#pragma once

void power_init(void);
void power_notify_activity(void);
void power_process(void);

/* Returns the configured idle timeout in seconds for the actuator task. */
uint32_t power_idle_timeout_ms(void);
