#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#include "board.h"      /* led_conf_t */
#include "routines.h"   /* routine_entry_t */

/* ── Event group bits (g_net_evt) ────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0

/* ── Actuator commands (relay / LED / boot / tz) ─────────────────────────── */
typedef enum {
    CMD_TURN_ON = 0,
    CMD_TURN_OFF,
    CMD_TOGGLE,
    CMD_SET_LED,
    CMD_SET_LED_PATTERN,
    CMD_SET_BOOT,
    CMD_SET_TZ,
    CMD_LED_UPDATE,
} actuator_cmd_type_t;

#define IPC_TZ_MAX 32

typedef struct {
    actuator_cmd_type_t type;
    uint8_t   led_mode;      /* CMD_SET_LED */
    uint8_t   boot_mode;     /* CMD_SET_BOOT */
    led_conf_t led_pattern;  /* CMD_SET_LED_PATTERN */
    char      tz[IPC_TZ_MAX];/* CMD_SET_TZ */
    SemaphoreHandle_t ack;   /* NULL = fire-and-forget */
    esp_err_t result;        /* set by actuator task */
} actuator_msg_t;

/* ── Routines commands (table ownership) ─────────────────────────────────── */
typedef enum {
    RCMD_ADD = 0,
    RCMD_REMOVE,
    RCMD_ARM,
} routines_cmd_type_t;

typedef struct routines_msg_s {
    routines_cmd_type_t type;
    routine_entry_t entry;   /* RCMD_ADD */
    uint8_t  id;             /* RCMD_REMOVE */
    SemaphoreHandle_t ack;
    esp_err_t result;
} routines_msg_t;

/* ── Net commands (WiFi / NTP orchestration) ─────────────────────────────── */
typedef enum {
    NCMD_RECONNECT = 0,
} net_cmd_type_t;

typedef struct {
    net_cmd_type_t type;
    SemaphoreHandle_t ack;
} net_msg_t;

/* ── Shared IPC handles (created in ipc_init()) ──────────────────────────── */
extern QueueHandle_t     g_actuator_q;
extern QueueHandle_t     g_routines_q;
extern QueueHandle_t     g_net_q;
extern EventGroupHandle_t g_net_evt;
extern SemaphoreHandle_t  g_relay_mutex;
extern SemaphoreHandle_t  g_routines_mutex;
extern TaskHandle_t       g_actuator_task;
extern TaskHandle_t       g_routines_task;
extern TaskHandle_t       g_net_task;

/* Create all IPC objects (queues, event group, mutexes). Call once from
 * app_main after FreeRTOS is up but before spawning tasks. */
void ipc_init(void);
