# switch_firm

ESP8266 firmware for a WiFi-controlled relay/switch. Built with ESP-IDF v3.4 (ESP8266_RTOS_SDK).

## Features

- **Relay control** via GPIO3 (active-low) — switch AC/DC loads
- **LED indicator** via GPIO2 (active-low) — 6 programmable blink patterns
- **Button** on GPIO0 — short press toggles relay, long press (5s) clears WiFi and enters SmartConfig
- **WiFi** — connects to saved credentials (NVS, XOR-obfuscated with MAC), falls back to EspTouch SmartConfig
- **HTTP server** on port 80 — full JSON API with CORS, unified routines engine, boot/led configuration
- **LED blink system** — 6 software-defined patterns (on/off/slow/fast/error/ok) via `esp_timer`
- **NTP sync** — hardcoded IP (time.google.com), persists epoch across reboots, routines deferred until sync
- **mDNS** — advertised as `switchiot.local`
- **State acode** — 3-digit code encoding relay/routine state for lightweight polling
- **Routines engine** — schedule, countdown, circulate, and iching modes in a single unified system

## Module Layout

| File | Responsibility |
|------|---------------|
| `main.c` | Boot sequence, main loop |
| `board.c/h` | Relay/LED/Button GPIO control, LED patterns, boot behavior, NVS persistence |
| `connection.c/h` | STA connection, SmartConfig provisioning, exponential backoff, event handling |
| `timing.c/h` | Time persistence, timezone, NTP health check |
| `routines.c/h` | All behavior modes: schedule, countdown, circulate, iching |
| `http_server.c/h` | HTTP server port 80, unified handler, JSON serialization |
| `nvs_store.c/h` | Centralized NVS wrapper |
| `power.c/h` | Power management (modem sleep when no HTTP clients) |
| `state.c/h` | Acode + state version counter from relay/routine state |

## HTTP API (port 80)

| Method | Endpoint              | Body                          | Response                    |
|--------|-----------------------|-------------------------------|-----------------------------|
| GET    | `/state`              | —                             | `{"relay":bool,"led":bool,"timer":bool,"timer_rem":0,"timer_total":0,"time":bool,"boot":0,"led_mode":1,"acode":0,"uptime":0,"circulate":false,"iching":false}` |
| GET    | `/info`               | —                             | `{"mac":"XX:XX:XX:XX:XX:XX","relay":...,"led":...,"timer":...,"timer_rem":0,"timer_total":0,"time":...,"boot":0,"led_mode":1,"ssid":"...","rssi":-50,"ip":"...","acode":0,"uptime":0,"heap":12345,"fw_ver":"v1.0","circulate":false,"iching":false}` |
| GET    | `/check?a=N`         | —                             | `{"ok":true}` or full state if acode changed |
| GET    | `/routines`           | —                             | `{"routines":[{"i":int,"t":int,"h":int,"m":int,"eh":int,"em":int,"ion":int,"ioff":int,"dur":int,"d":int,"on":bool,"e":bool}]}` |
| POST   | `/on`                 | —                             | `{"relay":true,"acode":1}`    |
| POST   | `/off`                | —                             | `{"relay":false,"acode":0}`   |
| POST   | `/toggle`             | —                             | `{"relay":bool,"acode":int}`  |
| POST   | `/routines`           | `{"t":int,"h":int,"m":int,"eh":int,"em":int,"ion":int,"ioff":int,"dur":int,"d":int,"on":bool}` | `{"ok":bool,"acode":int}` |
| POST   | `/routines/remove`    | `{"i":int}`                   | `{"ok":true,"acode":int}`    |
| POST   | `/boot`               | `{"mode":0\|1\|2}`            | `{"ok":true,"acode":int}`    |
| POST   | `/led`                | `{"mode":0..15}`              | `{"ok":true,"acode":int}`    |

All POST handlers return `acode` encoding combined device state. CORS enabled (`Access-Control-Allow-Origin: *`). `d` (days) is a bitmask: bit0=Sun … bit6=Sat. Routine type `t`: 0=schedule, 1=countdown, 2=circulate, 3=iching.

**Boot modes:** 0=OFF, 1=ON, 2=AUTO (restore last saved state)

**LED mode bitmask:** bit0=Relay, bit1=WiFi, bit2=Timer, bit3=Schedule

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

# Get all routines
curl http://<device-ip>/routines

# Add schedule: weekdays 08:00 relay on
curl -X POST -d '{"t":0,"h":8,"m":0,"on":true,"d":62}' http://<device-ip>/routines

# Add countdown: turn relay on after 300s
curl -X POST -d '{"t":1,"dur":300,"on":true}' http://<device-ip>/routines

# Add circulate: cycle 30s on / 30s off, weekdays 09:00-17:00
curl -X POST -d '{"t":2,"h":9,"m":0,"eh":17,"em":0,"ion":30,"ioff":30,"d":62}' http://<device-ip>/routines

# Add iching: turn relay off at 22:00 weekdays
curl -X POST -d '{"t":3,"h":22,"m":0,"d":62}' http://<device-ip>/routines

# Remove routine (id from /routines)
curl -X POST -d '{"i":0}' http://<device-ip>/routines/remove

# Get device info (MAC, SSID, RSSI, IP)
curl http://<device-ip>/info

# Poll state (returns {ok:true} if unchanged)
curl http://<device-ip>/check?a=0

# Set boot behavior: 0=OFF, 1=ON, 2=AUTO
curl -X POST -d '{"mode":2}' http://<device-ip>/boot

# Set LED mode: bitmask 0..15
curl -X POST -d '{"mode":3}' http://<device-ip>/led
```

## First-time setup

On first boot with no saved WiFi credentials, the device starts EspTouch SmartConfig. Use the **Espressif Esptouch** app (Android / iOS) to configure WiFi without a serial connection.

Saved credentials persist in NVS across reboots. To clear them and re-enter SmartConfig mode, erase the NVS partition:

```bash
python -m esptool erase_region 0x9000 0x6000
```

## Hardware

| Pin   | Function    | Notes                      |
|-------|-------------|----------------------------|
| GPIO0 | Button      | Active-low, internal pull-up |
| GPIO2 | LED         | Active-low                 |
| GPIO3 | Relay       | Active-low                 |
| UART0 | Console     | 74880 baud, 8N1            |

GPIO3 is the relay control line — active-low: GPIO3 LOW = relay ON. GPIO0 is the ESP8266 strapping pin — the button circuit must not pull it low during power-on, otherwise the chip enters flash download mode. The internal pull-up on GPIO0 handles this.
