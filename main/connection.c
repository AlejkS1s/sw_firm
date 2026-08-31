/*
 * connection.c — WiFi station + SmartConfig provisioning
 *
 * State machine:
 *   SC_LISTEN  -> SmartConfig listening for phone app
 *   SC_VERIFY  -> received creds, testing connection
 *   SAVED      -> have saved NVS creds, connecting
 *   CONNECTED  -> IP obtained, online
 *
 * Retry policy:
 *   SC_VERIFY:    MAX_WIFI_RETRY_VERIFY (1) attempts, then sc_restart().
 *   SAVED:        exponential backoff 1s/2s/4s/…/32s cap, indefinite.
 *   CONNECTED:    falls back to SAVED on disconnect.
 *   Auth failure: reason 202 is often transient (the AP may still hold the
 *                 stale association from before a quick reset and reject the
 *                 first re-auth). It follows the normal retry path; only
 *                 after MAX_AUTH_FAIL_WIPE consecutive 202s are the creds
 *                 cleared and SmartConfig started.
 *   SC_LISTEN:    if saved creds exist and nobody provisions within
 *                 SC_RESCUE_TIMEOUT_US, the rescue timer falls back to the
 *                 saved-credentials path — a transient failure can never
 *                 strand the device in provisioning mode.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "tcpip_adapter.h"
#include "nvs_store.h"
#include "connection.h"
#include "board.h"
#include "timing.h"
#include "device_id.h"

#define TAG "wifi"

/* ── Timeout & retry constants ─────────────────────────────────────────── */

#define MAX_WIFI_RETRY_VERIFY 5
#define WIFI_BACKOFF_MAX_SHIFT 6

/* Consecutive AUTH_FAIL (202) disconnects before the credentials are
 * declared bad and SmartConfig starts. A single 202 right after a reset is
 * usually the AP rejecting a re-auth while it still holds the stale
 * association from the previous session — retrying succeeds. */
#define MAX_AUTH_FAIL_WIPE 3
/* If SmartConfig is listening and saved creds exist, fall back to them after
 * this long with no provisioning app. Prevents a transient failure from
 * stranding the device in SC_LISTEN until a human power-cycles it. */
#define SC_RESCUE_TIMEOUT_US 60000000ULL

#define WIFI_ASSOC_TIMEOUT_US 15000000ULL
#define SC_ACK_GRACE_TIMEOUT_US 15000000ULL
#define SC_SETTLE_US     50000
#define SC_VERIFY_US     12000000
#define SC_RETRY_US      1000000
#define BACKOFF_BASE_MS 1000
#define WIFI_WARMUP_ATTEMPTS 3

/* Radio stabilization delay before the first connect after esp_wifi_start().
 * The PHY needs a brief settle after the driver comes up; connecting
 * immediately can fail on marginal hardware. On a cold boot with a cheap
 * AC-DC supply (HLK-PM01 + LDO), the rail is still sagging from TX bursts
 * even after the 500 ms power-stabilize delay in main.c — 300 ms gives the
 * PA's internal LDO a clean window to lock before the first association
 * attempt. ESPHome's wifi_component.cpp uses similar timing (300-500 ms)
 * for the same reason. */
#define WIFI_WARMUP_US 300000ULL

/* Stuck-WiFi watchdog: if we've been trying to associate (SAVED/SC_VERIFY)
 * with NO progress (no STA_CONNECTED / GOT_IP) for this long, the radio or
 * driver is wedged. A full esp_wifi_stop()/start() cycle recovers it without
 * a chip reboot. Generous so a genuinely slow AP in a harsh RF environment
 * is never falsely restarted. */
#define WIFI_STUCK_TIMEOUT_US 90000000ULL

#define MDNS_INSTANCE_NAME "ESP8266 Relay"
#define MDNS_SERVICE_TYPE  "_http"
#define MDNS_SERVICE_PROTO "_tcp"
#define MDNS_SERVICE_PORT  80

/* ── State enum ────────────────────────────────────────────────────────── */

