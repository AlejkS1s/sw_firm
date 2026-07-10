#pragma once
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"

/* Minimum valid Unix timestamp to distinguish "clock not set" from real time. */
#define TIME_VALID_THRESHOLD 100000

void timing_init(void);
void timing_save(void);
void timing_on_ntp_synced(void);
void timing_ntp_start(void);
void timing_ntp_health_start(void);
void timing_ntp_sync_cb(struct timeval *tv);
bool timing_time_ok(void);
const char *timing_get_timezone(void);
esp_err_t timing_set_timezone(const char *tz);
