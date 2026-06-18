# switch_firm

ESP8266 firmware for a WiFi-controlled relay/switch. Built with ESP-IDF v3.4 (ESP8266_RTOS_SDK).

## Features

- **Relay control** via GPIO0 (active-low) — switch AC/DC loads
- **LED indicator** via GPIO2 (active-low)
- **WiFi** — connects to saved credentials (NVS), falls back to EspTouch SmartConfig
- **TCP server** on port 8080 — remote control over LAN
- **UART interface** on UART0 at 74880 baud — local serial control

## Protocol

Single-byte commands. On any command the device replies with a 2-byte state.

| Byte | Command            |
|------|--------------------|
| 0x01 | Relay OFF          |
| 0x02 | Relay ON           |
| 0x05 | LED ON             |
| 0x06 | LED OFF            |

Response format: `[relay_state] [led_state]`

| Byte | State              |
|------|--------------------|
| 0xA1 | Relay OFF          |
| 0xA2 | Relay ON           |
| 0xB1 | LED OFF            |
| 0xB2 | LED ON             |

Example: send `0x02` → device replies `0xA2 0xB1` (relay on, led off).

## Build & Flash

```bash
# Set up the ESP8266_RTOS_SDK environment, then:
make PYTHON=/path/to/python -j$(nproc)
make flash PYTHON=/path/to/python
make monitor PYTHON=/path/to/python
```

Default UART: `/dev/ttyUSB0` at 74880 baud (bootloader) / 74880 baud (app console).

## Usage

### TCP (over LAN)

```bash
# Relay ON
echo -n -e '\x02' | nc <device-ip> 8080

# Relay OFF
echo -n -e '\x01' | nc <device-ip> 8080

# LED ON
echo -n -e '\x05' | nc <device-ip> 8080

# LED OFF
echo -n -e '\x06' | nc <device-ip> 8080
```

### UART (serial)

```bash
python3 -c "
import serial
s = serial.Serial('/dev/ttyUSB0', 74880)
s.write(b'\x02')  # relay on
s.close()
"
```

## First-time setup

On first boot with no saved WiFi credentials, the device starts EspTouch SmartConfig. Use the **Espressif Esptouch** app (Android / iOS) to configure WiFi without a serial connection.

Saved credentials persist in NVS across reboots. To clear them and re-enter SmartConfig mode, erase the NVS partition:

```bash
python -m esptool erase_region 0x9000 0x6000
```

## Hardware

| Pin  | Function    | Notes                      |
|------|-------------|----------------------------|
| GPIO0 | Relay      | Active-low; external pull-up required for boot |
| GPIO2 | LED        | Active-low                 |
| UART0 | Console    | 74880 baud, 8N1            |

GPIO0 is the ESP8266 strapping pin — the relay circuit must not pull it low during power-on, otherwise the chip enters flash download mode. Use a pull-up resistor (10 kΩ to 3.3 V) on the relay control line.
