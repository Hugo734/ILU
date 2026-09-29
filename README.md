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
diagrams/           Electronic connection
```

## How to run it

Not yet defined. Will be here ...
### Dashboard / logger

Not yet implemented. Will document here once built.




