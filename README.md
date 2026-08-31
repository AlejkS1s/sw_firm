# switch_firm

ESP8266 firmware for a WiFi-controlled relay/switch. Built with ESP-IDF v3.4 (ESP8266_RTOS_SDK).

## Features

- **Relay control** via GPIO3 (active-low) — switch AC/DC loads
- **LED indicator** via GPIO2 (active-low) — 6 blink patterns, bitmask-driven modes
- **Button** on GPIO0 — short press toggles relay, long press (5s) clears WiFi and enters SmartConfig
- **WiFi** — connects to saved credentials (NVS, XOR-obfuscated with MAC), falls back to EspTouch SmartConfig
- **HTTP server** on port 80 — full JSON API with CORS, routines engine, boot/led/timezone configuration
- **LED blink system** — 6 software-defined patterns (on/off/slow/fast/error/ok) via `esp_timer`
- **NTP sync** — hardcoded IP (time.google.com), persists epoch across reboots, deferred routines activation
- **mDNS** — advertised as `switchiot.local`
- **State version + snapshot** — version counter for SSE event IDs, FNV-1a hash for ETag-based conditional polling
- **Routines engine** — schedule and circulate modes in a unified system (event-driven FreeRTOS task)
- **Countdown timer** — independent 1-second `esp_timer` with NVS crash recovery
- **Auto-off (Inching)** — timer-based relay auto turn-off, disables while countdown/routines are active

## Module Layout

