#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

/* Set relay on/off or toggle. Mutex-guarded, persists to NVS, bumps state.
 * Safe from any context (task, timer callback). */
void relay_set(bool on);
void relay_toggle(void);

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
void      relay_auto_off_process(void);

/* Apply an LED override pattern immediately. Safe from any task/context. */
void led_set_pattern(led_conf_t pat);

void led_set_mode(uint8_t bitmask);
uint8_t led_get_mode(void);
bool   led_get(void);
