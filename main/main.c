#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"

#define PIN_RELAY GPIO_NUM_0
#define PIN_LED GPIO_NUM_2

#define UART_PORT UART_NUM_0
#define UART_BAUD 74880
#define UART_BUF_SIZE 256
#define UART_QUEUE_DEPTH 10

#define TCP_PORT        8080
#define TCP_TIMEOUT_MS 5000
#define CLIENT_TIMEOUT_MS 2000
#define MAX_WIFI_RETRY 5

#define UART_TASK_STACK 1536
#define UART_TASK_PRIO 5
#define SC_TASK_STACK 2560
#define SC_TASK_PRIO 3
#define TCP_TASK_STACK 4096
#define TCP_TASK_PRIO 4

#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SC_DONE_BIT BIT1

static const char* TAG_SYS = "system";
static const char* TAG_WIFI = "wifi";
static const char* TAG_NET = "tcp_srv";

typedef enum {
  CMD_RELAY_OFF = 0x01,
  CMD_RELAY_ON = 0x02,
  CMD_LED_ON = 0x05,
  CMD_LED_OFF = 0x06
} protocol_cmd_t;

typedef enum {
  STATE_RELAY_OFF = 0xA1,
  STATE_RELAY_ON = 0xA2,
  STATE_LED_OFF = 0xB1,
  STATE_LED_ON = 0xB2
} protocol_state_t;

static QueueHandle_t s_uart_queue;
static EventGroupHandle_t s_wifi_event_group;
static TaskHandle_t s_sc_task_handle = NULL;
static bool s_has_creds = false;
static int s_retry_num = 0;

static bool s_relay_active = false;
static bool s_led_active = false;

static void hardware_init(void);
static void set_relay(bool active);
static void set_led(bool active);
static void process_commands(const uint8_t* buf, size_t len);
static void network_init(void);
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id,
                               void* data);
static esp_err_t load_creds(wifi_config_t* out_cfg);
static esp_err_t persist_creds(const uint8_t* ssid, const uint8_t* password);
static void uart_event_task(void* arg);
static void tcp_server_task(void* arg);
static void smartconfig_task(void* arg);
static void handle_client_connection(struct netconn* client_conn);

void app_main(void) {
    ESP_LOGI(TAG_SYS, "Firmware starting...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    hardware_init();
    network_init();

    xTaskCreate(tcp_server_task, "tcp_srv", TCP_TASK_STACK, NULL, TCP_TASK_PRIO, NULL);

    ESP_LOGI(TAG_SYS, "Init complete.");
}

static void hardware_init(void) {
  const gpio_config_t io = {
      .pin_bit_mask = BIT(PIN_RELAY) | BIT(PIN_LED),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));

  set_relay(false);
  set_led(false);

  uart_config_t cfg = {
      .baud_rate = UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  };
  ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
  ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE,
                                      UART_QUEUE_DEPTH, &s_uart_queue, 0));

  xTaskCreate(uart_event_task, "uart_ev", UART_TASK_STACK, NULL, UART_TASK_PRIO, NULL);
  ESP_LOGI(TAG_SYS, "Hardware OK.");
}

static void network_init(void) {
  s_wifi_event_group = xEventGroupCreate();

  tcpip_adapter_init();

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t saved_cfg = {0};
  if (load_creds(&saved_cfg) == ESP_OK) {
    s_has_creds = true;
    esp_wifi_set_config(ESP_IF_WIFI_STA, &saved_cfg);
  }

  ESP_ERROR_CHECK(esp_wifi_start());
}

static void set_relay(bool active) {
  taskENTER_CRITICAL();
  s_relay_active = active;
  gpio_set_level(PIN_RELAY, active ? 0 : 1);
  taskEXIT_CRITICAL();
}

static void set_led(bool active) {
  taskENTER_CRITICAL();
  s_led_active = active;
  gpio_set_level(PIN_LED, active ? 0 : 1);
  taskEXIT_CRITICAL();
}

