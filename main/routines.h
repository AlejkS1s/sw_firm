#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define ROUTINES_MAX        12
#define DAYS_MASK           0x7F
#define ROUTINE_FWD_DAYS    8

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
 *   relay_on          — for schedule: relay state; for circulate: unused */
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
} routine_entry_t;

/* Handle — stable pointer into the slot array. Obtained from create,
 * used for remove and field access. Stable because slots never move. */
typedef routine_entry_t *routine_handle_t;

void routines_init(void);
void routines_wake(void);

/* Shared active mask — bit0=schedule, bit1=circulate. */
extern volatile uint8_t g_routine_active_mask;

routine_handle_t routine_create(uint8_t type, const routine_entry_t *params);
esp_err_t       routine_remove(routine_handle_t h);
routine_handle_t routine_at(int idx);
int             routine_count(void);
int             routine_index(routine_handle_t h);
bool circulate_in_window_for_entry(const routine_entry_t *e, time_t now, time_t *out_end);
bool circulate_overlaps(const routine_entry_t *entry, int exclude_idx);
uint32_t date_today_ymd(void);
