# IoT Dual-Node Challenge — Distributed Alarm System

Two wirelessly-linked IoT nodes with a live dashboard for observation, control and history.

**Branch `v2.0`.** Adds remote commands with two-stage acknowledgements, controllable variables on
both nodes, an ESP-NOW heartbeat from node_a to node_b, MQTT Last Will, and a SQLite history. The
branch `main` holds the earlier version, without commands.

What has been checked, and how, is in [Verification](#verification). In short: on 8 October the
v2.0 firmware ran on both boards and every command, acknowledgement and loss-of-link check passed
on hardware. The checks that need a person in front of the sensors are listed there as still open.

---

## What this is

A distributed alarm and access controller made of two nodes that talk to each other directly over
ESP-NOW, plus a platform that lets a human watch both of them live, change their settings, and
review what happened.

| | **node_a — Room** | **node_b — Door** |
|---|---|---|
| MCU | ESP32 DevKitC (ESP-WROOM-32) | ESP32 DevKitC (ESP-WROOM-32) |
| MAC | `78:42:1c:68:44:98` | `f4:65:0b:c0:e0:a4` |
| Sensor | PIR HC-SR501 — motion inside | HC-SR04 — distance at the door |
| Input | — | access switch (3-pin, two pins used) — the credential |
| Actuator | active buzzer — alarm | RGB LED — door state |
| Status indicator | RGB LED | RGB LED |
| Radio | ESP-NOW (peer) + Wi-Fi (MQTT) | ESP-NOW (peer) + Wi-Fi (MQTT) |

In one sentence: with access closed, motion in the room makes node_a sound its buzzer and turns
node_b's LED to blinking red, until someone turns the access switch on at the door.

### Why two identical ESP32s

Both boards carry their own radio, so each node is an independent MQTT client and the dashboard
reaches either one directly, neither node depends on the other to be visible. The wire format and
the command parser live in an ESP-IDF component shared by both projects.

---

## Technology stack

Everything on the microcontroller is ESP-IDF. The communication layer uses the native Espressif
APIs directly.

| Layer | Technology | Why this one |
|---|---|---|
| Firmware | **C++17 on ESP-IDF v5.5.5** | Native Espressif framework |
| Build | **CMake + Ninja**, driven by `idf.py` | ESP-IDF's own build system |
| Code sharing | **ESP-IDF components** via `EXTRA_COMPONENT_DIRS` | `shared/core` and `shared/net` are compiled into both node projects |
| Node ↔ node | **ESP-NOW** (`esp_now`, unicast, registered peer) | Connectionless, no broker, no IP; hardware delivery ACK |
| Wi-Fi | **`esp_wifi`** station mode, brought up explicitly | Init order is `nvs_flash` → `esp_netif` → `esp_event` → `esp_wifi` → `esp_now` |
| Node ↔ platform | **`esp-mqtt`** to a **Mosquitto** broker | Event-driven client, automatic reconnection, Last Will |
| Wire format, node ↔ node | **Packed binary struct**, 10 bytes | Compact on a constrained link, explicit byte layout |
| Wire format, node ↔ platform | **JSON** — state QoS 0, commands and acks QoS 1 | Human-readable, inspectable live with `mosquitto_sub` |
| Command parsing on the node | Hand-written parser in `shared/core/command.cpp` | One flat object with three fields; no library to explain, every rule tested |
| Platform server | **Python 3 · Flask · Flask-SocketIO · paho-mqtt** | Stack easy to build, and great for testing |
| Storage | **SQLite** (`platform/history.db`) | Single file, no server, queryable with plain SQL, exported as CSV |
| Dashboard | Server-rendered HTML + **Socket.IO** push, client vendored in `platform/static/` | No build step, works without internet |
| Tests | **CMake + CTest** on the host; a Python end-to-end test with simulated nodes | The parser and the platform are checked without boards |

---

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
| HC-SR04 `ECHO` | 18 | **through a 1 kΩ / 2 kΩ divider** |
| HC-SR04 `VCC` / `GND` | — | 5 V and GND from the ESP32 board, ~15 mA |
| Access switch | 4 | 3-pin switch: middle pin to GPIO 4, one outer pin to GND, the other unused. Internal pull-up, **no external resistor**. On reads 0 |
| RGB LED R / G / B | 25 / 26 / 27 | 220 Ω in series per colour, common cathode to GND |

Pins deliberately avoided: 0, 2, 12 and 15 are strapping pins whose level at power-up selects
the boot mode; 6–11 are wired to the module's internal SPI flash; 1 and 3 are UART0 and carry the
serial console.

---

## Behavior

The two nodes cooperate over ESP-NOW, with no platform involved. The platform watches, opens or
closes access and drives the buzzer by hand; it is never in the path of an alarm.

**Both LEDs always show the same colour**, so the link between the nodes is visible at a glance:

| Situation | LED on **both** nodes | Buzzer (node_a) |
|---|---|---|
| Access **open** (switch or dashboard) | blue | silent |
| Access closed, nobody at the door | red | silent |
| Access closed, someone between 4 cm and `near_cm` of node_b | green | silent |
| Motion in the room while access is closed (**alarm latched**) | **red blinking, in step** | **sounds until access opens** |
| The other node is not heard for 3 s | **blue blinking** (on the node that lost the other) | — |

Priority, identical on both nodes: alarm → lost link → open → (node_a only: PIR warm-up, yellow)
→ someone near → idle. node_a never raises the alarm during warm-up (`warmup_s`, 60 s by
default): the HC-SR501 fires on its own while its pyroelectric element settles.

How each piece of shared state reaches the other node:

- **Access and "someone near"** start on node_b. node_b sends `AccessOpen`/`AccessClosed` and
  `NearOn`/`NearOff` over ESP-NOW on every change and again every `repeat_ms`, and node_a mirrors
  them.
- **The alarm** starts on node_a. Motion while armed **latches** it: the buzzer keeps sounding and
  the LED keeps blinking even after the motion stops. node_a sends `MotionStarted` at once and
  repeats it every second for as long as the alarm stays latched (in place of the heartbeat), so
  one lost frame cannot leave node_b unaware.
- **Clearing the alarm** = opening access, with the switch or with the dashboard's button. node_b
  clears its alert, node_a clears its latch (it learns from node_b's `AccessOpen`, or directly from
  the same broadcast command), and both turn blue.
- **Blinking in step.** node_b learns node_a's clock from the `uptime_ms` of every frame and
  blinks on that clock, with the same 500 ms half-period. The two LEDs are in phase to within the
  100 ms loop period.

**Switch and dashboard together: whichever acted last wins.** The switch acts when it is
flipped, not by its position. After the dashboard closes access, a switch left on does not
reopen it until it is flipped again. node_b reports `access_by` (`switch` or `platform`) so the
dashboard shows who changed it last. After a reboot, node_b starts from the switch's position.

**The headline interaction:** with access closed, one gesture in front of node_a and both boards
react at once — buzzer and red blinking on both — over ESP-NOW, with no platform involved. Opening
access (switch or button) is the acknowledgement: both go blue and the buzzer stops.

### Loss of communication and safe behaviour

Each failure is detected by the side that suffers it, and each one ends in the safe state.

| What is lost | Who detects it, and how | What the node does |
|---|---|---|
| node_b, seen from node_a | access lease: node_b repeats its access state every `repeat_ms`; node_a counts access as open only while an `AccessOpen` from the last 3 s backs it | **stays armed, LED blue blinking**. A dead node_b, a lost `AccessClosed` or a broken link can never disarm the room. Reports `"peer":false` |
| node_a, seen from node_b | heartbeat: node_a sends an ESP-NOW frame every second (heartbeat, or `MotionStarted` while the alarm is latched); 3 s of silence means the link is lost | **LED blue blinking**: the room is no longer watched, and the door says so. An alert already raised stays (red blinking has priority). Reports `"peer":false` |
| either node, seen from the platform | silence: no state report for 3 s | dashboard marks it **offline**, greys out its card, logs the event; commands to it time out |
| either node, seen from the broker | MQTT Last Will: the broker publishes `offline` on `ilu/<node>/status` after 1.5 × the 5 s keepalive | dashboard shows the broker's verdict too; a second, independent detector |
| the platform | — | the nodes keep cooperating over ESP-NOW; MQTT publishes are skipped until it returns |

---

## Remotely controllable variables

The dashboard's two main buttons drive `access` and `buzzer_on`. The other settings stay
available in the dashboard's collapsed **Advanced** panel, so each node exposes more than the two
remotely controllable variables requirement #5 asks for.

| Variable | Node | Range | Default | Effect |
|---|---|---|---|---|
| `access` | **A and B** | 0 – 1 | switch position at boot | **Open/Close button**, sent once on `ilu/all/cmd`. node_b: opens (1) or closes (0) access, like a flip of the switch. node_a: starts or ends its access lease at once; node_b's frames keep deciding within 3 s, so a platform "open" that node_b did not apply cannot keep the room disarmed |
| `buzzer_on` | A | 0 – 1 | 0 | **Buzzer button.** Manual buzzer, independent of the alarm. A latched alarm keeps sounding even if it is set to 0 |
| `buzzer_enabled` | A | 0 – 1 | 1 | 0 = silent alarm: LED red and node_b alerted, no sound |
| `warmup_s` | A | 0 – 300 | 60 | PIR warm-up |
| `near_cm` | B | 5 – 100 | 15 | upper edge of the "near" window (the lower edge is fixed at 4 cm) |
| `repeat_ms` | B | 200 – 1000 | 1000 | access-state repeat to node_a. Capped at 1 s so three repeats fit in node_a's 3 s lease |
| `publish_ms` | A and B | 250 – 2000 | 1000 | state report period. Capped at 2 s so the platform's 3 s offline rule still holds |

**The node is the authority.** The platform does not check ranges: it forwards whatever the user
types, and the node accepts or rejects it. Every rejection on screen is the node's own answer.
Values live in RAM and return to the defaults on reboot; the dashboard shows that, because it
displays what the node reports, not what was sent.

`access` and `buzzer_on` are state rather than settings: `access` is reported as `access` (with
`access_by` on node_b), and the buzzer as `buzzer`, read back from the pin.

Fixed in the firmware: node_a's access lease (3 s), the peer timeout (3 s), the heartbeat period
(1 s), the blink half-period (500 ms), and the 4 cm lower edge of the near window.

---

## Commands and acknowledgements

### MQTT topics

| Topic | Direction | QoS | Payload |
|---|---|---|---|
| `ilu/<node>/state` | node → platform | 0 | full state, every `publish_ms` and at once on any change |
| `ilu/<node>/cmd` | platform → one node | 1 | `{"id":"143005-12","var":"near_cm","value":20}` |
| `ilu/all/cmd` | platform → both nodes | 1 | same; both nodes are subscribed, so one publish reaches both at once |
| `ilu/<node>/ack` | node → platform | 1 | see below |
| `ilu/<node>/status` | node / broker → platform | 1, retained | `online` from the node on connect; `offline` from the broker as Last Will |

```
ilu/a/state {"node":"a","up":29557,"warm":true,"motion":false,"alarm":false,"buzzer":0,"access":false,"near":true,"peer":true,"led":"green","buzzer_on":0,"buzzer_enabled":1,"warmup_s":60,"publish_ms":1000}
ilu/b/state {"node":"b","up":29603,"cm":9,"near":true,"access":false,"access_by":"platform","alert":false,"peer":true,"led":"green","near_cm":15,"repeat_ms":1000,"publish_ms":1000}
```

`led` is one of `red`, `yellow`, `green`, `blue`, `red_blink`, `blue_blink`. On node_a, `near` is
node_b's near state as heard over ESP-NOW, and `alarm` is the latched alarm. `buzzer` is read back from
the pin, not taken from the code's intention.

### Two acknowledgements per command

Requirement #6 asks for confirmation of reception **and** of execution, so each node answers
twice:

```
ilu/b/ack {"node":"b","id":"143005-12","var":"near_cm","stage":"received"}
ilu/b/ack {"node":"b","id":"143005-12","var":"near_cm","stage":"applied","value":20}
```

or, for a value the node refuses:

```
ilu/b/ack {"node":"b","id":"143005-13","var":"near_cm","stage":"rejected","reason":"out of range","value":20}
```

1. **Received** is sent by the MQTT task as soon as the payload parses. A payload that does not
   parse is answered there with `rejected` and the parser's reason (`missing value`,
   `value must be an integer`, `unknown field`, …).
