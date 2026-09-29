# IoT Dual-Node Challenge — Distributed Greenhouse

Technical challenge submission: two independent, wirelessly-linked IoT nodes with a live
dashboard for observation and control. Built for a job-application evaluation; every decision
below is one I can defend live.

**Status: in progress.** See the checklist at the bottom for what is built vs. pending, and
`docs/requirements.md` for the mandatory-constraint-by-constraint mapping. Nothing in this repo
is claimed to work unless it has actually been run — anything simulated-only or unverified says
so explicitly.

## What this is

A distributed greenhouse controller made of two ESP32 nodes that never need the dashboard to
cooperate with each other, plus a dashboard that lets a human observe and override them.

| | **Node A — Climate** | **Node B — Soil** |
|---|---|---|
| MCU | ESP32 DevKit (ESP-WROOM-32) | ESP32 DevKit |
| Real sensor | DHT22 (temperature + humidity) | — |
| Simulated sensor | — | Soil moisture, via potentiometer (capacitive probe arrives later) |
| Actuator | Fan (relay/MOSFET) + status LED | Water pump/solenoid valve (relay) + status LED |
| Status indicator | RGB LED: green=normal, amber=degraded, red=link lost | same |

**Cross-node behavior (works with the dashboard closed):**
- Node A too hot → publishes `climate_alert` → Node B shortens its irrigation interval.
- Node B irrigating → publishes `irrigating` → Node A pauses its fan so moisture isn't blown off.

Full rationale for every choice below lives in `docs/decisions.md`.

## Architecture (summary)

```
 DHT22 ─┐                                    ┌─ potentiometer (soil sim)
        │  Node A (ESP32)                    │  Node B (ESP32)
        │  sense→control→actuate             │  sense→control→actuate
 fan  ◄─┘        │  ▲                        └─►  pump/valve
                 │  │ MQTT (Wi-Fi)                    │  ▲
                 ▼  │                                 ▼  │
              ┌────────────── MQTT broker ──────────────┐
              │ Mosquitto (native) / broker.hivemq.com   │
              │ (Wokwi)                                  │
              └──────────────┬────────────────────────┬─┘
                              │                        │
                     platform/dashboard          platform/logger
                    (static HTML + MQTT.js       (Python subscriber
                     over WebSockets)              → SQLite/CSV)
```

Phase 1 (now, simulator + native): all traffic — including inter-node events — rides MQTT over
Wi-Fi, because that's what Wokwi can simulate without real radios.

Phase 2 (after hardware arrives): the node-to-node path (climate_alert/irrigating/heartbeat)
moves to ESP-NOW, direct and broker-independent, behind the same transport interface used today.
MQTT keeps serving the dashboard. Why two transports: see `docs/decisions.md`.

Full diagram and data flow: `docs/architecture.md`. Message schema: `docs/protocol.md`.

## Repository layout

```
docs/               architecture, protocol, decisions, requirements mapping, demo script
firmware/common/    shared protocol (de)serialization, HMAC, transport interface — PlatformIO library
firmware/node_a/    Node A PlatformIO project (ESP32, Arduino framework, C++)
firmware/node_b/    Node B PlatformIO project
platform/dashboard/ single-page dashboard (vanilla JS + MQTT.js over WebSockets)
platform/logger/    Python MQTT subscriber logging to SQLite/CSV
sim/wokwi/          Wokwi simulation projects (diagram.json + wokwi.toml) per node
tools/              misc scripts
```

## How to run it

### Native / simulator prerequisites
- A local MQTT broker for native testing: `docker run -it -p 1883:1883 -p 9001:9001 eclipse-mosquitto`
  (a `mosquitto.conf` enabling the websocket listener on 9001 is required — see
  `platform/dashboard/README` once written).
- For Wokwi: no broker to run yourself; nodes are configured to reach `broker.hivemq.com`, a
  public test broker, under a unique topic prefix (see `docs/protocol.md`).
- PlatformIO (`pip install platformio` or the VS Code extension) to build/flash the firmware.

### Firmware
```
cd firmware/node_a && cp include/secrets.h.example include/secrets.h  # fill in real values
pio run       # build Node A
cd firmware/node_b && cp include/secrets.h.example include/secrets.h
pio run       # build Node B
```
**Verified:** both build cleanly with PlatformIO (`espressif32`/Arduino framework) —
RAM ~14%, Flash ~59–60% of an ESP32's budget, no compile or link errors. Not yet flashed to real
hardware or run in Wokwi — that's still pending (see checklist).

The pure control-logic functions are also unit-tested independently of the ESP32 toolchain:
```
cd firmware/node_a/test && g++ -std=c++17 -I../src test_control_logic.cpp ../src/control_logic.cpp -o /tmp/test_a && /tmp/test_a
cd firmware/node_b/test && g++ -std=c++17 -I../src test_control_logic.cpp ../src/control_logic.cpp -o /tmp/test_b && /tmp/test_b
```
**Verified:** both suites pass (7 and 9 assertions respectively, see terminal output when run).

### Wokwi simulation
See `sim/README.md`. **Not yet verified** — the diagram files are written but haven't been opened
in Wokwi yet; that's explicitly flagged there, including an open question about whether Wokwi can
simulate ESP-NOW between two independent node projects (relevant for phase 2, not phase 1).

### Dashboard / logger
Not yet implemented — next session. Will document here once built.

## Progress checklist

- [x] Repo scaffold, `.gitignore`, docs stubs
- [x] `docs/protocol.md` — message schema, HMAC algorithm with a Python-verified worked example
- [x] `firmware/common/` — protocol (de)serialization, HMAC, transport interface + MQTT impl
- [x] Node A firmware — builds clean with PlatformIO; control logic unit-tested natively
- [x] Node B firmware — builds clean with PlatformIO; control logic unit-tested natively
- [ ] Wokwi simulation verified end-to-end (diagrams written, not yet opened/run — see `sim/README.md`)
- [ ] Dashboard
- [ ] Logger
- [ ] OTA — explicitly out of scope for this submission (noted, not attempted)

Not yet done, and known to be needed before this is demo-ready: flashing/testing on real
hardware (arrives Wednesday), running the Wokwi simulation to confirm the diagrams actually work,
and everything platform-side (dashboard + logger), which is next session's work.

## Author

Hugo — built for a live technical evaluation. Every library and pattern used is explained inline
or in `docs/decisions.md` so it can be defended in front of evaluators.
