/*
 * gpio.c — GPIO control for ESP-01S Relay
 *
 * Hardware:
 *   GPIO3 = Relay (active-low)
 *   GPIO2 = LED   (active-low)
 *   GPIO0 = Button (active-low, internal pull-up, CONFIG_BUTTON_ENABLE)
 *
 * NVS persistence (deferred — writes happen in main loop):
 *   relay/state — last relay state (u8)
 *   relay/boot  — boot behavior mode (u8: 0=OFF, 1=ON, 2=AUTO)
 *   led/mode    — LED mode bitmask (u8)
 *
 * LED priority: TIMER > SCHEDULE > WIFI > RELAY > slow blink (unconfigured).
 * When no override is active, led_update() applies the highest priority
 * state that matches. WiFi module drives override patterns directly.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_store.h"
#include "gpio.h"
#include "notify.h"
#include "timer_sched.h"
#include "wifi.h"

#define TAG "gpio"

/* ── Pin and timing constants ──────────────────────────────────────────── */

#define PIN_RELAY            GPIO_NUM_3
#define PIN_LED              GPIO_NUM_2
#define PIN_UART_TX          GPIO_NUM_1
#define PIN_BUTTON           GPIO_NUM_0

#define LED_TIME_SLOW_US     550000
#define LED_TIME_FAST_US     100000
#define LED_TIME_ERR_US      95000
#define LED_TIME_ERR_PAUSE_US 2000000
#define BUTTON_POLL_INTERVAL_US 100000

/* ── NVS key constants ──────────────────────────────────────────────────── */

#define NVS_RELAY_NS  "relay"
#define NVS_RELAY_KEY "state"
#define NVS_BOOT_KEY  "boot"
#define NVS_LED_NS    "led"
#define NVS_LED_KEY   "mode"

/* ── Button constants (CONFIG_BUTTON_ENABLE) ────────────────────────────── */

#if defined(CONFIG_BUTTON_ENABLE)
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_LONG_PRESS_MS    5000
typedef enum { BTN_NONE, BTN_TOGGLE, BTN_RECONNECT } btn_action_t;
#endif

/* ── Static state ───────────────────────────────────────────────────────── */

/* Relay */
static bool               s_relay_active     = false;
static bool               s_relay_dirty      = false;
static uint8_t            s_boot_mode        = BOOT_OFF;

/* LED */
/* LED controller state machine
 *
 * Two states:
 *   CTL_NORMAL    - led_update() applies s_user_mode bitmask
 *   CTL_OVERRIDE  - led_update() applies s_override_pat directly
 *
 * Transitions:
 *   led_set_pattern(pat != LED_OFF) -> CTL_OVERRIDE, calls led_update()
 *   led_set_pattern(LED_OFF)         -> CTL_NORMAL (LED off, no led_update)
 *   led_clear_override()             -> CTL_NORMAL, calls led_update()
 *   LED_BLINK_OK completes           -> CTL_NORMAL, calls led_update()
 */
typedef enum { CTL_NORMAL, CTL_OVERRIDE } led_ctl_t;

static uint8_t            s_user_mode        = LED_MODE_RELAY;
static led_ctl_t          s_ctl              = CTL_NORMAL;
static led_pattern_t      s_override_pat;
static esp_timer_handle_t s_led_timer;
static int                s_blink_on_ticks;
static int                s_blink_off_ticks;
static int                s_blink_counter;
static int                s_blink_phase;

/* Button (CONFIG_BUTTON_ENABLE) */
#if defined(CONFIG_BUTTON_ENABLE)
static volatile btn_action_t s_button_action = BTN_NONE;
static bool               s_btn_long_triggered = false;
static int64_t            s_btn_press_time   = 0;
static bool               s_btn_pressed      = false;
static int                s_btn_last_level   = 1;
static esp_timer_handle_t s_btn_timer;
#endif

/* Task */
static TaskHandle_t       s_main_task        = NULL;

/* ══════════════════════════════════════════════════════════════════════════
 * LED Control API
 * ══════════════════════════════════════════════════════════════════════════ */

