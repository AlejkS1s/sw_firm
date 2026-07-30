#pragma once
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_timer.h"

/* Minimum valid Unix timestamp to distinguish "clock not set" from real time. */
#define TIME_VALID_THRESHOLD 100000
#define USEC_PER_SEC         1000000ULL
#define USEC_PER_MSEC        1000ULL
#define MSEC_PER_SEC         1000
#define MIN_PER_HOUR         60
#define HMS_TO_SEC(h,m,s)    ((uint64_t)(h) * 3600ULL + (uint64_t)(m) * 60ULL + (uint64_t)(s))

/* Max timezone string length (POSIX format, e.g. "COT5"). Shared by nvs_store.h,
 * state.c, and timing.c — defined here once. */
#define TZ_MAX_LEN 32

/* Convenience: create + start a periodic or one-shot esp_timer in one call.
 * Reduces the 3-line esp_timer_create_args_t / create / start boilerplate
 * that appears in 6+ locations across the firmware. */
static inline esp_err_t timer_create_and_start(
    esp_timer_cb_t cb, const char *name,
    esp_timer_handle_t *out, uint64_t period_us, bool periodic)
{
    esp_timer_create_args_t ta = { .callback = cb, .name = name };
    esp_err_t e = esp_timer_create(&ta, out);
    if (e != ESP_OK) return e;
    return periodic ? esp_timer_start_periodic(*out, period_us)
                    : esp_timer_start_once(*out, period_us);
}

/* Check if the system clock has been set (time(NULL) returns a value above
 * TIME_VALID_THRESHOLD).  Used by countdown.c and routines.c to avoid
 * acting on an uninitialized clock. */
static inline bool timing_is_time_valid(void) {
    return time(NULL) >= TIME_VALID_THRESHOLD;
}

void timing_init(void);
void timing_save(void);
void timing_on_ntp_synced(void);
void timing_ntp_start(void);
void timing_ntp_health_start(void);
void timing_ntp_sync_cb(struct timeval *tv);
bool timing_time_ok(void);
const char *timing_get_timezone(void);
esp_err_t timing_set_timezone(const char *tz);
