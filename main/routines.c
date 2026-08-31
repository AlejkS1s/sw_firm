#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_store.h"

#include "board.h"     /* relay_set_async, RELAY_STR, LOCK_GUARD */
#include "countdown.h" /* countdown_is_active — runtime priority arbitration */
#include "power.h"
#include "routines.h"
#include "sse.h"
#include "timing.h"
#include "state.h"

#define TAG "rt"

#define ROUTINES_STACK  2560
/* Control task: highest user priority — GPIO/scheduling outrank HTTP (3).
 * Owns ALL periodic work (routines, countdown, auto-off, relay persist flush,
 * LED eval, power management, SSE heartbeat). Max sleep bounds worst-case
 * lateness of every tick-driven action. */
#define ROUTINES_PRIO   5
#define CTRL_TICK_US    500000
#define CTRL_TICK_MS    (CTRL_TICK_US / USEC_PER_MSEC)

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
            if (!s_slots[i].in_use || !s_slots[i].enabled) continue;
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

/* Control-task tick: flush a pending routine-slot NVS write. Keeps the
 * flash write (10-50 ms SPI-cache stall) off the HTTP handler path —
 * routine_create/update/remove only set s_dirty. */
void routines_persist_tick(void) {
    bool dirty;
    LOCK_GUARD(g_routines_mutex) {
        dirty = s_dirty;
        s_dirty = false;
    }
    if (dirty) routines_nvs_save();
}

static void routines_nvs_load(void) {
    size_t sz = sizeof(s_slots);
    if (nvs_store_get_blob(NVS_NS_ROUTINES, NVS_KEY_ROUTINE_SLOTS,
                           s_slots, &sz) != ESP_OK) {
        memset(s_slots, 0, sizeof(s_slots));
        return;
    }
    /* v1 blob (pre-`enabled`/`magic`, 36 B/slot): all fields up to relay_on
     * are valid, but `enabled`/`magic` read padding garbage. Migrate instead
     * of clearing so upgrading firmware keeps the user's routines. */
    if (sz == ROUTINE_LEGACY_BLOB_SIZE) {
        int n = 0;
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (!s_slots[i].in_use) continue;
            s_slots[i].enabled = true;
            s_slots[i].magic   = ROUTINE_BLOB_MAGIC;
            n++;
        }
        ESP_LOGW(TAG, "migrated %d routine slot(s) from v1 blob layout", n);
        routines_nvs_save();
        return;
    }
    /* If blob size doesn't match current struct layout, invalidate all slots
     * to avoid misinterpreting bytes from an unknown layout. */
    if (sz != sizeof(s_slots)) {
        ESP_LOGW(TAG, "slot blob size mismatch (%u vs %u), clearing",
                 (unsigned)sz, (unsigned)sizeof(s_slots));
        memset(s_slots, 0, sizeof(s_slots));
        return;
    }
    /* v2 blob — sanity-check the magic of every used slot. */
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (s_slots[i].in_use && s_slots[i].magic != ROUTINE_BLOB_MAGIC) {
            ESP_LOGW(TAG, "slot %d magic mismatch, clearing all", i);
            memset(s_slots, 0, sizeof(s_slots));
            break;
        }
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
    /* A disabled routine is inert: it cannot conflict with anything, and the
     * scheduler never fires it. This is what makes disable-then-rearrange
     * workflows work — disabling frees the time window (see PUT handler). */
    if (!entry->enabled) return false;

    int new_start = entry->hour * MIN_PER_HOUR + entry->minute;
    int new_end   = entry->end_hour * MIN_PER_HOUR + entry->end_minute;

    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (i == exclude_idx) continue;
        if (!s_slots[i].in_use || !s_slots[i].enabled ||
            s_slots[i].type != RT_CIRCULATE)
            continue;

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

/* A schedule fires at a fixed h:m on matching days. It conflicts with:
 *  - another schedule firing at the same h:m (same date range + overlapping days)
 *  - a circulate whose window contains that h:m (same date range + overlapping days)
 * Disabled entries are inert and never conflict. */
static bool schedule_conflicts(const routine_entry_t *entry, int exclude_idx) {
    if (!entry->enabled) return false;
    int fire_min = entry->hour * MIN_PER_HOUR + entry->minute;
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (i == exclude_idx) continue;
        const routine_entry_t *o = &s_slots[i];
        if (!o->in_use || !o->enabled) continue;
        /* Different date ranges cannot overlap */
        if (entry->date_start != o->date_start ||
            entry->date_end   != o->date_end)
            continue;
        /* days=0 means "any weekday" — skip the weekday intersection check
         * for either side being 0 (they overlap on every matching day) */
        if (entry->days != 0 && o->days != 0 &&
            !(entry->days & o->days))
            continue;
        if (o->type == RT_CIRCULATE) {
            int ws = o->hour * MIN_PER_HOUR + o->minute;
            int we = o->end_hour * MIN_PER_HOUR + o->end_minute;
            if (fire_min >= ws && fire_min < we) return true;
        } else { /* RT_SCHEDULE */
            int of = o->hour * MIN_PER_HOUR + o->minute;
            if (fire_min == of) return true;
        }
    }
    return false;
}

