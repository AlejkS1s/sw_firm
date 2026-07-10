#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_store.h"

#include "routines.h"
#include "ipc.h"
#include "timing.h"

#define TAG "rt"
#define MAX_COUNTDOWN_SECONDS 86400

#define NVS_NS "routines"
#define NVS_KEY_SLOTS "slots"
#define NVS_KEY_COUNT "count"

/* ── Static state ───────────────────────────────────────────────────────── */
static routine_entry_t s_slots[ROUTINES_MAX];
static int             s_slot_count = 0;
static bool            s_time_ok    = false;   /* set on NTP sync */
static bool            s_dirty      = false;

static uint32_t s_last_toggle_epoch[ROUTINES_MAX];
static bool     s_circ_phase[ROUTINES_MAX];

/* ══════════════════════════════════════════════════════════════════════════
 * NVS helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t routines_nvs_save(void) {
    if (s_slot_count > ROUTINES_MAX) return ESP_ERR_INVALID_SIZE;
    esp_err_t e = nvs_store_set_blob(NVS_NS, NVS_KEY_SLOTS,
                       s_slots, sizeof(routine_entry_t) * s_slot_count);
    if (e == ESP_OK)
        e = nvs_store_set_u32(NVS_NS, NVS_KEY_COUNT, (uint32_t)s_slot_count);
    return e;
}

static void routines_nvs_load(void) {
    uint32_t cnt = 0;
    esp_err_t e = nvs_store_get_u32(NVS_NS, NVS_KEY_COUNT, &cnt);
    if (e != ESP_OK) { s_slot_count = 0; return; }
    if (cnt > ROUTINES_MAX) cnt = ROUTINES_MAX;
    size_t sz = sizeof(routine_entry_t) * cnt;
    e = nvs_store_get_blob(NVS_NS, NVS_KEY_SLOTS, s_slots, &sz);
    if (e != ESP_OK) { s_slot_count = 0; return; }
    s_slot_count = (int)cnt;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Time math — compute the EXACT next fire time for a routine
 * ══════════════════════════════════════════════════════════════════════════ */

/* Next absolute time at hh:mm on a weekday whose bit is set in days_mask,
 * at or after `now`. Returns -1 if mask is empty or none found. */
static time_t next_weekday_time(int hour, int minute, int days_mask, time_t now) {
    if (days_mask == 0) return -1;
    struct tm tmv;
    localtime_r(&now, &tmv);
    for (int d = 0; d < 8; d++) {
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
        if (!(days_mask & (1 << ctv.tm_wday))) continue;
        if (ts >= now) return ts;
    }
    return -1;
}

/* Is `now` inside the active window for routine i? Writes the window-end
 * timestamp to *out_end when non-NULL. */
