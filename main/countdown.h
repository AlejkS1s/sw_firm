#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAX_DURATION_S  86400

void     countdown_init(void);
void     countdown_set(uint32_t duration_s, bool relay_on);
void     countdown_cancel(void);
bool     countdown_is_active(void);
uint32_t countdown_get_remaining(void);
uint32_t countdown_get_total(void);
uint32_t countdown_get_target_epoch(void);