| File | Responsibility |
|------|---------------|
| `main.c` | Boot sequence, task spawn |
| `board.c/h` | Relay/LED/Button GPIO control, LED patterns, boot behavior, auto-off, NVS persistence |
| `connection.c/h` | STA connection, SmartConfig provisioning, exponential backoff, event handling |
| `countdown.c/h` | Independent countdown timer, NVS persistence, 1s tick (also drives auto-off check) |
| `timing.c/h` | Time persistence, timezone, NTP health check |
| `routines.c/h` | Behavior modes: schedule (1) and circulate (2) |
| `diagnostics.c/h` | System diagnostics (reset reason, boot count, heap watermarks) |
| `http/http_server.c/h` | HTTP server port 80, route table, CORS dispatcher |
| `http/http_handlers_state.c` | GET state / system / state/stream / ping |
| `http/http_handlers_control.c` | POST relay, PUT config/*, timer CRUD |
| `http/http_handlers_routines.c` | Routines CRUD |
| `http/http_util.c/h` | Shared HTTP utilities (JSON helpers, body parse, error envelopes) |
| `sse.c/h` | Server-Sent Events push module |
| `nvs_store.c/h` | Centralized NVS wrapper (no direct nvs_open in business logic) |
| `power.c/h` | Power management (modem sleep when no HTTP clients) |
| `state.c/h` | State version counter, snapshot building, FNV-1a hashing |

> IPC primitives (queues, event groups, mutexes) are owned per-module — there
> is no central `ipc.c`. `board.c` owns the actuator queue, `connection.c` owns
> the net queue, `state.c` owns the state event group.

## HTTP API (port 80, namespace `/api/v1/`)

All endpoints live under `/api/v1/`. CORS is enabled (`Access-Control-Allow-Origin: *`) with OPTIONS preflight for mutation endpoints. See `../docs/http_api.md` for the full reference.

| Method | Endpoint              | Body                          | Response                    |
|--------|-----------------------|-------------------------------|-----------------------------|
| GET    | `/api/v1/state`       | —                             | control + countdown + auto-off snapshot (ETag-cached) |
| GET    | `/api/v1/state/stream`| —                             | SSE stream (full state push on change, 15s heartbeat) |
| GET    | `/api/v1/system`      | —                             | device identity + diagnostics + WiFi info |
| POST   | `/api/v1/system/reset`| —                             | `{"ok":true,"message":"rebooting"}` |
| POST   | `/api/v1/system/factory-reset`| —                       | `{"ok":true,"message":"factory reset, rebooting"}` |
| GET    | `/api/v1/ping`        | —                             | `{"ok":true}`                 |
| POST   | `/api/v1/relay`       | `{"action":"on"\|"off"\|"toggle"}` | `{"relay":bool}` |
| PUT    | `/api/v1/config/boot`       | `{"mode":0\|1\|2}`              | `{"ok":true}` |
| PUT    | `/api/v1/config/led`        | `{"mode":0..127}`               | `{"ok":true}` |
| PUT    | `/api/v1/config/timezone`   | `{"tz":"COT5"}`                 | `{"ok":true}` |
| PUT    | `/api/v1/config/power-save` | `{"disabled":bool}`             | `{"ok":true}` |
| PUT    | `/api/v1/config/auto-off`   | `{"enabled":bool,"h":int,"m":int,"s":int}` | `{"ok":true}` |
| POST   | `/api/v1/timer`       | `{"dur":int,"on":bool}`       | `{"ok":true}` |
| DELETE | `/api/v1/timer`       | —                             | `{"ok":true}` |
| GET    | `/api/v1/routines`    | —                             | `[{...}]` (full array) or `?id=N` for one entry |
| POST   | `/api/v1/routines`    | `{"t":int,"h":int,"m":int,"eh":int,"em":int,"ion":int,"ioff":int,"dur":int,"d":int,"on":bool}` | `{"ok":true,"i":int}` |
| PUT    | `/api/v1/routines?id=N` | any subset (all fields optional), e.g. `{"en":false}` | `{"ok":true}` |
| DELETE | `/api/v1/routines?id=N`| —                            | `{"ok":true}` |

CORS enabled (`Access-Control-Allow-Origin: *`). `d` (days) is a bitmask: bit0=Sun … bit6=Sat. Routine type `t`: 1=schedule, 2=circulate.

Routines carry an **enabled** flag (`en` in PUT bodies, `e` in GET responses). Disabled routines are skipped by the scheduler. `PUT /api/v1/routines?id=N` with `{"en":false}` disables, `{"en":true}` re-enables; it also serves as the edit endpoint (any subset of fields, type `t` immutable).

**Boot modes:** 0=OFF, 1=ON, 2=AUTO (restore last saved state)

**LED mode bitmask:** bit0=Relay, bit1=WiFi, bit2=Timer, bit3=Schedule, bit4=Any routine, bit5=Circulate, bit6=Auto-off

## Build & Flash

```bash
# Build (Python venv path hardcoded in Makefile)
make -C sw_firm -j$(nproc)

# Flash & monitor
make -C sw_firm flash monitor
```

Default flash port: `/dev/ttyUSB0`. Serial console is disabled (`CONFIG_LOG_DEFAULT_LEVEL_NONE`).

## Usage

```bash
# Get control/observable state (ETag-cached)
curl http://<device-ip>/api/v1/state

# Device identity + diagnostics + WiFi info
curl http://<device-ip>/api/v1/system

# Relay ON / OFF / TOGGLE
curl -X POST -H 'Content-Type: application/json' -d '{"action":"on"}'   http://<device-ip>/api/v1/relay
curl -X POST -H 'Content-Type: application/json' -d '{"action":"off"}'  http://<device-ip>/api/v1/relay
curl -X POST -H 'Content-Type: application/json' -d '{"action":"toggle"}' http://<device-ip>/api/v1/relay

# List routines
curl http://<device-ip>/api/v1/routines

# Get a single routine by index
curl http://<device-ip>/api/v1/routines?id=0

# Add schedule: weekdays 08:00 relay on  (t=1 schedule)
curl -X POST -H 'Content-Type: application/json' -d '{"t":1,"h":8,"m":0,"on":true,"d":62}' http://<device-ip>/api/v1/routines

# Add circulate: cycle 30s on / 30s off, weekdays 09:00-17:00  (t=2 circulate)
curl -X POST -H 'Content-Type: application/json' -d '{"t":2,"h":9,"m":0,"eh":17,"em":0,"ion":30,"ioff":30,"d":62}' http://<device-ip>/api/v1/routines

# Remove routine (index from /api/v1/routines)
curl -X DELETE 'http://<device-ip>/api/v1/routines?id=0'

# Set countdown: turn relay on after 300s
curl -X POST -H 'Content-Type: application/json' -d '{"dur":300,"on":true}' http://<device-ip>/api/v1/timer

# Cancel countdown
curl -X DELETE http://<device-ip>/api/v1/timer

# Set auto-off: turn relay off at 00:30:00  (PUT under /config/auto-off)
curl -X PUT -H 'Content-Type: application/json' -d '{"enabled":true,"h":0,"m":30,"s":0}' http://<device-ip>/api/v1/config/auto-off

# Clear auto-off
curl -X PUT -H 'Content-Type: application/json' -d '{"enabled":false}' http://<device-ip>/api/v1/config/auto-off

# Set timezone
curl -X PUT -H 'Content-Type: application/json' -d '{"tz":"COT5"}' http://<device-ip>/api/v1/config/timezone

# Set boot behavior: 0=OFF, 1=ON, 2=AUTO
curl -X PUT -H 'Content-Type: application/json' -d '{"mode":2}' http://<device-ip>/api/v1/config/boot

# Set LED mode: bitmask 0..127
curl -X PUT -H 'Content-Type: application/json' -d '{"mode":3}' http://<device-ip>/api/v1/config/led

# Toggle power-save (modem sleep)
curl -X PUT -H 'Content-Type: application/json' -d '{"disabled":false}' http://<device-ip>/api/v1/config/power-save

# Reboot
curl -X POST http://<device-ip>/api/v1/system/reset
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