/* Symmetric guard for the circulate path: reject a circulate whose window
 * contains any enabled schedule's fire time (same date range + overlapping
 * days). Keeps schedule-vs-circulate conflicts out in both directions. */
static bool circulate_schedule_conflict(const routine_entry_t *entry, int exclude_idx) {
    if (!entry->enabled) return false;
    int ws = entry->hour * MIN_PER_HOUR + entry->minute;
    int we = entry->end_hour * MIN_PER_HOUR + entry->end_minute;
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (i == exclude_idx) continue;
        const routine_entry_t *o = &s_slots[i];
        if (!o->in_use || !o->enabled || o->type != RT_SCHEDULE) continue;
        if (entry->date_start != o->date_start ||
            entry->date_end   != o->date_end)
            continue;
        if (entry->days != 0 && o->days != 0 &&
            !(entry->days & o->days))
            continue;
        int of = o->hour * MIN_PER_HOUR + o->minute;
        if (of >= ws && of < we) return true;
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
    if (!e->enabled) return -1;   /* disabled routines never fire */

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

typedef struct { uint8_t idx; bool on; } due_action_t;

static void fire_due(time_t now) {
    /* Runtime priority arbitration: a countdown owns the relay while it is
     * active, so routines pause (do not fire) until it completes or is
     * cancelled — the two never fight over the output. Manual toggles still
     * win (relay_set is always applied); auto-off is already mutually
     * exclusive with countdown/routines at creation time. */
    if (countdown_is_active()) return;

    due_action_t actions[ROUTINES_MAX];
    int n_actions = 0;
    bool dirty;

    /* Phase 1 — under lock: compute due slots and update phase bookkeeping
     * ONLY (cheap, pure memory). Never call relay_set/NVS while holding the
     * mutex: state_snapshot_build() needs this lock on every /state request,
     * and holding it across flash writes stalled the whole device under load.
     *
     * NOTE: we deliberately do NOT clear s_dirty here. routines_persist_tick()
     * runs in the control task context (same task as fire_due), but the actual
     * NVS write is deferred to its next tick. This keeps the flash-cache stall
     * (which blocks lwIP) off the httpd task's send() path. */
    LOCK_GUARD(g_routines_mutex) {
        circulate_cleanup_expired();

        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (!s_slots[i].in_use) continue;
            time_t t = slot_next_fire(i, now);
            if (t < 0 || t > now) continue;
            /* Schedule fired while we were offline/timeless? Fire it only
             * within the grace window; older ones are skipped (the next
             * occurrence is recomputed fresh on every tick). Circulate is
             * exempt: skipping would stall its phase bookkeeping — instead
             * it fires once and re-anchors s_last_toggle_epoch to now. */
            if (s_slots[i].type == RT_SCHEDULE &&
                (now - t) > ROUTINE_FIRE_MAX_LATE_S) continue;

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
                s_dirty = true;  /* circulate phase/epoch changed — persist on next tick */
            }
            actions[n_actions].idx = (uint8_t)i;
            actions[n_actions].on  = on;
            n_actions++;
        }
    }

    /* Phase 2 — outside lock: apply relay actions. The relay_set() calls
     * notify_bump_state() which queues SSE pushes to the httpd task via
     * httpd_queue_work() (non-blocking). NVS persistence is handled by
     * routines_persist_tick() on the next control-task tick — NEVER inline
     * here, because the flash-cache stall from nvs_commit() blocks lwIP and
     * would stall the httpd task's send() for up to SO_SNDTIMEO. */
    for (int k = 0; k < n_actions; k++) {
        const routine_entry_t *e = &s_slots[actions[k].idx];
        relay_set(actions[k].on);
        ESP_LOGI(TAG, "fired slot %d type=%d -> %s",
                 actions[k].idx, e->type, RELAY_STR(actions[k].on));
    }
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
    notify_bump_state();
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

        /* Sleep until the next routine fire, capped at CTRL_TICK_MS so the
         * consolidated housekeeping ticks never lag beyond one interval. */
        time_t next = arm_next();
        TickType_t wait;
        if (next > 0) {
            time_t delta = next - time(NULL);
            if (delta < 0) delta = 0;
            TickType_t fire_ticks =
                (TickType_t)delta * (MSEC_PER_SEC / portTICK_PERIOD_MS);
            TickType_t cap = CTRL_TICK_MS / portTICK_PERIOD_MS;
            wait = (fire_ticks < cap) ? fire_ticks : cap;
        } else {
            wait = CTRL_TICK_MS / portTICK_PERIOD_MS;
        }
        if (wait == 0) wait = 1;
        ulTaskNotifyTake(pdTRUE, wait);

        if (timing_time_ok() && !s_time_ok)
            routines_sync();

        if (s_time_ok)
            fire_due(time(NULL));

        refresh_active_mask();

        /* Consolidated housekeeping — each is bounded and non-blocking. */
        countdown_tick();
        relay_persist_tick();
        routines_persist_tick();
        led_update();
        power_process();
        sse_heartbeat_tick();
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
        if (circulate_overlaps(params, -1) ||
            circulate_schedule_conflict(params, -1))
            return NULL;
    } else {
        /* Schedule validations (pre-lock) — same fire time as another schedule
         * or inside a circulate window is rejected. */
        if (schedule_conflicts(params, -1)) return NULL;
    }

    routine_handle_t h = NULL;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (s_slots[i].in_use) continue;
            s_slots[i] = *params;
            s_slots[i].type = type;
            s_slots[i].in_use = true;
            s_slots[i].enabled = true;   /* new routines start enabled */
            s_slots[i].magic   = ROUTINE_BLOB_MAGIC;
            h = &s_slots[i];
            break;
        }
    }
    if (h) { s_dirty = true;
        notify_bump_state();
        refresh_active_mask();
        routines_wake();
    }
    return h;
}

