/*
 * connection.c — WiFi station + SmartConfig provisioning
 *
 * State machine:
 *   CSTATE_SC_LISTEN  -> SmartConfig listening for phone app
 *   CSTATE_SC_VERIFY  -> received creds, testing connection
 *   CSTATE_SAVED      -> have saved NVS creds, connecting
 *   CSTATE_CONNECTED  -> IP obtained, online
 *
 * Credential storage: NVS namespace "wifi_creds", keys "ssid"/"password",
 * XOR-obfuscated with device MAC via xor_mac().
 *
 * Retry policy:
 *   SC_VERIFY:    MAX_WIFI_RETRY_VERIFY (1) attempts, then sc_restart().
 *   SAVED:        exponential backoff 1s/2s/4s/…/32s cap, indefinite.
 *   CONNECTED:    same as SAVED (falls back on disconnect).
 *   Auth failure: always clears creds and restarts SmartConfig.
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "mdns.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "lwip/def.h"
#include "nvs_store.h"
#include "connection.h"
#include "board.h"
#include "ipc.h"
#include "timing.h"

#define TAG "wifi"

/* ── Timeout & retry constants ─────────────────────────────────────────── */

/* SC_VERIFY = unproven, freshly-received SmartConfig creds -> fail fast
 * and go back to listening if they don't work.
 * SAVED/CONNECTED retries use exponential backoff so a real outage on a
 * known-good network never gets treated the same as a bad password. */
#define MAX_WIFI_RETRY_VERIFY 1
#define WIFI_BACKOFF_MAX_SHIFT 6          /* backoff caps at 2^6 = 32s */

/* Associated but no DHCP reply after this long -> force a clean
 * disconnect/retry (lwIP's DHCP client does not raise
 * WIFI_EVENT_STA_DISCONNECTED on its own). */
#define WIFI_ASSOC_TIMEOUT_US 15000000ULL

/* If SmartConfig's ACK round-trip never confirms (phone app closed
 * early, etc.) while already online, stop broadcasting. */
#define SC_ACK_GRACE_TIMEOUT_US 15000000ULL

/* ── NVS keys ──────────────────────────────────────────────────────────── */

#define NVS_NS     "wifi_creds"
#define NVS_SSID   "ssid"
#define NVS_PASS   "password"

/* ── State enum ────────────────────────────────────────────────────────── */

typedef enum {
    CSTATE_SC_LISTEN,
    CSTATE_SC_VERIFY,
    CSTATE_SAVED,
    CSTATE_CONNECTED,
} wifi_cstate_t;

/* Human-readable state names for logging -- mirrors board.c's LED_NAMES[]. */
static const char *STATE_NAMES[] = {
    [CSTATE_SC_LISTEN] = "SC_LISTEN",
    [CSTATE_SC_VERIFY]  = "SC_VERIFY",
    [CSTATE_SAVED]      = "SAVED",
    [CSTATE_CONNECTED]  = "CONNECTED",
};

/* ── Static state ──────────────────────────────────────────────────────── */

static esp_timer_handle_t s_sc_timer;
static wifi_cstate_t      s_state          = CSTATE_SC_LISTEN;
static int                s_retry          = 0;
static bool               s_ack_sent       = false;
static bool               s_retry_pending  = false;


/* ══════════════════════════════════════════════════════════════════════════
 * Credential Helpers (XOR-obfuscated with MAC)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * xor_mac — XORs a buffer with the device's WiFi station MAC, repeating
 * as needed. Light obfuscation for credentials at rest in NVS -- not
 * real encryption, just avoids storing SSID/password as flash plaintext.
 */
static void xor_mac(uint8_t *d, size_t n) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) d[i] ^= mac[i % 6];
}

/**
 * creds_load — Reads and de-obfuscates saved WiFi credentials from NVS
 * into *out. Returns the underlying NVS error if either blob is
 * missing or corrupt.
 */
static esp_err_t creds_load(wifi_config_t *out) {
    size_t sl = sizeof(out->sta.ssid);
    size_t pl = sizeof(out->sta.password);
    esp_err_t e = nvs_store_get_blob(NVS_NS, NVS_SSID, out->sta.ssid, &sl);
    if (e == ESP_OK) e = nvs_store_get_blob(NVS_NS, NVS_PASS, out->sta.password, &pl);
    if (e == ESP_OK) {
        xor_mac((uint8_t *)out->sta.ssid, sizeof(out->sta.ssid));
        xor_mac((uint8_t *)out->sta.password, sizeof(out->sta.password));
    }
    return e;
}

