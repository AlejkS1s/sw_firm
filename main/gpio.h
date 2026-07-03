#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_MODE_RELAY    (1 << 0)
#define LED_MODE_WIFI     (1 << 1)
#define LED_MODE_TIMER    (1 << 2)
#define LED_MODE_SCHEDULE (1 << 3)

#define BOOT_OFF   0
#define BOOT_ON    1
#define BOOT_AUTO  2

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,
    LED_BLINK_FAST,
    LED_BLINK_ERROR,
    LED_PATTERN_MAX
} led_pattern_t;

void gpio_init(void);
void gpio_set_main_task(TaskHandle_t task);
void relay_set(bool on);
bool relay_get(void);
void relay_set_boot_behavior(uint8_t mode);
uint8_t relay_get_boot_behavior(void);

void led_set(bool on);
bool led_get(void);
void led_set_pattern(led_pattern_t pat);
void led_set_mode(uint8_t bitmask);
uint8_t led_get_mode(void);
void led_update(void);
void led_update_request(void);
void wifi_set_connected(bool connected);
