#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_store.h"

#include "board.h"     /* relay_set_async, RELAY_STR, LOCK_GUARD */
#include "routines.h"
#include "timing.h"
#include "state.h"

#define TAG "rt"

#define ROUTINES_STACK  2560
#define ROUTINES_PRIO   4

/* ── Static state ───────────────────────────────────────────────────────── */
static routine_entry_t s_slots[ROUTINES_MAX];
static bool            s_time_ok = false;
static bool            s_dirty   = false;

static uint32_t s_last_toggle_epoch[ROUTINES_MAX];
static bool     s_circ_phase[ROUTINES_MAX];

static SemaphoreHandle_t  g_routines_mutex  = NULL;
static TaskHandle_t       g_routines_task   = NULL;

/* Shared mask — bit0=schedule, bit1=circulate. Updated on every mutation,
 * sync, and task loop iteration. */
volatile uint8_t g_routine_active_mask = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * Active mask
 * ══════════════════════════════════════════════════════════════════════════ */

static void refresh_active_mask(void) {
    uint8_t mask = 0;
    time_t now = time(NULL);
    bool time_ok = timing_is_time_valid();
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (!s_slots[i].in_use) continue;
            if (s_slots[i].type == RT_SCHEDULE)
                mask |= RT_SCHEDULE;
            else if (time_ok && circulate_in_window_for_entry(&s_slots[i], now, NULL))
                mask |= RT_CIRCULATE;
        }
    }
    g_routine_active_mask = mask;
}

/* ══════════════════════════════════════════════════════════════════════════
 * NVS helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t routines_nvs_save(void) {
    return nvs_store_set_blob(NVS_NS_ROUTINES, NVS_KEY_ROUTINE_SLOTS,
                              s_slots, sizeof(s_slots));
}

static void routines_nvs_load(void) {
    size_t sz = sizeof(s_slots);
    if (nvs_store_get_blob(NVS_NS_ROUTINES, NVS_KEY_ROUTINE_SLOTS,
                           s_slots, &sz) != ESP_OK)
        memset(s_slots, 0, sizeof(s_slots));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Time math
 * ══════════════════════════════════════════════════════════════════════════ */

static time_t next_weekday_time(int hour, int minute, int days, time_t now) {
    if (days == 0) return -1;
    struct tm tmv;
    localtime_r(&now, &tmv);
    for (int d = 0; d < ROUTINE_FWD_DAYS; d++) {
        struct tm cand = tmv;
        cand.tm_hour = hour;
        cand.tm_min  = minute;
        cand.tm_sec  = 0;
        cand.tm_mday += d;
        cand.tm_isdst = -1;
        time_t ts = mktime(&cand);
        if (ts < 0) continue;
        struct tm ctv;
        localtime_r(&ts, &ctv);
        if (!(days & (1 << ctv.tm_wday))) continue;
        if (ts >= now) return ts;
    }
    return -1;
}

bool circulate_in_window_for_entry(const routine_entry_t *e, time_t now, time_t *out_end) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (!(e->days & (1 << tmv.tm_wday))) return false;
    int start_min = e->hour * MIN_PER_HOUR + e->minute;
    int end_min   = e->end_hour * MIN_PER_HOUR + e->end_minute;
    int cur_min   = tmv.tm_hour * MIN_PER_HOUR + tmv.tm_min;
    if (cur_min < start_min || cur_min >= end_min) return false;
    if (out_end) {
        struct tm et = tmv;
        et.tm_hour = e->end_hour;
        et.tm_min  = e->end_minute;
        et.tm_sec  = 0;
        et.tm_isdst = -1;
        *out_end = mktime(&et);
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Per-slot fire time
 * ══════════════════════════════════════════════════════════════════════════ */

static time_t slot_next_fire(int i, time_t now) {
    const routine_entry_t *e = &s_slots[i];
    if (!e->in_use) return -1;

    if (e->type == RT_SCHEDULE)
        return next_weekday_time(e->hour, e->minute, e->days, now);

    /* circulate — compute window end */
    struct tm tmv;
    localtime_r(&now, &tmv);
    int start_min = e->hour * MIN_PER_HOUR + e->minute;
    int end_min   = e->end_hour * MIN_PER_HOUR + e->end_minute;
    int cur_min   = tmv.tm_hour * MIN_PER_HOUR + tmv.tm_min;
    bool in_win   = (e->days & (1 << tmv.tm_wday)) &&
                    cur_min >= start_min && cur_min < end_min;

    if (!in_win)
        return next_weekday_time(e->hour, e->minute, e->days, now);

    if (s_last_toggle_epoch[i] == 0) return now;
    int interval = s_circ_phase[i] ? e->interval_on : e->interval_off;
    time_t toggle = (time_t)s_last_toggle_epoch[i] + interval;

    struct tm et = tmv;
    et.tm_hour = e->end_hour;
    et.tm_min  = e->end_minute;
    et.tm_sec  = 0;
    et.tm_isdst = -1;
    time_t end = mktime(&et);
    return (toggle < end) ? toggle : end;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Firing
 * ══════════════════════════════════════════════════════════════════════════ */

static void fire_due(time_t now) {
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (!s_slots[i].in_use) continue;
            time_t t = slot_next_fire(i, now);
            if (t < 0 || t > now) continue;

            bool on;
            if (s_slots[i].type == RT_SCHEDULE) {
                on = s_slots[i].relay_on;
            } else {
                time_t end;
                bool in_win = circulate_in_window_for_entry(&s_slots[i], now, &end);
                if (!in_win) {
                    s_circ_phase[i] = true;
                    s_last_toggle_epoch[i] = (uint32_t)now;
                    on = true;
                } else if (now >= end) {
                    s_circ_phase[i] = false;
                    s_last_toggle_epoch[i] = 0;
                    on = false;
                } else {
                    s_circ_phase[i] = !s_circ_phase[i];
                    s_last_toggle_epoch[i] = (uint32_t)now;
                    on = s_circ_phase[i];
                }
            }
            relay_set_async(on);
            ESP_LOGI(TAG, "fired slot %d type=%d -> %s",
                     i, s_slots[i].type, RELAY_STR(on));
        }
        if (s_dirty) { routines_nvs_save(); s_dirty = false; }
    }
}