typedef enum {
    CSTATE_SC_LISTEN,
    CSTATE_SC_VERIFY,
    CSTATE_SAVED,
    CSTATE_CONNECTED,
} wifi_cstate_t;

static const char *STATE_NAMES[] = {
    [CSTATE_SC_LISTEN]  = "SC_LISTEN",
    [CSTATE_SC_VERIFY]  = "SC_VERIFY",
    [CSTATE_SAVED]      = "SAVED",
    [CSTATE_CONNECTED]  = "CONNECTED",
};

/* ── Static state ──────────────────────────────────────────────────────── */

static EventGroupHandle_t g_net_evt  = NULL;
static esp_timer_handle_t s_sc_timer;
static esp_timer_handle_t s_rescue_timer;   /* SC_LISTEN → saved-creds fallback */
static wifi_cstate_t s_state          = CSTATE_SC_LISTEN;
static int           s_retry          = 0;
static int           s_auth_fail_count = 0;
static bool          s_ack_sent       = false;
static bool          s_retry_pending  = false;
static bool          s_ntp_done       = false;
static bool          s_mdns_done      = false;
/* Last time we made connection progress (STA_CONNECTED or GOT_IP). Used by
 * the stuck-WiFi watchdog to detect a wedged radio/driver. */
static int64_t       s_last_progress_us = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * Credential Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Securely zero memory — volatile barrier prevents compiler from optimizing
 * away the memset when the buffer is about to go out of scope. */
static void secure_memzero(void *buf, size_t n) {
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (n--) *p++ = 0;
}

static void xor_mac(uint8_t *d, size_t n) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) d[i] ^= mac[i % sizeof(mac)];
}

static esp_err_t creds_load(wifi_config_t *out) {
    size_t sl = sizeof(out->sta.ssid);
    size_t pl = sizeof(out->sta.password);
    esp_err_t e = nvs_store_get_blob(NVS_NS_WIFI_CREDS, NVS_KEY_WIFI_SSID, out->sta.ssid, &sl);
    if (e == ESP_OK) e = nvs_store_get_blob(NVS_NS_WIFI_CREDS, NVS_KEY_WIFI_PASSWORD, out->sta.password, &pl);
    if (e == ESP_OK) {
        xor_mac((uint8_t *)out->sta.ssid, sizeof(out->sta.ssid));
        xor_mac((uint8_t *)out->sta.password, sizeof(out->sta.password));
    }
    return e;
}

static void creds_clear(void) {
    nvs_store_erase_all(NVS_NS_WIFI_CREDS);
}

static esp_err_t creds_save(const uint8_t *ssid, const uint8_t *pass) {
    uint8_t sb[32] = {0};
    uint8_t pb[64] = {0};
    memcpy(sb, ssid, sizeof(sb));
    memcpy(pb, pass, sizeof(pb));
    xor_mac(sb, sizeof(sb));
    xor_mac(pb, sizeof(pb));
    esp_err_t e = nvs_store_set_blob(NVS_NS_WIFI_CREDS, NVS_KEY_WIFI_SSID, sb, sizeof(sb));
    if (e == ESP_OK) e = nvs_store_set_blob(NVS_NS_WIFI_CREDS, NVS_KEY_WIFI_PASSWORD, pb, sizeof(pb));
    secure_memzero(sb, sizeof(sb));
    secure_memzero(pb, sizeof(pb));
    return e;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SmartConfig Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void sc_state_reset(void) {
    s_retry = 0;
    s_ack_sent = false;
    s_retry_pending = false;
}

static void sc_start(void) {
    ESP_LOGW(TAG, "SC start (prev state=%s)", STATE_NAMES[s_state]);
    sc_state_reset();
    s_state = CSTATE_SC_LISTEN;
    esp_timer_stop(s_sc_timer);
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_FAST);
    ESP_ERROR_CHECK(esp_smartconfig_set_type(CONFIG_ESP_SMARTCONFIG_TYPE));
    /* Enable fast mode: remembers the last AP channel from a successful SC.
     * On re-provisioning (NVS wiped, but AP is the same), SC locks onto
     * that channel immediately instead of scanning all 13 channels.
     * Safe to call even if fast mode is unavailable in this SDK version. */
    esp_smartconfig_fast_mode(true);
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    /* Rescue: if valid creds are still in NVS, don't wait for the app
     * forever — fall back to them after SC_RESCUE_TIMEOUT_US. When SC was
     * entered because the creds are genuinely gone/wiped, creds_load fails
     * and no rescue is armed (the app is then the only way forward). */
    wifi_config_t probe;
    if (creds_load(&probe) == ESP_OK)
        ESP_ERROR_CHECK(esp_timer_start_once(s_rescue_timer, SC_RESCUE_TIMEOUT_US));
    ESP_LOGI(TAG, "SC started");
}

