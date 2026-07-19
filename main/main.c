#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "board.h"
#include "countdown.h"
#include "diagnostics.h"
#include "http_server.h"
#include "power.h"
#include "routines.h"
#include "sse.h"
#include "state.h"
#include "timing.h"
#include "connection.h"

/* Task handles and IPC primitives are owned by each module:
 *   - board.c      — g_actuator_task, g_actuator_q, g_relay_mutex
 *   - routines.c   — g_routines_task, g_routines_mutex
 *   - connection.c — g_net_evt
 *   - state.c      — g_state_evt
 * No central ipc.c/ipc.h exists. */

void app_main(void) {
    /* ── Phase 1: Core infrastructure (synchronous, blocking) ──────── */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
        e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* Set the base MAC address once from EFUSE so the system layer stops
     * logging "Base MAC address is not set" on every esp_base_mac_addr_get()
     * call (WiFi init, tcpip, etc.). This is purely cosmetic — the same
     * MAC is read from EFUSE by default, we just cache it up front. */
    {
        uint8_t base_mac[6];
        if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
            esp_base_mac_addr_set(base_mac);
        }
    }

    tcpip_adapter_init();
    tcpip_adapter_set_default_wifi_handlers();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    connection_init();  /* g_net_evt */
    state_init();       /* g_state_evt */
    diagnostics_init(); /* boot counter */

    /* ── Phase 2: Hardware + local state restoration ──────────────── */
    board_init();       /* GPIO, NVS restore, relay mutex; spawns actuator_task */
    timing_init();      /* timezone, NVS epoch seed */
    countdown_init();   /* countdown state + 1s tick timer */
    routines_init();    /* routine table load; spawns routines_task */
    power_init();       /* idle timeout from Kconfig */
    sse_init();
    /* ── Phase 3: System services ──────────────────────────────────── */
    ESP_ERROR_CHECK(esp_task_wdt_init());

    http_server_start();

    /* ── Phase 4: Network bringup (async — connects in background) ── */
    wifi_init();        /* initializes WiFi driver; returns immediately */

    /* All runtime work is now performed by the spawned tasks.
     * Relinquish this (main) task so its stack is reclaimed. */
    vTaskDelete(NULL);
}
