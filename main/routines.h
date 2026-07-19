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

/* Slot-based entry. Slot index (0..ROUTINES_MAX-1) is the routine's
 * stable identity — no separate ID field needed. */
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
