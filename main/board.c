#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "countdown.h"
#include "routines.h"
#include "nvs_store.h"

#include "board.h"
#include "connection.h"
#include "power.h"
#include "timing.h"
#include "state.h"

#define TAG "board"

SemaphoreHandle_t      g_relay_mutex  = NULL;

/* ── Pin and timing constants ──────────────────────────────────────────── */
#define PIN_RELAY            GPIO_NUM_3
#define PIN_LED              GPIO_NUM_2
#define PIN_UART_TX          GPIO_NUM_1
#define PIN_BUTTON           GPIO_NUM_0

#define LED_TIME_SLOW_US       550000
#define LED_TIME_FAST_US       100000
#define LED_TIME_ERR_US        95000
#define LED_TIME_ERR_PAUSE_US  2000000
#define LED_BLINK_OK_COUNT     3

/* NVS key constants live in nvs_store.h — do NOT redefine locally */


/* ── Button constants (CONFIG_BUTTON_ENABLE) ────────────────────────────── */
#if defined(CONFIG_BUTTON_ENABLE)
#define BTN_POLL_INTERVAL_US    10000
#define BTN_DEBOUNCE_SAMPLES    5
#define BTN_LONG_PRESS_MS       5000
#endif

/* ── Static state ───────────────────────────────────────────────────────── */
static bool               s_relay_active = false;
static uint8_t            s_boot_mode    = RELAY_BOOT_AUTO;
static bool               s_auto_off_fired_in_cycle = false;
static uint64_t           s_auto_off_relay_on_time = 0;

static volatile bool      s_relay_save_pending = false;
/* Relay persistence latency budget: control task flush interval (see routines.c). */
#define RELAY_SAVE_MAX_LATENCY_MS 500   /* matches CTRL_TICK_MS in routines.c */

/* LED — user bitmask + override state */
static uint8_t            s_led_mode  = LED_MODE_RELAY;
static bool               s_override  = false;     /* pattern active, bitmask bypassed */
static esp_timer_handle_t s_led_timer;static int                s_blink_counter;
static int                s_blink_on_ticks;
static int                s_blink_off_ticks;
static int                s_blink_phase;           /* remaining repeats (0 = infinite) */
static led_conf_t         s_active_pattern = LED_OFF; /* current override pattern */

/* Button (CONFIG_BUTTON_ENABLE) */
#if defined(CONFIG_BUTTON_ENABLE)
static btn_state_t        s_btn_state = BTN_IDLE;
static int                s_btn_samples = 0;
static int                s_btn_inv_samples = 0;  /* consecutive opposite samples */
static int64_t            s_btn_press_time = 0;
static bool               s_btn_long_triggered = false;
static esp_timer_handle_t s_btn_timer;
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * LED Control — two modes: override (pattern from connection.c) and normal
 * (user bitmask consulted). led_update() is the normal-mode evaluation.
 * ══════════════════════════════════════════════════════════════════════════ */

static void nvs_load_led(void) {
    uint8_t val = LED_MODE_RELAY;
    if (nvs_store_get_u8(NVS_NS_LED, NVS_KEY_LED_MODE, &val) == ESP_OK)
        s_led_mode = val;
}

static void nvs_save_led(void) {
    nvs_store_set_u8(NVS_NS_LED, NVS_KEY_LED_MODE, s_led_mode);
}

static void led_hw_on(void)  { gpio_set_level(PIN_LED, 0); }
static void led_hw_off(void) { gpio_set_level(PIN_LED, 1); }

static void led_blink_start(int period_us, int off_ratio, int repeats) {
    s_blink_on_ticks  = 1;
    s_blink_off_ticks = off_ratio;
    s_blink_counter   = 1;
    s_blink_phase     = repeats;
    led_hw_on();
    esp_timer_start_periodic(s_led_timer, period_us);
}

/* Evaluate the user-mode bitmask against current state.
 * Called from the control task tick (routines.c) — s_led_mode and
 * s_relay_active are read without locking (written under g_relay_mutex,
 * read here in task context which is benign as long as volatile isn't
 * needed for correctness). Reads g_routine_active_mask (routines.c) for
 * routine-activity status without acquiring any mutex. */
static bool led_eval(void) {
    uint8_t mode = s_led_mode;
    if ((mode & LED_MODE_RELAY) && s_relay_active) return true;
    if ((mode & LED_MODE_WIFI) && wifi_is_connected()) return true;
    if ((mode & LED_MODE_TIMER)     && countdown_is_active())            return true;
    if ((mode & LED_MODE_SCHEDULE)  && (g_routine_active_mask & (1 << 0))) return true;
    if ((mode & LED_MODE_CIRCULATE) && (g_routine_active_mask & (1 << 1))) return true;
    if ((mode & LED_MODE_AUTOOFF)   && relay_auto_off_is_armed())           return true;
    if ((mode & LED_MODE_ROUTINE)   && g_routine_active_mask)                return true;
    return false;
}

