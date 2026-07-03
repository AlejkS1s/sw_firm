#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "gpio.h"

#define PIN_RELAY   GPIO_NUM_0
#define PIN_LED     GPIO_NUM_2
#define PIN_UART_TX GPIO_NUM_1
#define PIN_UART_RX GPIO_NUM_3

#define LED_TIME_SLOW_US  550000
#define LED_TIME_FAST_US  100000
#define LED_TIME_ERR_US   100000
#define LED_TIME_ERR_PAUSE_US 2000000

static bool s_relay_active = false;
static esp_timer_handle_t s_led_timer;
static int s_blink_on_ticks;
static int s_blink_off_ticks;
static int s_blink_counter;

static void led_timer_cb(void *arg) {
    if (--s_blink_counter > 0) return;
    if (gpio_get_level(PIN_LED) == 0) {
        gpio_set_level(PIN_LED, 1);
        s_blink_counter = s_blink_off_ticks;
    } else {
        gpio_set_level(PIN_LED, 0);
        s_blink_counter = s_blink_on_ticks;
    }
}

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
    s_relay_active = false;

    gpio_config_t in = {
        .pin_bit_mask = BIT(PIN_UART_RX),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    esp_timer_create_args_t ta = { .callback = &led_timer_cb, .name = "led" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_led_timer));
}

void relay_set(bool on) {
    taskENTER_CRITICAL();
    s_relay_active = on;
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    taskEXIT_CRITICAL();
}

bool relay_get(void) {
    return s_relay_active;
}

void led_set(bool on) {
    led_set_pattern(on ? LED_ON : LED_OFF);
}

bool led_get(void) {
    return gpio_get_level(PIN_LED) == 0;
}

void led_set_pattern(led_pattern_t pat) {
    esp_timer_stop(s_led_timer);
    gpio_set_level(PIN_LED, 1);

    switch (pat) {
    case LED_OFF:
        return;
    case LED_ON:
        gpio_set_level(PIN_LED, 0);
        return;
    case LED_BLINK_SLOW:
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = 1;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_SLOW_US);
        return;
    case LED_BLINK_FAST:
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = 1;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_FAST_US);
        return;
    case LED_BLINK_ERROR:
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = LED_TIME_ERR_PAUSE_US / LED_TIME_ERR_US;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_ERR_US);
        return;
    default:
        return;
    }
}
