#pragma once
#include <stdbool.h>

void gpio_init(void);
void relay_set(bool on);
bool relay_get(void);
void led_set(bool on);
bool led_get(void);
