#include "gpio.h"
#include "protocol.h"

void protocol_process(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (buf[i]) {
            case CMD_RELAY_OFF: relay_set(false); break;
            case CMD_RELAY_ON:  relay_set(true);  break;
            case CMD_LED_ON:    led_set(true);    break;
            case CMD_LED_OFF:   led_set(false);   break;
        }
    }
}
