#include <string.h>
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
#include "nvs.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "lwip/apps/sntp.h"
#include "mdns.h"

#include "wifi.h"
#include "gpio.h"

#define TAG "wifi"
#define MAX_WIFI_RETRY 1
#define NVS_NS  "wifi_creds"
#define NVS_SSID "ssid"
#define NVS_PASS "password"

typedef enum {
    CSTATE_SC_LISTEN,
    CSTATE_SC_VERIFY,
    CSTATE_SAVED,
    CSTATE_CONNECTED,
} wifi_cstate_t;

static EventGroupHandle_t s_evt_grp;
static esp_timer_handle_t s_sc_timer;
static wifi_cstate_t s_state = CSTATE_SC_LISTEN;
static int s_retry = 0;
static bool s_ack_sent = false;

static void xor_mac(uint8_t* d, size_t n) {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) d[i] ^= mac[i % 6];
}

static esp_err_t creds_load(wifi_config_t* out) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t sl = sizeof(out->sta.ssid);
    size_t pl = sizeof(out->sta.password);
    e = nvs_get_blob(h, NVS_SSID, out->sta.ssid, &sl);
    if (e == ESP_OK) e = nvs_get_blob(h, NVS_PASS, out->sta.password, &pl);
    nvs_close(h);

    if (e == ESP_OK) {
        xor_mac((uint8_t*)out->sta.ssid, sizeof(out->sta.ssid));
        xor_mac((uint8_t*)out->sta.password, sizeof(out->sta.password));
    }
    return e;
}

static esp_err_t creds_save(const uint8_t* ssid, const uint8_t* pass) {
    uint8_t sb[32] = {0};
    uint8_t pb[64] = {0};
    memcpy(sb, ssid, sizeof(sb));
    memcpy(pb, pass, sizeof(pb));
    xor_mac(sb, sizeof(sb));
    xor_mac(pb, sizeof(pb));

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) goto out;
    e = nvs_set_blob(h, NVS_SSID, sb, sizeof(sb));
    if (e == ESP_OK) e = nvs_set_blob(h, NVS_PASS, pb, sizeof(pb));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
out:
    memset(sb, 0, sizeof(sb));
    memset(pb, 0, sizeof(pb));
    return e;
}

static void sc_start(void) {
    s_state = CSTATE_SC_LISTEN;
    s_retry = 0;
    s_ack_sent = false;
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_FAST);
    ESP_ERROR_CHECK(esp_smartconfig_set_type(CONFIG_ESP_SMARTCONFIG_TYPE));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    ESP_LOGI(TAG, "SmartConfig listening");
}

static void sc_restart(void) {
    esp_smartconfig_stop();
    led_set_pattern(LED_BLINK_ERROR);
    s_state = CSTATE_SC_LISTEN;
    s_retry = 0;
    s_ack_sent = false;
    ESP_ERROR_CHECK(esp_timer_start_once(s_sc_timer, 50000));
}

static void sc_timer_cb(void* arg) {
    sc_start();
}

static void ntp_start(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_sync_interval(3600000);
    sntp_init();
    ESP_LOGI(TAG, "NTP started");
}

static void mdns_start(void) {
    mdns_free();
    mdns_init();
    mdns_hostname_set("central");
    mdns_service_add("ESP8266 Relay", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started");
}

static void handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_state == CSTATE_SAVED) {
                ESP_LOGI(TAG, "Connecting with saved credentials");
                esp_wifi_connect();
            } else {
                sc_start();
            }

        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t* d =
                (wifi_event_sta_disconnected_t*)data;
            xEventGroupClearBits(s_evt_grp, WIFI_CONNECTED_BIT);

            switch (s_state) {
            case CSTATE_SC_VERIFY:
                if (d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    d->reason == WIFI_REASON_AUTH_FAIL) {
                    ESP_LOGW(TAG, "Wrong password (reason %d)", d->reason);
                    sc_restart();
                } else if (s_retry < MAX_WIFI_RETRY) {
                    s_retry++;
                    ESP_LOGW(TAG, "Transient fail (reason %d), retry %d/%d",
                             d->reason, s_retry, MAX_WIFI_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "SC verify failed after retry");
                    sc_restart();
                }
                break;

            case CSTATE_SAVED:
            case CSTATE_CONNECTED:
                if (d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    d->reason == WIFI_REASON_AUTH_FAIL) {
                    ESP_LOGW(TAG, "Saved creds invalid (reason %d)", d->reason);
                    led_set_pattern(LED_BLINK_ERROR);
                    sc_restart();
                } else if (s_retry < MAX_WIFI_RETRY) {
                    s_retry++;
                    ESP_LOGI(TAG, "Reconnect %d/%d (reason %d)",
                             s_retry, MAX_WIFI_RETRY, d->reason);
                    led_set_pattern(LED_BLINK_SLOW);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "Connect failed, fallback to SC");
                    led_set_pattern(LED_BLINK_ERROR);
                    sc_restart();
                }
                break;

            default:
                break;
            }
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;

        if (s_state == CSTATE_SC_VERIFY) {
            wifi_config_t cfg = {0};
            esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg);
            esp_err_t e = creds_save(cfg.sta.ssid, cfg.sta.password);
            if (e == ESP_OK)
                ESP_LOGI(TAG, "Credentials saved");
            else
                ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(e));
            if (s_ack_sent)
                esp_smartconfig_stop();
        }

        led_set_pattern(LED_OFF);
        s_state = CSTATE_CONNECTED;
        xEventGroupSetBits(s_evt_grp, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Online");

        ntp_start();
        mdns_start();

    } else if (base == SC_EVENT) {
        if (id == SC_EVENT_SEND_ACK_DONE) {
            s_ack_sent = true;
            if (s_state == CSTATE_CONNECTED)
                esp_smartconfig_stop();

        } else if (id == SC_EVENT_GOT_SSID_PSWD) {
            if (s_state == CSTATE_SC_VERIFY || s_state == CSTATE_CONNECTED)
                return;
            smartconfig_event_got_ssid_pswd_t* evt =
                (smartconfig_event_got_ssid_pswd_t*)data;

            wifi_config_t cfg = {0};
            cfg.sta.bssid_set = evt->bssid_set;
            memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
            memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
            if (evt->bssid_set)
                memcpy(cfg.sta.bssid, evt->bssid, sizeof(cfg.sta.bssid));

            if (evt->type == SC_TYPE_ESPTOUCH_V2) {
                uint8_t rvd[33] = {0};
                esp_smartconfig_get_rvd_data(rvd, sizeof(rvd));
                ESP_LOGI(TAG, "RVD: %s", rvd);
            }

            ESP_LOGI(TAG, "SC creds for SSID: %.32s, verifying...",
                     (const char*)cfg.sta.ssid);

            s_state = CSTATE_SC_VERIFY;
            s_retry = 0;

            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg));
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
    }
}

void wifi_init(void) {
    s_evt_grp = xEventGroupCreate();

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

    wifi_config_t saved = {0};
    if (creds_load(&saved) == ESP_OK) {
        s_state = CSTATE_SAVED;
        led_set_pattern(LED_BLINK_SLOW);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &saved));
        ESP_LOGI(TAG, "Saved credentials loaded");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    esp_timer_create_args_t ta = {
        .callback = &sc_timer_cb,
        .name = "sc_rstrt",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_sc_timer));
}

EventGroupHandle_t wifi_get_event_group(void) {
    return s_evt_grp;
}
