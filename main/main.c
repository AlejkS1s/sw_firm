#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "gpio.h"
#include "http_server.h"
#include "timer_sched.h"
#include "wifi.h"

static const char* TAG = "main";

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
        e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    gpio_init();

    timer_sched_init();

    gpio_set_main_task(xTaskGetCurrentTaskHandle());

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    http_server_start();

    wifi_init();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        led_update();
    }
}