static void sc_restart(void) {
    ESP_LOGW(TAG, "SC restart (state=%s)", STATE_NAMES[s_state]);
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_ERROR);
    sc_state_reset();
    s_state = CSTATE_SC_LISTEN;
    esp_timer_stop(s_sc_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_SETTLE_US));
}

/* Leave SmartConfig and reconnect using the NVS credentials. Invoked by the
 * rescue timer when SC_LISTEN has seen no provisioning app for
 * SC_RESCUE_TIMEOUT_US. If no NVS creds exist (factory reset), restart
 * SmartConfig so the user can re-provision without a power cycle. */
static void saved_reconnect(void) {
    wifi_config_t saved = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .listen_interval = 0,
            .pmf_cfg = { .capable = false, .required = false },
            .rm_enabled = false,
            .btm_enabled = false,
        },
    };
    if (creds_load(&saved) != ESP_OK) {
        /* No NVS creds — re-arm SmartConfig so the user can re-provision.
         * The previous SC was stopped after retries exhausted; restart it
         * fresh. */
        ESP_LOGW(TAG, "SC rescue: no NVS creds, restarting SmartConfig");
        sc_start();
        return;
    }

    esp_smartconfig_stop();
    esp_timer_stop(s_sc_timer);
    s_state = CSTATE_SAVED;
    s_retry = 0;
    led_set_pattern(LED_BLINK_SLOW);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &saved));
    esp_wifi_connect();
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_ASSOC_TIMEOUT_US));
    ESP_LOGI(TAG, "SC rescue: no provisioning, reconnecting saved creds");
}

