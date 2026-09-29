# IoT Dual-Node Challenge — Distributed Alarm System

Technical challenge submission: two independent, wirelessly-linked IoT nodes with a live
dashboard for observation and control. Built for a job-application evaluation; every decision
below is one I can defend live.

**Status: in progress.** See the checklist at the bottom for what is built vs. pending, and
`docs/requirements.md` for the mandatory-constraint-by-constraint mapping. Nothing in this repo
is claimed to work unless it has actually been run — anything simulated-only or unverified says
so explicitly.

## What this is

A distributed alarm and access controller made of two nodes on **different microcontroller
families**, linked by their own radio so they keep cooperating with no router, no broker and no
dashboard, plus a dashboard that lets a human observe and override them.

| | **Node A — Access** | **Node B — Zone** |
|---|---|---|
| MCU | ESP32 DevKit (ESP-WROOM-32, Xtensa LX6) | Teensy 4.1 (IMXRT1062, Cortex-M7 @ 600 MHz) |
| Real sensor | PIR motion detector | HC-SR04 ultrasonic rangefinder |
| Actuator | Servo — see open decision #1 | Buzzer + status LED |
| Status indicator | RGB LED: green = normal, amber = degraded, red = peer lost | same |
| Radio | nRF24L01+ over SPI | nRF24L01+ over SPI |
| Extra role | Wi-Fi gateway between the radio link and MQTT | none |

### Why these two MCUs, and why this split

The Teensy 4.1 has no wireless interface of its own, so both nodes carry an nRF24L01+ and the
node-to-node link runs on that radio rather than on Wi-Fi. The ESP32 additionally holds the Wi-Fi
connection to the broker and acts as gateway to the dashboard.

The sensor assignment is deliberate: the ultrasonic rangefinder needs precise echo pulse-width
measurement, so it lives on the deterministic MCU that has no RF stack competing for cycles. The
PIR is a plain digital level and costs nothing, so it sits on the node that also carries Wi-Fi.
Same reasoning I used on a previous robot project to keep a real-time control loop off a
general-purpose compute board.

### Cross-node behavior (works with the dashboard closed and the router off)

- Node A detects motion (PIR) → `MOTION_DETECTED` over the radio → Node B raises its alert level.
- Node B confirms intrusion (distance below threshold) → `INTRUSION_CONFIRMED` → Node A actuates
  the servo.

These two paths are what satisfy mandatory constraint #3, and the demo shows them with the
network infrastructure switched off entirely.

### Topology consequence, stated openly

The dashboard reaches Node B **through** Node A. The challenge permits a gateway topology, and
the trade-off is deliberate: the node-to-node path stays independent of any infrastructure, at
the cost of the dashboard's view of Node B depending on Node A being alive. The dashboard
therefore distinguishes "Node B is offline" from "the gateway is offline" — it can, because
Node A reports the health of its radio link to B separately from its own.

## Open design decisions

Listed here rather than quietly defaulted, because these are the questions an evaluator is most
likely to ask. Each one gets resolved and justified in `docs/decisions.md` before the
corresponding code is written.

1. **What the servo physically does, and its safe state.** Barrier, latch or indicator flag? And
   on loss of the peer link: *fail-secure* (closed — protects the perimeter, traps whoever is
   inside) or *fail-safe* (open — prioritizes egress)? Both are defensible; neither is defensible
   without an argument. **Unresolved.**
2. **Alarm escalation rules.** Does PIR motion alone arm the buzzer, or only motion followed by a
   confirmed distance reading? How long does the alert level stay raised after motion clears?
   **Unresolved.**
3. **Buzzer stop policy.** An isolated node must never be able to leave the buzzer sounding with
   no way to silence it remotely, so `alarm_duration_s` is enforced locally regardless of link
   state. The remaining question is whether a re-trigger during an active alarm restarts the
   timer. **Partially resolved.**
4. **Distance threshold semantics.** Fixed threshold, or learned baseline with a deviation band
   (more robust to a sensor pointed at a wall at an arbitrary distance)? **Unresolved.**

## Architecture (summary)

```
       PIR ──┐                                          ┌── HC-SR04
             │   Node A — Access (ESP32)                │   Node B — Zone (Teensy 4.1)
             │   sense → FSM → actuate                  │   sense → FSM → actuate
     servo ◄─┘            ▲                             └─► buzzer + LED
                          │                                        ▲
                          │        nRF24L01+ 2.4 GHz               │
                          └────────── SPI both ends ───────────────┘
                                  binary frames, ≤32 B
                          │
                    Wi-Fi │ MQTT (JSON)
                          ▼
                 ┌──────────── MQTT broker ────────────┐
                 │  Mosquitto, local                    │
                 └──────────────────┬───────────────────┘
                                    │
                         platform/  (Flask + Flask-SocketIO)
                         ├─ dashboard: live state, per-node and broadcast commands
                         └─ logger:    telemetry / commands / acks → SQLite
```

