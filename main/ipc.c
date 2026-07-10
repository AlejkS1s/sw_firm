#include "ipc.h"

/* Shared IPC object storage. Initialized to NULL so any early reference
 * before ipc_init() is safe. */
QueueHandle_t      g_actuator_q     = NULL;
QueueHandle_t      g_routines_q     = NULL;
QueueHandle_t      g_net_q          = NULL;
EventGroupHandle_t g_net_evt        = NULL;
SemaphoreHandle_t  g_relay_mutex    = NULL;
SemaphoreHandle_t  g_routines_mutex = NULL;
TaskHandle_t       g_actuator_task  = NULL;
TaskHandle_t       g_routines_task  = NULL;
TaskHandle_t       g_net_task       = NULL;

void ipc_init(void) {
    g_actuator_q    = xQueueCreate(8, sizeof(actuator_msg_t));
    g_routines_q    = xQueueCreate(8, sizeof(routines_msg_t));
    g_net_q         = xQueueCreate(4, sizeof(net_msg_t));
    g_net_evt       = xEventGroupCreate();
    g_relay_mutex   = xSemaphoreCreateMutex();
    g_routines_mutex = xSemaphoreCreateMutex();
}
