#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_store.h"

#include "countdown.h"
#include "board.h"
#include "state.h"
#include "timing.h"

#define TAG "cnt"

typedef struct {
    uint32_t duration_s;
    uint32_t target_epoch;
    bool     relay_on;
    bool     active;
} countdown_persist_t;

static countdown_persist_t s_cd;
static esp_timer_handle_t  s_timer;

static void countdown_reset(void) {
    memset(&s_cd, 0, sizeof(s_cd));
}

static void nvs_save(void) {
    nvs_store_set_blob(NVS_NS_COUNTDOWN, NVS_KEY_COUNTDOWN_STATE, &s_cd, sizeof(s_cd));
}

static void nvs_load(void) {
    size_t sz = sizeof(s_cd);
    countdown_reset();
    if (nvs_store_get_blob(NVS_NS_COUNTDOWN, NVS_KEY_COUNTDOWN_STATE, &s_cd, &sz) != ESP_OK)
        return;
    if (!s_cd.active) { countdown_reset(); return; }
    time_t now = time(NULL);
    if (!timing_is_time_valid() || s_cd.target_epoch <= (uint32_t)now) {
        countdown_reset();
        nvs_store_erase(NVS_NS_COUNTDOWN, NVS_KEY_COUNTDOWN_STATE);
    }
}

static void timer_cb(void *arg) {
    time_t now = time(NULL);
    if (!timing_is_time_valid()) return;

    countdown_persist_t cd;
    taskENTER_CRITICAL();
    cd = s_cd;
    taskEXIT_CRITICAL();

    if (cd.active && cd.target_epoch > 0 && (uint32_t)now >= cd.target_epoch) {
        relay_set_async(cd.relay_on);
        ESP_LOGI(TAG, "fired -> %s", RELAY_STR(cd.relay_on));
        taskENTER_CRITICAL();
        memset(&s_cd, 0, sizeof(s_cd));
        taskEXIT_CRITICAL();
        nvs_store_erase(NVS_NS_COUNTDOWN, NVS_KEY_COUNTDOWN_STATE);
        notify_bump_state();
    }

    relay_auto_off_process(now);
}

void countdown_init(void) {
    nvs_load();
    ESP_ERROR_CHECK(timer_create_and_start(&timer_cb, "cnt", &s_timer, USEC_PER_SEC, true));
    ESP_LOGI(TAG, "init: active=%d", s_cd.active);
}

void countdown_set(uint32_t duration_s, bool relay_on) {
    if (duration_s == 0 || duration_s > MAX_DURATION_S) return;
    time_t now = time(NULL);
    taskENTER_CRITICAL();
    s_cd.duration_s = duration_s;
    s_cd.target_epoch = (uint32_t)(now + duration_s);
    s_cd.relay_on = relay_on;
    s_cd.active = true;
    taskEXIT_CRITICAL();
    nvs_save();
    notify_bump_state();
    ESP_LOGI(TAG, "set %lus -> %s at %lu", (unsigned long)duration_s,
             RELAY_STR(relay_on), (unsigned long)s_cd.target_epoch);
}

void countdown_cancel(void) {
    taskENTER_CRITICAL();
    memset(&s_cd, 0, sizeof(s_cd));
    taskEXIT_CRITICAL();
    nvs_store_erase(NVS_NS_COUNTDOWN, NVS_KEY_COUNTDOWN_STATE);
    notify_bump_state();
    ESP_LOGI(TAG, "cancelled");
}

bool countdown_is_active(void) {
    countdown_persist_t cd;
    taskENTER_CRITICAL();
    cd = s_cd;
    taskEXIT_CRITICAL();
    if (!cd.active || cd.target_epoch == 0) return false;
    if (!timing_is_time_valid()) return false;
    return (uint32_t)time(NULL) < cd.target_epoch;
}

uint32_t countdown_get_remaining(void) {
    countdown_persist_t cd;
    taskENTER_CRITICAL();
    cd = s_cd;
    taskEXIT_CRITICAL();
    if (!cd.active || cd.target_epoch == 0) return 0;
    time_t now = time(NULL);
    if ((uint32_t)now >= cd.target_epoch) return 0;
    return cd.target_epoch - (uint32_t)now;
}

uint32_t countdown_get_total(void) {
    bool active;
    uint32_t duration_s;
    taskENTER_CRITICAL();
    active = s_cd.active;
    duration_s = s_cd.duration_s;
    taskEXIT_CRITICAL();
    return active ? duration_s : 0;
}

uint32_t countdown_get_target_epoch(void) {
    countdown_persist_t cd;
    taskENTER_CRITICAL();
    cd = s_cd;
    taskEXIT_CRITICAL();
    return cd.active ? cd.target_epoch : 0;
}
