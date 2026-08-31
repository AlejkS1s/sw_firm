#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define ROUTINES_MAX        12
#define DAYS_MASK           0x7F
#define ROUTINE_FWD_DAYS    8
/* Max seconds a schedule fire may be overdue and still fire. Fires older
 * than this (e.g. after a late NTP sync following an offline period) are
 * skipped instead of firing hours late. Circulate catch-up is exempt —
 * its phase bookkeeping re-anchors on the next fire. */
#define ROUTINE_FIRE_MAX_LATE_S  120

/* Blob layout versioning — the slots blob is persisted in NVS. v1 (the
 * pre-`enabled` layout, 36 bytes/slot) is migrated on load instead of being
 * wiped, so upgrading firmware preserves user routines. Any other size is
 * treated as unknown/corrupt and cleared (existing behavior). */
#define ROUTINE_LEGACY_SLOT_SIZE  36
#define ROUTINE_LEGACY_BLOB_SIZE  (ROUTINE_LEGACY_SLOT_SIZE * ROUTINES_MAX)
#define ROUTINE_BLOB_MAGIC        0x52544E31u   /* "RTN1" */

/* Type bitmask — each routine carries a single type flag. */
#define RT_SCHEDULE  0x01
#define RT_CIRCULATE 0x02

/* Date encoding — YYYYMMDD as uint32 (e.g. 20260725). 0 = no date. */
#define CIRC_DATE_NONE  0

/* Slot-based entry. Slot index (0..ROUTINES_MAX-1) is the routine's
 * stable identity — no separate ID field needed.
 *
 * Circulate fields:
 *   hour/minute       — window start (h:m)
 *   end_hour/end_minute — window end (h:m), must be > start
 *   interval_on/off   — relay cycling durations (seconds)
 *   days              — weekday bitmask (bit0=Sun..bit6=Sat), always applied
 *   date_start/end    — YYYYMMDD range restriction:
 *                         both 0   = any date (recurring on matching weekdays)
 *                         equal     = single date (one-time)
 *                         start<end = date range (active on matching weekdays
 *                                     within the range, inclusive)
 *                       After date_end passes, the slot is auto-deleted.
 *   relay_on          — for schedule: relay state; for circulate: unused
 *   enabled           — user toggle: false = disabled, never fires
 *   magic             — ROUTINE_BLOB_MAGIC, distinguishes v2 blobs from v1
 *                       (pre-`enabled`) ones for the NVS migration. */
typedef struct {
    bool     in_use;      /* slot occupied */
    uint8_t  type;        /* RT_SCHEDULE or RT_CIRCULATE */
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  end_hour;
    uint8_t  end_minute;
    uint16_t interval_on;
    uint16_t interval_off;
    uint32_t duration_s;
    uint32_t target_epoch;
    uint8_t  days;
    uint32_t date_start;  /* YYYYMMDD or 0 */
    uint32_t date_end;    /* YYYYMMDD or 0 */
    bool     relay_on;
    bool     enabled;
    uint32_t magic;
} routine_entry_t;

/* Handle — stable pointer into the slot array. Obtained from create,
 * used for remove and field access. Stable because slots never move. */
typedef routine_entry_t *routine_handle_t;

void routines_init(void);
void routines_wake(void);

/* Flush a pending routine-slot NVS write. Called from the control task tick;
 * routine_create/update/remove only mark the dirty flag so HTTP handlers
 * never block on flash (mirrors relay_persist_tick()/countdown_tick()). */
void routines_persist_tick(void);

/* Shared active mask — bit0=schedule, bit1=circulate. */
extern volatile uint8_t g_routine_active_mask;

routine_handle_t routine_create(uint8_t type, const routine_entry_t *params);
/* Edit an existing slot in place (type is immutable). `params` is a full
 * candidate entry — every field is copied over the slot, so callers build it
 * from the current entry with only the desired fields changed. Validates like
 * create (circulate window/duty/overlap), but overlap excludes self. */
esp_err_t       routine_update(routine_handle_t h, const routine_entry_t *params);
esp_err_t       routine_remove(routine_handle_t h);
routine_handle_t routine_at(int idx);
/* Snapshot used routine indices under one mutex acquisition. */
int             routine_snapshot_ids(uint8_t *ids, int max);
int             routine_count(void);
int             routine_index(routine_handle_t h);
/* True if any slot is in_use AND enabled. Used to reject arming auto-off
 * while a routine exists (the safety auto-off would fight it). */
bool            routine_any_enabled(void);
bool circulate_in_window_for_entry(const routine_entry_t *e, time_t now, time_t *out_end);
bool circulate_overlaps(const routine_entry_t *entry, int exclude_idx);
uint32_t date_today_ymd(void);