2. The command then goes through a 4-slot queue to the main loop, the same pattern as the ESP-NOW
   receive path, so all control logic stays in one task. A full queue is answered `node busy`.
3. **Applied / rejected** is sent by the main loop after it checks the variable name and the
   range. `value` is the value **now in effect**: the new one, or the old one after a rejection.
4. The node's next state report carries the setting too, and the dashboard logs
   "Node reports near_cm = 20 (was 15)". That is the node's behaviour, not just its word.

The platform tracks each command per node: **pending → received → applied | rejected**, or
**timed out** after 3 s without a final answer. A late answer still updates the command, marked
as late: the node's answer wins over the platform's guess.

---

## The platform

`platform/app.py` is a single Python process. Run it on the laptop that hosts the broker.

1. **MQTT client.** Subscribes to `ilu/#`: state, acks and status.
2. **Authoritative state.** Holds the last state each node reported. Every value in the UI comes
   from a report, never from an assumption.
3. **Commands.** Publishes to `ilu/a/cmd`, `ilu/b/cmd` or `ilu/all/cmd` and tracks the answers.
4. **Socket.IO push.** Every update reaches the browser at once, without polling. A browser that
   opens late receives a full snapshot, commands included.
5. **Watchdog.** A node with no report for 3 s is marked offline; the first report afterwards
   marks it back online. `up` going backwards is shown as "Node rebooted". Commands without a
   final answer after 3 s are marked timed out.
