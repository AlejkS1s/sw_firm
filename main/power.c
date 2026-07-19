#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "power.h"
#include "nvs_store.h"
#include "state.h"
#include "timing.h"

#define TAG                  "power"
#define DEFAULT_IDLE_TIMEOUT 300
#define IDLE_TIMEOUT_MIN     10
/* NVS keys live in nvs_store.h */

static uint32_t s_idle_timeout_s = DEFAULT_IDLE_TIMEOUT;
static int64_t  s_last_activity_us;
static bool     s_active         = true;
static bool     s_pending_active = false;

/* User-facing override: when true, the device stays in WIFI_PS_NONE
 * permanently and the idle-timeout logic below never runs, regardless of
 * SSE subscriber count or time since last activity. This trades power
 * consumption for guaranteed low command/push latency — useful when the
 * relay is mains-powered and responsiveness matters more than radio
 * power draw, or while debugging timing-sensitive behavior. Persisted so
 * it survives a reboot. */
static bool s_save_disabled = false;

bool power_save_is_disabled(void) {
    return s_save_disabled;
}

void power_set_save_disabled(bool disabled) {
    s_save_disabled = disabled;
    nvs_store_set_u8(NVS_NS_POWER, NVS_KEY_POWER_SAVE, disabled ? 1 : 0);
    if (disabled) {
        /* Force out of modem sleep immediately rather than waiting for the
         * next power_process() tick — the whole point of this switch is
         * "stop delaying packets right now". */
        taskENTER_CRITICAL();
        s_active = true;
        s_last_activity_us = esp_timer_get_time();
        taskEXIT_CRITICAL();
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    /* Re-enabling doesn't force sleep immediately — that stays
     * power_process()'s decision, based on idle time and SSE subscribers,
     * exactly as if the setting had always been off. */
    notify_bump_state();
}

/* Count of live SSE subscribers (http_server.c). While this is non-zero we
 * refuse to enter WIFI_PS_MAX_MODEM regardless of idle time: modem sleep
 * delays inbound/outbound packets to the next DTIM beacon, which would
 * make relay-state pushes and commands sluggish for whoever still has the
 * dashboard open. With zero subscribers, the existing idle-timeout path
 * behaves exactly as before. */
static volatile int s_sse_clients = 0;

void power_sse_client_connected(void) {
    taskENTER_CRITICAL();
    s_sse_clients++;
    taskEXIT_CRITICAL();
    power_notify_activity();
}

void power_sse_client_disconnected(void) {
    taskENTER_CRITICAL();
    if (s_sse_clients > 0) s_sse_clients--;
    taskEXIT_CRITICAL();
}

void power_init(void) {
#ifdef CONFIG_POWER_IDLE_TIMEOUT
    s_idle_timeout_s = CONFIG_POWER_IDLE_TIMEOUT;
#endif
    if (s_idle_timeout_s < IDLE_TIMEOUT_MIN)
        s_idle_timeout_s = IDLE_TIMEOUT_MIN;
    s_last_activity_us = esp_timer_get_time();
    s_active = true;

    uint8_t v = 0;
    if (nvs_store_get_u8(NVS_NS_POWER, NVS_KEY_POWER_SAVE, &v) == ESP_OK)
        s_save_disabled = (v != 0);

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
    if (s_save_disabled) return;   /* explicit override — never sleep the modem */

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
    bool has_subscriber = (s_sse_clients > 0);
    taskEXIT_CRITICAL();

    if (!cur_active) return;
    if (has_subscriber) return;   /* stay in PS_NONE while a dashboard is watching */

    int64_t now = esp_timer_get_time();
    if (now - last_act > (int64_t)s_idle_timeout_s * (int64_t)USEC_PER_SEC) {
        s_active = false;
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
}

uint32_t power_idle_timeout_ms(void) {
    return s_idle_timeout_s * MSEC_PER_SEC;
}
