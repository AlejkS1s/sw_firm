#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>

#include "driver/uart.h"
#include "esp_log.h"

#include "protocol.h"
#include "uart.h"

#define TAG "uart"

#define PORT UART_NUM_0
#define BUF_SIZE 256
#define QUEUE_DEPTH 10
#define STACK 1536
#define PRIO  5

static QueueHandle_t s_queue;

static void event_task(void* arg) {
    uart_event_t ev;
    uint8_t* buf = (uint8_t*)malloc(BUF_SIZE);

    while (xQueueReceive(s_queue, &ev, portMAX_DELAY) == pdTRUE) {
        if (ev.type == UART_DATA) {
            int len = uart_read_bytes(PORT, buf, ev.size, portMAX_DELAY);
            if (len > 0) protocol_process(buf, len);
        } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "Overrun, flushing");
            uart_flush_input(PORT);
            xQueueReset(s_queue);
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

void uart_init(void) {
    uart_config_t cfg = {
        .baud_rate  = 74880,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(PORT, BUF_SIZE, BUF_SIZE,
                                        QUEUE_DEPTH, &s_queue, 0));
    xTaskCreate(event_task, "uart_ev", STACK, NULL, PRIO, NULL);
}