6. **History.** Every report, event, command and ack goes to SQLite as it arrives.

### Dashboard

- Header with the broker connection status.
- Topology diagram: a dot pulses along a node's MQTT wire on each report. The ESP-NOW link is
  drawn and labelled *not seen by the platform*, because that traffic never touches the broker.
- One card per node: mirrored LED including the blink, online pill (its tooltip shows the Last
  Will status), "last report x s ago", every field including the ESP-NOW link, the **settings in
  effect as reported by the node**, a collapsible raw JSON, and for node_b a distance bar whose
  near zone follows the reported `near_cm`, with a 60 s sparkline.
- **Control panel:** two big buttons. **Open/Close access** sends `access` to both nodes on
  `ilu/all/cmd`. **Buzzer ON/OFF** sends `buzzer_on` to node_a. Each button shows the state the
  node reported and offers the opposite; its label changes only when a report confirms it. The
  generic form (target, variable, value) stays in a collapsed **Advanced** panel. Below that are
  the last commands, with one chip per node: pending, received, applied with the value and the
  round-trip time, rejected with the reason and the value still in effect, or timed out.
- Event timeline with filters All / Alarm / Access / Commands / Link / Info, showing server time
  and node uptime. `near` is deliberately not a timeline event, because it flickers.
- CSV export links for the four history tables.