/**
 * creds_clear — Erases the entire wifi_creds NVS namespace. Called on
 * explicit reconnect (button) to force SmartConfig re-provisioning.
 */
static void creds_clear(void) {
    nvs_store_erase_all(NVS_NS);
}

/**
 * creds_save — Obfuscates and persists the SSID/password pair after
 * SmartConfig verification succeeds (GOT_IP while in CSTATE_SC_VERIFY).
 * Clears the local plaintext copies before returning.
 */
static esp_err_t creds_save(const uint8_t *ssid, const uint8_t *pass) {
    uint8_t sb[32] = {0};
    uint8_t pb[64] = {0};
    memcpy(sb, ssid, sizeof(sb));
    memcpy(pb, pass, sizeof(pb));
    xor_mac(sb, sizeof(sb));
    xor_mac(pb, sizeof(pb));

    esp_err_t e = nvs_store_set_blob(NVS_NS, NVS_SSID, sb, sizeof(sb));
    if (e == ESP_OK) e = nvs_store_set_blob(NVS_NS, NVS_PASS, pb, sizeof(pb));
    memset(sb, 0, sizeof(sb));
    memset(pb, 0, sizeof(pb));
    return e;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SmartConfig Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * sc_state_reset — Resets retry/ack bookkeeping shared by both
 * SmartConfig entry points (sc_start, sc_restart).
 */
static void sc_state_reset(void) {
    s_retry = 0;
    s_ack_sent = false;
    s_retry_pending = false;
}

/**
 * sc_start — Enter CSTATE_SC_LISTEN and begin broadcasting for
 * SmartConfig credentials. Called on cold boot with no saved creds,
 * and from wifi_reconnect().
 */
static void sc_start(void) {
    sc_state_reset();
    s_state = CSTATE_SC_LISTEN;
    esp_timer_stop(s_sc_timer);
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_FAST);
    ESP_ERROR_CHECK(esp_smartconfig_set_type(CONFIG_ESP_SMARTCONFIG_TYPE));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    ESP_LOGI(TAG, "SC started (type=%d)", CONFIG_ESP_SMARTCONFIG_TYPE);
}

/**
 * sc_restart — Abandon the current SmartConfig/connection attempt and
 * re-enter listening after a short settle delay (handled by
 * sc_timer_cb's default case). Used when credentials are known-bad
 * (AUTH_FAIL) or SC verification exhausts its retries. Sets the error
 * LED pattern itself -- callers should not set it again beforehand.
 */
static void sc_restart(void) {
    ESP_LOGW(TAG, "SC restart (state=%s)", STATE_NAMES[s_state]);
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_ERROR);
    sc_state_reset();
    s_state = CSTATE_SC_LISTEN;
    esp_timer_stop(s_sc_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, 50000));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Backoff / Timer Callback
 * ══════════════════════════════════════════════════════════════════════════ */

/* Exponential backoff for reconnecting to a *known-good* saved network:
 * 1s, 2s, 4s, 8s, 16s, 32s, 32s, 32s... Resets to 1s on the next
 * successful connection (s_retry zeroed in GOT_IP handler). */
static uint32_t backoff_delay_ms(void) {
    int shift = LWIP_MIN(s_retry, WIFI_BACKOFF_MAX_SHIFT);
    if (shift < 1) return 1000;
    return 1000u << (shift - 1);
}

/**
 * sc_timer_cb — Single shared alarm for the module. What it means is
 * dispatched entirely by the current state:
 *   SC_VERIFY  -> retry-once-then-timeout for unproven creds
 *   SAVED      -> backoff-elapsed reconnect, or DHCP-stall watchdog
 *   CONNECTED  -> SmartConfig ACK grace timeout
 *   otherwise  -> settle delay after sc_restart(), re-enter listening
 */
