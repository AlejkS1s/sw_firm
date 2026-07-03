# switch_firm

ESP8266 firmware for a WiFi-controlled relay/switch. Built with ESP-IDF v3.4 (ESP8266_RTOS_SDK).

## Features

- **Relay control** via GPIO0 (active-low) — switch AC/DC loads
- **LED indicator** via GPIO2 (active-low)
- **WiFi** — connects to saved credentials (NVS), falls back to EspTouch SmartConfig
- **HTTP server** on port 80 — JSON API with CORS, one-shot timer, daily schedule engine
- **LED blink system** — 5 software-defined patterns (on/off/slow/fast/error) via `esp_timer`

## HTTP API (port 80)

| Method | Endpoint              | Body                          | Response                    |
|--------|-----------------------|-------------------------------|-----------------------------|
| GET    | `/state`              | —                             | `{"relay":bool,"led":bool,"timer":bool,"timer_rem":0,"time":bool}` |
| GET    | `/info`               | —                             | `{"mac":"XX:XX:XX:XX:XX:XX"}` |
| POST   | `/on`                 | —                             | state JSON                  |
| POST   | `/off`                | —                             | state JSON                  |
| POST   | `/toggle`             | —                             | state JSON                  |
| POST   | `/timer`              | `{"s":300,"on":true}`         | state JSON                  |
| POST   | `/timer/cancel`       | —                             | state JSON                  |
| GET    | `/schedules`          | —                             | `{"s":[{"i":0,"h":8,"m":0,"o":true,"d":62,"e":true}]}` |
| POST   | `/schedules`          | `{"h":8,"m":0,"on":true,"d":62}` | `{"ok":true}`           |
| POST   | `/schedules/remove`   | `{"i":0}`                     | `{"ok":true}`               |

All POST handlers return the new state after mutation. CORS enabled (`Access-Control-Allow-Origin: *`). `d` (days) is a bitmask: bit0=Sun … bit6=Sat.

## Build & Flash

```bash
# Set up the ESP8266_RTOS_SDK environment, then:
make PYTHON=/path/to/python -j$(nproc)
make flash PYTHON=/path/to/python
make monitor PYTHON=/path/to/python
```

Default flash port: `/dev/ttyUSB0`. Serial console is disabled (CONFIG_LOG_DEFAULT_LEVEL_NONE).

## Usage

```bash
# Get state
curl http://<device-ip>/state

# Relay ON
curl -X POST http://<device-ip>/on

# Relay OFF
curl -X POST http://<device-ip>/off

# Toggle
curl -X POST http://<device-ip>/toggle

# One-shot timer: turn relay on after 300s
curl -X POST -d '{"s":300,"on":true}' http://<device-ip>/timer

# Cancel timer
curl -X POST http://<device-ip>/timer/cancel

# Get schedules
curl http://<device-ip>/schedules

# Add schedule: weekdays 08:00 relay on
curl -X POST -d '{"h":8,"m":0,"on":true,"d":62}' http://<device-ip>/schedules

# Remove schedule (id from /schedules)
curl -X POST -d '{"i":0}' http://<device-ip>/schedules/remove

# Get device info (MAC)
curl http://<device-ip>/info
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
