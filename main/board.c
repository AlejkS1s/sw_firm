#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_store.h"

#include "board.h"
#include "ipc.h"
#include "power.h"
#include "timing.h"
#include "connection.h"

#define TAG "board"

/* ── Pin and timing constants ──────────────────────────────────────────── */
#define PIN_RELAY            GPIO_NUM_3
#define PIN_LED              GPIO_NUM_2
#define PIN_UART_TX          GPIO_NUM_1
#define PIN_BUTTON           GPIO_NUM_0

#define LED_TIME_SLOW_US       550000
#define LED_TIME_FAST_US       100000
#define LED_TIME_ERR_US        95000
#define LED_TIME_ERR_PAUSE_US  2000000

/* ── NVS key constants ──────────────────────────────────────────────────── */
#define NVS_RELAY_NS            "relay"
#define NVS_RELAY_KEY           "state"
#define NVS_RELAY_BOOT_KEY      "boot"
#define NVS_LED_NS              "led"
#define NVS_LED_KEY             "mode"

/* ── Button constants (CONFIG_BUTTON_ENABLE) ────────────────────────────── */
#if defined(CONFIG_BUTTON_ENABLE)
#define BTN_POLL_INTERVAL_US    10000
#define BTN_DEBOUNCE_SAMPLES    5
#define BTN_LONG_PRESS_MS       5000
#endif

/* ── Static state ───────────────────────────────────────────────────────── */
static bool               s_relay_active = false;
static uint8_t            s_boot_mode    = RELAY_BOOT_OFF;

/* LED — user bitmask + override state */
static uint8_t            s_led_mode  = LED_MODE_RELAY;
static bool               s_override  = false;     /* pattern active, bitmask bypassed */
static esp_timer_handle_t s_led_timer;
static int                s_blink_counter;
static int                s_blink_on_ticks;
static int                s_blink_off_ticks;
static int                s_blink_phase;           /* remaining repeats (0 = infinite) */

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
    if (nvs_store_get_u8(NVS_LED_NS, NVS_LED_KEY, &val) == ESP_OK)
        s_led_mode = val;
}