static void sc_timer_cb(void *arg) {
    switch (s_state) {
    case CSTATE_SC_VERIFY:
        if (s_retry_pending) {
            s_retry_pending = false;
            ESP_LOGI(TAG, "Retry delay elapsed, reconnecting (retry=%d)", s_retry);
            esp_wifi_connect();
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, 12000000));
        } else {
            ESP_LOGW(TAG, "SC verify timeout (disconnecting, retry=%d)", s_retry);
            esp_wifi_disconnect();
        }
        break;

    case CSTATE_SAVED:
        if (s_retry_pending) {
            /* Backoff elapsed: try again, re-arm association watchdog */
            s_retry_pending = false;
            ESP_LOGI(TAG, "Backoff elapsed, reconnecting (retry=%d)", s_retry);
            esp_wifi_connect();
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_ASSOC_TIMEOUT_US));
        } else {
            /* Associated per WiFi driver but never got IP — force clean
             * disconnect; DISCONNECTED handler picks it up for backoff. */
            ESP_LOGW(TAG, "No IP after %d s, forcing disconnect/retry",
                     (int)(WIFI_ASSOC_TIMEOUT_US / 1000000));
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
        ESP_LOGI(TAG, "SC timer state=%s -> sc_start", STATE_NAMES[s_state]);
        sc_start();
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Event Handlers
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * on_sta_start — WIFI_EVENT_STA_START: begin a saved-credential
 * connection attempt, or enter SmartConfig listening if none are
 * stored.
 */
static void on_sta_start(void) {
    if (s_state == CSTATE_SAVED) {
        ESP_LOGI(TAG, "Saved creds connect start");
        led_set_pattern(LED_BLINK_SLOW);
        esp_wifi_connect();
        esp_timer_stop(s_sc_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, WIFI_ASSOC_TIMEOUT_US));
    } else {
        sc_start();
    }
}

/**
 * on_sta_connected — WIFI_EVENT_STA_CONNECTED: association succeeded;
 * bring up the STA interface and kick off DHCP.
 */
static void on_sta_connected(void) {
    ESP_LOGI(TAG, "Association OK");
    tcpip_adapter_up(TCPIP_ADAPTER_IF_STA);
    ESP_LOGI(TAG, "Starting DHCP client");
    tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
}

/**
 * on_sta_disconnected — WIFI_EVENT_STA_DISCONNECTED: routes to the
 * recovery policy for the state the disconnect occurred in (see file
 * header for the full retry policy table).
 */
static void on_sta_disconnected(void *data) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    ESP_LOGW(TAG, "DISCONNECTED reason=%d", d->reason);
    xEventGroupClearBits(g_net_evt, WIFI_CONNECTED_BIT);

    switch (s_state) {
    case CSTATE_SC_VERIFY:
        esp_timer_stop(s_sc_timer);
        s_retry_pending = false;
        if (d->reason == WIFI_REASON_AUTH_FAIL) {
            ESP_LOGW(TAG, "Auth fail in VERIFY -> restart SC");
            sc_restart();
        } else if (s_retry < MAX_WIFI_RETRY_VERIFY) {
            s_retry++;
            ESP_LOGW(TAG, "VERIFY retry %d/%d in 500ms", s_retry, MAX_WIFI_RETRY_VERIFY);
            s_retry_pending = true;
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, 500000));
        } else {
            ESP_LOGE(TAG, "VERIFY failed after retries -> restart SC");
            sc_restart();
        }
        break;

    case CSTATE_SAVED:
    case CSTATE_CONNECTED:
        esp_timer_stop(s_sc_timer);
        s_retry_pending = false;
        if (d->reason == WIFI_REASON_AUTH_FAIL) {
            /* Password actually rejected (router changed password, etc.)
             * -> only this case gives up on saved credentials.
             * sc_restart() sets LED_BLINK_ERROR itself. */
            ESP_LOGW(TAG, "Saved creds auth fail -> restart SC");
            sc_restart();
        } else {
            /* Any other disconnect (beacon timeout, AP reboot,
             * DHCP-stall watchdog, transient) is a real outage: keep
             * saved credentials and retry indefinitely with backoff. */
            s_retry++;
            s_state = CSTATE_SAVED;
            led_set_pattern(LED_BLINK_SLOW);
            uint32_t delay_ms = backoff_delay_ms();
            ESP_LOGI(TAG, "SAVED/CONN reconnect in %lu ms (retry=%d)",
                     (unsigned long)delay_ms, s_retry);
            s_retry_pending = true;
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, (uint64_t)delay_ms * 1000));
        }
        break;

    default:
        break;
    }
}

