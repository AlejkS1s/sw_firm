#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "gpio.h"
#include "timer_sched.h"

#define TAG "gpio"

#define PIN_RELAY   GPIO_NUM_0
#define PIN_LED     GPIO_NUM_2
#define PIN_UART_TX GPIO_NUM_1
#define PIN_UART_RX GPIO_NUM_3

#define LED_TIME_SLOW_US  550000
#define LED_TIME_FAST_US  100000
#define LED_TIME_ERR_US   100000
#define LED_TIME_ERR_PAUSE_US 2000000

#define NVS_RELAY_NS  "relay"
#define NVS_RELAY_KEY "state"
#define NVS_BOOT_KEY  "boot"
#define NVS_LED_NS    "led"
#define NVS_LED_KEY   "mode"

static bool s_relay_active = false;
static uint8_t s_boot_mode = BOOT_OFF;
static uint8_t s_led_mode = LED_MODE_RELAY;
static bool s_wifi_connected = false;
static bool s_led_override = false;
static TaskHandle_t s_main_task = NULL;

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

static esp_err_t nvs_load_relay(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_RELAY_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    uint8_t val = 0;
    e = nvs_get_u8(h, NVS_RELAY_KEY, &val);
    if (e == ESP_OK) s_relay_active = (val != 0);
    nvs_close(h);
    return e;
}

static void nvs_save_relay(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_RELAY_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_RELAY_KEY, s_relay_active ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_boot(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_RELAY_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t val = BOOT_OFF;
    if (nvs_get_u8(h, NVS_BOOT_KEY, &val) == ESP_OK)
        s_boot_mode = val;
    nvs_close(h);
}

static void nvs_save_boot(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_RELAY_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_BOOT_KEY, s_boot_mode);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_led(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_LED_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t val = LED_MODE_RELAY;
    if (nvs_get_u8(h, NVS_LED_KEY, &val) == ESP_OK)
        s_led_mode = val;
    nvs_close(h);
}

static void nvs_save_led(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_LED_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_LED_KEY, s_led_mode);
    nvs_commit(h);
    nvs_close(h);
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

    ESP_LOGI(TAG, "LED mode: 0x%02x", s_led_mode);
}

void relay_set(bool on) {
    taskENTER_CRITICAL();
    s_relay_active = on;
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    taskEXIT_CRITICAL();
    nvs_save_relay();
    led_update_request();
}

bool relay_get(void) {
    return s_relay_active;
}

void relay_set_boot_behavior(uint8_t mode) {
    s_boot_mode = mode;
    nvs_save_boot();
    ESP_LOGI(TAG, "Boot mode: %d", mode);
}

uint8_t relay_get_boot_behavior(void) {
    return s_boot_mode;
}

void led_set(bool on) {
    led_set_pattern(on ? LED_ON : LED_OFF);
}

bool led_get(void) {
    return gpio_get_level(PIN_LED) == 0;
}

void led_set_pattern(led_pattern_t pat) {
    s_led_override = (pat != LED_OFF);
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

void led_set_mode(uint8_t bitmask) {
    s_led_mode = bitmask;
    nvs_save_led();
    ESP_LOGI(TAG, "LED mode: 0x%02x", bitmask);
    led_update_request();
}

uint8_t led_get_mode(void) {
    return s_led_mode;
}

void wifi_set_connected(bool connected) {
    s_wifi_connected = connected;
    if (connected) {
        s_led_override = false;
    }
    led_update_request();
}

void gpio_set_main_task(TaskHandle_t task) {
    s_main_task = task;
}

void led_update_request(void) {
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

void led_update(void) {
    if (s_led_override) return;

    esp_timer_stop(s_led_timer);

    if (!s_wifi_connected) {
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = 1;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_FAST_US);
        return;
    }

    if ((s_led_mode & LED_MODE_TIMER) && timer_sched_is_active()) {
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = 1;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_SLOW_US);
        return;
    }

    if ((s_led_mode & LED_MODE_SCHEDULE) && sched_is_any_enabled()) {
        s_blink_on_ticks  = 1;
        s_blink_off_ticks = 1;
        s_blink_counter   = 1;
        gpio_set_level(PIN_LED, 0);
        esp_timer_start_periodic(s_led_timer, LED_TIME_SLOW_US);
        return;
    }

    if ((s_led_mode & LED_MODE_WIFI) && s_wifi_connected) {
        gpio_set_level(PIN_LED, 0);
        return;
    }

    if ((s_led_mode & LED_MODE_RELAY) && s_relay_active) {
        gpio_set_level(PIN_LED, 0);
        return;
    }

    gpio_set_level(PIN_LED, 1);
}