/* Rescue timer: SmartConfig has been listening too long. */
static void rescue_timer_cb(void *arg) {
    if (s_state != CSTATE_SC_LISTEN) return;   /* creds arrived meanwhile */
    saved_reconnect();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Backoff / Timer Callback
 * ══════════════════════════════════════════════════════════════════════════ */

static uint32_t backoff_delay_ms(void) {
    int shift = s_retry < WIFI_BACKOFF_MAX_SHIFT ? s_retry : WIFI_BACKOFF_MAX_SHIFT;
    return shift < 1 ? BACKOFF_BASE_MS : BACKOFF_BASE_MS << (shift - 1);
}

/* Full WiFi driver restart — recovers a wedged radio without a chip reboot.
 * esp_wifi_stop() tears the driver down; esp_wifi_start() re-emits
 * WIFI_EVENT_STA_START, which routes through on_sta_start() and reconnects
 * with the saved credentials (or falls back to SmartConfig if none). */
static void wifi_driver_restart(void) {
    ESP_LOGW(TAG, "WiFi stuck (no progress %llus) — restarting driver",
             (unsigned long long)(WIFI_STUCK_TIMEOUT_US / USEC_PER_SEC));
    /* Force the saved-creds path so on_sta_start() reconnects cleanly. */
    if (s_state == CSTATE_SC_VERIFY) s_state = CSTATE_SAVED;
    esp_wifi_stop();
    esp_wifi_start();
}

static void sc_timer_cb(void *arg) {
    switch (s_state) {
    case CSTATE_SC_VERIFY:
        /* Stuck-WiFi watchdog: no progress for too long → full driver restart. */
        if (s_last_progress_us &&
            esp_timer_get_time() - s_last_progress_us > WIFI_STUCK_TIMEOUT_US) {
            wifi_driver_restart();
            return;
        }
        if (s_retry_pending) {
            s_retry_pending = false;
            esp_wifi_connect();
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_VERIFY_US));
        } else {
            ESP_LOGW(TAG, "verify timeout (state=%s, retry=%d)", STATE_NAMES[s_state], s_retry);
            esp_wifi_disconnect();
        }
        break;

    case CSTATE_SAVED:
        /* Stuck-WiFi watchdog: no progress for too long → full driver restart. */
        if (s_last_progress_us &&
            esp_timer_get_time() - s_last_progress_us > WIFI_STUCK_TIMEOUT_US) {
            wifi_driver_restart();
            return;
        }
        if (s_retry_pending) {
            s_retry_pending = false;
            esp_wifi_connect();
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_ASSOC_TIMEOUT_US));
        } else {
            ESP_LOGW(TAG, "assoc watchdog (%llu s)", (unsigned long long)(WIFI_ASSOC_TIMEOUT_US / USEC_PER_SEC));
            esp_wifi_disconnect();
        }
        break;

    case CSTATE_CONNECTED:
        if (!s_ack_sent) {
            ESP_LOGW(TAG, "SC ACK never confirmed, forcing SC stop");
            esp_smartconfig_stop();
        }
        break;

    default:
        sc_start();
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Event Handlers
 * ══════════════════════════════════════════════════════════════════════════ */

static void on_sta_start(void) {
    if (s_state == CSTATE_SAVED) {
        led_set_pattern(LED_BLINK_SLOW);
        esp_wifi_set_ps(WIFI_PS_NONE);
        /* Lower initial TX power for the first association on a marginal
         * supply. Full-power TX (80 / +20 dBm) draws ~250 mA in bursts; on
         * a sagging AC-DC rail the PA's PLL unlocks mid-packet, the ACK
         * is lost, and the retry compounds the problem. +12.5 dBm (50)
         * is enough for typical home APs at <10 m and keeps the rail clean.
         * ESP-01S's PCB antenna is the bottleneck anyway — going below
         * +10 dBm gains nothing. SmartConfig keeps full power in
         * on_sc_got_ssid_pwd() because the phone may be across the room
         * during initial provisioning. */
        esp_wifi_set_max_tx_power(50);
        {   wifi_config_t c;
            if (esp_wifi_get_config(ESP_IF_WIFI_STA, &c) == ESP_OK) {
                c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                c.sta.channel = 0;
                c.sta.bssid_set = false;
                c.sta.listen_interval = 0;
                esp_wifi_set_config(ESP_IF_WIFI_STA, &c);
            }
        }
        /* Warm-up: delay the first connect briefly so the radio stabilizes
         * after the driver comes up. The timer callback performs the connect
         * (s_retry_pending) after WIFI_WARMUP_US. */
        s_retry_pending = true;
        esp_timer_stop(s_sc_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_WARMUP_US));
        ESP_LOGI(TAG, "STA_START: connecting saved creds (warm-up %llu ms)",
                 (unsigned long long)(WIFI_WARMUP_US / USEC_PER_MSEC));
    } else {
        ESP_LOGI(TAG, "STA_START: no saved creds -> SmartConfig");
        sc_start();
    }
}

static void on_sta_connected(void) {
    esp_timer_stop(s_sc_timer);
    s_last_progress_us = esp_timer_get_time();   /* connection progress */
    tcpip_adapter_up(TCPIP_ADAPTER_IF_STA);
    /* REQUIRED after tcpip_adapter_up(): without explicit dhcpc_start DHCP
     * takes 140s; with it, ~9s. The 7 "handler already registered" warnings
     * from esp_wifi_init() re-registering default handlers are cosmetic. */
    tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
    ESP_LOGI(TAG, "STA_CONNECTED: association OK, DHCP started");
}

