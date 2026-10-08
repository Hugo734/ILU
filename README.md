# IoT Dual-Node Challenge — Distributed Alarm System

Two wirelessly-linked IoT nodes with a live dashboard for observation.

Status on 8 October 2026: both nodes, the node-to-node link, MQTT and the dashboard are built and
were run on hardware. Remote commands, acknowledgements and stored history are **not**
implemented; the section "Not implemented" and the requirements table say exactly what is missing.

---

## What this is

A distributed alarm and access controller made of two nodes that talk to each other directly over
ESP-NOW, plus a platform that lets a human watch both of them live.

| | **node_a — Room** | **node_b — Door** |
|---|---|---|
| MCU | ESP32 DevKitC (ESP-WROOM-32) | ESP32 DevKitC (ESP-WROOM-32) |
| MAC | `78:42:1c:68:44:98` | `f4:65:0b:c0:e0:a4` |
| Sensor | PIR HC-SR501 — motion inside | HC-SR04 — distance at the door |
| Input | — | 2-pin access switch — the credential |
| Actuator | active buzzer — alarm RGB LED status | RGB LED — door state |
| Status indicator | RGB LED | RGB LED|
| Radio | ESP-NOW (peer) + Wi-Fi (MQTT) | ESP-NOW (peer) + Wi-Fi (MQTT) |

In one sentence: with access closed, motion in the room makes node_a sound its buzzer and turns
node_b's LED to blinking red, until someone turns the access switch on at the door.

### Why two identical ESP32s

Both boards carry their own radio, so each node is an independent MQTT client and the dashboard
reaches either one directly, neither node depends on the other to be visible. The wire format
lives in an ESP-IDF component shared by both projects.

---

## Technology stack

Everything on the microcontroller is ESP-IDF. The communication layer uses the native Espressif
APIs directly.

| Layer | Technology | Why this one |
|---|---|---|
| Firmware | **C++17 on ESP-IDF v5.5.5** | Native Espressif framework. |
| Build | **CMake + Ninja**, driven by `idf.py` | ESP-IDF's own build system |
| Code sharing | **ESP-IDF components** via `EXTRA_COMPONENT_DIRS` | `shared/core` and `shared/net` are compiled into both node projects |
| Node ↔ node | **ESP-NOW** (`esp_now`, unicast, registered peer) | Connectionless, no broker, no IP; hardware delivery ACK |
| Wi-Fi | **`esp_wifi`** station mode, brought up explicitly | Init order is `nvs_flash` → `esp_netif` → `esp_event` → `esp_wifi` → `esp_now` |
| Node ↔ platform | **`esp-mqtt`** to a **Mosquitto** broker | Event-driven client with automatic reconnection |
| Wire format, node ↔ node | **Packed binary struct**, 10 bytes | Compact on a constrained link, explicit byte layout |
| Wire format, node ↔ platform | **JSON**, QoS 0 | Human-readable, inspectable live with `mosquitto_sub` |
| Platform server | **Python 3 · Flask · Flask-SocketIO · paho-mqtt** | Stack easy to build, and great for testing |
| Dashboard | Server-rendered HTML + **Socket.IO** push, client vendored in `platform/static/` | No build step, works without internet |
| Host tests | **CMake + CTest** | The wire-format parser compiles and runs on the laptop |

Demonstrated on hardware: ESP-NOW, Wi-Fi station mode, MQTT, GPIO input and output, and
HC-SR04 timing.


## Wiring

Interactive diagram for node_b: **https://wokwi.com/projects/477329946358273025**

![node_b circuit](images/node_b_circuit.png)

Interactive diagram for node_a: **https://wokwi.com/projects/477331000260311041**
![node_a circuit](images/node_a_circuit.png)
### Pin map — node_a (room)

| Signal | GPIO | How it is wired |
|---|---|---|
| PIR `OUT` | 32 | direct, internal pull-down. The HC-SR501 output is already 3.3 V — no divider. Jumper on **H** (repeat trigger) |
| PIR `VCC` / `GND` | — | 5 V and GND from the ESP32 board |
| Buzzer `+` | 14 | active buzzer straight off the pin, **no series resistor**: a 220 Ω resistor silenced it. `−` to GND |
| RGB LED R / G / B | 25 / 26 / 27 | 220 Ω in series per colour, common cathode to GND |

### Pin map — node_b (door)

| Signal | GPIO | How it is wired |
|---|---|---|
| HC-SR04 `TRIG` | 5 | direct. 3.3 V is enough to trigger the burst |
| HC-SR04 `ECHO` | 18 | **through a 1 kΩ / 2 kΩ divider** — see below |
| HC-SR04 `VCC` / `GND` | — | 5 V and GND from the ESP32 board, ~15 mA |
| Access switch | 4 | 3-pin switch: middle pin to GPIO 4, one outer pin to GND, the other unused. Internal pull-up, **no external resistor**. On reads 0 |
| RGB LED R / G / B | 25 / 26 / 27 | 220 Ω in series per colour, common cathode to GND |