static void nvs_load_led(void) {
    uint8_t val = LED_MODE_RELAY;
    if (nvs_store_get_u8(NVS_LED_NS, NVS_LED_KEY, &val) == ESP_OK)
        s_user_mode = val;
}

static void nvs_save_led(void) {
    nvs_store_set_u8(NVS_LED_NS, NVS_LED_KEY, s_user_mode);
}

static void led_blink_start(int period_us, int off_ticks) {
    s_blink_on_ticks  = 1;
    s_blink_off_ticks = off_ticks;
    s_blink_counter   = 1;
    gpio_set_level(PIN_LED, 0);
    esp_timer_start_periodic(s_led_timer, period_us);
}

/**
 * led_update_request — Notify main task to call led_update().
 * Safe to call from any context (ISR, timer, HTTP handler).
 */
void led_update_request(void) {
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

/**
 * led_update — Unified: applies current controller state to the LED.
 *   CTL_NORMAL:   evaluates s_user_mode bitmask (TIMER > SCHEDULE > WIFI >
 *                 RELAY > slow blink).
 *   CTL_OVERRIDE: applies s_override_pat directly (ON, BLINK_*, etc.).
 *
 * Called from main loop (via notification), led_set_pattern(),
 * led_clear_override(), and led_timer_cb() on pattern completion.
 */
void led_update(void) {
    esp_timer_stop(s_led_timer);
    gpio_set_level(PIN_LED, 1);

    if (s_ctl == CTL_OVERRIDE) {
        switch (s_override_pat) {
        case LED_ON:
            gpio_set_level(PIN_LED, 0);
            return;
        case LED_BLINK_SLOW:
            led_blink_start(LED_TIME_SLOW_US, 1);
            return;
        case LED_BLINK_FAST:
            led_blink_start(LED_TIME_FAST_US, 1);
            return;
        case LED_BLINK_ERROR:
            led_blink_start(LED_TIME_ERR_US, LED_TIME_ERR_PAUSE_US / LED_TIME_ERR_US);
            return;
        case LED_BLINK_OK:
            s_blink_phase = 3;
            led_blink_start(LED_TIME_FAST_US, 1);
            return;
        default:
            return;
        }
    }

    if ((s_user_mode & LED_MODE_TIMER) && timer_sched_is_active()) {
        led_blink_start(LED_TIME_SLOW_US, 1);
        return;
    }
    if ((s_user_mode & LED_MODE_SCHEDULE) && sched_is_any_enabled()) {
        led_blink_start(LED_TIME_SLOW_US, 1);
        return;
    }
    if (s_user_mode & LED_MODE_WIFI) {
        gpio_set_level(PIN_LED, 0);
        return;
    }
    if (s_user_mode & LED_MODE_RELAY) {
        gpio_set_level(PIN_LED, s_relay_active ? 0 : 1);
        return;
    }
    led_blink_start(LED_TIME_SLOW_US, 1);
}

/**
 * led_timer_cb — esp_timer callback for blink patterns.
 * Toggles the LED each tick; on LED_BLINK_OK completion transitions
 * CTL_OVERRIDE -> CTL_NORMAL via led_update().
 */
static void led_timer_cb(void *arg) {
    if (--s_blink_counter > 0) return;
    if (gpio_get_level(PIN_LED) == 0) {
        gpio_set_level(PIN_LED, 1);
        s_blink_counter = s_blink_off_ticks;
    } else {
        gpio_set_level(PIN_LED, 0);
        s_blink_counter = s_blink_on_ticks;
        if (s_blink_phase > 0) {
            if (--s_blink_phase == 0) {
                s_ctl = CTL_NORMAL;
                led_update();
                return;
            }
        }
    }
}

/**
 * led_set_pattern — Transition to CTL_OVERRIDE and apply pattern.
 * LED_OFF is special: turns LED off and returns to CTL_NORMAL silently.
 */
void led_set_pattern(led_pattern_t pat) {
    if (pat == LED_OFF) {
        s_ctl = CTL_NORMAL;
        esp_timer_stop(s_led_timer);
        gpio_set_level(PIN_LED, 1);
        return;
    }
    s_ctl = CTL_OVERRIDE;
    s_override_pat = pat;
    led_update();
}

void led_set(bool on) {
    led_set_pattern(on ? LED_ON : LED_OFF);
}

bool led_get(void) {
    return gpio_get_level(PIN_LED) == 0;
}

/**
 * led_set_mode — Set user-mode bitmask; persisted to NVS.
 * Only affects CTL_NORMAL behaviour; requests led_update().
 */
void led_set_mode(uint8_t bitmask) {
    s_user_mode = bitmask;
    nvs_save_led();
    notify_bump_state();
    ESP_LOGI(TAG, "LED mode: 0x%02x", bitmask);
    led_update_request();
}

uint8_t led_get_mode(void) {
    return s_user_mode;
}

/**
 * led_clear_override — Transition CTL_OVERRIDE -> CTL_NORMAL.
 * Stops any blink timer and applies user mode via led_update().
 * Safe to call when already CTL_NORMAL (no-op).
 */
void led_clear_override(void) {
    if (s_ctl == CTL_NORMAL) return;
    s_ctl = CTL_NORMAL;
    led_update();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Relay Control API
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t nvs_load_relay(void) {
    uint8_t val = 0;
    esp_err_t e = nvs_store_get_u8(NVS_RELAY_NS, NVS_RELAY_KEY, &val);
    if (e == ESP_OK) s_relay_active = (val != 0);
    return e;
}

static void nvs_save_relay(void) {
    nvs_store_set_u8(NVS_RELAY_NS, NVS_RELAY_KEY, s_relay_active ? 1 : 0);
}

static void nvs_load_boot(void) {
    uint8_t val = BOOT_OFF;
    if (nvs_store_get_u8(NVS_RELAY_NS, NVS_BOOT_KEY, &val) == ESP_OK)
        s_boot_mode = val;
}

static void nvs_save_boot(void) {
    nvs_store_set_u8(NVS_RELAY_NS, NVS_BOOT_KEY, s_boot_mode);
}

/**
 * relay_set — Set relay on/off (active-low: on = GPIO3 LOW).
 * Protected by critical section for SMP/interrupt safety.
 * Defers NVS write to main loop via dirty flag.
 * Notifies main task for LED update and bumps state version.
 */
void relay_set(bool on) {
    taskENTER_CRITICAL();
    s_relay_active = on;
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    taskEXIT_CRITICAL();
    s_relay_dirty = true;
    led_update_request();
    notify_bump_state();
}

bool relay_get(void) {
    return s_relay_active;
}

/**
 * relay_nvs_flush — Write pending relay state to NVS.
 * Called from main loop; no NVS I/O in ISR/timer/HTTP context.
 */
void relay_nvs_flush(void) {
    if (!s_relay_dirty) return;
    s_relay_dirty = false;
    nvs_save_relay();
}

/**
 * relay_set_boot_behavior — Set boot mode: OFF (0), ON (1), or AUTO (2).
 * Persisted to NVS immediately; bumps state version.
 */
void relay_set_boot_behavior(uint8_t mode) {
    s_boot_mode = mode;
    nvs_save_boot();
    notify_bump_state();
    ESP_LOGI(TAG, "Boot mode: %d", mode);
}

uint8_t relay_get_boot_behavior(void) {
    return s_boot_mode;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Button Processing (CONFIG_BUTTON_ENABLE)
 * ══════════════════════════════════════════════════════════════════════════ */

#if defined(CONFIG_BUTTON_ENABLE)

static void btn_timer_cb(void *arg) {
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

static void IRAM_ATTR button_isr(void *arg) {
    if (!s_main_task) return;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(s_main_task, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * gpio_process_button — Debounce and classify button press.
 * Called from main loop; driven by ISR on edge + periodic poll timer.
 *
 * Short press (< 5 s): toggles relay via relay_set().
 * Long press (>= 5 s): triggers wifi_reconnect() (clear creds → SmartConfig).
 */
void gpio_process_button(void) {
    int level = gpio_get_level(PIN_BUTTON);
    int64_t now_us = esp_timer_get_time();

    if (level == s_btn_last_level) {
        if (s_btn_pressed && !s_btn_long_triggered) {
            if ((now_us - s_btn_press_time) >= BUTTON_LONG_PRESS_MS * 1000) {
                s_btn_long_triggered = true;
                s_button_action = BTN_RECONNECT;
                led_set_pattern(LED_BLINK_ERROR);
            }
        }
        return;
    }

    if (level == 0 && s_btn_last_level == 1) {
        s_btn_pressed = true;
        s_btn_press_time = now_us;
        s_btn_long_triggered = false;
        esp_timer_stop(s_btn_timer);
        esp_timer_start_periodic(s_btn_timer, BUTTON_POLL_INTERVAL_US);
    } else if (level == 1 && s_btn_last_level == 0) {
        esp_timer_stop(s_btn_timer);
        if (s_btn_pressed && !s_btn_long_triggered) {
            int64_t held_us = now_us - s_btn_press_time;
            if (held_us >= BUTTON_DEBOUNCE_MS * 1000) {
                s_button_action = BTN_TOGGLE;
                led_update_request();
            }
        }
        s_btn_pressed = false;
        s_btn_long_triggered = false;
    }
    s_btn_last_level = level;

    btn_action_t act = s_button_action;
    s_button_action = BTN_NONE;
    switch (act) {
    case BTN_TOGGLE:
        relay_set(!relay_get());
        break;
    case BTN_RECONNECT:
        wifi_reconnect();
        break;
    default:
        break;
    }
}

#else /* !CONFIG_BUTTON_ENABLE */

void gpio_process_button(void) {}

#endif /* CONFIG_BUTTON_ENABLE */

/* ══════════════════════════════════════════════════════════════════════════
 * Init
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * gpio_set_main_task — Register the main task handle for task notifications.
 * Must be called before any ISR/timer that notifies the main loop.
 */
void gpio_set_main_task(TaskHandle_t task) {
    s_main_task = task;
}

/**
 * gpio_init — Initialise GPIO pins, LED timer, button ISR (if enabled),
 * load persisted state from NVS, apply boot behaviour, and run initial
 * led_update().
 *
 * GPIO levels are set BEFORE gpio_config() to avoid glitches on output pins.
 */
void gpio_init(void) {
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

#if defined(CONFIG_BUTTON_ENABLE)
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    gpio_config_t in = {
        .pin_bit_mask = BIT(PIN_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BUTTON, button_isr, (void*)PIN_BUTTON));
    s_btn_last_level = gpio_get_level(PIN_BUTTON);
#endif

    esp_timer_create_args_t ta_led = { .callback = &led_timer_cb, .name = "led" };
    ESP_ERROR_CHECK(esp_timer_create(&ta_led, &s_led_timer));

#if defined(CONFIG_BUTTON_ENABLE)
    esp_timer_create_args_t ta_btn = { .callback = &btn_timer_cb, .name = "btn" };
    ESP_ERROR_CHECK(esp_timer_create(&ta_btn, &s_btn_timer));
#endif

    nvs_load_led();
    nvs_load_boot();

    bool restore = false;
    if (s_boot_mode == BOOT_AUTO) {
        if (nvs_load_relay() == ESP_OK)
            restore = true;
    } else if (s_boot_mode == BOOT_ON) {
        s_relay_active = true;
        restore = true;
    }

    if (restore) {
        gpio_set_level(PIN_RELAY, s_relay_active ? 0 : 1);
        ESP_LOGI(TAG, "Boot: relay %s (mode %d)", s_relay_active ? "ON" : "OFF", s_boot_mode);
    } else {
        s_relay_active = false;
        ESP_LOGI(TAG, "Boot: relay OFF (mode %d)", s_boot_mode);
    }

    ESP_LOGI(TAG, "LED mode: 0x%02x", s_user_mode);
    led_update();
}