static void process_commands(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    switch (buf[i]) {
      case CMD_RELAY_OFF:
        set_relay(false);
        break;
      case CMD_RELAY_ON:
        set_relay(true);
        break;
      case CMD_LED_ON:
        set_led(true);
        break;
      case CMD_LED_OFF:
        set_led(false);
        break;
      default:
        break;
    }
  }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id,
                               void* data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_START) {
      if (s_has_creds) {
        esp_wifi_connect();
      } else if (s_sc_task_handle == NULL) {
        xTaskCreate(smartconfig_task, "sc_task", SC_TASK_STACK, NULL,
                    SC_TASK_PRIO, &s_sc_task_handle);
      }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

      if (s_has_creds && s_retry_num < MAX_WIFI_RETRY) {
        s_retry_num++;
        ESP_LOGI(TAG_WIFI, "Retrying station connection... (%d/%d)",
                 s_retry_num, MAX_WIFI_RETRY);
        esp_wifi_connect();
      } else if (s_sc_task_handle == NULL) {
        ESP_LOGW(TAG_WIFI,
                 "Max retries reached or missing credentials. Starting "
                 "SmartConfig fallback.");
        s_retry_num = 0;
        xTaskCreate(smartconfig_task, "sc_task", SC_TASK_STACK, NULL,
                    SC_TASK_PRIO, &s_sc_task_handle);
      }
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG_WIFI, "IP Network lease bound successfully.");
  } else if (base == SC_EVENT) {
    if (id == SC_EVENT_GOT_SSID_PSWD) {
      smartconfig_event_got_ssid_pswd_t* evt =
          (smartconfig_event_got_ssid_pswd_t*)data;
      wifi_config_t cfg = {.sta = {.bssid_set = evt->bssid_set}};

      memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
      memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
      if (evt->bssid_set)
        memcpy(cfg.sta.bssid, evt->bssid, sizeof(cfg.sta.bssid));

      persist_creds(evt->ssid, evt->password);
      s_has_creds = true;
      s_retry_num = 0;

      esp_wifi_disconnect();
      esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
      esp_wifi_connect();
    } else if (id == SC_EVENT_SEND_ACK_DONE) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_SC_DONE_BIT);
    }
  }
}

static esp_err_t load_creds(wifi_config_t* out_cfg) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (err != ESP_OK) return err;

  size_t s_len = sizeof(out_cfg->sta.ssid);
  size_t p_len = sizeof(out_cfg->sta.password);

  err = nvs_get_str(h, NVS_KEY_SSID, (char*)out_cfg->sta.ssid, &s_len);
  if (err == ESP_OK)
    err = nvs_get_str(h, NVS_KEY_PASS, (char*)out_cfg->sta.password, &p_len);

  nvs_close(h);
  return err;
}

static esp_err_t persist_creds(const uint8_t* ssid, const uint8_t* password) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_str(h, NVS_KEY_SSID, (const char*)ssid);
  if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, (const char*)password);
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

static void smartconfig_task(void* arg) {
  ESP_ERROR_CHECK(esp_smartconfig_set_type(CONFIG_ESP_SMARTCONFIG_TYPE));
  smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_smartconfig_start(&sc_cfg));

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_SC_DONE_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_SC_DONE_BIT) {
    ESP_LOGI(TAG_WIFI, "SmartConfig verification handshake exchanged.");
  }

  esp_smartconfig_stop();
  s_sc_task_handle = NULL;
  vTaskDelete(NULL);
}

static void uart_event_task(void* arg) {
  uart_event_t event;
  uint8_t* buf = (uint8_t*)malloc(UART_BUF_SIZE);
  configASSERT(buf);

  while (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) == pdTRUE) {
    if (event.type == UART_DATA) {
      int len = uart_read_bytes(UART_PORT, buf, event.size, portMAX_DELAY);
      if (len > 0) process_commands(buf, len);
    } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
      ESP_LOGW(TAG_SYS, "UART boundary overrun. Flushing driver lines.");
      uart_flush_input(UART_PORT);
      xQueueReset(s_uart_queue);
    }
  }

  free(buf);
  vTaskDelete(NULL);
}

static void handle_client_connection(struct netconn* client_conn) {
  struct netbuf* inbuf;
  netconn_set_recvtimeout(client_conn, CLIENT_TIMEOUT_MS);

  while (netconn_recv(client_conn, &inbuf) == ERR_OK) {
    void* data;
    u16_t len;
    netbuf_data(inbuf, &data, &len);
    if (len > 0) {
      process_commands((uint8_t*)data, len);
      uint8_t state[2];
      taskENTER_CRITICAL();
      state[0] = s_relay_active ? STATE_RELAY_ON : STATE_RELAY_OFF;
      state[1] = s_led_active ? STATE_LED_ON : STATE_LED_OFF;
      taskEXIT_CRITICAL();
      netconn_write(client_conn, state, sizeof(state), NETCONN_COPY);
    }
    netbuf_delete(inbuf);
  }
}

static void tcp_server_task(void* arg) {
  while (1) {
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    struct netconn* conn = netconn_new(NETCONN_TCP);
    if (!conn) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (netconn_bind(conn, NULL, TCP_PORT) == ERR_OK) {
      netconn_listen(conn);
      netconn_set_recvtimeout(conn, TCP_TIMEOUT_MS);
      while (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
        struct netconn* newconn;
        if (netconn_accept(conn, &newconn) == ERR_OK) {
          handle_client_connection(newconn);
          netconn_close(newconn);
          netconn_delete(newconn);
        }
      }
    }
    netconn_close(conn);
    netconn_delete(conn);
  }
}