The server uses Werkzeug's development server, which is enough for one laptop.

### Historical data

| Table | Holds | Columns |
|---|---|---|
| `telemetry` | every state report | `ts_server`, `node`, `up_ms`, `payload` (the JSON as received) |
| `events` | every timeline entry | `ts_server`, `node`, `up_ms`, `kind`, `text` |
| `commands` | every command sent | `ts_server`, `cmd_id`, `target`, `var`, `value` |
| `acks` | every acknowledgement received | `ts_server`, `cmd_id`, `node`, `stage`, `var`, `value`, `reason` |

`commands` and `acks` join on `cmd_id`: what was asked, what each node answered, and when.
`up_ms` is the node's own clock and `ts_server` the platform's, so a row's two timestamps can be
compared. Export: `http://<laptop>:5000/export/<table>.csv`, or open the file with `sqlite3`.

---

## Not implemented

- **ESP-NOW encryption.** The only filter is the sender's MAC address.
- **Authentication on MQTT.** The broker is anonymous, no TLS — a prototype shortcut, documented
  in `platform/mosquitto/ilu.conf`.
- **Settings survive a reboot.** They live in RAM; NVS persistence was left out on purpose so a
  reboot always returns to a known state.
- **Wi-Fi power-save control.** `esp_wifi_set_ps` is not called. Modem sleep can hurt ESP-NOW
  reception; it was not a problem in the hardware tests on `main`.
- **Router-off test.** Both nodes are on the router's Wi-Fi, so ESP-NOW rides on that access
  point's channel. ESP-NOW needs no router by design, but that was not tested with the router off.

---

## Verification

