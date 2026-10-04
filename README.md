# IoT Dual-Node Challenge — Distributed Alarm System

Wirelessly-linked IoT nodes with a live
dashboard for observation, control and historical review. 



---

## What this is

A distributed alarm and access controller made of two nodes that keep cooperating with **no
router, no broker and no dashboard**, plus a platform that lets a human observe them, override
them, and review what happened.

| | **Node A — Access** | **Node B — Zone** |
|---|---|---|
| MCU | ESP32 DevKitC (ESP-WROOM-32) | ESP32 DevKitC (ESP-WROOM-32) |
| Primary sensor | PIR HC-SR501 — motion | HC-SR04 — ultrasonic distance |
| Secondary input | RC522 RFID reader — arm / disarm by card | — |
| Actuator | Servo SG90 — see open decision #1 | Active buzzer — alarm |
| Local display | OLED SSD1306 — node state on site | OLED SSD1306 |
| Status indicator | RGB LED: green normal · amber degraded · red peer lost | same |
| Timekeeping | from the peer / broker | DS3231 RTC — wall clock with no network |
| Radio | ESP-NOW (peer) + Wi-Fi (MQTT) | ESP-NOW (peer) + Wi-Fi (MQTT) |

Node A is the door. Node B is the protected room.

### Why two identical ESP32s

Both boards carry their own radio, so each node is an independent MQTT client and the dashboard
reaches either one directly, no node depends on the other to be visible. The firmware is one
codebase compiled into two binaries through a build flag, which keeps the protocol and the
control logic literally identical on both sides instead of drifting between two copies.
---

## Technology stack

| Layer | Technology | Why this one |
|---|---|---|
| Firmware language | **C++17**, Arduino framework on ESP32 | Required by the brief. Arduino as the hardware layer for the deadline; the control logic includes no Arduino header and compiles on the laptop |
/// Cambiar el plataformIO a espfl



| Build | **** | Two projects, one shared library directory, reproducible builds |
| Node ↔ node | **ESP-NOW** (unicast, registered peer) | Connectionless, no router and no broker; hardware delivery ACK; works with the infrastructure off |
| Node ↔ platform | **MQTT** over Wi-Fi, **Mosquitto** broker | Publish/subscribe, tolerant of intermittent links, Last Will and Testament for offline detection |
| Wire format, node ↔ node | **Packed binary struct**, ≤ 32 bytes | Compact on the constrained link; keeps a migration path to nRF24 (32-byte limit) open |
| Wire format, node ↔ platform | **JSON** | Human-readable, inspectable live during the evaluation |
| Local display | **I²C** — SSD1306 | Each node shows its own state without a laptop |
| Card reader | **SPI** — RC522 | Arm/disarm at the door |
| Real-time clock | **I²C** — DS3231 | Correct timestamps while offline |
| Platform server | **Python 3 · Flask · Flask-SocketIO · paho-mqtt** | Stack I have built before and can explain line by line |
| Storage | **SQLite** | Single file, no server to run, queryable with plain SQL during the evaluation |
| Dashboard | Server-rendered HTML + **WebSocket** push | No build step, no framework to justify |
| Host tests | **g++**, no framework | The pure control logic runs and is tested without hardware |

**Protocols actually demonstrated on hardware:** ESP-NOW, MQTT, I²C, SPI — plus UART for the
serial console. The application-layer message protocol is my own and is specified in
`docs/protocol.md`.

---

## Cross-node behavior

These paths run over ESP-NOW and must work with the dashboard closed and the router switched
off.  

| Trigger | Message | Effect on the peer |
|---|---|---|
| Node A detects motion (PIR) | `MOTION_DETECTED` | Node B raises its alert level and shortens its sampling period |
| Node A reads a valid card (RFID) | `SYSTEM_ARMED` / `SYSTEM_DISARMED` | Node B arms or disarms with it |
| Node B measures distance below threshold while armed | `INTRUSION_CONFIRMED` | Node A actuates the servo |
| Node B starts or stops the alarm | `ALARM_ON` / `ALARM_OFF` | Node A mirrors the state on its RGB LED and OLED |
| Either node misses 3 heartbeats from the other | `PEER_LOST` (internal) | Degraded mode, red LED, safe state |

---

## The platform

A single Python process (`platform/app.py`) that does four jobs:

1. **MQTT client.** Subscribes to every node topic — telemetry, state, acknowledgements,
   heartbeats, events and the LWT status topic.
2. **Authoritative state.** Holds the last reported state of both nodes. Every value shown in
   the UI comes from what the node reported, never from a command the dashboard sent.
3. **WebSocket push.** Flask-SocketIO pushes each update to the browser, so the view is live
   without polling.
4. **Logger.** Writes telemetry, events, commands and acknowledgements to SQLite as they arrive.

### What the dashboard shows and does

**Live view, per node:** current sensor readings, actuator state, FSM state, operating mode,
armed/disarmed, link status (node online, peer link up), last-seen timestamp, and the current
value of every controllable variable.

**Commands:** each controllable variable has a control that can be sent to **Node A only**,
**Node B only**, or **both at once** through the broadcast topic — mandatory constraint #4.

**Command feedback — this is mandatory constraint #6 and it is the part most easily faked.**
Every command gets a `cmd_id` and the UI shows one of four states for it:

| State | Meaning |
|---|---|
| **Pending** | Sent, no acknowledgement yet |
| **Confirmed** | Node replied `APPLIED` and reported the value now in effect |
| **Rejected** | Node replied with a reason — out of range, or refused because it is in AUTO mode |
| **Timed out** | No acknowledgement within 2 s |

