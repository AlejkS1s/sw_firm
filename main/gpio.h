#pragma once
#include <stdbool.h>

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,
    LED_BLINK_FAST,
    LED_BLINK_ERROR,
    LED_PATTERN_MAX
} led_pattern_t;

void gpio_init(void);
void relay_set(bool on);
bool relay_get(void);
void led_set(bool on);
bool led_get(void);
void led_set_pattern(led_pattern_t pat);
