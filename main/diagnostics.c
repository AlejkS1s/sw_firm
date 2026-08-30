#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_store.h"
#include "diagnostics.h"
#include "power.h"
#include "sse.h"
#include "clients.h"
#include "state.h"
#include "timing.h"

static uint32_t s_boot_count = 0;

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
    unsigned long uptime = (unsigned long)(esp_timer_get_time() / USEC_PER_SEC);
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    int n = snprintf(buf, buflen,
        "{"
        "\"rreason\":\"%s\","
        "\"boots\":%lu,"
        "\"uptm\":%lu,"
        "\"free\":%lu,"
        "\"minfree\":%lu,"
        "\"cores\":%d,"
        "\"rev\":%d,"
        "\"tsync\":%s,"
        "\"pwsdis\":%s,"
        "\"clients\":%u"
        "}",
        diagnostics_reset_reason_str(esp_reset_reason()),
        (unsigned long)s_boot_count,
        uptime,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        chip.cores, chip.revision,
        timing_time_ok() ? "true" : "false",
        power_save_is_disabled() ? "true" : "false",
        client_count_active());

    return snprintf_guard(n, buflen);
}