/**
 * on_got_ip — IP_EVENT_STA_GOT_IP: DHCP succeeded, device is online.
 * If this connection came from SmartConfig verification, persists the
 * now-proven credentials and arms the ACK grace timeout if the phone
 * app hasn't confirmed yet. Always transitions to CSTATE_CONNECTED.
 */
static void on_got_ip(void *data) {
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "GOT_IP %d.%d.%d.%d (state=%s, ack_sent=%d)",
             IP2STR(&evt->ip_info.ip), STATE_NAMES[s_state], s_ack_sent);

    esp_timer_stop(s_sc_timer);
    s_retry = 0;
    s_retry_pending = false;

    if (s_state == CSTATE_SC_VERIFY) {
        wifi_config_t cfg = {0};
        esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg);
        esp_err_t e = creds_save(cfg.sta.ssid, cfg.sta.password);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "Credentials saved to NVS");
        else
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(e));
        if (s_ack_sent) {
            ESP_LOGI(TAG, "ACK already sent -> stopping SC");
            esp_smartconfig_stop();
        } else {
            ESP_LOGI(TAG, "ACK not yet sent, arming grace timeout");
            ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, SC_ACK_GRACE_TIMEOUT_US));
        }
    }

    s_state = CSTATE_CONNECTED;
    xEventGroupSetBits(g_net_evt, WIFI_CONNECTED_BIT);
    if (g_net_task) xTaskNotifyGive(g_net_task);
    led_set_pattern(LED_BLINK_OK);
    ESP_LOGI(TAG, "Online");
}

/**
 * on_sc_got_ssid_pwd — SC_EVENT_GOT_SSID_PSWD: applies freshly-received
 * SmartConfig credentials and attempts a connection to verify them
 * before persisting. Ignored if we're already verifying or connected,
 * so a duplicate broadcast can't restart an in-progress attempt.
 */
static void on_sc_got_ssid_pwd(void *data) {
    if (s_state == CSTATE_SC_VERIFY || s_state == CSTATE_CONNECTED) {
        ESP_LOGI(TAG, "Ignored (already have creds)");
        return;
    }
    smartconfig_event_got_ssid_pswd_t *evt =
        (smartconfig_event_got_ssid_pswd_t *)data;

    ESP_LOGI(TAG, "Creds: type=%d token=%d bssid_set=%d",
             evt->type, evt->token, evt->bssid_set);

    wifi_config_t cfg = {0};
    cfg.sta.bssid_set = evt->bssid_set;
    memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
    memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
    if (evt->bssid_set)
        memcpy(cfg.sta.bssid, evt->bssid, sizeof(cfg.sta.bssid));

    if (evt->type == SC_TYPE_ESPTOUCH_V2) {
        uint8_t rvd[33] = {0};
        esp_smartconfig_get_rvd_data(rvd, sizeof(rvd));
        ESP_LOGI(TAG, "RVD: %.32s", rvd);
    }

    ESP_LOGI(TAG, "SC creds SSID=%.32s, setting config + connecting",
             (const char *)cfg.sta.ssid);

    s_state = CSTATE_SC_VERIFY;
    s_retry = 0;
    led_set_pattern(LED_BLINK_SLOW);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    esp_timer_stop(s_sc_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, 12000000));
}

/**
 * on_sc_ack_done — SC_EVENT_SEND_ACK_DONE: the phone app has confirmed
 * receipt of the connection result. Safe to stop broadcasting once
 * we're actually online; otherwise SmartConfig keeps running until we
 * are (see sc_timer_cb's CSTATE_CONNECTED case for the timeout if this
 * never fires).
 */
static void on_sc_ack_done(void) {
    s_ack_sent = true;
    ESP_LOGI(TAG, "ACK sent successfully (state=%s)", STATE_NAMES[s_state]);
    if (s_state == CSTATE_CONNECTED) {
        ESP_LOGI(TAG, "Connected, stopping SC");
        esp_timer_stop(s_sc_timer);
        esp_smartconfig_stop();
    } else {
        ESP_LOGI(TAG, "NOT yet connected, SC will run until connected");
    }
}

