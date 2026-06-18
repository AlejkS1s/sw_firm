#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0

void wifi_init(void);
EventGroupHandle_t wifi_get_event_group(void);