| What | How | Result |
|---|---|---|
| Firmware of both nodes, v2.0 | `idf.py build` with ESP-IDF v5.5.5 | compiles with no warnings; each binary ~900 KB, 14% of the 1 MB app partition free |
| Command parser, range check, ack format | `test/test_command.cpp`, host build, CTest | all pass; a deliberately broken range check makes them fail |
| ESP-NOW frame parser | `test/test_protocol.cpp` | all pass |
| Platform end to end | `test/test_platform.py`: real broker, real `app.py`, two simulated nodes speaking the firmware's MQTT contract | 46 checks pass, including the two buttons: commands to one node and to both, applied / rejected / timed out, platform-side input checks, silence watchdog, Last Will, peer-loss events, SQLite and CSV |
| Dashboard page logic | the page loaded in a simulated DOM (jsdom) and fed the server's events | renders state, settings, command chips and filters with no script errors |
| Dashboard in a real browser | headless Chrome against the running server with both boards live | cards, settings in effect, command table with chips and round-trip times, timeline and CSV links all render with real data |

### On the boards (v2.0, 8 October)

Both boards flashed with v2.0 and driven through the running dashboard over Socket.IO, exactly as
the **Send** button does. Losing a node was simulated by holding the board in reset through the
USB adapter's RTS line, which is the same as cutting its power.

| Check | Result |
|---|---|
| Boot, Wi-Fi on channel 11, MQTT, `ilu/<node>/status online` | both nodes, ~12–18 s after reset |
| `near_cm = 25` to node_b | `received` then `applied = 25`, ~0.35 s; next state report carries 25 |
| `near_cm = 500` to node_b | `rejected: out of range · still 25` |
| `near_cm = 30` to **Both** (one publish on `ilu/all/cmd`) | node_a `rejected: unknown variable`, node_b `applied = 30` |
| `publish_ms = 500` to Both | both `applied = 500`; reports go from ~1/s to ~1.8/s |
| `buzzer_enabled = 0`, `warmup_s = 10` to node_a; `repeat_ms = 100` then `500` to node_b | applied, applied, rejected (still 1000), applied |
| Malformed commands by hand (`2.5`, missing value, misspelt field, not JSON, escape, 2 KB payload, 11-digit number) | each one `rejected` with the parser's reason; valid commands around them still applied |
| node_a held in reset | dashboard offline 2.8 s; node_b reports ESP-NOW link lost 2.8 s (heartbeat); Last Will 9.3 s; command to it timed out; back online with "rebooted, settings back to defaults" |
| node_b held in reset (access switch on) | node_a drops the access lease and **re-arms 3.0 s** later; dashboard offline 2.4 s; Last Will 10.8 s; recovers by itself |

### Synchronized LEDs and the two buttons (8 October, later the same day)

| Check | Result |
|---|---|
| Boot | both blue, access open by switch; node_a mirrors node_b's `near: true` (HC-SR04 reading 9 cm) |
| **Close access** button (one publish, both nodes) | node_a `applied = 0` in 0.22 s, node_b in 0.18 s; node_b reports `access_by: platform`; both LEDs green (someone near) and equal for 3 s |
| **Buzzer ON / OFF** button | applied; the pin read back goes 1, then 0 |
| **Open access** button | both applied; both LEDs blue; node_a's lease still held 4 s later, fed by node_b |
| node_a held in reset | node_b `blue_blink` at 2.7 s; offline 2.9 s; Last Will 8.8 s; recovers |
| node_b held in reset | node_a `blue_blink` and armed at 2.6 s; offline 2.9 s; Last Will 8.8 s; recovers, access back to the switch's position |

**Still to check by hand** (needs someone at the boards): motion with access closed → both LEDs
blink red together and the buzzer keeps sounding after the motion stops; then the switch or the
Open button stops it. Also: the switch flipped after a platform Close reopens access; and by eye,
that the two blinking LEDs look in step.

---

## How the code works

A walk through the code in the order data flows through it. Every name below is a real function
or file; read this next to the source.

### The ESP-NOW frame — `shared/core/protocol.{h,cpp}`

Node to node, every message is one 10-byte packed struct:

| Byte | Field | Meaning |
|---|---|---|
| 0 | `version` | `PROTOCOL_VERSION` (1); a frame from other firmware is refused |
| 1 | `type` | `Heartbeat` (1) or `Event` (2) |
| 2 | `src` | `'A'` or `'B'` |
| 3 | `event` | `MotionStarted` 1, `MotionStopped` 2, `AccessOpen` 3, `AccessClosed` 4, `NearOn` 5, `NearOff` 6; 0 for a heartbeat |
| 4–5 | `seq` | per-sender counter; a gap is logged as lost frames |
| 6–9 | `uptime_ms` | sender's clock: for the logs, and node_b blinks on node_a's clock taken from it |