/* Recompute LED state from the user bitmask. No-op while an override pattern
 * is active. Called from the control task tick. */
void led_update(void) {
    if (s_override) return;
    if (led_eval()) led_hw_on(); else led_hw_off();
}

/* Apply an override pattern. */
static void led_apply(led_conf_t pat) {
    if (pat == s_active_pattern && pat != LED_BLINK_OK) return;
    s_active_pattern = pat;

    esp_timer_stop(s_led_timer);
    led_hw_off();

    switch (pat) {
    case LED_ON:
        s_override = true;
        led_hw_on();
        return;
    case LED_BLINK_SLOW:
        s_override = true;
        led_blink_start(LED_TIME_SLOW_US, 1, 0);
        return;
    case LED_BLINK_FAST:
        s_override = true;
        led_blink_start(LED_TIME_FAST_US, 1, 0);
        return;
    case LED_BLINK_ERROR:
        s_override = true;
        led_blink_start(LED_TIME_ERR_US,
                        LED_TIME_ERR_PAUSE_US / LED_TIME_ERR_US, 0);
        return;
    case LED_BLINK_OK:
        s_override = true;
        led_blink_start(LED_TIME_FAST_US, 1, LED_BLINK_OK_COUNT);
        return;
    default:
        s_override = false;
        led_update();
        return;
    }
}

static void led_timer_cb(void *arg) {
    if (--s_blink_counter > 0) return;
    if (gpio_get_level(PIN_LED) == 0) {
        led_hw_off();
        s_blink_counter = s_blink_off_ticks;
    } else {
        led_hw_on();
        s_blink_counter = s_blink_on_ticks;
        if (s_blink_phase > 0 && --s_blink_phase == 0) {
            s_override = false;
            esp_timer_stop(s_led_timer);
            led_update();
        }
    }
}

/* Apply an LED override pattern immediately. */
void led_set_pattern(led_conf_t pat) {
    led_apply(pat);
}

bool led_get(void) {
    return gpio_get_level(PIN_LED) == 0;
}

void led_set_mode(uint8_t bitmask) {
    LOCK_GUARD(g_relay_mutex) s_led_mode = bitmask;
    nvs_save_led();
    notify_bump_state();
    ESP_LOGI(TAG, "LED mode: 0x%02x", bitmask);
}

