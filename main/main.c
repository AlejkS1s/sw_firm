#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "board.h"
#include "http_server.h"
#include "power.h"
#include "routines.h"
#include "timing.h"
#include "connection.h"
#include "ipc.h"

#define ACTUATOR_STACK 3072
#define ROUTINES_STACK 2560
#define NET_STACK      2560
#define TASK_PRIO       5

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
        e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* Bring up TCP/IP and the default event loop BEFORE wifi_init().
     * Do NOT call tcpip_adapter_set_default_wifi_handlers() here — wifi_init()
     * registers them internally; calling it early only yields cosmetic
     * "handler already registered" warnings. */
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ipc_init();

    board_init();
    timing_init();
    routines_init();
    power_init();

    xTaskCreate(actuator_task, "actuator", ACTUATOR_STACK, NULL,
                TASK_PRIO, &g_actuator_task);
    xTaskCreate(routines_task, "routines", ROUTINES_STACK, NULL,
                TASK_PRIO - 1, &g_routines_task);
    xTaskCreate(net_task,      "net",      NET_STACK,      NULL,
                TASK_PRIO, &g_net_task);

    http_server_start();

    wifi_init();

    /* All runtime work is now performed by the tasks spawned above.
     * Relinquish this (main) task so its stack is reclaimed. */
    vTaskDelete(NULL);
}