`__attribute__((packed))` and a `static_assert(sizeof(Frame) == 10)` keep both binaries agreeing
on the layout. `protocol_parse()` requires exactly 10 bytes, copies them into a local `Frame`
with `memcpy` (a radio buffer has no alignment guarantee), checks version, source and that the
event fits the type, and only then writes to the caller's frame. It holds no state, which makes it
easy to test on the host (`test/test_protocol.cpp`).

### Network layer — `shared/net/net.cpp`

Three tasks are involved, and the code keeps them apart on purpose:

| Task | Runs | Does |
|---|---|---|
| Wi-Fi task and default event loop (ESP-IDF) | `on_wifi_event`, `on_recv`, `on_sent` | joins the AP and retries on loss; starts MQTT on the first IP; filters ESP-NOW frames by sender MAC and `protocol_parse()`, then pushes them onto an 8-slot queue with timeout 0 |
| MQTT task (esp-mqtt) | `on_mqtt_event`, `on_command` | on connect: subscribe `ilu/<id>/cmd` and `ilu/all/cmd`, publish `online` (retained). On a command: parse it, send the `received` ack, push it onto a 4-slot queue |
| Main task (`app_main` loop) | everything else | drains both queues, decides every output, publishes the state |

**Why queues:** the callbacks run in tasks that must not block, and if they changed the node's
state directly, two tasks would write to it. With the queues, all control logic runs in one task.
Nothing needs a lock, and the behaviour can be read top to bottom in `main.cpp`. A full queue is
counted (ESP-NOW) or answered `node busy` (commands), never silent.

`net_init()` order matters and is commented: NVS → netif → event loop → Wi-Fi config → command
queue → MQTT client (keepalive 5 s, Last Will `offline` retained on `ilu/<id>/status`) → handlers
→ `esp_wifi_start()`. `espnow_init()` runs after that, because ESP-NOW rides on a running radio;
the peer uses channel 0 = "the channel the station is already on", which is how ESP-NOW and Wi-Fi
share one radio.

### Commands — `shared/core/command.{h,cpp}`

A hand-written parser for exactly one flat object, `{"id":"…","var":"…","value":N}`:

- `command_parse()` walks the buffer with a cursor (`skip_ws`, `eat`, `read_string`, `read_int`).
  It refuses unknown or duplicate fields, escapes, control characters, non-integers (`2.5`,
  `1e3`), values outside int32 and trailing data, and returns a short reason string that goes back
  to the platform as is. Because escapes are refused, the id and var can be echoed in the ack
  without re-escaping.
- `command_apply()` looks the name up in the node's `VarSpec` table (name, min, max), and writes
  the value only if it is in range. A rejected command changes nothing.
- `ack_format()` builds the ack JSON with `snprintf`, and reports when it does not fit.

None of this includes an ESP-IDF header, so `test/test_command.cpp` runs it on the laptop.

### node_a — `node_a/main/main.cpp`

`app_main()` runs an LED and buzzer self-test, then `net_init('a')` and `espnow_init(PEER_MAC)`,
then loops every 100 ms:

1. `handle_commands(now)`: apply every queued command, send `applied`/`rejected` with the value
   now in effect. An applied `access` goes through `apply_access()`, the same function the
   ESP-NOW path uses. It runs first so a new value drives this same iteration.
2. Read the PIR; `warm` = uptime ≥ `warmup_s`.
3. Drain the ESP-NOW queue. Any frame refreshes `last_peer_ms`; `AccessOpen`/`AccessClosed` call
   `apply_access()`; `NearOn`/`NearOff` set `near_b`.
4. `access` = an open lease less than 3 s old. `peer` = any frame within 3 s.
5. The latch: access open clears it; otherwise `warm && motion` sets it. `alarm` = the latch.
6. Buzzer pin = (`alarm` and `buzzer_enabled`) or `buzzer_on`.
7. LED, by priority: alarm → red blinking; no peer → blue blinking; access → blue; warming up →
   yellow; `near_b` → green; else red. The blink phase is `(now / 500) % 2`.