/**
 * on_sta_authmode_change — WIFI_EVENT_STA_AUTHMODE_CHANGE: informational
 * only, logs the AP's security mode transition.
 */
static void on_sta_authmode_change(void *data) {
    wifi_event_sta_authmode_change_t *a = (wifi_event_sta_authmode_change_t *)data;
    ESP_LOGI(TAG, "Auth mode: old=%d new=%d", a->old_mode, a->new_mode);
}

/* ── Main event dispatcher ─────────────────────────────────────────────── */

/**
 * handler — Single ESP event-loop callback registered for WIFI_EVENT,
 * IP_EVENT (GOT_IP only), and SC_EVENT. Dispatches to the on_*()
 * handlers above by event id; no event-specific logic lives here.
 */
static void handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        ESP_LOGI(TAG, "EVENT id=%d state=%s retry=%d",
                 id, STATE_NAMES[s_state], s_retry);

        switch (id) {
        case WIFI_EVENT_STA_START:           on_sta_start();              break;
        case WIFI_EVENT_STA_CONNECTED:       on_sta_connected();          break;
        case WIFI_EVENT_STA_DISCONNECTED:    on_sta_disconnected(data);   break;
        case WIFI_EVENT_STA_AUTHMODE_CHANGE: on_sta_authmode_change(data); break;
        default: break;
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(data);

    } else if (base == SC_EVENT) {
        ESP_LOGI(TAG, "SC_EVENT id=%d state=%s", id, STATE_NAMES[s_state]);

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

/**
 * wifi_init — Bring up the WiFi driver, register event handlers, and
 * either start a saved-credential connection or enter SmartConfig
 * listening, depending on what's in NVS. Must be called once, after
 * the default event loop has been created.
 */
void wifi_init(void) {
    /* g_net_evt is created globally elsewhere; no need to create here */
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

    wifi_config_t saved = {0};
    if (creds_load(&saved) == ESP_OK) {
        s_state = CSTATE_SAVED;
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &saved));
        ESP_LOGI(TAG, "Saved creds loaded: SSID=%.32s, state=SAVED",
                 (const char *)saved.sta.ssid);
    } else {
        ESP_LOGI(TAG, "No saved creds -> SC on STA_START");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
}

/**
 * wifi_reconnect — Force re-provisioning: clears saved credentials and
 * re-enters SmartConfig listening. Called from the board's long-press
 * button action.
 */
void wifi_reconnect(void) {
    ESP_LOGI(TAG, "WiFi reconnect requested (button)");
    creds_clear();
    s_state = CSTATE_SC_LISTEN;
    s_retry = 0;
    esp_wifi_disconnect();
    sc_start();
}

void net_reconnect_request(void) {
    net_msg_t m = { .type = NCMD_RECONNECT, .ack = NULL };
    xQueueSend(g_net_q, &m, 0);
    if (g_net_task) xTaskNotifyGive(g_net_task);
}

void net_task(void *arg) {
    (void)arg;
    static bool s_ntp_started = false;
    static bool s_mdns_started = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Process any queued net commands (reconnect requests). */
        net_msg_t m;
        while (xQueueReceive(g_net_q, &m, 0)) {
            if (m.type == NCMD_RECONNECT) wifi_reconnect();
            if (m.ack) xSemaphoreGive(m.ack);
        }

        /* Start NTP once WiFi is connected. */
        if ((xEventGroupGetBits(g_net_evt) & WIFI_CONNECTED_BIT) && !s_ntp_started) {
            ESP_LOGI(TAG, "WiFi connected -> starting NTP");
            timing_ntp_start();
            timing_ntp_health_start();
            s_ntp_started = true;
        }

        /* Start mDNS once WiFi is connected. */
        if ((xEventGroupGetBits(g_net_evt) & WIFI_CONNECTED_BIT) && !s_mdns_started) {
            ESP_LOGI(TAG, "WiFi connected -> starting mDNS");
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set("switchiot");
                mdns_txt_item_t txt[] = {
                    {"device", "relay"},
                    {"type", "switchiot"},
                    {"path", "/"}
                };
                mdns_service_add("ESP8266 Relay", "_http", "_tcp", 80,
                                 txt, sizeof(txt)/sizeof(txt[0]));
                s_mdns_started = true;
            }
        }
    }
}