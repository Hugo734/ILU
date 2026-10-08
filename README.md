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
| Input | — | 3-pin access switch — the credential |
| Actuator | active buzzer — alarm | RGB LED — door state (node_b's only physical output) |
| Status indicator | RGB LED | — (the RGB LED above is the actuator) |
| Radio | ESP-NOW (peer) + Wi-Fi (MQTT) | ESP-NOW (peer) + Wi-Fi (MQTT) |

**node_b is the door. node_a is the protected room.** The directory names `node_a` and `node_b`
are identifiers only; they were assigned before the roles were settled and renaming them would
break working builds for no benefit.

In one sentence: with access closed, motion in the room makes node_a sound its buzzer and turns
node_b's LED to blinking red, until someone turns the access switch on at the door.

### Why two identical ESP32s

Both boards carry their own radio, so each node is an independent MQTT client and the dashboard
reaches either one directly — neither node depends on the other to be visible. The wire format
lives in an ESP-IDF component shared by both projects, so there is one copy of that code compiled
into two firmwares rather than two copies drifting apart.

---

## Technology stack

Everything on the microcontroller is ESP-IDF. The communication layer uses the native Espressif
APIs directly — no Arduino framework and no third-party networking wrappers anywhere in the
firmware.

| Layer | Technology | Why this one |
|---|---|---|
| Firmware | **C++17 on ESP-IDF v5.5.5** | Native Espressif framework. Version pinned because `esp_now_recv_info_t` does not exist before v5.1 |
| Build | **CMake + Ninja**, driven by `idf.py` | ESP-IDF's own build system |
| Code sharing | **ESP-IDF components** via `EXTRA_COMPONENT_DIRS` | `shared/core` and `shared/net` are compiled into both node projects |
| Node ↔ node | **ESP-NOW** (`esp_now`, unicast, registered peer) | Connectionless, no broker, no IP; hardware delivery ACK |
| Wi-Fi | **`esp_wifi`** station mode, brought up explicitly | Init order is `nvs_flash` → `esp_netif` → `esp_event` → `esp_wifi` → `esp_now` |
| Node ↔ platform | **`esp-mqtt`** to a **Mosquitto** broker | Event-driven client with automatic reconnection |
| Wire format, node ↔ node | **Packed binary struct**, 10 bytes | Compact on a constrained link, explicit byte layout |
| Wire format, node ↔ platform | **JSON**, QoS 0 | Human-readable, inspectable live with `mosquitto_sub` |
| Platform server | **Python 3 · Flask · Flask-SocketIO · paho-mqtt** | Stack I have built before and can explain line by line |
| Dashboard | Server-rendered HTML + **Socket.IO** push, client vendored in `platform/static/` | No build step, works without internet |
| Host tests | **CMake + CTest** | The wire-format parser compiles and runs on the laptop |

Demonstrated on hardware: ESP-NOW, Wi-Fi station mode, MQTT, GPIO input and output, and
HC-SR04 timing. I²C and SPI are not used.

The ESP-NOW message layout is my own and is described in `shared/core/protocol.h`.

---

## Progress — 4 to 8 October

### Verified on hardware

| Node | Item | Evidence |
|---|---|---|
| A ↔ B | ESP-NOW bidirectional peer link | cross-matched uptimes in both monitors; link-layer ACKs |
| A → B | Motion alert over ESP-NOW with Wi-Fi up | node_a buzzer and red LED, node_b blinks red, dashboard shows ALERT (8 October) |
| B → A | Access state over ESP-NOW with Wi-Fi up | switch on at node_b → node_a goes blue and silences the buzzer (6 and 8 October) |
| B | HC-SR04 distance over a 1 kΩ/2 kΩ divider | ±1 cm steady, responds to a hand, 2 cm floor as the datasheet states |
| B | Access switch, internal pull-up | closed / open levels read correctly |
| A | PIR HC-SR501 with a 60 s warm-up | exactly one `MotionStarted` per rising edge, none during warm-up |
| both | RGB LED — red / green / yellow / blue | channel test at boot, all colours correct |
| both | Wi-Fi station mode, explicit configuration | both join channel 11; MAC printed matches the eFuse |
| both | MQTT to Mosquitto | about one message per second per node, expected JSON |
| platform | Live dashboard in a real browser | cards, LED mirror, distance bar, timeline (8 October) |
| platform | Offline detection | unplugging node_b marks it offline on the dashboard (8 October) |
| host | Protocol parser tests | 15 tests passed on 6 October; not re-run since |

### Known limitations

| Item | State |
|---|---|
| Router-off test | **Not run.** Both nodes are on the router's Wi-Fi, so ESP-NOW rides on that access point's channel (11). ESP-NOW needs no router by design, but the claim has not been tested with the router switched off, and each node's Wi-Fi retry loop could pull the radio off the channel |
| Lease fail-safe | Implemented in node_a. The dashboard showing node_b offline is verified; node_a's own `access CLOSED (lease expired)` message after unplugging node_b is not recorded as checked |
| PIR self-triggering | In two logs on 6 October the PIR fired every 7–10 s, including during warm-up. Whether that was movement or the sensor is not confirmed |
| Buzzer current | The buzzer is driven straight from a GPIO. Its current was never measured; the multimeter's mA fuse is suspect |
| Flash | Both binaries are about 875 KB in a 1 MB app partition, 14% free |

## Wiring

Interactive diagram for node_b: **https://wokwi.com/projects/477013744279158785**

![node_b circuit](images/node_b_circuit.png)

### Pin map

| Signal | Node | GPIO | How it is wired |
|---|---|---|---|
| HC-SR04 `TRIG` | B | 5 | direct. 3.3 V is enough to trigger the burst |
| HC-SR04 `ECHO` | B | 18 | **through a 1 kΩ / 2 kΩ divider** — see below |
| HC-SR04 `VCC` | B | — | 5 V from the ESP32 board, ~15 mA |
| Access switch | B | 4 | 3-pin switch: middle pin to GPIO 4, one outer pin to GND, the other unused. Internal pull-up, **no external resistor**. On reads 0 |
| RGB LED R / G / B | B | 25 / 26 / 27 | 220 Ω in series per colour, common cathode to GND |
| PIR output | A | 32 | direct, internal pull-down. The HC-SR501 output is already 3.3 V — no divider. Jumper on **H** (repeat trigger) |
| PIR `VCC` | A | — | 5 V |
| Buzzer | A | 14 | active buzzer straight off the pin, **no series resistor**: a 220 Ω resistor silenced it |
| RGB LED R / G / B | A | 25 / 26 / 27 | same wiring as node_b |

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

Both nodes run entirely from their ESP32 board: 5 V for the HC-SR04 and the PIR, 3.3 V for the
logic. Nothing is powered from the MB-102 any more. Two lessons from it are kept, because they
apply to any actuator added later:

**Star grounding.** An actuator's return current goes back to its own supply. The grounds of
the two supplies meet at one point, and that link carries only the signal reference, never
load current. Return current through the shared ground wire shifts the reference the ESP32
sees, and the symptom reads as a firmware bug.

**MB-102 rails are split at the middle.** On the breadboard the top and bottom power rails
are not bridged from the factory. A board wired across the gap gets no supply and no error
message. This cost hours of debugging because nothing reports it.

---

## Decisions taken 4–8 October

**The servo is out of scope (6 October).** On the night of 5 October the MB-102 breadboard
supply burned while the SG90 was stalled against an end stop. The probable cause: the LEDC pulse
range started at 0.5 ms, commanding an angle the servo could not reach, so it kept pushing and
drew roughly 700 mA continuously through a linear regulator fed at 12 V — about 4.9 W dissipated
in a part rated near 1 W. The cause is probable, not confirmed. With two days to delivery,
replacing the supply was a worse risk than dropping the actuator. The RC522 card reader is out
of scope too: it satisfies no mandatory requirement, ESP-IDF ships no driver for it, and a
third-party library would have to be explained under constraint #8.

**The repository ignores build output (6 October).** The old `.gitignore` was written for an
abandoned PlatformIO layout and ignored nothing real, so 2911 build artifacts were tracked
and GitHub Linguist reported "CMake 39%, Assembly 37%, C++ 5%". It is rewritten for ESP-IDF.
History was deliberately not rewritten: a `filter-repo` and force-push two days before delivery
is not worth the risk, and Linguist reads the current tree. A bare `secrets.h` pattern matches
at any depth, so the Wi-Fi and MQTT credentials never reach git.

**A switch, not a pushbutton.** The credential is a level, not an event. `accessSwitchOn()`
returns whether the switch is on, and access lasts exactly as long as it is. The first design was
a pushbutton with a 3 s window; it was replaced because a level cannot be missed and leaves no
timer to tune.

**Access is a lease, so every failure ends armed.** node_b sends `AccessOpen` or `AccessClosed`
on every switch change and repeats it every second. node_a counts access as open only while an
`AccessOpen` from the last 3 s backs it. A dead node_b, a lost frame or a broken link all end
with the alarm armed, never disarmed. Every message is idempotent, so duplicates need no
filtering; `seq` gaps are only logged.

**Incoming frames go through a queue.** The ESP-NOW callback runs in the Wi-Fi task and must not
block. It drops frames from a non-peer MAC or that fail `protocol_parse()`, pushes the rest onto
an 8-slot queue with timeout 0, and the main loop drains it. All control logic stays in one task.

**The parser is stateless and lives in `shared/core`.** `protocol_parse()` checks length,
version, type, source and event, and copies into the output only if every rule holds. Duplicate
and loss detection by `seq` is receiver state, so it is not in the parser. `shared/core` has no
ESP-IDF header, so it builds and is tested on the laptop.

**State goes to the platform on change and every second.** Each node publishes its full state
once a second, and at once when anything other than uptime or distance changes. The dashboard
shows edges without waiting for the next tick. Distance is excluded from the change check because
it jitters by a centimetre and would publish ten times a second.

**The dashboard derives events from state, not from commands.** Every timeline entry is the
difference between two consecutive reports. The UI shows only what a node reported, never what
the platform assumed. Offline means three seconds of silence, because a node that loses power
never gets to say goodbye.

**The broker is anonymous, on purpose.** Mosquitto listens on `0.0.0.0:1883` with no password and
no TLS. That is a prototype shortcut for a private network and is documented in
`platform/mosquitto/ilu.conf`. A deployment would add a password file and TLS on 8883.

**An input pin must be able to define its idle level.** The PIR moved from GPIO 34 to GPIO 32 so
an internal pull-down is available. GPIO 34–39 are type `I` in the datasheet (Table 2) and have
no internal pull resistors at all. An unheld high-impedance input does not read zero — it acts as
an antenna and picks up mains hum. A 7930 µs "echo" turned out to be the portion of a 60 Hz half
cycle sitting above the input threshold, and it cost two hours before being identified. In an
alarm, a sensor that fabricates readings is worse than one that fails.

**No return value may mean two different things.** `distanceCm()` originally returned 0 both for
"no echo" and for "closer than one centimetre". That ambiguity hid the problem above for an
entire debugging session. Failure modes are now distinct and visible.

**The buzzer pin is read back.** The buzzer pin is configured as input and output so
`alarmActive()` reports what the hardware is doing, not what the code intended. The platform
shows the read-back value.

**The status LED runs on plain GPIO, not PWM.** True orange needs PWM on the green channel;
yellow does not, and a state indicator only has to be unambiguous. Dropping the PWM removed a
timer, three channels and a function.

**Explicit Wi-Fi configuration, never inherited.** `esp_wifi_set_storage(WIFI_STORAGE_RAM)` plus
an explicit `esp_wifi_set_mode()`. Without them the driver restores mode and configuration from
NVS, so the same source can boot into different radio modes on two boards depending on what the
previous firmware left behind.

**Virtual interfaces for the hardware abstraction.** One indirection per call and a vtable per
object, against compile-time polymorphism which costs nothing at runtime. At ten calls per
second the indirection is irrelevant next to 240 MHz, and the explainable option won.

---

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

### State machine

There is no separate state-machine module. Each `main.cpp` reads its inputs, drains the ESP-NOW
queue, picks an output and publishes its state. `shared/core/fsm_a.*` and `fsm_b.*` are empty
files kept as placeholders.

| node_a LED | State |
|---|---|
| yellow | warming up — readings not trusted |
| green | armed, no motion |
| red + buzzer | alarm — motion with access closed |
| blue | access open — alarm disarmed |

| node_b LED | State |
|---|---|
| red | access closed |
| yellow | closed, something 4–15 cm from the sensor |
| red, blinking | alert — node_a reported motion while access was closed |
| green | access open |

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

---

## Architecture

```
   PIR ──┐                                              ┌── HC-SR04
         │   node_a — Room (ESP32)                      │   node_b — Door (ESP32)
         │   sense → decide → actuate  ESP-NOW          │   sense → decide → actuate
buzzer ◄─┤   RGB LED              ◄──────────────►      ├─► RGB LED
         │                        packed binary frames  │   access switch
         │                                              │
         └──────────── Wi-Fi · MQTT (JSON) ─────────────┘
                                  │
                      ┌───────────▼────────────┐
                      │  Mosquitto MQTT broker │
                      └───────────┬────────────┘
                                  │
                   platform/app.py  ·  Flask + Flask-SocketIO
                   ├─ last reported state of both nodes
                   ├─ live dashboard        (Socket.IO push)
                   └─ event timeline + offline watchdog
```

**Two representations, one per layer.** The peer link carries packed 10-byte binary frames
(version, type, source, event, sequence, uptime); each node publishes JSON to MQTT. Compact where
the link is constrained, readable where it is inspected.

**Link-layer acknowledgement only.** The ESP-NOW send callback confirms the bytes reached the
peer's radio — Espressif's own documentation is explicit that this is MAC-layer receipt and
nothing more. The application does not yet confirm that a message was acted on. The access
lease makes that safe for the one message that matters: a lost `AccessClosed` is corrected by
the next one a second later.

**ESP-NOW and Wi-Fi share one radio.** ESP-NOW needs no router, no association and no IP, but it
does ride on the 802.11 physical layer as vendor-specific action frames. A radio can only be
tuned to one channel at a time, so once a node associates with an access point its ESP-NOW frames
go out on that AP's channel. Both nodes must therefore use the same AP. The ESP-NOW peer is
registered with channel 0, meaning "whatever channel the station is on", so no number is
hardcoded.

The `docs/` folder was written for an earlier, abandoned design (PlatformIO, Arduino, DHT22, MQTT
for node-to-node traffic). It has not been brought up to date, and this README and the source are
what describe the system as built.

---

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
