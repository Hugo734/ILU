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
cd firmware/node_a && pio run       # build Node A
cd firmware/node_b && pio run       # build Node B
```
**Not yet run in this environment** — PlatformIO is not installed here yet. Marked untested until
verified.

### Wokwi simulation
See `sim/README.md`.

### Dashboard / logger
Not yet implemented — next session. Will document here once built.

## Progress checklist

- [x] Repo scaffold, `.gitignore`, docs stubs
- [ ] `docs/protocol.md` — message schema
- [ ] `firmware/common/` — protocol (de)serialization, HMAC, transport interface
- [ ] Node A firmware
- [ ] Node B firmware
- [ ] Wokwi simulation verified end-to-end
- [ ] Dashboard
- [ ] Logger
- [ ] OTA — explicitly out of scope for this submission (noted, not attempted)

## Author

Hugo — built for a live technical evaluation. Every library and pattern used is explained inline
or in `docs/decisions.md` so it can be defended in front of evaluators.
