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
                           s_slots, &sz) != ESP_OK) {
        memset(s_slots, 0, sizeof(s_slots));
        return;
    }
    /* If blob size doesn't match current struct layout (e.g. after upgrading
     * from older firmware), invalidate all slots to avoid misinterpreting
     * old bytes as the new date_start/date_end fields. */
    if (sz != sizeof(s_slots)) {
        ESP_LOGW(TAG, "slot blob size mismatch (%u vs %u), clearing",
                 (unsigned)sz, (unsigned)sizeof(s_slots));
        memset(s_slots, 0, sizeof(s_slots));
    }
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

/* ── Date helpers ─────────────────────────────────────────────────────── */

/* Encode today's local date as YYYYMMDD (e.g. 20260725). */
uint32_t date_today_ymd(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return (uint32_t)((tmv.tm_year + 1900) * 10000 +
                      (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

/* Extract weekday (0=Sun..6=Sat) from a YYYYMMDD value.
 * Uses mktime to let the C library compute the day-of-week. */
static int ymd_weekday(uint32_t ymd) {
    int y = (int)(ymd / 10000);
    int m = (int)((ymd / 100) % 100);
    int d = (int)(ymd % 100);
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d,
                    .tm_hour = 12, .tm_isdst = -1 };
    mktime(&t);
    return t.tm_wday;
}

/* Check if today falls within the date range of a circulate entry.
 * Returns true if the entry's date restriction is satisfied. */
static bool circulate_date_matches(const routine_entry_t *e) {
    if (e->date_start == CIRC_DATE_NONE) return true;  /* no date restriction */
    uint32_t today = date_today_ymd();
    if (today < e->date_start || today > e->date_end) return false;
    /* Date range matches — now check weekday filter if days != 0 */
    if (e->days != 0) {
        int wday = ymd_weekday(today);
        if (!(e->days & (1 << wday))) return false;
    }
    return true;
}

/* Check if today is past the end of a one-time or range date.
 * Used to detect expired entries for auto-deletion. */
static bool circulate_date_expired(const routine_entry_t *e) {
    if (e->date_start == CIRC_DATE_NONE) return false;
    uint32_t today = date_today_ymd();
    return today > e->date_end;
}

bool circulate_in_window_for_entry(const routine_entry_t *e, time_t now, time_t *out_end) {
    /* Date restriction check (applies before weekday/time checks) */
    if (!circulate_date_matches(e)) return false;

    struct tm tmv;
    localtime_r(&now, &tmv);

    /* If no date is set and weekday filter is active, check weekday.
     * If date is set, weekday was already checked in circulate_date_matches().
     * If days == 0 (no filter), skip weekday check entirely. */
    if (e->date_start == CIRC_DATE_NONE && e->days != 0 &&
        !(e->days & (1 << tmv.tm_wday)))
        return false;

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
 * Overlap check
 * ══════════════════════════════════════════════════════════════════════════ */

bool circulate_overlaps(const routine_entry_t *entry, int exclude_idx) {
    int new_start = entry->hour * MIN_PER_HOUR + entry->minute;
    int new_end   = entry->end_hour * MIN_PER_HOUR + entry->end_minute;

    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (i == exclude_idx) continue;
        if (!s_slots[i].in_use || s_slots[i].type != RT_CIRCULATE) continue;

        const routine_entry_t *exist = &s_slots[i];

        /* Different date ranges cannot overlap */
        if (entry->date_start != exist->date_start ||
            entry->date_end   != exist->date_end)
            continue;

        /* days=0 means "any weekday" — skip the weekday intersection check
         * for either side being 0 (they overlap on every matching day) */
        if (entry->days != 0 && exist->days != 0 &&
            !(entry->days & exist->days))
            continue;

        /* Check time window overlap: max(start) < min(end) */
        int exist_start = exist->hour * MIN_PER_HOUR + exist->minute;
        int exist_end   = exist->end_hour * MIN_PER_HOUR + exist->end_minute;
        if (new_start < exist_end && exist_start < new_end)
            return true;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Expired entry cleanup (auto-delete one-time/range dates after expiry)
 * ══════════════════════════════════════════════════════════════════════════ */

static void circulate_cleanup_expired(void) {
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (!s_slots[i].in_use || s_slots[i].type != RT_CIRCULATE) continue;
        if (circulate_date_expired(&s_slots[i])) {
            ESP_LOGI(TAG, "auto-deleting expired slot %d (date_end=%lu)",
                     i, (unsigned long)s_slots[i].date_end);
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_last_toggle_epoch[i] = 0;
            s_circ_phase[i] = false;
            s_dirty = true;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Per-slot fire time
 * ══════════════════════════════════════════════════════════════════════════ */

static time_t slot_next_fire(int i, time_t now) {
    const routine_entry_t *e = &s_slots[i];
    if (!e->in_use) return -1;

    if (e->type == RT_SCHEDULE)
        return next_weekday_time(e->hour, e->minute, e->days, now);

    /* circulate — check date expiry first */
    if (circulate_date_expired(e)) return -1;

    struct tm tmv;
    localtime_r(&now, &tmv);
    int start_min = e->hour * MIN_PER_HOUR + e->minute;

    /* Check if in window (date + weekday + time-of-day) */
    bool in_win = circulate_in_window_for_entry(e, now, NULL);

    if (!in_win) {
        /* Not in window — find next matching window start.
         * For date-restricted entries, only search within the date range. */
        if (e->date_start != CIRC_DATE_NONE) {
            uint32_t today = date_today_ymd();
            if (today > e->date_end) return -1;  /* expired */

            if (e->days != 0) {
                /* Has weekday filter — use next_weekday_time */
                time_t t = next_weekday_time(e->hour, e->minute, e->days, now);
                if (t < 0) return -1;
                struct tm ctv;
                localtime_r(&t, &ctv);
                uint32_t cand_ymd = (uint32_t)((ctv.tm_year + 1900) * 10000 +
                                               (ctv.tm_mon + 1) * 100 + ctv.tm_mday);
                if (cand_ymd < e->date_start || cand_ymd > e->date_end) return -1;
                return t;
            } else {
                /* days=0: "any weekday" — just check if window is still
                 * ahead today, or schedule for tomorrow within range */
                int cur_min = tmv.tm_hour * MIN_PER_HOUR + tmv.tm_min;
                if (today >= e->date_start && today <= e->date_end) {
                    /* Today is in range — if window is ahead, schedule it */
                    if (cur_min < start_min) {
                        struct tm svc = tmv;
                        svc.tm_hour = e->hour;
                        svc.tm_min  = e->minute;
                        svc.tm_sec  = 0;
                        svc.tm_isdst = -1;
                        return mktime(&svc);
                    }
                    /* Window already started or passed — if we're in the
                     * window, slot_next_fire will be called again and the
                     * in_win path handles it. If past, nothing to do today. */
                }
                /* Not today — find next day in range (just tomorrow) */
                if (today < e->date_end) {
                    struct tm tomorrow = tmv;
                    tomorrow.tm_mday += 1;
                    tomorrow.tm_isdst = -1;
                    mktime(&tomorrow);
                    uint32_t next_ymd = (uint32_t)((tomorrow.tm_year + 1900) * 10000 +
                                                   (tomorrow.tm_mon + 1) * 100 + tomorrow.tm_mday);
                    if (next_ymd >= e->date_start && next_ymd <= e->date_end) {
                        tomorrow.tm_hour = e->hour;
                        tomorrow.tm_min  = e->minute;
                        tomorrow.tm_sec  = 0;
                        tomorrow.tm_isdst = -1;
                        return mktime(&tomorrow);
                    }
                }
                return -1;
            }
        }
        return next_weekday_time(e->hour, e->minute, e->days, now);
    }

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
    bool dirty;
    LOCK_GUARD(g_routines_mutex) {
        circulate_cleanup_expired();

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
            relay_set(on);
            ESP_LOGI(TAG, "fired slot %d type=%d -> %s",
                     i, s_slots[i].type, RELAY_STR(on));
        }
        dirty = s_dirty;
        s_dirty = false;
    }
    if (dirty) routines_nvs_save();
}

static void routines_sync(void) {
    bool dirty;
    LOCK_GUARD(g_routines_mutex) {
        s_time_ok = true;
        dirty = s_dirty;
        s_dirty = false;
    }
    if (dirty) routines_nvs_save();
    timing_save();
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
    if (relay_auto_off_is_armed()) return NULL;
    if (type == RT_CIRCULATE && (params->interval_on == 0 || params->interval_off == 0))
        return NULL;

    /* Circulate validations (pre-lock) */
    if (type == RT_CIRCULATE) {
        int smin = params->hour * MIN_PER_HOUR + params->minute;
        int emin = params->end_hour * MIN_PER_HOUR + params->end_minute;
        if (smin >= emin) return NULL;

        if (params->date_start != CIRC_DATE_NONE &&
            params->date_start > params->date_end)
            return NULL;

        /* If date + days mask both set, verify at least one day in range matches */
        if (params->date_start != CIRC_DATE_NONE && params->days != 0) {
            bool any_match = false;
            uint32_t ymd = params->date_start;
            struct tm t = { .tm_year = (int)(ymd / 10000) - 1900,
                            .tm_mon  = (int)((ymd / 100) % 100) - 1,
                            .tm_mday = (int)(ymd % 100),
                            .tm_hour = 12, .tm_isdst = -1 };
            uint32_t end_ymd = params->date_end;
            struct tm end_tm = { .tm_year = (int)(end_ymd / 10000) - 1900,
                                 .tm_mon  = (int)((end_ymd / 100) % 100) - 1,
                                 .tm_mday = (int)(end_ymd % 100),
                                 .tm_hour = 12, .tm_isdst = -1 };
            time_t end_epoch = mktime(&end_tm);
            time_t cur = mktime(&t);
            while (cur <= end_epoch) {
                struct tm *lt = localtime(&cur);
                if (params->days & (1 << lt->tm_wday)) { any_match = true; break; }
                cur += 86400;
            }
            if (!any_match) return NULL;
        }

        /* Overlap check (pre-lock read is safe — slots only grow monotonically) */
        if (circulate_overlaps(params, -1)) return NULL;
    }

    routine_handle_t h = NULL;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (s_slots[i].in_use) continue;
            s_slots[i] = *params;
            s_slots[i].type = type;
            s_slots[i].in_use = true;
            h = &s_slots[i];
            break;
        }
    }
    if (h) { routines_nvs_save();
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
    }
    routines_nvs_save();
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
    int cleaned = 0;
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (s_slots[i].in_use &&
            s_slots[i].type != RT_SCHEDULE &&
            s_slots[i].type != RT_CIRCULATE) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            cleaned += 1;
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
