#include <time.h>

#include "board.h"
#include "routines.h"
#include "state.h"
#include "timing.h"

#define ACODE_TIMER_BASE     100
#define ACODE_SCHED_BASE     200
#define ACODE_CIRCULATE_BASE 300
#define ACODE_ICHING_BASE    400
#define ACODE_RELAY_OFF      0
#define ACODE_RELAY_ON       1

/* Priority order: COUNTDOWN > CIRCULATE > ICHING > SCHEDULE (exact minute) > MANUAL */
int state_compute_acode(void) {
    bool relay = relay_get();
    routine_entry_t entries[ROUTINES_MAX];
    int n = routine_get_all(entries, ROUTINES_MAX);
    time_t now = time(NULL);
    bool time_ok = now > TIME_VALID_THRESHOLD;

    for (int i = 0; i < n; i++) {
        if (!entries[i].enabled) continue;

        if (entries[i].type == RT_COUNTDOWN && time_ok &&
            entries[i].target_epoch > (uint32_t)now)
            return ACODE_TIMER_BASE + (relay ? ACODE_RELAY_ON : ACODE_RELAY_OFF);

        if (entries[i].type == RT_CIRCULATE && time_ok) {
            struct tm tmv;
            localtime_r(&now, &tmv);
            if ((entries[i].days & (1 << tmv.tm_wday))) {
                int cur = tmv.tm_hour * 60 + tmv.tm_min;
                int start = entries[i].hour * 60 + entries[i].minute;
                int end   = entries[i].end_hour * 60 + entries[i].end_minute;
                if (cur >= start && cur < end)
                    return ACODE_CIRCULATE_BASE + (relay ? ACODE_RELAY_ON : ACODE_RELAY_OFF);
            }
            continue;
        }

        if (entries[i].type == RT_ICHING)
            return ACODE_ICHING_BASE + (relay ? ACODE_RELAY_ON : ACODE_RELAY_OFF);
    }

    if (n > 0 && time_ok) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        int today_bit = 1 << tmv.tm_wday;
        int current_min = tmv.tm_hour * 60 + tmv.tm_min;
        for (int i = 0; i < n; i++) {
            if (entries[i].type != RT_SCHEDULE) continue;
            if (!entries[i].enabled) continue;
            if (!(entries[i].days & today_bit)) continue;
            int sched_min = entries[i].hour * 60 + entries[i].minute;
            if (sched_min == current_min)
                return ACODE_SCHED_BASE + (entries[i].relay_on ? ACODE_RELAY_ON : ACODE_RELAY_OFF);
        }
    }

    return relay ? ACODE_RELAY_ON : ACODE_RELAY_OFF;
}
