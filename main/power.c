#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "power.h"

#define TAG                  "power"
#define DEFAULT_IDLE_TIMEOUT 300
#define IDLE_TIMEOUT_MIN     10

static uint32_t s_idle_timeout_s = DEFAULT_IDLE_TIMEOUT;
static int64_t  s_last_activity_us;
static bool     s_active         = true;
static bool     s_pending_active = false;

void power_init(void) {
#ifdef CONFIG_POWER_IDLE_TIMEOUT
    s_idle_timeout_s = CONFIG_POWER_IDLE_TIMEOUT;
#endif
    if (s_idle_timeout_s < IDLE_TIMEOUT_MIN)
        s_idle_timeout_s = IDLE_TIMEOUT_MIN;
    s_last_activity_us = esp_timer_get_time();
    s_active = true;
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void power_notify_activity(void) {
    taskENTER_CRITICAL();
    s_last_activity_us = esp_timer_get_time();
    if (!s_active)
        s_pending_active = true;
    taskEXIT_CRITICAL();
}

void power_process(void) {
    taskENTER_CRITICAL();
    if (s_pending_active) {
        s_pending_active = false;
        s_active = true;
        taskEXIT_CRITICAL();
        esp_wifi_set_ps(WIFI_PS_NONE);
        return;
    }
    bool cur_active = s_active;
    int64_t last_act = s_last_activity_us;
    taskEXIT_CRITICAL();

    if (!cur_active) return;

    int64_t now = esp_timer_get_time();
    if (now - last_act > (int64_t)s_idle_timeout_s * 1000000LL) {
        s_active = false;
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
}

uint32_t power_idle_timeout_ms(void) {
    return s_idle_timeout_s * 1000U;
}