static void on_sta_disconnected(void *data) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    /* Map common disconnect reason codes to short names for log clarity.
     * Full list: see esp_wifi_types.h WIFI_REASON_*. */
    const char *rname = "?";
    switch (d->reason) {
        case 1:  rname = "UNSPECIFIED";       break;
        case 2:  rname = "AUTH_EXPIRE";       break;
        case 3:  rname = "AUTH_LEAVE";        break;
        case 4:  rname = "ASSOC_EXPIRE";      break;
        case 5:  rname = "ASSOC_TOOMANY";     break;
        case 6:  rname = "NOT_AUTHED";        break;
        case 7:  rname = "NOT_ASSOCED";       break;
        case 8:  rname = "ASSOC_LEAVE";       break;
        case 15: rname = "4WAY_TIMEOUT";      break;
        case 16: rname = "GROUP_KEY_UPDATE";  break;
        case 17: rname = "IE_IN_4WAY";        break;
        case 18: rname = "MIC_FAIL";          break;
        case 19: rname = "4WAY_HANDSHAKE";    break;
        case 23: rname = "802_1X_AUTH";       break;
        case 200: rname = "BEACON_TIMEOUT";   break;
        case 201: rname = "NO_AP_FOUND";      break;
        case 202: rname = "AUTH_FAIL";        break;
        case 203: rname = "ASSOC_FAIL";       break;
        case 204: rname = "HANDSHAKE_TIMEOUT"; break;
        default: break;
    }
    ESP_LOGW(TAG, "STA_DISCONNECTED: reason=%d(%s) state=%s",
             d->reason, rname, STATE_NAMES[s_state]);
    xEventGroupClearBits(g_net_evt, WIFI_CONNECTED_BIT);

    /* AUTH_FAIL (202) is retried like any other failure: a single 202 right
     * after a reset is usually the AP rejecting the re-auth while it still
     * holds the stale association from the previous session. Only after
     * MAX_AUTH_FAIL_WIPE consecutive auth failures are the credentials
     * declared bad — wiped, then SmartConfig. */
    if (d->reason == WIFI_REASON_AUTH_FAIL &&
        ++s_auth_fail_count >= MAX_AUTH_FAIL_WIPE) {
        ESP_LOGW(TAG, "auth failed %d consecutive times -> clearing creds, SmartConfig",
                 s_auth_fail_count);
        s_auth_fail_count = 0;
        creds_clear();
        sc_restart();
        return;
    }

    switch (s_state) {
    case CSTATE_SC_VERIFY:
        esp_timer_stop(s_sc_timer);
        s_retry_pending = false;
        if (s_retry < MAX_WIFI_RETRY_VERIFY) {
            s_retry++;
            s_retry_pending = true;
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_RETRY_US));
        } else {
            /* Verify retries exhausted. Don't call sc_restart() — it makes
             * the phone re-send the same bad credentials, producing an
             * infinite provision-fail-restart loop. Instead, stop SC and
             * return to SC_LISTEN so the user can re-provision with
             * different credentials. The LED shows an error pattern.
             * Re-arm the rescue timer so it auto-retries if the user
             * hasn't moved the phone yet — a brief move to reduce
             * interference may fix the CRC errors on the next attempt. */
            ESP_LOGE(TAG, "verify failed after %d retries — stopping SC, back to LISTEN",
                     MAX_WIFI_RETRY_VERIFY);
            esp_smartconfig_stop();
            s_state = CSTATE_SC_LISTEN;
            s_retry = 0;
            led_set_pattern(LED_BLINK_ERROR);
            ESP_ERROR_CHECK(esp_timer_start_once(s_rescue_timer, SC_RESCUE_TIMEOUT_US));
        }
        break;

    case CSTATE_SAVED:
    case CSTATE_CONNECTED:
        esp_timer_stop(s_sc_timer);
        s_retry_pending = false;
        esp_wifi_set_ps(WIFI_PS_NONE);
        {   wifi_config_t c;
            if (esp_wifi_get_config(ESP_IF_WIFI_STA, &c) == ESP_OK) {
                c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                c.sta.channel = 0;
                c.sta.bssid_set = false;
                c.sta.listen_interval = 0;
                esp_wifi_set_config(ESP_IF_WIFI_STA, &c);
            }
        }
        s_retry++;
        s_state = CSTATE_SAVED;
        led_set_pattern(LED_BLINK_SLOW);
        if (s_retry <= WIFI_WARMUP_ATTEMPTS) {
            esp_wifi_connect();
            esp_timer_stop(s_sc_timer);
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_ASSOC_TIMEOUT_US));
        } else {
            uint32_t delay_ms = backoff_delay_ms();
            s_retry_pending = true;
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, (uint64_t)delay_ms * USEC_PER_MSEC));
        }
        break;

    default:
        break;
    }
}

