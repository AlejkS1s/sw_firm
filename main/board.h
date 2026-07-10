#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── User LED Modes (Bitmask Flags) ── */
typedef enum {
    LED_MODE_NONE      = 0,
    LED_MODE_RELAY     = (1 << 0),
    LED_MODE_WIFI      = (1 << 1),
    LED_MODE_TIMER     = (1 << 2),
    LED_MODE_SCHEDULE  = (1 << 3),
    LED_MODE_ROUTINE   = (1 << 4),
    LED_MODE_CIRCULATE = (1 << 5),
    LED_MODE_ICHING    = (1 << 6),
} led_mode_t;

/* ── Relay Boot / Recovery Modes ── */
typedef enum {
    RELAY_BOOT_OFF  = 0,
    RELAY_BOOT_ON   = 1,
    RELAY_BOOT_AUTO = 2
} relay_boot_t;

/* ── LED Overrides ── */
typedef enum {
    LED_OFF = 0,
    LED_ON,
    LED_BLINK_SLOW,
    LED_BLINK_FAST,
    LED_BLINK_ERROR,
    LED_BLINK_OK
} led_conf_t;

/* ── Button States & Events (CONFIG_BUTTON_ENABLE) ── */
#if defined(CONFIG_BUTTON_ENABLE)
typedef enum {
    BTN_IDLE = 0,
    BTN_PRESSING,
    BTN_PRESSED,
    BTN_RELEASING
} btn_state_t;

typedef enum {
    BTN_NONE = 0,
    BTN_SHORT_PRESS,
    BTN_LONG_PRESS
} btn_event_t;
#endif /* CONFIG_BUTTON_ENABLE */

void board_init(void);

/* Returns the current relay state (mutex-guarded read). */
bool relay_get(void);

/* Returns the configured boot/recovery mode (mutex-guarded read). */
uint8_t relay_get_boot_behavior(void);

/* Request an LED pattern. Routes a CMD_SET_LED_PATTERN to the actuator task
 * (does NOT apply directly — the actuator owns the LED). Safe to call from
 * any task/context. */
void led_set_pattern(led_conf_t pat);

uint8_t led_get_mode(void);
bool   led_get(void);

/* Actuator task entry — owns relay, LED, boot mode, tz, and the power FSM. */
void actuator_task(void *arg);