static bool circulate_in_window(int i, time_t now, time_t *out_end) {
    const routine_entry_t *e = &s_slots[i];
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (!(e->days & (1 << tmv.tm_wday))) return false;
    int start_min = e->hour * 60 + e->minute;
    int end_min   = e->end_hour * 60 + e->end_minute;
    int cur_min   = tmv.tm_hour * 60 + tmv.tm_min;
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

/* Exact next fire time for slot i (>= now), or -1 if none. Caller holds
 * g_routines_mutex. */
static time_t slot_next_fire(int i, time_t now) {
    const routine_entry_t *e = &s_slots[i];
    if (!e->enabled) return -1;

    switch (e->type) {
    case RT_SCHEDULE:
    case RT_ICHING:
        return next_weekday_time(e->hour, e->minute, e->days, now);

    case RT_COUNTDOWN:
        if (e->target_epoch == 0) return -1;               /* deferred to sync */
        if ((uint32_t)now >= e->target_epoch) return now;  /* due now */
        return (time_t)e->target_epoch;

    case RT_CIRCULATE: {
        time_t end = 0;
        if (!circulate_in_window(i, now, &end)) {
            return next_weekday_time(e->hour, e->minute, e->days, now);
        }
        if (s_last_toggle_epoch[i] == 0) return now;       /* window start */
        int interval = s_circ_phase[i] ? e->interval_on : e->interval_off;
        time_t toggle = (time_t)s_last_toggle_epoch[i] + interval;
        return (toggle < end) ? toggle : end;
    }
    default:
        return -1;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Firing
 * ══════════════════════════════════════════════════════════════════════════ */

/* Apply all routines whose next fire time is due at/before `now`. Posts
 * CMD_TURN_* messages to the actuator queue. Caller need not hold the mutex;
 * this function locks it. */
static void fire_due(time_t now) {
    actuator_msg_t pending[ROUTINES_MAX];
    int n_pending = 0;

    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    for (int i = 0; i < s_slot_count && n_pending < ROUTINES_MAX; i++) {
        if (!s_slots[i].enabled) continue;
        time_t t = slot_next_fire(i, now);
        if (t < 0 || t > now) continue;

        actuator_msg_t m = { .type = CMD_TURN_ON, .ack = NULL };
        switch (s_slots[i].type) {
        case RT_SCHEDULE:
            m.type = s_slots[i].relay_on ? CMD_TURN_ON : CMD_TURN_OFF;
            break;
        case RT_ICHING:
            m.type = CMD_TURN_OFF;
            s_slots[i].enabled = false;
            s_dirty = true;
            break;
        case RT_COUNTDOWN:
            m.type = s_slots[i].relay_on ? CMD_TURN_ON : CMD_TURN_OFF;
            s_slots[i].enabled = false;
            s_slots[i].target_epoch = 0;
            s_dirty = true;
            break;
        case RT_CIRCULATE: {
            time_t end = 0;
            bool in_win = circulate_in_window(i, now, &end);
            if (!in_win) {
                s_circ_phase[i] = true;
                s_last_toggle_epoch[i] = (uint32_t)now;
                m.type = CMD_TURN_ON;
            } else if (now >= end) {
                s_circ_phase[i] = false;
                s_last_toggle_epoch[i] = 0;
                m.type = CMD_TURN_OFF;
            } else {
                s_circ_phase[i] = !s_circ_phase[i];
                s_last_toggle_epoch[i] = (uint32_t)now;
                m.type = s_circ_phase[i] ? CMD_TURN_ON : CMD_TURN_OFF;
            }
            break;
        }
        default:
            break;
        }
        ESP_LOGI(TAG, "fired routine #%d type=%d -> %s",
                 i, s_slots[i].type, m.type == CMD_TURN_ON ? "ON" : "OFF");
        pending[n_pending++] = m;
    }
    if (s_dirty) { routines_nvs_save(); s_dirty = false; }
    xSemaphoreGive(g_routines_mutex);

    for (int i = 0; i < n_pending; i++)
        xQueueSend(g_actuator_q, &pending[i], 0);
}

/* Called once NTP has synced: mark time valid and resolve any deferred
 * countdowns (added before sync) to concrete target epochs. */
static void routines_sync(void) {
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    s_time_ok = true;
    time_t now = time(NULL);
    for (int i = 0; i < s_slot_count; i++) {
        if (!s_slots[i].enabled) continue;
        if (s_slots[i].type == RT_COUNTDOWN && s_slots[i].target_epoch == 0) {
            if (s_slots[i].duration_s > 0 && s_slots[i].duration_s <= MAX_COUNTDOWN_SECONDS) {
                s_slots[i].target_epoch = (uint32_t)(now + s_slots[i].duration_s);
                s_dirty = true;
            }
        }
    }
    if (s_dirty) { routines_nvs_save(); s_dirty = false; }
    xSemaphoreGive(g_routines_mutex);
    ESP_LOGI(TAG, "Time synced, routines active");
}

/* Earliest next fire time across all enabled routines, or -1 if none. */
static time_t arm_next(void) {
    if (!s_time_ok) return -1;
    time_t now = time(NULL);
    if (now < TIME_VALID_THRESHOLD) return -1;

    time_t best = -1;
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    for (int i = 0; i < s_slot_count; i++) {
        time_t t = slot_next_fire(i, now);
        if (t < 0) continue;
        if (best < 0 || t < best) best = t;
    }
    xSemaphoreGive(g_routines_mutex);
    return best;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Command handling (runs in routines_task)
 * ══════════════════════════════════════════════════════════════════════════ */

static void handle_rcmd(routines_msg_t *m) {
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    switch (m->type) {
    case RCMD_ADD: {
        if (s_slot_count >= ROUTINES_MAX) { m->result = ESP_ERR_NO_MEM; break; }
        if (m->entry.type > RT_ICHING) { m->result = ESP_ERR_INVALID_ARG; break; }
        if (m->entry.type == RT_COUNTDOWN &&
            (m->entry.duration_s == 0 || m->entry.duration_s > MAX_COUNTDOWN_SECONDS)) {
            m->result = ESP_ERR_INVALID_ARG; break;
        }
        if (m->entry.type == RT_CIRCULATE &&
            (m->entry.interval_on == 0 || m->entry.interval_off == 0)) {
            m->result = ESP_ERR_INVALID_ARG; break;
        }
        int id = 0;
        for (int i = 0; i < s_slot_count; i++)
            if (s_slots[i].id >= id) id = s_slots[i].id + 1;
        int idx = s_slot_count;
        s_slots[idx] = m->entry;
        s_slots[idx].id = (uint8_t)(id & 0xFF);
        s_slots[idx].enabled = true;
        if (m->entry.type == RT_COUNTDOWN)
            s_slots[idx].target_epoch = s_time_ok
                ? (uint32_t)(time(NULL) + m->entry.duration_s) : 0;  /* defer */
        s_slot_count++;
        s_dirty = true;
        m->result = ESP_OK;
        break;
    }
    case RCMD_REMOVE: {
        m->result = ESP_ERR_NOT_FOUND;
        for (int i = 0; i < s_slot_count; i++) {
            if (s_slots[i].id == m->id) {
                s_slots[i] = s_slots[s_slot_count - 1];
                s_last_toggle_epoch[i] = s_last_toggle_epoch[s_slot_count - 1];
                s_circ_phase[i] = s_circ_phase[s_slot_count - 1];
                s_slot_count--;
                s_dirty = true;
                m->result = ESP_OK;
                break;
            }
        }
        break;
    }
    case RCMD_ARM:
    default:
        m->result = ESP_OK;
        break;
    }
    if (s_dirty) { routines_nvs_save(); s_dirty = false; }
    xSemaphoreGive(g_routines_mutex);

    if (m->ack) xSemaphoreGive(m->ack);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t routines_submit(routines_msg_t *m) {
    if (xQueueSend(g_routines_q, m, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_FAIL;
    if (g_routines_task) xTaskNotifyGive(g_routines_task);
    return ESP_OK;
}

void routines_task(void *arg) {
    (void)arg;
    for (;;) {
        time_t next = arm_next();

        TickType_t wait;
        if (next > 0) {
            time_t delta = next - time(NULL);
            if (delta < 0) delta = 0;
            wait = (TickType_t)delta * (1000U / portTICK_PERIOD_MS);
        } else {
            wait = portMAX_DELAY;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        /* Drain any queued add/remove/arm commands. */
        routines_msg_t m;
        while (xQueueReceive(g_routines_q, &m, 0))
            handle_rcmd(&m);

        if (timing_time_ok() && !s_time_ok)
            routines_sync();

        if (s_time_ok)
            fire_due(time(NULL));
    }
}

bool routine_is_active(uint8_t type) {
    time_t now = time(NULL);
    if (now < TIME_VALID_THRESHOLD) {
        if (type != RT_SCHEDULE && type != RT_ICHING) return false;
    }

    if (type == RT_SCHEDULE || type == RT_ICHING) {
        xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
        for (int i = 0; i < s_slot_count; i++) {
            if (s_slots[i].enabled && s_slots[i].type == type) {
                xSemaphoreGive(g_routines_mutex);
                return true;
            }
        }
        xSemaphoreGive(g_routines_mutex);
        return false;
    }

    if (now < TIME_VALID_THRESHOLD) return false;

    if (type == RT_COUNTDOWN) {
        xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
        for (int i = 0; i < s_slot_count; i++) {
            if (s_slots[i].enabled && s_slots[i].type == RT_COUNTDOWN &&
                s_slots[i].target_epoch > (uint32_t)now) {
                xSemaphoreGive(g_routines_mutex);
                return true;
            }
        }
        xSemaphoreGive(g_routines_mutex);
        return false;
    }

    if (type == RT_CIRCULATE) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        int cur_min = tmv.tm_hour * 60 + tmv.tm_min;
        int wday = tmv.tm_wday;
        xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
        for (int i = 0; i < s_slot_count; i++) {
            if (!s_slots[i].enabled || s_slots[i].type != RT_CIRCULATE) continue;
            if (!(s_slots[i].days & (1 << wday))) continue;
            int start = s_slots[i].hour * 60 + s_slots[i].minute;
            int end   = s_slots[i].end_hour * 60 + s_slots[i].end_minute;
            if (cur_min >= start && cur_min < end) {
                xSemaphoreGive(g_routines_mutex);
                return true;
            }
        }
        xSemaphoreGive(g_routines_mutex);
        return false;
    }

    return false;
}

int routine_get_all(routine_entry_t *entries, int max) {
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    int n = s_slot_count < max ? s_slot_count : max;
    memcpy(entries, s_slots, sizeof(routine_entry_t) * n);
    xSemaphoreGive(g_routines_mutex);
    return n;
}

int routine_get_ids(int *ids, int max) {
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    int n = s_slot_count < max ? s_slot_count : max;
    for (int i = 0; i < n; i++) ids[i] = s_slots[i].id;
    xSemaphoreGive(g_routines_mutex);
    return n;
}

bool routine_get_by_id(uint8_t id, routine_entry_t *entry) {
    xSemaphoreTake(g_routines_mutex, portMAX_DELAY);
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].id == id) {
            *entry = s_slots[i];
            xSemaphoreGive(g_routines_mutex);
            return true;
        }
    }
    xSemaphoreGive(g_routines_mutex);
    return false;
}

void routines_init(void) {
    memset(s_last_toggle_epoch, 0, sizeof(s_last_toggle_epoch));
    memset(s_circ_phase, 0, sizeof(s_circ_phase));
    routines_nvs_load();
    s_time_ok = false;
    ESP_LOGI(TAG, "init: %d routines loaded", s_slot_count);
}