static void on_got_ip(void *data) {
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "GOT_IP: " IPSTR " online (state=%s)", IP2STR(&evt->ip_info.ip), STATE_NAMES[s_state]);

    uint8_t mac[6];
    if (esp_wifi_get_mac(ESP_IF_WIFI_STA, mac) == ESP_OK)
        ESP_LOGI(TAG, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        ESP_LOGW(TAG, "STA MAC: failed to read");

    esp_timer_stop(s_sc_timer);
    esp_timer_stop(s_rescue_timer);
    s_retry = 0;
    s_retry_pending = false;
    s_auth_fail_count = 0;   /* online — auth-fail streak over */
    s_last_progress_us = esp_timer_get_time();   /* connection progress */

    if (s_state == CSTATE_SC_VERIFY) {
        wifi_config_t cfg = {0};
        esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg);
        if (creds_save(cfg.sta.ssid, cfg.sta.password) == ESP_OK)
            ESP_LOGI(TAG, "Credentials saved to NVS");
        if (s_ack_sent) {
            esp_smartconfig_stop();
        } else {
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_ACK_GRACE_TIMEOUT_US));
        }
    }

    s_state = CSTATE_CONNECTED;
    xEventGroupSetBits(g_net_evt, WIFI_CONNECTED_BIT);
    led_set_pattern(LED_BLINK_OK);

    if (!s_ntp_done) {
        timing_ntp_start();
        timing_ntp_health_start();
        s_ntp_done = true;
        ESP_LOGI(TAG, "NTP started");
    } else if (!timing_time_ok()) {
        /* Reconnected but the clock was never re-synced (e.g. a long offline
         * period). Restart NTP to recover the clock — safe because we only
         * do this when time is NOT already synced, so an already-good clock
         * is never disrupted. */
        ESP_LOGW(TAG, "reconnected with unsynced clock — restarting NTP");
        timing_ntp_start();
        timing_ntp_health_start();
    }
    if (!s_mdns_done) {
        char dev_id[DEVICE_ID_MIN_LEN];
        device_id(dev_id, sizeof(dev_id));
        char mdns_host[DEVICE_ID_MIN_LEN + 8];   /* "switch" + id + NUL */
        snprintf(mdns_host, sizeof(mdns_host), "switch%s", dev_id);
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set(mdns_host);
            mdns_txt_item_t txt[] = {
                {"device", dev_id},
                {"type",   "esp8266-relay"},
                {"path",   "/"}
            };
            mdns_service_add(MDNS_INSTANCE_NAME,
                             MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
                             MDNS_SERVICE_PORT,
                             txt, sizeof(txt)/sizeof(txt[0]));
            s_mdns_done = true;
            ESP_LOGI(TAG, "mDNS started as %s.local", mdns_host);
        }
    }
}

