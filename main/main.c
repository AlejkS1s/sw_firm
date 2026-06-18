#include "esp_log.h"
#include "gpio.h"
#include "tcp.h"
#include "uart.h"
#include "wifi.h"

static const char* TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "Starting...");
    gpio_init();
    uart_init();
    wifi_init();
    tcp_server_start();
    ESP_LOGI(TAG, "Ready");
}
