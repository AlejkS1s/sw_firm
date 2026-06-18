#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CMD_RELAY_OFF = 0x01,
    CMD_RELAY_ON  = 0x02,
    CMD_LED_ON    = 0x05,
    CMD_LED_OFF   = 0x06,
} protocol_cmd_t;

typedef enum {
    STATE_RELAY_OFF = 0xA1,
    STATE_RELAY_ON  = 0xA2,
    STATE_LED_OFF   = 0xB1,
    STATE_LED_ON    = 0xB2,
} protocol_state_t;

void protocol_process(const uint8_t* buf, size_t len);
