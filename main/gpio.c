#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "gpio.h"

#define PIN_RELAY GPIO_NUM_0
#define PIN_LED   GPIO_NUM_2

static bool s_relay_active = false;
static bool s_led_active   = false;

void gpio_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = BIT(PIN_RELAY) | BIT(PIN_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    relay_set(false);
    led_set(false);
}

void relay_set(bool on) {
    taskENTER_CRITICAL();
    s_relay_active = on;
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    taskEXIT_CRITICAL();
}

bool relay_get(void) {
    bool v;
    taskENTER_CRITICAL();
    v = s_relay_active;
    taskEXIT_CRITICAL();
    return v;
}

void led_set(bool on) {
    taskENTER_CRITICAL();
    s_led_active = on;
    gpio_set_level(PIN_LED, on ? 0 : 1);
    taskEXIT_CRITICAL();
}

bool led_get(void) {
    bool v;
    taskENTER_CRITICAL();
    v = s_led_active;
    taskEXIT_CRITICAL();
    return v;
}
