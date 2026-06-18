#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lwip/api.h"

#include "gpio.h"
#include "protocol.h"
#include "tcp.h"
#include "wifi.h"

#define STACK 4096
#define PRIO  4
#define PORT  8080
#define TIMEOUT_MS 5000
#define CLIENT_TIMEOUT_MS 2000

static void handle_client(struct netconn* cl) {
    struct netbuf* buf;
    netconn_set_recvtimeout(cl, CLIENT_TIMEOUT_MS);

    while (netconn_recv(cl, &buf) == ERR_OK) {
        void* d;
        u16_t len;
        netbuf_data(buf, &d, &len);
        if (len > 0) {
            protocol_process((uint8_t*)d, len);
            uint8_t reply[2] = {
                relay_get() ? STATE_RELAY_ON : STATE_RELAY_OFF,
                led_get()   ? STATE_LED_ON   : STATE_LED_OFF,
            };
            netconn_write(cl, reply, sizeof(reply), NETCONN_COPY);
        }
        netbuf_delete(buf);
    }
}

static void task(void* arg) {
    EventGroupHandle_t eg = wifi_get_event_group();

    while (1) {
        xEventGroupWaitBits(eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                            portMAX_DELAY);

        struct netconn* srv = netconn_new(NETCONN_TCP);
        if (!srv) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (netconn_bind(srv, NULL, PORT) == ERR_OK) {
            netconn_listen(srv);
            netconn_set_recvtimeout(srv, TIMEOUT_MS);
            while (xEventGroupGetBits(eg) & WIFI_CONNECTED_BIT) {
                struct netconn* cl;
                if (netconn_accept(srv, &cl) == ERR_OK) {
                    handle_client(cl);
                    netconn_close(cl);
                    netconn_delete(cl);
                }
            }
        }

        netconn_close(srv);
        netconn_delete(srv);
    }
}

void tcp_server_start(void) {
    xTaskCreate(task, "tcp", STACK, NULL, PRIO, NULL);
}
