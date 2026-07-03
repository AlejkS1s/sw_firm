#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SCHED_MAX 8

typedef struct {
    uint8_t id;
    uint8_t hour;
    uint8_t minute;
    bool    action;
    uint8_t days;
    bool    enabled;
} sched_entry_t;

void timer_sched_init(void);
bool timer_sched_is_active(void);
esp_err_t timer_start(uint32_t seconds, bool relay_on);
void timer_cancel(void);

esp_err_t sched_add(const sched_entry_t *entry);
esp_err_t sched_remove(uint8_t id);
int sched_get_all(sched_entry_t *entries, int max);
