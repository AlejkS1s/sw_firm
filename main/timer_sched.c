#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sntp.h"

#include "timer_sched.h"
#include "gpio.h"

#define TAG "sched"
#define NVS_NS "schedules"
#define NVS_KEY "slots"
#define NVS_CNT "count"

static sched_entry_t s_slots[SCHED_MAX];
static int s_slot_count = 0;
static bool s_time_ok = false;

static esp_timer_handle_t s_oneshot;
static esp_timer_handle_t s_sched_timer;
static int s_last_minute = -1;
static bool s_oneshot_active = false;
static bool s_oneshot_relay = false;

static void oneshot_cb(void *arg) {
    s_oneshot_active = false;
    relay_set(s_oneshot_relay);
    ESP_LOGI(TAG, "Timer: relay %s", s_oneshot_relay ? "ON" : "OFF");
}

static void sched_check_cb(void *arg) {
    if (!s_time_ok) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            s_time_ok = true;
            ESP_LOGI(TAG, "Time synced, scheduler active");
        } else {
            return;
        }
    }

    time_t now = time(NULL);
    if (now < 100000) return;

    struct tm *tm = gmtime(&now);
    int cur_min_total = tm->tm_hour * 60 + tm->tm_min;
    if (cur_min_total == s_last_minute) return;
    s_last_minute = cur_min_total;

    int wday = tm->tm_wday;
    for (int i = 0; i < s_slot_count; i++) {
        if (!s_slots[i].enabled) continue;
        if (!(s_slots[i].days & (1 << wday))) continue;
        if (s_slots[i].hour == (uint8_t)tm->tm_hour &&
            s_slots[i].minute == (uint8_t)tm->tm_min) {
            relay_set(s_slots[i].action);
            ESP_LOGI(TAG, "Sched #%d: relay %s @ %02d:%02d day=%d",
                     i, s_slots[i].action ? "ON" : "OFF",
                     tm->tm_hour, tm->tm_min, wday);
        }
    }
}

static esp_err_t sched_nvs_save(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, NVS_KEY, s_slots, sizeof(sched_entry_t) * s_slot_count);
    if (e == ESP_OK) e = nvs_set_u32(h, NVS_CNT, s_slot_count);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t sched_nvs_load(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        s_slot_count = 0;
        return e;
    }
    uint32_t cnt = 0;
    e = nvs_get_u32(h, NVS_CNT, &cnt);
    if (e != ESP_OK) { s_slot_count = 0; nvs_close(h); return e; }
    if (cnt > SCHED_MAX) cnt = SCHED_MAX;
    size_t sz = sizeof(sched_entry_t) * cnt;
    e = nvs_get_blob(h, NVS_KEY, s_slots, &sz);
    nvs_close(h);
    if (e != ESP_OK) {
        s_slot_count = 0;
        return e;
    }
    s_slot_count = (int)cnt;
    return ESP_OK;
}

void timer_sched_init(void) {
    sched_nvs_load();

    esp_timer_create_args_t ta = { .callback = &oneshot_cb, .name = "oneshot" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_oneshot));

    ta.callback = &sched_check_cb;
    ta.name = "sched_chk";
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_sched_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_sched_timer, 15000000));

    ESP_LOGI(TAG, "init: %d schedules loaded", s_slot_count);
}

bool timer_sched_is_active(void) {
    return s_oneshot_active;
}

esp_err_t timer_start(uint32_t seconds, bool relay_on) {
    esp_timer_stop(s_oneshot);
    s_oneshot_relay = relay_on;
    uint64_t us = (uint64_t)seconds * 1000000ULL;
    ESP_ERROR_CHECK(esp_timer_start_once(s_oneshot, us));
    s_oneshot_active = true;
    ESP_LOGI(TAG, "Timer set: relay %s in %lu s", relay_on ? "ON" : "OFF",
             (unsigned long)seconds);
    return ESP_OK;
}

void timer_cancel(void) {
    esp_timer_stop(s_oneshot);
    s_oneshot_active = false;
    ESP_LOGI(TAG, "Timer cancelled");
}

esp_err_t sched_add(const sched_entry_t *entry) {
    if (s_slot_count >= SCHED_MAX) return ESP_ERR_NO_MEM;
    int id = 0;
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].id >= id) id = s_slots[i].id + 1;
    }
    s_slots[s_slot_count] = *entry;
    s_slots[s_slot_count].id = (uint8_t)id;
    s_slots[s_slot_count].enabled = true;
    s_slot_count++;
    return sched_nvs_save();
}

esp_err_t sched_remove(uint8_t id) {
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].id == id) {
            s_slots[i] = s_slots[s_slot_count - 1];
            s_slot_count--;
            return sched_nvs_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int sched_get_all(sched_entry_t *entries, int max) {
    int n = s_slot_count < max ? s_slot_count : max;
    memcpy(entries, s_slots, sizeof(sched_entry_t) * n);
    return n;
}