static void routines_sync(void) {
    LOCK_GUARD(g_routines_mutex) {
        s_time_ok = true;
        if (s_dirty) { routines_nvs_save(); s_dirty = false; }
    }
    refresh_active_mask();
    ESP_LOGI(TAG, "Time synced, routines active");
}

static time_t arm_next(void) {
    if (!s_time_ok) return -1;
    time_t now = time(NULL);
    if (!timing_is_time_valid()) return -1;

    time_t best = -1;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            time_t t = slot_next_fire(i, now);
            if (t < 0) continue;
            if (best < 0 || t < best) best = t;
        }
    }
    return best;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

void routines_wake(void) {
    if (g_routines_task) xTaskNotifyGive(g_routines_task);
}

void routines_task(void *arg) {
    (void)arg;
    for (;;) {
        esp_task_wdt_reset();
        time_t next = arm_next();

        TickType_t wait;
        if (next > 0) {
            time_t delta = next - time(NULL);
            if (delta < 0) delta = 0;
            wait = (TickType_t)delta * (MSEC_PER_SEC / portTICK_PERIOD_MS);
        } else {
            wait = portMAX_DELAY;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        if (timing_time_ok() && !s_time_ok)
            routines_sync();

        if (s_time_ok)
            fire_due(time(NULL));

        refresh_active_mask();
    }
}

/* ── Handle-based API ────────────────────────────────────────────────── */

routine_handle_t routine_create(uint8_t type, const routine_entry_t *params) {
    if (type != RT_SCHEDULE && type != RT_CIRCULATE) return NULL;
    if (type == RT_CIRCULATE && (params->interval_on == 0 || params->interval_off == 0))
        return NULL;

    routine_handle_t h = NULL;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (s_slots[i].in_use) continue;
            s_slots[i] = *params;
            s_slots[i].type = type;
            s_slots[i].in_use = true;
            routines_nvs_save();
            h = &s_slots[i];
            break;
        }
    }
    if (h) {
        notify_bump_state();
        refresh_active_mask();
        routines_wake();
    }
    return h;
}

esp_err_t routine_remove(routine_handle_t h) {
    if (!h || !h->in_use) return ESP_ERR_INVALID_ARG;
    int i = (int)(h - s_slots);
    if (i < 0 || i >= ROUTINES_MAX) return ESP_ERR_INVALID_ARG;

    LOCK_GUARD(g_routines_mutex) {
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
        s_last_toggle_epoch[i] = 0;
        s_circ_phase[i] = false;
        routines_nvs_save();
    }
    notify_bump_state();
    refresh_active_mask();
    routines_wake();
    return ESP_OK;
}

routine_handle_t routine_at(int idx) {
    if (idx < 0 || idx >= ROUTINES_MAX) return NULL;
    routine_handle_t h = NULL;
    LOCK_GUARD(g_routines_mutex) {
        if (s_slots[idx].in_use) h = &s_slots[idx];
    }
    return h;
}

int routine_count(void) {
    int n = 0;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++)
            if (s_slots[i].in_use) n++;
    }
    return n;
}

int routine_index(routine_handle_t h) {
    if (!h) return -1;
    ptrdiff_t d = h - s_slots;
    if (d < 0 || d >= ROUTINES_MAX) return -1;
    return (int)d;
}

void routines_init(void) {
    g_routines_mutex = xSemaphoreCreateMutex();

    memset(s_slots, 0, sizeof(s_slots));
    memset(s_last_toggle_epoch, 0, sizeof(s_last_toggle_epoch));
    memset(s_circ_phase, 0, sizeof(s_circ_phase));
    routines_nvs_load();

    /* Validate loaded entries — clear any ghost slot where in_use is true
     * but type is not a recognized routine type. This recovers from NVS
     * corruption caused by the earlier deadlock bug. */
    bool cleaned = false;
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (s_slots[i].in_use &&
            s_slots[i].type != RT_SCHEDULE &&
            s_slots[i].type != RT_CIRCULATE) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            cleaned = true;
        }
    }
    if (cleaned) {
        routines_nvs_save();
        ESP_LOGW(TAG, "cleaned %d invalid routine slot(s)", cleaned);
    }

    refresh_active_mask();
    s_time_ok = false;
    ESP_LOGI(TAG, "init: %d routines loaded", routine_count());

    xTaskCreate(routines_task, "routines", ROUTINES_STACK, NULL,
                ROUTINES_PRIO, &g_routines_task);
}
