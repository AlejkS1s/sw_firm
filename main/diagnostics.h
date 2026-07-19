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

/* Serializes system-level diagnostics — reset reason, boot count, uptime,
 * heap watermarks, chip info, WiFi RSSI, time sync status, and power save
 * state — as a JSON object into buf. This is deliberately a separate
 * resource from state_build_json(): state.c reports the device's CONTROL
 * state (relay, timers, routines...), this reports the device's HEALTH
 * (why did it last reboot, how close is it to running out of RAM, what's
 * the connection quality). Different audiences, different update cadence,
 * no reason to force them into one payload.
 *
 * Returns the number of bytes written (excluding the NUL terminator), or
 * 0 if the buffer was too small. */
size_t diagnostics_build_json(char *buf, size_t buflen);