uint8_t led_get_mode(void) {
    uint8_t v;
    LOCK_GUARD(g_relay_mutex) v = s_led_mode;
    return v;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Relay Control
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t nvs_load_relay(void) {
    uint8_t val = 0;
    esp_err_t e = nvs_store_get_u8(NVS_NS_RELAY, NVS_KEY_RELAY_STATE, &val);
    if (e == ESP_OK) s_relay_active = (val != 0);
    return e;
}

static void nvs_save_relay(void) {
    nvs_store_set_u8(NVS_NS_RELAY, NVS_KEY_RELAY_STATE, s_relay_active ? 1 : 0);
}

static void nvs_load_boot_mode(void) {
    uint8_t val = RELAY_BOOT_AUTO;
    if (nvs_store_get_u8(NVS_NS_RELAY, NVS_KEY_RELAY_BOOT, &val) == ESP_OK)
        s_boot_mode = val;
}

static void nvs_save_boot_mode(void) {
    nvs_store_set_u8(NVS_NS_RELAY, NVS_KEY_RELAY_BOOT, s_boot_mode);
}

void relay_set(bool on) {
    bool was;
    LOCK_GUARD(g_relay_mutex) {
        was = s_relay_active;
        s_relay_active = on;
        gpio_set_level(PIN_RELAY, on ? 0 : 1);
    }

    if (on && !was) {
        s_auto_off_fired_in_cycle = false;
        s_auto_off_relay_on_time = esp_timer_get_time();
    }

    /* RAM state is authoritative immediately; flash write is deferred to the
     * control task (relay_persist_tick) so callers never stall on NVS. A power
     * cut within RELAY_SAVE_MAX_LATENCY_MS may lose the last toggle. */
    s_relay_save_pending = true;
    notify_bump_state();
    power_notify_activity();
    /* Re-evaluate the LED immediately so it tracks the relay edge-for-edge;
     * the control-task tick would otherwise lag it by up to CTRL_TICK_MS.
     * No-op while a connection override pattern is active. */
    led_update();
    ESP_LOGI(TAG, "Relay: %s (was=%s)", RELAY_STR(on), RELAY_STR(was));
}

void relay_toggle(void) {
    relay_set(!relay_get());
}

bool relay_get(void) {
    bool v;
    LOCK_GUARD(g_relay_mutex) v = s_relay_active;
    return v;
}

void relay_set_boot_behavior(uint8_t mode) {
    LOCK_GUARD(g_relay_mutex) s_boot_mode = mode;
    nvs_save_boot_mode();
    notify_bump_state();
    ESP_LOGI(TAG, "Boot mode: %d", mode);
}

uint8_t relay_get_boot_behavior(void) {
    uint8_t v;
    LOCK_GUARD(g_relay_mutex) v = s_boot_mode;
    return v;
}

/* ── Auto-off state ─────────────────────────────────────────────────── */
static uint8_t s_auto_off_h = 0;
static uint8_t s_auto_off_m = 0;
static uint8_t s_auto_off_s = 0;
static bool    s_auto_off_enabled = false;

typedef struct __attribute__((packed)) {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t enabled;
} auto_off_persist_t;

static void nvs_save_auto_off(void) {
    auto_off_persist_t p = {
        .hour    = s_auto_off_h,
        .minute  = s_auto_off_m,
        .second  = s_auto_off_s,
        .enabled = s_auto_off_enabled ? 1 : 0,
    };
    nvs_store_set_blob(NVS_NS_RELAY, NVS_KEY_AOFF, &p, sizeof(p));
}

static void nvs_load_auto_off(void) {
    auto_off_persist_t p = {0};
    size_t sz = sizeof(p);
    if (nvs_store_get_blob(NVS_NS_RELAY, NVS_KEY_AOFF, &p, &sz) == ESP_OK && sz == sizeof(p)) {
        s_auto_off_h = p.hour;
        s_auto_off_m = p.minute;
        s_auto_off_s = p.second;
        s_auto_off_enabled = (p.enabled != 0);
    }
}

bool relay_set_auto_off(uint8_t h, uint8_t m, uint8_t s) {
    if (countdown_is_active() || g_routine_active_mask) {
        ESP_LOGW(TAG, "auto-off rejected: countdown or routine active");
        return false;
    }
    s_auto_off_h = h;
    s_auto_off_m = m;
    s_auto_off_s = s;
    s_auto_off_enabled = true;
    s_auto_off_fired_in_cycle = false;
    nvs_save_auto_off();
    notify_bump_state();
    ESP_LOGI(TAG, "auto-off set to %02d:%02d:%02d", h, m, s);
    return true;
}

void relay_auto_off_clear(void) {
    s_auto_off_h = 0;
    s_auto_off_m = 0;
    s_auto_off_s = 0;
    s_auto_off_enabled = false;
    s_auto_off_fired_in_cycle = false;
    nvs_save_auto_off();
    notify_bump_state();
    ESP_LOGI(TAG, "auto-off cleared");
}

bool relay_auto_off_is_armed(void) {
    return s_auto_off_enabled;
}

auto_off_t relay_get_auto_off(void) {
    auto_off_t a = { .hour = s_auto_off_h, .minute = s_auto_off_m,
                     .second = s_auto_off_s,          .enabled = s_auto_off_enabled };
    return a;
}

void relay_persist_tick(void) {
    if (!s_relay_save_pending) return;
    s_relay_save_pending = false;
    nvs_save_relay();
}

void relay_auto_off_process(void) {
    if (!s_auto_off_enabled) return;
    if (!relay_get()) return;
    if (s_auto_off_fired_in_cycle) return;
    uint64_t delay_us = HMS_TO_SEC(s_auto_off_h, s_auto_off_m, s_auto_off_s) * USEC_PER_SEC;
    if (delay_us == 0) return;
    if (s_auto_off_relay_on_time == 0) return;
    if (esp_timer_get_time() - s_auto_off_relay_on_time < delay_us) return;
    relay_set(false);
    ESP_LOGI(TAG, "auto-off fired (delay %02d:%02d:%02d)", s_auto_off_h, s_auto_off_m, s_auto_off_s);
    s_auto_off_fired_in_cycle = true;
    notify_bump_state();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Button Processing (CONFIG_BUTTON_ENABLE)
 * ══════════════════════════════════════════════════════════════════════════ */

#if defined(CONFIG_BUTTON_ENABLE)

static void handle_button_event(btn_event_t evt) {
    switch (evt) {
    case BTN_SHORT_PRESS:
        ESP_LOGI(TAG, "Short press -> toggle relay");
        relay_toggle();
        break;
    case BTN_LONG_PRESS:
        ESP_LOGI(TAG, "Long press -> WiFi reconnect");
        wifi_reconnect();
        break;
    default:
        break;
    }
}

/* Periodic debounce sampler. Runs in an esp_timer task (not ISR), so
 * calling wifi_reconnect() is safe. */
static void btn_timer_cb(void *arg) {
    int level = gpio_get_level(PIN_BUTTON);
    bool pressed = (level == 0);
    int64_t now = esp_timer_get_time();

    switch (s_btn_state) {
    case BTN_IDLE:
        if (pressed) {
            s_btn_samples = 1;
            s_btn_state = BTN_PRESSING;
        }
        break;

    case BTN_PRESSING:
        if (pressed) {
            s_btn_inv_samples = 0;
            if (++s_btn_samples >= BTN_DEBOUNCE_SAMPLES) {
                s_btn_state = BTN_PRESSED;
                s_btn_press_time = now;
                s_btn_long_triggered = false;
                s_btn_samples = 0;
                ESP_LOGD(TAG, "Button confirmed pressed");
            }
        } else {
            if (++s_btn_inv_samples >= BTN_DEBOUNCE_SAMPLES) {
                s_btn_state = BTN_IDLE;
                s_btn_samples = 0;
                s_btn_inv_samples = 0;
                ESP_LOGD(TAG, "Press abandoned (bounce)");
            }
        }
        break;

    case BTN_PRESSED:
        if (!pressed) {
            s_btn_inv_samples = 1;
            s_btn_state = BTN_RELEASING;
        } else if (!s_btn_long_triggered &&
                   (now - s_btn_press_time) >= (int64_t)BTN_LONG_PRESS_MS * USEC_PER_MSEC) {
            s_btn_long_triggered = true;
            handle_button_event(BTN_LONG_PRESS);
        }
        break;

    case BTN_RELEASING:
        if (!pressed) {
            s_btn_inv_samples = 0;
            if (++s_btn_samples >= BTN_DEBOUNCE_SAMPLES) {
                if (!s_btn_long_triggered) {
                    handle_button_event(BTN_SHORT_PRESS);
                }
                s_btn_state = BTN_IDLE;
                s_btn_samples = 0;
            }
        } else {
            if (++s_btn_inv_samples >= BTN_DEBOUNCE_SAMPLES) {
                s_btn_state = BTN_PRESSED;
                s_btn_samples = 0;
                s_btn_inv_samples = 0;
                ESP_LOGD(TAG, "Release bounced, back to PRESSED");
            }
        }
        break;
    }
}

#endif /* CONFIG_BUTTON_ENABLE */

/* ══════════════════════════════════════════════════════════════════════════
 * Init
 * ══════════════════════════════════════════════════════════════════════════ */

void board_init(void) {
    g_relay_mutex = xSemaphoreCreateMutex();

    gpio_set_level(PIN_RELAY, 1);
    gpio_set_level(PIN_LED, 1);

    gpio_config_t out = {
        .pin_bit_mask = BIT(PIN_RELAY) | BIT(PIN_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    esp_timer_create_args_t ta_led = { .callback = &led_timer_cb, .name = "led" };
    ESP_ERROR_CHECK(esp_timer_create(&ta_led, &s_led_timer));

#if defined(CONFIG_BUTTON_ENABLE)
    gpio_config_t in = {
        .pin_bit_mask = BIT(PIN_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    ESP_ERROR_CHECK(timer_create_and_start(&btn_timer_cb, "btn", &s_btn_timer,
                                           BTN_POLL_INTERVAL_US, true));
    ESP_LOGI(TAG, "Button timer started - polling GPIO%d every %dms",
             PIN_BUTTON, (int)(BTN_POLL_INTERVAL_US / USEC_PER_MSEC));
#endif

    nvs_load_led();
    nvs_load_boot_mode();
    nvs_load_auto_off();

    bool restore = false;
    if (s_boot_mode == RELAY_BOOT_AUTO) {
        if (nvs_load_relay() == ESP_OK)
            restore = true;
    } else if (s_boot_mode == RELAY_BOOT_ON) {
        s_relay_active = true;
        restore = true;
    }

    if (restore) {
        gpio_set_level(PIN_RELAY, s_relay_active ? 0 : 1);
        if (s_auto_off_enabled && s_relay_active)
            s_auto_off_relay_on_time = esp_timer_get_time();
        ESP_LOGI(TAG, "relay recovery %s (mode %d)", RELAY_STR(s_relay_active), s_boot_mode);
    } else {
        s_relay_active = false;
        ESP_LOGI(TAG, "relay recovery OFF (mode %d)", s_boot_mode);
    }

    ESP_LOGI(TAG, "LED mode 0x%02x, housekeeping on control task",
             s_led_mode);
}
