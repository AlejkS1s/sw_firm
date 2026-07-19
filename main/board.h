#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

/* ── LED Overrides ── */
typedef enum {
    LED_OFF = 0,
    LED_ON,
    LED_BLINK_SLOW,
    LED_BLINK_FAST,
    LED_BLINK_ERROR,
    LED_BLINK_OK
} led_conf_t;

/* ── User LED Modes (Bitmask Flags) ── */
#define LED_MODE_MAX      0x7F
typedef enum {
    LED_MODE_NONE      = 0,
    LED_MODE_RELAY     = (1 << 0),
    LED_MODE_WIFI      = (1 << 1),
    LED_MODE_TIMER     = (1 << 2),
    LED_MODE_SCHEDULE  = (1 << 3),
    LED_MODE_ROUTINE   = (1 << 4),
    LED_MODE_CIRCULATE = (1 << 5),
    LED_MODE_AUTOOFF   = (1 << 6),
} led_mode_t;

/* Relay helpers — single source for common boolean → value mappings. */
#define RELAY_STR(on)   ((on) ? "ON" : "OFF")

/* ── Auto-off (Inching) Config ── */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool    enabled;
} auto_off_t;

/* ── Relay Boot / Recovery Modes ── */
typedef enum {
    RELAY_BOOT_OFF  = 0,
    RELAY_BOOT_ON   = 1,
    RELAY_BOOT_AUTO = 2
} relay_boot_t;

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

/* ── Scoped mutex lock ───────────────────────────────────────────────────
 * The body runs exactly once with m held.  break inside a switch or inner
 * loop is safe (exits the innermost construct); return/goto inside the
 * body LEAK the mutex. */
#define LOCK_GUARD(m) \
    for (int _lk_##__LINE__ = ((void)xSemaphoreTake(m, portMAX_DELAY), 0); \
         !_lk_##__LINE__; \
         _lk_##__LINE__ = ((void)xSemaphoreGive(m), 1))

void board_init(void);

/* ── Relay command API ─────────────────────────────────────────────────────
 * Three tiers, all callers pick the right one:
 *
 *   relay_set_sync / relay_toggle_sync  — block until the actuatator task
 *     has applied the change.  Only HTTP handlers need this (they must send
 *     the response after the relay actually toggles).
 *
 *   relay_set_async(on) / relay_toggle_async  — fire-and-forget, never
 *     blocks.  Safe from any context (task, timer callback).
 *
 *   relay_set_async_retry(on) — alias for relay_set_async.  Kept as a
 *     separate symbol for auto-off call sites where a dropped message
 *     is permanent (the queue depth increase from 8 to 16 makes loss
 *     effectively impossible under normal operation). */
esp_err_t relay_set_sync(bool on);
esp_err_t relay_toggle_sync(void);
void     relay_set_async(bool on);
void     relay_set_async_retry(bool on);
void     relay_toggle_async(void);

/* Returns the current relay state (mutex-guarded read). */
bool relay_get(void);

/* Set boot/recovery mode (persisted to NVS, bump state). Safe from any task. */
void relay_set_boot_behavior(uint8_t mode);

/* Returns the configured boot/recovery mode (mutex-guarded read). */
uint8_t relay_get_boot_behavior(void);

/* Auto-off (Inching) — relay domain configuration. */
bool      relay_set_auto_off(uint8_t h, uint8_t m, uint8_t s);
void      relay_auto_off_clear(void);
bool      relay_auto_off_is_armed(void);
auto_off_t relay_get_auto_off(void);
void      relay_auto_off_process(time_t now);

/* Request an LED pattern. Routes a CMD_SET_LED_PATTERN to the actuator task
 * (does NOT apply directly — the actuator owns the LED). Safe to call from
 * any task/context. */
void led_set_pattern(led_conf_t pat);

void led_set_mode(uint8_t bitmask);
uint8_t led_get_mode(void);
bool   led_get(void);
