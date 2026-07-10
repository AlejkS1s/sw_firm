#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ROUTINES_MAX 12

/* Forward declaration — full definition in ipc.h (struct routines_msg_s).
 * Required here so routines_submit() can be declared without a circular
 * include (ipc.h includes routines.h for routine_entry_t). */
struct routines_msg_s;
typedef struct routines_msg_s routines_msg_t;

typedef enum {
    RT_SCHEDULE   = 0,
    RT_COUNTDOWN  = 1,
    RT_CIRCULATE  = 2,
    RT_ICHING     = 3,
} routine_type_t;

typedef struct {
    uint8_t  id;
    uint8_t  type;
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
    bool     enabled;
} routine_entry_t;

/* Load the persisted routine table (no timer started). */
void routines_init(void);

/* Event-scheduled routines task: sleeps until the exact next fire time,
 * fires due actions as CMD_TURN_* to the actuator queue, then re-arms. */
void routines_task(void *arg);

/* Submit a routines command: enqueue + wake the routines task to re-arm. */
esp_err_t routines_submit(routines_msg_t *m);

bool  routine_is_active(uint8_t type);
int   routine_get_all(routine_entry_t *entries, int max);
int   routine_get_ids(int *ids, int max);
bool  routine_get_by_id(uint8_t id, routine_entry_t *entry);