The value displayed after a command is the `applied_value` the node reported back, which is not
always the value that was sent.

---

## Historical data

A node that loses its link keeps operating and keeps recording. When the link returns, those
records arrive late. So every row carries **two timestamps**, and the difference between them is
itself information:

- `ts_node` — when the event actually happened, from the node's own clock. Node B keeps wall
  time through the DS3231, so this stays correct even with no network.
- `ts_server` — when the platform received it.

A row where the two differ by minutes is a row that was buffered during an outage. The history
view flags those rather than hiding them.

### SQLite schema

| Table | Holds | Key columns |
|---|---|---|
| `telemetry` | Periodic sensor readings | `ts_node`, `ts_server`, `node`, `sensor_primary`, `sensor_secondary`, `actuator`, `fsm_state`, `mode`, `flags` |
| `events` | Discrete occurrences — motion, intrusion, alarm on/off, arm/disarm, peer lost | `ts_node`, `ts_server`, `node`, `event_id`, `severity`, `value` |
| `commands` | Every command the platform sent | `ts_server`, `cmd_id`, `target` (A / B / both), `var_id`, `value`, `source` |
| `acks` | Every acknowledgement received | `ts_server`, `cmd_id`, `node`, `var_id`, `result`, `applied_value` |
| `link_status` | Transitions online/offline, per node and per peer link | `ts_server`, `node`, `kind`, `up` |

`commands` and `acks` join on `cmd_id`, which gives a full audit trail: what was asked, what the
node did about it, and how long it took.

### How it is viewed

The dashboard has a **History** tab with:

- A **time-range filter** (last hour, last 24 h, custom).
- A **chart** of the selected node's sensor readings over that range.
- An **event timeline** — the discrete occurrences, colored by severity, with buffered rows
  marked.
- A **command audit table** — command, target, result, applied value, round-trip latency.
- **CSV export** of any of the above, so the data can be examined outside the tool.

The raw database is a single file (`platform/history.db`) and can be queried directly with
`sqlite3` during the evaluation. That is deliberate: the fastest way to prove the data is real
is to open it with something that is not my own code.

---


## Architecture

```
   PIR ──┐                                                    ┌── HC-SR04
 RC522 ──┤   Node A — Access (ESP32)                          │   Node B — Zone (ESP32)
         │   sense → FSM → actuate       ESP-NOW              │   sense → FSM → actuate
 servo ◄─┤   OLED · RGB LED         ◄──────────────────►      ├─► buzzer · OLED · RGB
         │                          binary frames ≤32 B       │   DS3231 RTC
         │                                                    │
         └───────────── Wi-Fi · MQTT (JSON) ──────────────────┘
                                  │
                      ┌───────────▼────────────┐
                      │  Mosquitto MQTT broker │
                      └───────────┬────────────┘
                                  │
                   platform/app.py  ·  Flask + Flask-SocketIO
                   ├─ authoritative state of both nodes
                   ├─ live dashboard        (WebSocket push)
                   ├─ command dispatch      (per node / broadcast)
                   └─ logger → SQLite → History tab + CSV export
```

**Two representations, one per layer.** The peer link carries packed binary frames; each node
publishes JSON to MQTT. Compact where the link is constrained, readable where it is inspected.

**Two levels of acknowledgement.** ESP-NOW's send callback confirms the bytes reached the peer's
radio. The application-level ACK confirms the node validated the command, applied it, and
reports the value now in effect. Constraint #6 needs the second one.

Full data flow: `docs/architecture.md`. Frame layout and message schema: `docs/protocol.md`.
Rationale for every choice: `docs/decisions.md`.

---

## Repository layout

```
docs/                    architecture, protocol, decisions, requirements mapping, demo script
shared/core/             no Arduino headers — compiles and is tested on the laptop
  protocol.h/.cpp          frame, message types, pack/unpack, sequence validation
  config.h/.cpp            Config struct, ranges, applyCommand() with result codes
  fsm_a.h/.cpp             Node A control logic — pure function
  fsm_b.h/.cpp             Node B control logic — pure function
shared/net/              ESP-NOW peers and callbacks, MQTT client — shared by both nodes
node_a/src/              main.cpp (timers, watchdog) + hal_a (PIR, RFID, servo, OLED, LED)
node_b/src/              main.cpp + hal_b (HC-SR04, buzzer, RTC, OLED, LED)
platform/                Flask + Flask-SocketIO server, dashboard template, SQLite logger
test/                    host tests for the pure control logic — g++, no hardware
diagrams/                wiring diagrams
```

`shared/core` exists so the graded logic — protocol, state machines, command validation — can be
exercised without a board. If it ever fails to compile with plain `g++`, hardware has leaked
into it.

---

## How to run it

Pending to develop...

---
## Progress

- [x] **Setup** — repo, folder structure, docs stubs
- [ ] **Design decisions** — servo behavior and safe state, alarm rules, distance threshold
- [ ] **Shared logic** — protocol, config validation, state machines, host tests
- [ ] **Node-to-node link** — ESP-NOW, heartbeat, peer-loss detection, safe states
- [ ] **Hardware** — sensors, actuators, OLED, RFID
- [ ] **Platform** — MQTT, live dashboard, commands with ACK states, SQLite history
- [ ] **Extras** — message authentication. OTA out of scope for this submission.

---



## Author

Robotics Engineer Hugo Castillo