8. Every second, or at once when the alarm latches: send `MotionStarted` while latched, else a
   `Heartbeat`.
9. Build the state JSON; publish it if anything but the uptime changed, or every `publish_ms`.
   `buzzer` is read back from the pin (`alarmActive()`).

### node_b — `node_b/main/main.cpp`

Same skeleton. Each 100 ms:

1. `handle_commands()`; an applied `access` sets `access_by = "platform"`.
2. Read the HC-SR04 (`distanceCm()`: 10 µs trigger, time the echo, 58 µs per cm; 0 when there is
   no echo). Read the switch: **a flip** writes `cfg[ACCESS]` and sets `access_by = "switch"`.
   `access = cfg[ACCESS]`; `near` = 4 cm ≤ distance ≤ `near_cm`.
3. Drain the ESP-NOW queue: log `seq` gaps; refresh `last_peer_ms` and `clock_offset` (node_a's
   uptime minus ours); `MotionStarted` while access is closed sets `alert`.
4. `peer` = a frame from node_a within 3 s (the heartbeat makes this meaningful).
5. Access open clears the alert, because opening access is the acknowledgement.
6. On a change of access or near, and every `repeat_ms`, send two frames: the access state (node_a's
   lease) and the near state (node_a's mirror).
7. LED, same priority as node_a: alert → red blinking; node_a lost → blue blinking; access →
   blue; near → green; else red. The blink phase is `((now + clock_offset) / 500) % 2`, i.e.
   node_a's clock, which is what keeps the two LEDs in step.
8. Publish the state; `cm` is left out of change detection because it jitters.

### Platform — `platform/app.py`

paho runs the MQTT client in its own thread; Flask-SocketIO serves the page and pushes updates.
One re-entrant lock guards the shared state and the SQLite connection.

- `on_message` routes `ilu/<node>/<kind>` to `handle_state`, `handle_ack` or `handle_status`.
- `handle_state` stores the report, and `diff_events` turns the difference with the previous
  report into timeline events (the `WATCHED` table, settings changes, reboot detection). Then it
  emits `node` to the browsers and writes a `telemetry` row.
- `send_command` (called from the browser's `command` Socket.IO event) checks only the shape
  (target, name, integer), never the range. It gives the command an id `HHMMSS-n` and publishes
  once on `ilu/a/cmd`, `ilu/b/cmd` or `ilu/all/cmd`. It tracks one slot per target node.
- `handle_ack` moves a slot `pending → received → applied | rejected`; a final ack after a
  timeout still lands, marked late.
- `watchdog` (every 0.5 s): a node silent for 3 s → offline; a command without a final ack after
  3 s → timed out.
- `on_browser_connect` sends a full snapshot, so a late browser sees everything at once.
- `/export/<table>.csv` streams a whitelisted SQLite table.

`platform/templates/index.html` only renders what the server sends (`snapshot`, `node`, `event`,
`command`, `broker`). It corrects for the browser's clock (`skew`) so "last report x s ago" is
measured against the server's clock.

### One command, end to end

```
browser  --Socket.IO "command"-->  app.py send_command  --MQTT ilu/all/cmd (QoS 1)-->  broker
broker   --> node_a MQTT task: command_parse -> ack "received" -> queue
         --> node_b MQTT task: same, in parallel
main loop: handle_commands -> command_apply -> ack "applied"/"rejected" + value in effect
app.py handle_ack -> slot updated -> Socket.IO "command" -> chip turns green/red
next state report -> diff_events -> "Node reports near_cm = 30 (was 15)"
```

---

## Repository layout

```
CMakeLists.txt           host build for the pure logic — not an ESP-IDF project
CLAUDE.md                working notes for the coding assistant
docs/instructionOfUse.md demo-day command guide
shared/
  core/                  no ESP-IDF headers — compiles and is tested on the laptop
    CMakeLists.txt         dual mode: ESP-IDF component, or plain library for the host build
    hal.h                  IHalA / IHalB — the hardware interfaces the nodes use
    protocol.h/.cpp        10-byte Frame, MsgType, EventId, protocol_parse()
    command.h/.cpp         command parser, variable range check, ack formatting
  net/                   Wi-Fi, MQTT (state, commands, acks, Last Will), ESP-NOW queues
    secrets.h.example      template for the git-ignored secrets.h
node_a/
  main/                  main.cpp + hal_a_esp32 (PIR, buzzer, RGB LED)
node_b/
  main/                  main.cpp + hal_b_esp32 (HC-SR04, access switch, RGB LED)
platform/
  app.py                 MQTT client, state store, commands, Socket.IO, watchdog, SQLite
  templates/index.html   the dashboard page
  static/                vendored Socket.IO client
  mosquitto/ilu.conf     broker configuration
test/
  test_protocol.cpp      host tests, ESP-NOW frame parser
  test_command.cpp       host tests, command parser and range check
  test_platform.py       end-to-end platform test with its own broker
  sim_nodes.py           MQTT stand-ins for both nodes
images/                  circuit diagrams
```

The node logic lives in each `main.cpp`: read inputs, apply commands, drain the ESP-NOW queue,
decide the outputs, publish the state. There is no separate state-machine module.

`shared/core` exists so the wire formats can be exercised without a board. **If it ever fails to
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

The nodes have the broker's address compiled in: **the laptop's IP must be the one in
`MQTT_BROKER_URI` when the nodes were built.** There is no fixed address; any IP works as long as
the two match. If the laptop's IP changes, the nodes still join Wi-Fi but never reach the broker,
and the dashboard marks them offline. Reserve the address in the router, or change
`MQTT_BROKER_URI` and reflash both nodes.

**Broker.** Install `platform/mosquitto/ilu.conf` into `/etc/mosquitto/conf.d/` and restart
Mosquitto. Watch the raw traffic, or send a command by hand:

```bash
mosquitto_sub -h <broker-ip> -t 'ilu/#' -v
mosquitto_pub -h <broker-ip> -t ilu/all/cmd -m '{"id":"manual-1","var":"publish_ms","value":500}'
```

**Dashboard.**

```bash
python3 -m venv platform/.venv
platform/.venv/bin/pip install -r platform/requirements.txt
env -u PYTHONPATH platform/.venv/bin/python platform/app.py   # broker on localhost by default
# open http://localhost:5000, or http://<laptop-ip>:5000 from another device on the LAN
```

**Tests.**

```bash
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure

platform/.venv/bin/pip install "python-socketio[client]" amqtt
platform/.venv/bin/python test/test_platform.py
```

**Back to the verified version.** `git switch main`, reflash both nodes and restart `app.py`.

The full demo-day command list is in `docs/instructionOfUse.md`.

---

## Mandatory requirements

| # | Requirement | How it is met | Status |
|---|---|---|---|
| 1 | Two independent devices, own MCU, C/C++ | two ESP32 boards, C++17 on ESP-IDF | done |
| 2 | Each node: ≥1 sensor and ≥1 actuator with observable output | node_a: PIR + buzzer and LED. node_b: HC-SR04 + LED | done, verified on hardware |
| 3 | Wireless node-to-node, ≥1 interaction without the platform | latched alarm A→B (both LEDs blink red), access and near state B→A (both LEDs show the same colour), heartbeat A→B, all over ESP-NOW | done; access, near and heartbeat verified on hardware with v2.0, the latched alarm still to be seen by hand |
| 4 | Platform: view both nodes; command one node; command both | live dashboard; buzzer button → `ilu/a/cmd`; Open/Close button → one publish on `ilu/all/cmd` that both nodes apply and acknowledge | done, verified on hardware |
| 5 | ≥2 remotely controllable variables per node | node_a: five, node_b: four (two behind the main buttons, the rest in the Advanced panel) | done, verified on hardware |
| 6 | Confirm reception **and** execution; show the state the node reports | `received` then `applied`/`rejected` with the value in effect; settings carried in every state report | done, verified on hardware |
| 7 | Detect loss of communication, show it, define safe behaviour | access lease, A→B heartbeat, platform watchdog, MQTT Last Will; safe states in the table above | done, verified on hardware (node_b's blue LED not yet seen by eye) |
| 8 | Able to explain the data and command flow | this README | — |

Optional items also covered: message validation (both parsers reject anything malformed, and the
nodes only accept ESP-NOW frames from their peer's MAC), history of readings and commands, alarm
notifications on the dashboard, automatic reconnection (Wi-Fi, MQTT, platform to broker).

---

## Author

Hugo Daniel Castillo Ovando — Robotics and Digital Systems Engineer
