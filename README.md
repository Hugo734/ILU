# IoT Dual-Node Challenge — Distributed Alarm System

Two wirelessly-linked IoT nodes with a live dashboard for observation, control and historical
review.

---

## What this is

A distributed alarm and access controller made of two nodes that keep cooperating with **no
router, no broker and no dashboard**, plus a platform that lets a human observe them, override
them, and review what happened.

| | **node_a — Room** | **node_b — Door** |
|---|---|---|
| MCU | ESP32 DevKitC (ESP-WROOM-32) | ESP32 DevKitC (ESP-WROOM-32) |
| MAC | `78:42:1c:68:44:98` | `f4:65:0b:c0:e0:a4` |
| Sensor | PIR HC-SR501 — motion inside | HC-SR04 — distance at the door |
| Credential | — | pushbutton (RFID reader if time allows) |
| Actuator | active buzzer — alarm | RGB LED — door state (node_b's only physical output) |
| Status indicator | RGB LED | — (the RGB LED above is the actuator) |
| Radio | ESP-NOW (peer) + Wi-Fi (MQTT) | ESP-NOW (peer) + Wi-Fi (MQTT) |

**node_b is the door. node_a is the protected room.** The directory names `node_a` and `node_b`
are identifiers only; they were assigned before the roles were settled and renaming them would
break working builds for no benefit.

### Why two identical ESP32s

Both boards carry their own radio, so each node is an independent MQTT client and the dashboard
reaches either one directly — neither node depends on the other to be visible. The protocol and
the control logic live in **ESP-IDF components shared by both projects**, so there is one copy of
that code compiled into two firmwares rather than two copies drifting apart.

---

## Technology stack

Everything on the microcontroller is ESP-IDF. The communication layer uses the native Espressif
APIs directly — no Arduino framework and no third-party networking wrappers anywhere in the
firmware.

| Layer | Technology | Why this one |
|---|---|---|
| Firmware | **C++17 on ESP-IDF v5.5.5** | Native Espressif framework. Version pinned because `esp_now_recv_info_t` does not exist before v5.1 |
| Build | **CMake + Ninja**, driven by `idf.py` | ESP-IDF's own build system |
| Code sharing | **ESP-IDF components** via `EXTRA_COMPONENT_DIRS` | One copy of the protocol and control logic, compiled into both node projects |
| Node ↔ node | **ESP-NOW** (`esp_now`, unicast, registered peer) | Connectionless, no router and no broker; hardware delivery ACK; works with the infrastructure off |
| Wi-Fi | **`esp_wifi`** station mode, brought up explicitly | Init order is `nvs_flash` → `esp_netif` → `esp_event` → `esp_wifi` → `esp_now` |
| Node ↔ platform | **`esp-mqtt`** to a **Mosquitto** broker | Event-driven client, native Last Will and Testament, automatic reconnection |
| Wire format, node ↔ node | **Packed binary struct** | Compact on a constrained link, and explicit about byte layout |
| Wire format, node ↔ platform | **JSON** | Human-readable, inspectable live during the evaluation |
| Platform server | **Python 3 · Flask · Flask-SocketIO · paho-mqtt** | Stack I have built before and can explain line by line |
| Storage | **SQLite** | Single file, no server to run, queryable with plain SQL during the evaluation |
| Dashboard | Server-rendered HTML + **WebSocket** push | No build step, no framework to justify |
| Host tests | **CMake + CTest**, with ASan and UBSan | The pure control logic compiles and runs on the laptop |

**Demonstrated on hardware by 4 October:** ESP-NOW and UART.
**Planned and not yet demonstrated:** MQTT, and I²C/SPI only if the optional display and card
reader make it into scope.

The application-layer message protocol is my own and is specified in `docs/protocol.md`.

---

## Progress — weekend of 4 October

> **Status on 6 October.** The servo was removed from node_b's source after the hardware
> results below were recorded (see Decisions). That change is committed but **has not been
> compiled**; no build has run since. The results below were obtained before it, and the
> current node_b source is not claimed to work.

### Verified on hardware

| Node | Item | Evidence |
|---|---|---|
| A ↔ B | ESP-NOW bidirectional peer link | cross-matched uptimes in both monitors, no lost link-layer ACKs |
| B | HC-SR04 distance over a 1 kΩ/2 kΩ divider | ±1 cm steady, responds to a hand, 2 cm floor as the datasheet states |
| B | Credential pushbutton, internal pull-up | 3 s access window, confirmed to the millisecond in the log |
| B | RGB LED — red / yellow / green | all three colours correct |
| both | Wi-Fi station mode with an explicit, deterministic configuration | MAC printed matches the eFuse |

### Not yet working

| Item | State |
|---|---|
| PIR HC-SR501 | wired on node A, not yet exercised |
| Buzzer | not connected; its current must be measured before driving it from a GPIO |

Everything not listed as verified above is designed, not built.

## Wiring

Interactive diagram for node_b: **https://wokwi.com/projects/477013744279158785**

![node_b circuit](images/node_b_circuit.png)

### Pin map

Supersedes the pin table in `cableado.html`, which is stale.

| Signal | Node | GPIO | How it is wired |
|---|---|---|---|
| HC-SR04 `TRIG` | B | 5 | direct. 3.3 V is enough to trigger the burst |
| HC-SR04 `ECHO` | B | 18 | **through a 1 kΩ / 2 kΩ divider** — see below |
| HC-SR04 `VCC` | B | — | 5 V from the ESP32 board, ~15 mA |
| Credential button | B | 4 | one leg to the pin, the other to GND. Internal pull-up, **no external resistor**. Pressed reads 0 |
| RGB LED R / G / B | B | 25 / 26 / 27 | 220 Ω in series per colour, common cathode to GND |
| PIR output | A | 32 | direct. The HC-SR501 output is already 3.3 V — no divider needed |
| PIR `VCC` | A | — | 5 V |
| Buzzer | A | 14 | pending — its current must be measured before driving it from a GPIO |

Pins deliberately avoided: 0, 2, 12 and 15 are strapping pins whose level at power-up selects
the boot mode; 6–11 are wired to the module's internal SPI flash; 1 and 3 are UART0 and carry the
serial console.

### The HC-SR04 echo divider

The sensor is powered at 5 V and drives its echo line to 5 V. The ESP32 tolerates 3.6 V
absolute maximum (datasheet Table 12), so the echo cannot reach a GPIO directly.

```
HC-SR04 ECHO ──[ 1 kΩ ]──┬────────► GPIO 18
                         │
                      [ 2 kΩ ]
                         │
                        GND
```

```
5.0 V × 2 kΩ / (1 kΩ + 2 kΩ) = 3.33 V
```

Both bounds matter. 3.33 V is below the 3.6 V maximum, and it is also above the 2.475 V minimum
high-level input voltage (0.75 × VDD, Table 14) — so the pulse is both safe and recognised.
Swapping the two resistors yields 1.67 V, which damages nothing and never registers as a logic
high.

### Power and grounding

node_b runs entirely from the ESP32 board: 5 V for the HC-SR04, 3.3 V for the logic. Nothing
is powered from the MB-102 any more. Two lessons from it are kept, because they apply to any
actuator added later:

**Star grounding.** An actuator's return current goes back to its own supply. The grounds of
the two supplies meet at one point, and that link carries only the signal reference, never
load current. Return current through the shared ground wire shifts the reference the ESP32
sees, and the symptom reads as a firmware bug.

**MB-102 rails are split at the middle.** On the breadboard the top and bottom power rails
are not bridged from the factory. A board wired across the gap gets no supply and no error
message. This cost hours of debugging because nothing reports it.

---

## Decisions taken 4–6 October

Full rationale in `docs/decisions.md`. The ones that shaped the code:

**The servo is out of scope for now (6 October).** On the night of 5 October the MB-102
breadboard supply burned while the SG90 was stalled against an end stop. The probable cause:
the LEDC pulse range started at 0.5 ms, commanding an angle the servo could not reach, so it
kept pushing and drew roughly 700 mA continuously through a linear regulator fed at 12 V —
about 4.9 W dissipated in a part rated near 1 W. The cause is probable, not confirmed. With
two days to delivery, replacing the supply was a worse risk than dropping the actuator, and
the servo will not be reconsidered until both nodes and the platform work end to end.
node_b is now HC-SR04 (sensor), RGB LED (actuator, its only observable output) and
pushbutton (credential). `setServoAngle()` is gone from `IHalB`, the `HalBEsp32` constructor
takes no servo pin, and the LEDC code is removed. Committed, not compiled.

**The repository ignores build output (6 October).** The old `.gitignore` was written for an
abandoned PlatformIO layout and ignored nothing real, so 2911 build artifacts were tracked
and GitHub Linguist reported "CMake 39%, Assembly 37%, C++ 5%". It is rewritten for ESP-IDF;
`node_a/build/`, `node_b/build/` and `node_b/sdkconfig` are untracked. History was
deliberately not rewritten: a `filter-repo` and force-push two days before delivery is not
worth the risk, and Linguist reads the current tree. A bare `secrets.h` pattern now matches at
any depth; the old pattern pointed at a path that does not exist.

**An input pin must be able to define its idle level.** The PIR moved from GPIO 34 to GPIO 32 so
an internal pull-down is available. GPIO 34–39 are type `I` in the datasheet (Table 2) and have
no internal pull resistors at all. An unheld high-impedance input does not read zero — it acts as
an antenna and picks up mains hum. A 7930 µs "echo" turned out to be the portion of a 60 Hz half
cycle sitting above the input threshold, and it cost two hours before being identified. In an
alarm, a sensor that fabricates readings is worse than one that fails.

**No return value may mean two different things.** `distanceCm()` originally returned 0 both for
"no echo" and for "closer than one centimetre". That ambiguity hid the problem above for an
entire debugging session. Failure modes are now distinct and visible.

**The credential is an abstract event, not a card reader.** The state machine depends on
`accessGranted()`, served today by a pushbutton. An RC522 can serve it later without the logic
changing by one line. The reader stays out of the committed scope: it satisfies no mandatory
requirement, ESP-IDF ships no driver for it, and a third-party library would have to be explained
under constraint #8.

**The status LED runs on plain GPIO, not PWM.** True orange needs PWM on the green channel;
yellow does not, and a state indicator only has to be unambiguous. Dropping the PWM removed a
timer, three channels and a function.

**Explicit Wi-Fi configuration, never inherited.** `esp_wifi_set_storage(WIFI_STORAGE_RAM)` plus
an explicit `esp_wifi_set_mode()`. Without them the driver restores mode and configuration from
NVS, so the same source can boot into different radio modes on two boards depending on what the
previous firmware left behind.

**Virtual interfaces for the hardware abstraction.** One indirection per call and a vtable per
object, against compile-time polymorphism which costs nothing at runtime. At a few calls per
second the indirection is irrelevant next to 240 MHz, and the explainable option won.

---

## Cross-node behavior

These paths run over ESP-NOW and must work with the dashboard closed and the router switched off.
**Designed, not yet implemented.**

| Trigger | Node | Effect on node_a (room) | Effect on node_b (door) |
|---|---|---|---|
| Someone approaches, distance below threshold | B | amber | amber |
| Credential accepted | B | green, alarm silent | green |
| Access window expires | B | red | red |
| **Motion with no credential** | A | red flashing, **buzzer sounds** | red flashing |
| Three heartbeats missed | both | blue, buzzer silent | blue |

The fourth row is the headline interaction: one gesture in front of node A, and two boards react
at once — one with sound, the other with light — with no router and no platform involved.

### State machine

Both nodes run the same five named states and react with whatever they have connected.

```
                 ACCESS_GRANTED
        ┌──────────────────────────────┐
        │                              │
   ┌────▼─────┐  arm_delay   ┌─────────┴┐
   │ DISARMED │─────────────►│  ARMED   │
   └──────────┘              └────┬─────┘
                                  │ PIR, or distance below threshold
                             ┌────▼──────┐
                             │ PRE_ALARM │
                             └────┬──────┘
                                  │ grace period with no credential
                             ┌────▼─────┐
                             │  ALARM   │
                             └──────────┘

   any state ──3 heartbeats missed──► DEGRADED
```

| LED | State |
|---|---|
| green | `DISARMED` — access granted |
| red, steady | `ARMED` — closed, watching |
| amber | `PRE_ALARM` — someone approaching |
| red, flashing | `ALARM` — intrusion |
| blue | `DEGRADED` — no link to the peer |

### Remotely controllable variables

| Variable | Node | Range |
|---|---|---|
| `ARMED` | both — the broadcast one | 0 / 1 |
| `GRACE_S` | both | 5 – 60 s |
| `ACCESS_HOLD_S` | B | 1 – 30 s |
| `DIST_THRESHOLD_CM` | B | 5 – 200 |
| `BUZZER_ENABLED` | A | 0 / 1 |

---

## The platform

Not yet built. A single Python process (`platform/app.py`) that does four jobs:

1. **MQTT client.** Subscribes to every node topic — telemetry, state, acknowledgements,
   heartbeats, events and the LWT status topic.
2. **Authoritative state.** Holds the last reported state of both nodes. Every value shown in
   the UI comes from what the node reported, never from a command the dashboard sent.
3. **WebSocket push.** Flask-SocketIO pushes each update to the browser, so the view is live
   without polling.
4. **Logger.** Writes telemetry, events, commands and acknowledgements to SQLite as they arrive.

### Commands and feedback

Each controllable variable has a control that can be sent to **node_a only**, **node_b only**, or
**both at once** through the broadcast topic — mandatory constraint #4.

Every command gets a `cmd_id`, and the UI shows one of four states for it — this is mandatory
constraint #6 and the part most easily faked:

| State | Meaning |
|---|---|
| **Pending** | Sent, no acknowledgement yet |
| **Confirmed** | Node replied `APPLIED` and reported the value now in effect |
| **Rejected** | Node replied with a reason — out of range, or refused in its current state |
| **Timed out** | No acknowledgement within 2 s |

The value displayed after a command is the `applied_value` the node reported back, which is not
always the value that was sent.

---

## Historical data

A node that loses its link keeps operating and keeps recording. When the link returns, those
records arrive late. So every row carries **two timestamps**, and the difference between them is
itself information:

- `ts_node` — when the event actually happened, from the node's own clock.
- `ts_server` — when the platform received it.

A row where the two differ by minutes is a row that was buffered during an outage. The history
view flags those rather than hiding them.

### SQLite schema

| Table | Holds | Key columns |
|---|---|---|
| `telemetry` | Periodic sensor readings | `ts_node`, `ts_server`, `node`, `sensor_primary`, `actuator`, `fsm_state`, `flags` |
| `events` | Discrete occurrences — motion, intrusion, alarm on/off, access, peer lost | `ts_node`, `ts_server`, `node`, `event_id`, `severity`, `value` |
| `commands` | Every command the platform sent | `ts_server`, `cmd_id`, `target`, `var_id`, `value`, `source` |
| `acks` | Every acknowledgement received | `ts_server`, `cmd_id`, `node`, `var_id`, `result`, `applied_value` |
| `link_status` | Transitions online/offline, per node and per peer link | `ts_server`, `node`, `kind`, `up` |

`commands` and `acks` join on `cmd_id`, which gives a full audit trail: what was asked, what the
node did about it, and how long it took.

The raw database is a single file (`platform/history.db`) and can be queried directly with
`sqlite3` during the evaluation. That is deliberate: the fastest way to prove the data is real is
to open it with something that is not my own code.

---

## Architecture

```
   PIR ──┐                                              ┌── HC-SR04
         │   node_a — Room (ESP32)                      │   node_b — Door (ESP32)
         │   sense → FSM → actuate     ESP-NOW          │   sense → FSM → actuate
buzzer ◄─┤   RGB LED              ◄──────────────►      ├─► RGB LED
         │                        packed binary frames  │   pushbutton
         │                                              │
         └──────────── Wi-Fi · MQTT (JSON) ─────────────┘
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

**Two levels of acknowledgement.** The ESP-NOW send callback confirms the bytes reached the
peer's radio — Espressif's own documentation is explicit that this is MAC-layer receipt and
nothing more. The application-level ACK confirms the node validated the command, applied it, and
reports the value now in effect. Constraint #6 needs the second one.

**ESP-NOW and Wi-Fi share one radio.** ESP-NOW needs no router, no association and no IP, but it
does ride on the 802.11 physical layer as vendor-specific action frames. A radio can only be
tuned to one channel at a time, so once a node associates with an access point its ESP-NOW frames
go out on that AP's channel. Both nodes must therefore use the same AP, and Wi-Fi power save has
to be disabled so the radio does not sleep through incoming frames.

Full data flow: `docs/architecture.md`. Frame layout and message schema: `docs/protocol.md`.
Rationale for every choice: `docs/decisions.md`.

---

## Repository layout

```
CMakeLists.txt           host build for the pure logic — not an ESP-IDF project
docs/                    architecture, protocol, decisions, requirements mapping, demo script
shared/
  core/                  no framework headers at all — compiles and is tested on the laptop
    CMakeLists.txt         dual mode: ESP-IDF component, or plain library for the host build
    hal.h                  IHalA / IHalB — the hardware interfaces the logic depends on
    protocol.h/.cpp        frame, message types, pack/unpack, sequence validation
  net/                   Wi-Fi bring-up, ESP-NOW peers and callbacks — shared by both nodes
node_a/
  main/                  main.cpp + hal_a_esp32 (PIR, buzzer, RGB LED)
node_b/
  main/                  main.cpp + hal_b_esp32 (HC-SR04, button, RGB LED)
platform/                Flask + Flask-SocketIO server, dashboard template, SQLite logger
test/                    host tests for the pure control logic — CTest, no hardware
images/                  circuit photographs and diagram exports
```

`shared/core` exists so the graded logic — protocol, state machines, command validation — can be
exercised without a board. **If it ever fails to compile on the host, hardware has leaked into
it.**

---

## How to run it

Each node is its own ESP-IDF project. From the repository root:

```bash
get_idf                              # export.sh is per terminal, not global

idf.py -C node_b -p /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... build flash monitor
idf.py -C node_a -p /dev/serial/by-id/usb-Silicon_Labs_CP2102_...  build flash monitor
```

`sdkconfig` is no longer committed, so the first build on a fresh clone regenerates it.
Copy `secrets.h.example` to `secrets.h` before building; `secrets.h` is git-ignored.

Always address the boards by their `/dev/serial/by-id/` path. The `ttyUSB0` / `ttyUSB1` numbers
are assigned in plug order and swap between sessions, and flashing the wrong board is silent.

| Role | Adapter | MAC |
|---|---|---|
| node_a — Room | CP2102, serial `0001` | `78:42:1c:68:44:98` |
| node_b — Door | CP2102N, serial `7a041be6…` | `f4:65:0b:c0:e0:a4` |

Host tests and the platform: pending.

---

## Mandatory requirements

| # | Requirement | Status |
|---|---|---|
| 1 | Two independent devices, own MCU, C/C++ | done |
| 2 | Each node: ≥1 sensor and ≥1 actuator with observable output | node B: HC-SR04 and RGB LED verified on hardware by 4 October, before the servo removal; current source not compiled. Node A pending |
| 3 | Wireless node-to-node, ≥1 interaction without the platform | link verified, interaction designed not built |
| 4 | Platform commands to one node and to both | not started |
| 5 | ≥2 remotely controllable variables per node | designed, not built |
| 6 | Confirmation of reception **and** of execution | designed, not built |
| 7 | Loss-of-communication detection and safe behaviour | designed, not built |
| 8 | Able to explain the data and command flow | on track — see `docs/decisions.md` |

---

## Author

Hugo Daniel Castillo Ovando — Robotics and Digital Systems Engineer