static void on_sc_got_ssid_pwd(void *data) {
    if (s_state == CSTATE_SC_VERIFY || s_state == CSTATE_CONNECTED) return;

    smartconfig_event_got_ssid_pswd_t *evt =
        (smartconfig_event_got_ssid_pswd_t *)data;

    wifi_config_t cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .listen_interval = 1,
            .pmf_cfg = { .capable = false, .required = false },
            .rm_enabled = false,
            .btm_enabled = false,
            .bssid_set = evt->bssid_set,
        },
    };
    memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
    memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
    if (evt->bssid_set)
        memcpy(cfg.sta.bssid, evt->bssid, sizeof(cfg.sta.bssid));

    ESP_LOGI(TAG, "SC creds SSID=%.32s (len=%u, pass_len=%u), connecting",
             (const char *)cfg.sta.ssid,
             (unsigned)strlen((const char *)cfg.sta.ssid),
             (unsigned)strlen((const char *)cfg.sta.password));

    s_state = CSTATE_SC_VERIFY;
    s_retry = 0;
    s_last_progress_us = esp_timer_get_time();   /* SC delivered creds = progress */
    led_set_pattern(LED_BLINK_SLOW);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg));
    esp_wifi_set_ps(WIFI_PS_NONE);   /* active mode for initial connect */
    /* Boost TX power for the first association attempt — marginal power
     * supplies and far APs benefit from full-power transmit. The power
     * manager will throttle this down to the configured level once
     * connected. */
    esp_wifi_set_max_tx_power(80);   /* 80 == +20 dBm (max for ESP8266) */
    ESP_ERROR_CHECK(esp_wifi_connect());
    esp_timer_stop(s_sc_timer);
    esp_timer_stop(s_rescue_timer);   /* creds received — rescue no longer needed */
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_VERIFY_US));
}

static void on_sc_ack_done(void) {
    s_ack_sent = true;
    if (s_state == CSTATE_CONNECTED) {
        esp_timer_stop(s_sc_timer);
        esp_smartconfig_stop();
        ESP_LOGI(TAG, "ACK sent: SC stopped (online)");
    }
}

/* ── Main event dispatcher ─────────────────────────────────────────────── */

static void handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:           on_sta_start();              break;
        case WIFI_EVENT_STA_CONNECTED:       on_sta_connected();          break;
        case WIFI_EVENT_STA_DISCONNECTED:    on_sta_disconnected(data);   break;
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(data);
    } else if (base == SC_EVENT) {
        switch (id) {
        case SC_EVENT_SEND_ACK_DONE: on_sc_ack_done();        break;
        case SC_EVENT_GOT_SSID_PSWD: on_sc_got_ssid_pwd(data); break;
        default: break;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

bool wifi_is_connected(void) {
    return g_net_evt && (xEventGroupGetBits(g_net_evt) & WIFI_CONNECTED_BIT) != 0;
}

void connection_init(void) {
    g_net_evt = xEventGroupCreate();
}

void wifi_init(void) {
    ESP_LOGI(TAG, "wifi_init");
    s_last_progress_us = esp_timer_get_time();   /* anchor the stuck watchdog */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        SC_EVENT, ESP_EVENT_ANY_ID, handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    esp_timer_create_args_t ta = {
        .callback = &sc_timer_cb,
        .name = "sc_rstrt",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_sc_timer));

    esp_timer_create_args_t tra = {
        .callback = &rescue_timer_cb,
        .name = "sc_rescue",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tra, &s_rescue_timer));

    wifi_config_t saved = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .listen_interval = 0,
            .pmf_cfg = { .capable = false, .required = false },
            .rm_enabled = false,
            .btm_enabled = false,
        },
    };
    if (creds_load(&saved) == ESP_OK) {
        s_state = CSTATE_SAVED;
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &saved));
        ESP_LOGI(TAG, "saved creds loaded: SSID=%.32s", (const char *)saved.sta.ssid);
    } else {
        ESP_LOGI(TAG, "no saved creds");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_reconnect(void) {
    ESP_LOGI(TAG, "WiFi reconnect (button)");
    creds_clear();
    s_state = CSTATE_SC_LISTEN;
    s_retry = 0;
    esp_timer_stop(s_rescue_timer);   /* creds just wiped — nothing to rescue */
    esp_wifi_disconnect();
    sc_start();
}