esp_err_t routine_update(routine_handle_t h, const routine_entry_t *params) {
    if (!h || !h->in_use) return ESP_ERR_INVALID_ARG;
    int i = (int)(h - s_slots);
    if (i < 0 || i >= ROUTINES_MAX) return ESP_ERR_INVALID_ARG;
    if (!params || params->type != h->type) return ESP_ERR_INVALID_ARG;

    /* Same validations as create (auto-off conflict is handled by the HTTP
     * layer, since disabling is always allowed while auto-off is armed). */
    if (params->type == RT_CIRCULATE) {
        if (params->interval_on == 0 || params->interval_off == 0)
            return ESP_ERR_INVALID_ARG;
        int smin = params->hour * MIN_PER_HOUR + params->minute;
        int emin = params->end_hour * MIN_PER_HOUR + params->end_minute;
        if (smin >= emin) return ESP_ERR_INVALID_ARG;
        if (params->date_start != CIRC_DATE_NONE &&
            params->date_start > params->date_end)
            return ESP_ERR_INVALID_ARG;
        if (circulate_overlaps(params, i) ||
            circulate_schedule_conflict(params, i))
            return ESP_ERR_INVALID_ARG;
    } else {
        if (schedule_conflicts(params, i)) return ESP_ERR_INVALID_ARG;
    }

    LOCK_GUARD(g_routines_mutex) {
        s_slots[i] = *params;
        s_slots[i].type   = h->type;    /* type immutable on edit */
        s_slots[i].in_use = true;
        s_slots[i].magic  = ROUTINE_BLOB_MAGIC;
        /* Restart cycle bookkeeping so a changed window/duty takes effect
         * cleanly instead of firing on a stale phase/interval. */
        s_circ_phase[i]       = false;
        s_last_toggle_epoch[i] = 0;
    }
    s_dirty = true;
    notify_bump_state();
    refresh_active_mask();
    routines_wake();
    return ESP_OK;
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
    s_dirty = true;
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

/* Snapshot used routine indices under ONE mutex acquisition — replaces
 * N× routine_at() lock round-trips in hot read paths (state snapshot,
 * which previously contended with fire_due() on every /state request). */
int routine_snapshot_ids(uint8_t *ids, int max) {
    int n = 0;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX && n < max; i++) {
            if (s_slots[i].in_use) ids[n++] = (uint8_t)i;
        }
    }
    return n;
}

int routine_count(void) {
    int n = 0;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++)
            if (s_slots[i].in_use) n++;
    }
    return n;
}

bool routine_any_enabled(void) {
    bool any = false;
    LOCK_GUARD(g_routines_mutex) {
        for (int i = 0; i < ROUTINES_MAX; i++) {
            if (s_slots[i].in_use && s_slots[i].enabled) { any = true; break; }
        }
    }
    return any;
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
