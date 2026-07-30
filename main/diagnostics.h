#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_system.h"

/* Firmware version — single source of truth for the whole firmware.
 * FW_BUILD is set at compile time from __DATE__ and __TIME__. */
#define FW_VER    "2.1.0"
#define FW_BUILD  __DATE__ " " __TIME__

/* Increments and persists the boot counter. Call once at startup, after
 * NVS is initialized. */
void diagnostics_init(void);

/* Human-readable reset reason string (maps esp_reset_reason_t enum). */
const char *diagnostics_reset_reason_str(esp_reset_reason_t r);

/* Current boot count (persisted in NVS across reboots). */
unsigned long diagnostics_boot_count(void);

/* Serializes system diagnostics — reset reason, boot count, uptime,
 * heap watermarks, chip info, time sync status, power save state, and
 * connected client count — as a JSON object into buf. Field names are
 * intentionally kept short ("rreason", "boots", "uptm", etc.) to match
 * the /api/v1/system response so get_system() can directly inject other
 * fields (id, mac, ssid, ip, tz) before calling diagnostics_build_json().
 *
 * Returns the number of bytes written (excluding the NUL terminator), or
 * 0 if the buffer was too small. */
size_t diagnostics_build_json(char *buf, size_t buflen);