static void nvs_save_led(void) {
    nvs_store_set_u8(NVS_LED_NS, NVS_LED_KEY, s_led_mode);
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
 * Called from the actuator task only — s_led_mode and s_relay_active are
 * private to this task.  Delegates routine-activity queries to routines.c
 * (routine_is_active), which holds g_routines_mutex internally. */
static bool led_eval(void) {
    uint8_t mode = s_led_mode;
    if ((mode & LED_MODE_RELAY) && s_relay_active) return true;
    if ((mode & LED_MODE_WIFI) &&
        (xEventGroupGetBits(g_net_evt) & WIFI_CONNECTED_BIT)) return true;
    if ((mode & LED_MODE_TIMER)     && routine_is_active(RT_COUNTDOWN))  return true;
    if ((mode & LED_MODE_SCHEDULE)  && routine_is_active(RT_SCHEDULE))   return true;
    if ((mode & LED_MODE_CIRCULATE) && routine_is_active(RT_CIRCULATE))  return true;
    if ((mode & LED_MODE_ICHING)    && routine_is_active(RT_ICHING))     return true;
    if ((mode & LED_MODE_ROUTINE) &&
        (routine_is_active(RT_COUNTDOWN) ||
         routine_is_active(RT_SCHEDULE)  ||
         routine_is_active(RT_CIRCULATE) ||
         routine_is_active(RT_ICHING))) return true;
    return false;
}

/* Recompute LED state from the user bitmask. No-op while an override pattern
 * is active. Called from the actuator task only. */
void led_update(void) {
    if (s_override) return;
    if (led_eval()) led_hw_on(); else led_hw_off();
}

/* Apply an override pattern (called from actuator task only). */
static void led_apply(led_conf_t pat) {
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
        led_blink_start(LED_TIME_FAST_US, 1, 3);
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

/* Request an override pattern from outside the actuator task.
 * Routes CMD_SET_LED_PATTERN to the actuator queue. */
void led_set_pattern(led_conf_t pat) {
    actuator_msg_t m = { .type = CMD_SET_LED_PATTERN, .led_pattern = pat, .ack = NULL };
    xQueueSend(g_actuator_q, &m, 0);
}

bool led_get(void) {
    return gpio_get_level(PIN_LED) == 0;
}

void led_set_mode(uint8_t bitmask) {
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    s_led_mode = bitmask;
    xSemaphoreGive(g_relay_mutex);
    nvs_save_led();
    ESP_LOGI(TAG, "LED mode: 0x%02x", bitmask);
}

uint8_t led_get_mode(void) {
    uint8_t v;
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    v = s_led_mode;
    xSemaphoreGive(g_relay_mutex);
    return v;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Relay Control
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

static void nvs_load_boot_mode(void) {
    uint8_t val = RELAY_BOOT_OFF;
    if (nvs_store_get_u8(NVS_RELAY_NS, NVS_RELAY_BOOT_KEY, &val) == ESP_OK)
        s_boot_mode = val;
}

static void nvs_save_boot_mode(void) {
    nvs_store_set_u8(NVS_RELAY_NS, NVS_RELAY_BOOT_KEY, s_boot_mode);
}

/* Set relay on/off (active-low: on = GPIO3 LOW). Mutex-guarded; persists to
 * NVS and bumps the state version. Called only by the actuator task. */
void relay_set(bool on) {
    bool was = s_relay_active;
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    s_relay_active = on;
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    xSemaphoreGive(g_relay_mutex);

    nvs_save_relay();
    ESP_LOGI(TAG, "Relay set: %s (was=%s)", on ? "ON" : "OFF", was ? "ON" : "OFF");
}

bool relay_get(void) {
    bool v;
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    v = s_relay_active;
    xSemaphoreGive(g_relay_mutex);
    return v;
}

void relay_set_boot_behavior(uint8_t mode) {
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    s_boot_mode = mode;
    xSemaphoreGive(g_relay_mutex);
    nvs_save_boot_mode();
    ESP_LOGI(TAG, "Boot mode: %d", mode);
}

uint8_t relay_get_boot_behavior(void) {
    uint8_t v;
    xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
    v = s_boot_mode;
    xSemaphoreGive(g_relay_mutex);
    return v;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Button Processing (CONFIG_BUTTON_ENABLE)
 * ══════════════════════════════════════════════════════════════════════════ */

#if defined(CONFIG_BUTTON_ENABLE)

static void handle_button_event(btn_event_t evt) {
    switch (evt) {
    case BTN_SHORT_PRESS:
        ESP_LOGI(TAG, "Short press -> toggle relay");
        { actuator_msg_t m = { .type = CMD_TOGGLE, .ack = NULL };
          xQueueSend(g_actuator_q, &m, 0); }
        break;
    case BTN_LONG_PRESS:
        ESP_LOGI(TAG, "Long press -> WiFi reconnect");
        net_reconnect_request();
        break;
    default:
        break;
    }
}

/* Periodic debounce sampler. Runs in an esp_timer task (not ISR), so posting
 * to a queue / calling net_reconnect_request() is safe. */
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
                   (now - s_btn_press_time) >= (int64_t)BTN_LONG_PRESS_MS * 1000) {
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
 * Actuator task — owns relay, LED, boot mode, tz, and the power FSM
 * ══════════════════════════════════════════════════════════════════════════ */

static void actuator_apply(actuator_msg_t *m) {
    switch (m->type) {
    case CMD_TURN_ON:           relay_set(true);                 break;
    case CMD_TURN_OFF:          relay_set(false);                break;
    case CMD_TOGGLE:            relay_set(!relay_get());         break;
    case CMD_SET_LED:           led_set_mode(m->led_mode);       break;
    case CMD_SET_LED_PATTERN:   led_apply(m->led_pattern); break;
    case CMD_SET_BOOT:          relay_set_boot_behavior(m->boot_mode); break;
    case CMD_SET_TZ:
        m->result = timing_set_timezone(m->tz);
        break;
    case CMD_LED_UPDATE:
        led_update();
        break;
    default: break;
    }
}

void actuator_task(void *arg) {
    actuator_msg_t m;
    const TickType_t q_wait = pdMS_TO_TICKS(1000);

    for (;;) {
        if (xQueueReceive(g_actuator_q, &m, q_wait)) {
            power_notify_activity();
            actuator_apply(&m);
            if (m.ack) xSemaphoreGive(m.ack);
        }
        led_update();
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Init
 * ══════════════════════════════════════════════════════════════════════════ */

void board_init(void) {
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
    esp_timer_create_args_t ta_btn = { .callback = &btn_timer_cb, .name = "btn" };
    ESP_ERROR_CHECK(esp_timer_create(&ta_btn, &s_btn_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_btn_timer, BTN_POLL_INTERVAL_US));
    ESP_LOGI(TAG, "Button timer started - polling GPIO%d every %dms",
             PIN_BUTTON, BTN_POLL_INTERVAL_US / 1000);
#endif

    nvs_load_led();
    nvs_load_boot_mode();

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
        ESP_LOGI(TAG, "relay recovery %s (mode %d)", s_relay_active ? "ON" : "OFF", s_boot_mode);
    } else {
        s_relay_active = false;
        ESP_LOGI(TAG, "relay recovery OFF (mode %d)", s_boot_mode);
    }

    ESP_LOGI(TAG, "LED mode 0x%02x", s_led_mode);
}