**Two representations, one per layer.** The nRF24L01+ caps payloads at 32 bytes, so JSON does not
fit: the radio link carries packed binary frames and the gateway translates to JSON for MQTT.
That is the design, not a workaround — compact on the constrained link, readable on the
supervisory one.

**Two levels of acknowledgement.** The radio's Enhanced ShockBurst gives a hardware ACK and
automatic retransmission, which proves the bytes reached the peer's radio. Mandatory constraint #6
asks for something else: confirmation of *reception and execution*, with the dashboard showing the
state the node reports rather than the command that was sent. So there is also an application-level
ACK carrying `cmd_id`, a result code, and the value actually in effect after validation.

Full diagram and data flow: `docs/architecture.md`. Message schema and frame layout:
`docs/protocol.md`. Rationale for every choice: `docs/decisions.md`.

## Repository layout

```
docs/               architecture, protocol, decisions, requirements mapping, demo script
firmware/common/    shared protocol (de)serialization, transport interface, control logic
firmware/node_a/    Node A — PlatformIO, ESP32, Arduino framework, C++
firmware/node_b/    Node B — Teensy 4.1, C++
firmware/host/      native build: both nodes as Linux processes against mock hardware
platform/           Flask + Flask-SocketIO dashboard and SQLite logger
sim/                simulation notes and any Wokwi project files
tools/              misc scripts
```

## How to run it

### Host simulation — the primary path until the hardware arrives

Wokwi does not simulate the Teensy 4.1 or the nRF24L01+, so it cannot host this system. Instead,
all sensor reads and actuator writes go through an interface with two implementations: the real
one per board, and a **mock** used by a native Linux build. Both node firmwares then compile and
run as ordinary processes on the development machine, with a local-socket transport standing in
for the radio.

This is the same hardware-abstraction pattern I used on a previous project to drive an FPGA over
SPI with a mock for simulation. It is also why the transport abstraction exists at all — it is
load-bearing here, not speculative architecture.

Prerequisites, once the code exists:
- MQTT broker for the platform side:
  `docker run -it -p 1883:1883 -p 9001:9001 eclipse-mosquitto` (needs a `mosquitto.conf` enabling
  the websocket listener on 9001).
- A C++ toolchain for the host build; PlatformIO for the board builds.

**Not yet runnable** — see the checklist.

### Wokwi

Optional and limited: it can run the Node A (ESP32) firmware in isolation to exercise the Wi-Fi
and MQTT path, but not the radio link and not Node B. Notes in `sim/README.md`.

### Firmware on hardware

A first pass was written and verified to build, then scrapped: it moved too fast and added
complexity that was not understood line by line as it went in. It is being rebuilt incrementally.

Note on the features that were in that pass: message authentication and replay protection **are**
now part of the specification in `docs/protocol.md`, deliberately and with a stated reason. The
difference is ordering — the plain link works and is demonstrated first, and the MAC goes on top
of a system that already runs.

### Dashboard / logger

Not yet implemented. Will document here once built.

## Hardware notes

Two cautions recorded here because both cause damage or hard-to-diagnose failures:

- **The HC-SR04 echo pin outputs 5 V and the Teensy 4.1 is not 5 V tolerant.** Its pins are 3.3 V
  only. The echo line goes through a resistor divider (1 kΩ / 2 kΩ) or a level shifter.
- **The nRF24L01+ browns out** on a dev board's 3.3 V rail because transmit current peaks above
  what the regulator supplies cleanly. 10 µF electrolytic plus 100 nF ceramic across VCC/GND, as
  close to the module as possible.
- Servo power comes from its own supply, never from an MCU pin, with grounds tied together.

## Progress checklist

- [x] Repo scaffold, `.gitignore`, docs stubs
- [x] `docs/protocol.md` — frame layout, message types, variable map
- [ ] Resolve open design decisions #1, #2 and #4 → `docs/decisions.md`
- [ ] `firmware/common/` — protocol (de)serialization, rebuilt incrementally
- [ ] Mock hardware layer + native host build for both nodes
- [ ] Node A firmware (ESP32): PIR, servo, radio, Wi-Fi gateway
- [ ] Node B firmware (Teensy 4.1): ultrasonic, buzzer, radio
- [ ] Radio link verified on real hardware
- [ ] Heartbeat, peer-loss detection and safe states verified
- [ ] Application-level command ACK with reported-state display
- [ ] Dashboard (Flask + SocketIO), per-node and broadcast commands
- [ ] Logger → SQLite
- [ ] Message authentication (HMAC) — added once the plain path is demonstrated
- [ ] OTA — explicitly out of scope for this submission (noted, not attempted)

## Author

Hugo — built for a live technical evaluation. Every library and pattern used is explained inline
or in `docs/decisions.md` so it can be defended in front of evaluators.