Pins deliberately avoided: 0, 2, 12 and 15 are strapping pins whose level at power-up selects
the boot mode; 6–11 are wired to the module's internal SPI flash; 1 and 3 are UART0 and carry the
serial console.


## Behavior

The two nodes cooperate over ESP-NOW, with no platform involved. The platform only watches.

| node_b switch | node_a | node_b |
|---|---|---|
| **Off** — access closed (default) | motion after warm-up: buzzer sounds, LED red, sends `MotionStarted` once per rising edge | LED red; yellow when something is 4–15 cm away |
| Off, after `MotionStarted` arrives | — | **LED blinks red until the switch is turned on** |
| **On** — access open | **LED blue, buzzer silent, sends nothing** | LED green; clears any alert |

node_a's LED outside an alarm is yellow during the 60 s PIR warm-up and green once the sensor can
be trusted. node_a never raises the alarm during warm-up: the HC-SR501 fires on its own while its
pyroelectric element settles.

**The headline interaction:** with access closed, one gesture in front of node A and two boards
react at once — node A sounds its buzzer, node B goes red — over ESP-NOW, with no platform
involved. Turning the switch on is the acknowledgement: node B clears the alert and node A goes
blue.


### Tunable values

These are compile-time constants, not remotely controllable. Changing one means rebuilding and
reflashing.

| Constant | Node | Value | Meaning |
|---|---|---|---|
| `WARMUP_MS` | A | 60000 | PIR warm-up |
| `ACCESS_LEASE_MS` | A | 3000 | how long an `AccessOpen` keeps access open |
| `CERCA_MIN` / `CERCA_MAX` | B | 4 / 15 cm | "near" window of the HC-SR04 |
| `STATE_REPEAT_MS` | B | 1000 | access-state repeat interval |
| `PUBLISH_MS` | both | 1000 | MQTT publish period |

---

## The platform

`platform/app.py` is a single Python process. Run it on the laptop that hosts the broker.

1. **MQTT client.** Subscribes to `ilu/#`. Only `ilu/<node>/state` is handled.
2. **Authoritative state.** Holds the last state each node reported. Every value in the UI comes
   from a report, never from an assumption.
3. **Socket.IO push.** Every update reaches the browser at once, without polling. A browser that
   opens late receives a full snapshot.
4. **Watchdog.** A node with no report for 3 s is marked offline; the first report afterwards
   marks it back online. `up` going backwards is shown as "Node rebooted".

### MQTT state messages

```
ilu/a/state {"node":"a","up":26480,"warm":true,"motion":false,"alarm":false,"buzzer":0,"access":false,"led":"green"}
ilu/b/state {"node":"b","up":26700,"cm":40,"near":false,"access":false,"alert":false,"led":"red"}
```

`led` is one of `off`, `red`, `yellow`, `green`, `blue`, `red_blink`. `buzzer` is read back from
the pin.

### Dashboard

- Header with the broker connection status.
- Topology diagram: a dot pulses along a node's MQTT wire on each report. The ESP-NOW link is
  drawn and labelled *not seen by the platform*, because that traffic never touches the broker.
- One card per node: mirrored LED including the blink, online pill, "last report x s ago", every
  field, a collapsible raw JSON, and for node_b a distance bar with the 4–15 cm zone and a 60 s
  sparkline.
- Event timeline with filters All / Alarm / Access / Link / Info, showing server time and node
  uptime. `near` is deliberately not a timeline event, because it flickers.

The server uses Werkzeug's development server, which is enough for one laptop.

---

## Not implemented

These were designed and are **not built**. They are listed so the README does not claim them.

- **Commands from the platform** to one node or to both, and any remotely controllable variable.
  The platform is read-only today. Mandatory requirement #4 and #5.
- **Application-level acknowledgement** of a command. Today only the ESP-NOW link-layer ACK
  exists, and it is only logged: it proves the peer's radio received the bytes, nothing more.
  Requirement #6.
- **MQTT Last Will and Testament.** Offline detection is the platform's silence watchdog.
- **A heartbeat from node_a to node_b.** `MsgType::Heartbeat` is defined but nothing sends it, so
  node_b cannot tell that node_a is gone. The reverse direction is covered by the access lease.
- **Stored history.** No SQLite, no history view, no CSV export. The timeline holds the last 200
  events in memory and is lost when the server restarts.
