#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ══════════════════════════════════════════════════════════════════════════
 * NVS namespace & key constants — single source of truth.
 *
 * Each module used to define its own local copies of these strings.  A typo
 * in a namespace string silently creates a second NVS partition and loses
 * data.  All NVS consumers now get their keys from here.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Namespaces ───────────────────────────────────────────────────────── */
#define NVS_NS_RELAY       "relay"
#define NVS_NS_LED         "led"
#define NVS_NS_WIFI_CREDS  "wifi_creds"
#define NVS_NS_ROUTINES    "routines"
#define NVS_NS_COUNTDOWN   "countdown"
#define NVS_NS_TIME        "time"
#define NVS_NS_DIAG        "diag"
#define NVS_NS_POWER       "power"
#define NVS_NS_SSE         "sse"

/* ── Keys — relay namespace ───────────────────────────────────────────── */
#define NVS_KEY_RELAY_STATE    "state"
#define NVS_KEY_RELAY_BOOT     "boot"
#define NVS_KEY_AOFF           "aoff"

/* ── Keys — LED namespace ─────────────────────────────────────────────── */
#define NVS_KEY_LED_MODE       "mode"

/* ── Keys — wifi_creds namespace ──────────────────────────────────────── */
#define NVS_KEY_WIFI_SSID      "ssid"
#define NVS_KEY_WIFI_PASSWORD  "password"

/* ── Keys — routines namespace ────────────────────────────────────────── */
#define NVS_KEY_ROUTINE_SLOTS  "slots"
#define NVS_KEY_ROUTINE_COUNT  "count"

/* ── Keys — countdown namespace ───────────────────────────────────────── */
#define NVS_KEY_COUNTDOWN_STATE "state"

/* ── Keys — time namespace ────────────────────────────────────────────── */
#define NVS_KEY_TIME_EPOCH     "epoch"
#define NVS_KEY_TIME_TZ        "tz"

/* ── Keys — diag namespace ────────────────────────────────────────────── */
#define NVS_KEY_DIAG_BOOTS     "boots"

/* ── Keys — power namespace ───────────────────────────────────────────── */
#define NVS_KEY_POWER_SAVE     "ps_off"

/* ── Keys — sse namespace ────────────────────────────────────────────── */
#define NVS_KEY_SSE_ENABLED    "enabled"

/* ══════════════════════════════════════════════════════════════════════════ */

esp_err_t nvs_store_get_u8(const char *ns, const char *key, uint8_t *out);
esp_err_t nvs_store_set_u8(const char *ns, const char *key, uint8_t val);
esp_err_t nvs_store_get_u32(const char *ns, const char *key, uint32_t *out);
esp_err_t nvs_store_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t nvs_store_get_blob(const char *ns, const char *key, void *out, size_t *sz);
esp_err_t nvs_store_set_blob(const char *ns, const char *key, const void *val, size_t sz);
esp_err_t nvs_store_erase(const char *ns, const char *key);
esp_err_t nvs_store_erase_all(const char *ns);
