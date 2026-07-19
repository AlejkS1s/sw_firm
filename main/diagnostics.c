#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_store.h"
#include "diagnostics.h"
#include "power.h"
#include "sse.h"
#include "state.h"
#include "timing.h"

static uint32_t s_boot_count = 0;

/* Names match esp_reset_reason_t exactly (esp_system.h) — verified against
 * ESP8266_RTOS_SDK's esp_system.h, which does carry esp_reset_reason() /
 * ESP_RST_* despite the ESP8266 predating ESP-IDF's esp_reset_reason API
 * on other targets. ESP_RST_EXT and ESP_RST_SDIO are enumerated but the
 * SDK's own docs note they aren't reachable on this chip; kept here only
 * so the switch is exhaustive and -Wswitch stays quiet. */
const char *diagnostics_reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_UNKNOWN:
        default:                return "unknown";
    }
}

void diagnostics_init(void) {
    nvs_store_get_u32(NVS_NS_DIAG, NVS_KEY_DIAG_BOOTS, &s_boot_count);
    s_boot_count++;
    nvs_store_set_u32(NVS_NS_DIAG, NVS_KEY_DIAG_BOOTS, s_boot_count);
}

unsigned long diagnostics_boot_count(void) {
    return (unsigned long)s_boot_count;
}

size_t diagnostics_build_json(char *buf, size_t buflen) {
    esp_reset_reason_t rr = esp_reset_reason();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    unsigned long uptime = (unsigned long)(esp_timer_get_time() / USEC_PER_SEC);

    int rssi = -1;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;

    int n = snprintf(buf, buflen,
        "{"
        "\"reset_reason\":\"%s\","
        "\"boot_count\":%lu,"
        "\"uptime\":%lu,"
        "\"heap_free\":%lu,"
        "\"heap_min_free\":%lu,"
        "\"chip_cores\":%d,"
        "\"chip_revision\":%d,"
        "\"wifi_rssi\":%d,"
        "\"time_synced\":%s,"
        "\"power_save_disabled\":%s,"
        "\"connected_clients\":%u,"
        "\"fw_ver\":\"" FW_VER "\","
        "\"fw_build\":\"" FW_BUILD "\""
        "}",
        diagnostics_reset_reason_str(rr),
        (unsigned long)s_boot_count,
        uptime,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        chip.cores, chip.revision,
        rssi,
        timing_time_ok() ? "true" : "false",
        power_save_is_disabled() ? "true" : "false",
        sse_client_count());

    return snprintf_guard(n, buflen);
}