- **ESP-NOW encryption.** The only filter is the sender's MAC address.
- **Wi-Fi power-save control.** `esp_wifi_set_ps` is not called. Modem sleep can hurt ESP-NOW
  reception; it was not a problem in the tests above.


## Repository layout

```
CMakeLists.txt           host build for the pure logic — not an ESP-IDF project
CLAUDE.md                working notes for the coding assistant
docs/                    earlier design documents (stale, see above); instructionOfUse.md is current
shared/
  core/                  no ESP-IDF headers — compiles and is tested on the laptop
    CMakeLists.txt         dual mode: ESP-IDF component, or plain library for the host build
    hal.h                  IHalA / IHalB — the hardware interfaces the nodes use
    protocol.h/.cpp        10-byte Frame, MsgType, EventId, protocol_parse()
    fsm_a, fsm_b           empty placeholders
  net/                   Wi-Fi bring-up, MQTT, ESP-NOW queue and callbacks — shared by both nodes
    secrets.h.example      template for the git-ignored secrets.h
node_a/
  main/                  main.cpp + hal_a_esp32 (PIR, buzzer, RGB LED)
node_b/
  main/                  main.cpp + hal_b_esp32 (HC-SR04, access switch, RGB LED)
platform/
  app.py                 MQTT client, state store, Socket.IO server, watchdog
  templates/index.html   the dashboard page
  static/                vendored Socket.IO client
  mosquitto/ilu.conf     broker configuration
test/                    host tests for the frame parser — CTest, no hardware
images/                  circuit photographs and diagram exports
```

`shared/core` exists so the wire format can be exercised without a board. **If it ever fails to
compile on the host, hardware has leaked into it.**

---

## How to run it

Each node is its own ESP-IDF project. Before the first build, copy
`shared/net/secrets.h.example` to `shared/net/secrets.h` and fill in the Wi-Fi name, the password
and `MQTT_BROKER_URI`. `secrets.h` is git-ignored. `sdkconfig` is not committed either, so the
first build on a fresh clone regenerates it.

```bash
get_idf                              # export.sh is per terminal, not global

idf.py -C node_b -p /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... build flash monitor
idf.py -C node_a -p /dev/serial/by-id/usb-Silicon_Labs_CP2102_...  build flash monitor
```

Always address the boards by their `/dev/serial/by-id/` path. The `ttyUSB0` / `ttyUSB1` numbers
are assigned in plug order and swap between sessions, and flashing the wrong board is silent.

| Role | Adapter | MAC |
|---|---|---|
| node_a — Room | CP2102, serial `0001` | `78:42:1c:68:44:98` |
| node_b — Door | CP2102N, serial `7a041be6…` | `f4:65:0b:c0:e0:a4` |

The nodes have the broker's address compiled in. If the laptop's IP changes, the nodes still join
Wi-Fi but never reach the broker, and the dashboard marks them offline. Reserve the address in the
router, or change `MQTT_BROKER_URI` and reflash both nodes.

**Broker.** Install `platform/mosquitto/ilu.conf` into `/etc/mosquitto/conf.d/` and restart
Mosquitto. Watch the raw traffic with:

```bash
mosquitto_sub -h <broker-ip> -t 'ilu/#' -v
```

**Dashboard.**

```bash
python3 -m venv platform/.venv
platform/.venv/bin/pip install -r platform/requirements.txt
ILU_BROKER=localhost platform/.venv/bin/python platform/app.py
# open http://localhost:5000, or http://<laptop-ip>:5000 from another device on the LAN
```

**Host tests.**

```bash
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

The full demo-day command list, in Spanish, is in `docs/instructionOfUse.md`.

---

## Mandatory requirements

| # | Requirement | Status |
|---|---|---|
| 1 | Two independent devices, own MCU, C/C++ | done |
| 2 | Each node: ≥1 sensor and ≥1 actuator with observable output | done. node_a: PIR + buzzer and LED. node_b: HC-SR04 + LED |
| 3 | Wireless node-to-node, ≥1 interaction without the platform | done and verified: motion alert A→B and access state B→A over ESP-NOW |
| 4 | Platform commands to one node and to both | **not implemented** — the platform is read-only |
| 5 | ≥2 remotely controllable variables per node | **not implemented** — values are compile-time constants |
| 6 | Confirmation of reception **and** of execution | **not implemented** — only the link-layer ACK, which is logged |
| 7 | Loss-of-communication detection and safe behaviour | partial. node_a ends armed when node_b goes silent (access lease, implemented); the dashboard marks a silent node offline (verified). No heartbeat from node_a to node_b |
| 8 | Able to explain the data and command flow | data flow: yes, see Architecture and Decisions. Command flow: there is none yet |

---

## Author

Hugo Daniel Castillo Ovando — Robotics and Digital Systems Engineer