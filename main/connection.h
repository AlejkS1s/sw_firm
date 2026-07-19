#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT  BIT0

void connection_init(void);
void wifi_init(void);
void wifi_reconnect(void);
bool wifi_is_connected(void);
