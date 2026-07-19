#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_store.h"

#include "lwip/apps/sntp.h"
#include "lwip/ip_addr.h"

#include "timing.h"
#include "routines.h"
#include "state.h"

#define TAG "timing"
#define MIN_VALID_EPOCH 1000000000
#define NTP_SYNC_INTERVAL_MS 3600000
#define NTP_HEALTH_CHECK_US 10000000ULL
#define NTP_HEALTH_MAX_CHECKS 6

/* ── NTP server (bypasses DNS — use raw IP) ─── */
#define NTP_SERVER_IP "216.239.35.4"

/* NVS keys live in nvs_store.h */

/* ════════════════════════════════════════════════ *
 *  Static state                                   *
 * ════════════════════════════════════════════════ */

/* s_ntp_synced is written from the lwIP SNTP callback and the health-check
 * esp_timer callback, but read from any task (via timing_time_ok()). Must be
 * volatile so the compiler never caches a stale value across task switches. */
static volatile bool s_ntp_synced = false;
static int s_ntp_health_count = 0;
static esp_timer_handle_t s_health_timer;
static char s_tz[TZ_MAX_LEN];

/* ════════════════════════════════════════════════ *
 *  NTP health helpers (static)                    *
 * ════════════════════════════════════════════════ */

/* ── ntp_health_cb ────────────────────── */
/* Periodic health check for NTP sync. Called by the health esp_timer.
 * Monitors sntp_get_sync_status() and restarts NTP if no sync after 60s.
 * On success, saves epoch and notifies via timing_on_ntp_synced(). */
static void ntp_health_cb(void *arg) {
    if (s_ntp_synced) return;

    sntp_sync_status_t st = sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_COMPLETED) {
        s_ntp_synced = true;
        ESP_LOGI(TAG, "NTP sync completed");
        timing_save();
        timing_on_ntp_synced();
        return;
    }

    s_ntp_health_count++;
    if (s_ntp_health_count % 6 == 0) {
        time_t t = time(NULL);
        ESP_LOGW(TAG, "NTP diag: sync_status=%d time=%ld count=%d",
                 (int)st, (long)t, s_ntp_health_count);
    }
    if (s_ntp_health_count >= NTP_HEALTH_MAX_CHECKS) {
        ESP_LOGW(TAG, "NTP no sync after 60s, restarting");
        sntp_stop();
        s_ntp_health_count = 0;
        sntp_init();
    }
}

/* ════════════════════════════════════════════════ *
 *  Public API                                     *
 * ════════════════════════════════════════════════ */

/* ── timing_init ──────────────────────── */
/* Initialises the timing subsystem:
 * 1. Sets timezone
 * 2. Seeds the clock from last NVS-saved epoch (if available)
 * Called once at boot from main(). */
void timing_init(void) {
    size_t tz_len = sizeof(s_tz);
    if (nvs_store_get_blob(NVS_NS_TIME, NVS_KEY_TIME_TZ, s_tz, &tz_len) == ESP_OK && tz_len > 1) {
        setenv("TZ", s_tz, 1);
    } else {
        s_tz[0] = '\0';
    }
    tzset();

    time_t saved_epoch = 0;
    if (nvs_store_get_u32(NVS_NS_TIME, NVS_KEY_TIME_EPOCH, (uint32_t*)&saved_epoch) != ESP_OK ||
        saved_epoch < MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "No valid saved epoch, using uptime");
    } else {
        struct timeval tv = { .tv_sec = saved_epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Boot: seeded clock with last saved epoch=%ld (stale until NTP syncs)",
                 (long)saved_epoch);
    }
}

/* ── timing_save ──────────────────────── */
/* Persists the current system time to NVS.
 * Called on successful NTP sync to preserve the epoch across reboots. */
void timing_save(void) {
    time_t now = time(NULL);
    if (now < MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "Not saving time: system clock not set");
        return;
    }
    nvs_store_set_u32(NVS_NS_TIME, NVS_KEY_TIME_EPOCH, (uint32_t)now);
    ESP_LOGI(TAG, "Saved epoch=%ld", (long)now);
}

/* ── timing_on_ntp_synced ─────────────── */
/* Called when NTP sync completes (directly or via health check).
 * Notifies the routines task to re-arm its event-scheduled timers. */
void timing_on_ntp_synced(void) {
    ESP_LOGI(TAG, "Time synced");
    routines_wake();
    /* time_ok flips false -> true here and feeds the state hash; without
     * this, SSE subscribers and /state's ETag wouldn't reflect the sync
     * until some unrelated event bumped state. */
    notify_bump_state();
}

/* ── timing_ntp_start ─────────────────── */
/* Initialises lwIP SNTP with hardcoded time.google.com IP (bypasses
 * DNS which fails with IP strings). Registers timing_ntp_sync_cb()
 * as the sync notification callback.  Safe to call repeatedly —
 * stops before re-initialising. */
void timing_ntp_start(void) {
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    ip_addr_t server;
    ipaddr_aton(NTP_SERVER_IP, &server);
    sntp_setserver(0, &server);
    sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    sntp_set_time_sync_notification_cb(timing_ntp_sync_cb);
    sntp_init();
}

/* ── timing_ntp_health_start ──────────── */
/* Creates and starts the periodic NTP health-check timer.
 * Resets health tracking counters for a fresh connection cycle.
 * Called from main when WiFi connects and NTP is started. */
void timing_ntp_health_start(void) {
    s_ntp_health_count = 0;
    s_ntp_synced = false;

    if (!s_health_timer) {
        esp_timer_create_args_t ta = {
            .callback = &ntp_health_cb,
            .name = "ntp_health"
        };
        esp_timer_create(&ta, &s_health_timer);
    }
    esp_timer_start_periodic(s_health_timer, NTP_HEALTH_CHECK_US);
}

/* ── timing_ntp_sync_cb ───────────────── */
/* SNTP notification callback, registered via
 * sntp_set_time_sync_notification_cb(). Called by lwIP SNTP when
 * an NTP response is received and the clock is updated. Persists
 * the synced epoch and notifies via timing_on_ntp_synced(). */
void timing_ntp_sync_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "NTP sync callback: time=%ld", (long)tv->tv_sec);
    s_ntp_synced = true;
    timing_save();
    timing_on_ntp_synced();
}

/* ── timing_time_ok ───────────────────── */
/* Returns true if NTP has completed at least one successful sync.
 * Used by routines.c and other modules to check time validity. */
bool timing_time_ok(void) {
    return s_ntp_synced;
}

const char *timing_get_timezone(void) {
    return s_tz;
}

esp_err_t timing_set_timezone(const char *tz) {
    if (!tz || *tz == '\0') return ESP_ERR_INVALID_ARG;
    size_t len = strlen(tz);
    if (len >= TZ_MAX_LEN) return ESP_ERR_INVALID_ARG;
    memcpy(s_tz, tz, len + 1);
    setenv("TZ", s_tz, 1);
    tzset();
    esp_err_t e = nvs_store_set_blob(NVS_NS_TIME, NVS_KEY_TIME_TZ, s_tz, len + 1);
    if (e == ESP_OK) notify_bump_state();
    return e;
}