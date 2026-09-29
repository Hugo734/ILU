# Mandatory requirements — where each one is met

Table kept up to date as the implementation lands, so nothing is shipped with a requirement
quietly unmet. "TBD" means designed but not yet implemented in code.

| # | Requirement | Where it's met | Status |
|---|---|---|---|
| 1 | Two independent physical devices, own MCU, C/C++ | `firmware/node_a/`, `firmware/node_b/` — two separate PlatformIO (Arduino/C++) projects, each targeting its own ESP32 | TBD (scaffolded) |
| 2a | ≥1 sensor per node, ≥1 real in the system | Node A: DHT22 (real) — `firmware/node_a/src/main.cpp` sensor read. Node B: potentiometer standing in for soil probe (simulated, documented as such) | TBD |
| 2b | ≥1 actuator with observable physical output | Node A: fan relay/MOSFET + status LED. Node B: pump/valve relay + status LED | TBD |
| 3 | Nodes communicate wirelessly; ≥1 interaction happens node-to-node without the platform | `climate_alert` (A→B) and `irrigating` (B→A) events over MQTT `events` topic (phase 1) / ESP-NOW (phase 2); both nodes subscribe to each other independent of the dashboard — `firmware/common/include/transport.h`, control loops in each node's `main.cpp` | TBD |
| 4a | Platform visualizes state/readings of both nodes | `platform/dashboard/index.html`, subscribes to `a/state`, `a/telemetry`, `b/state`, `b/telemetry` | Not started |
| 4b | Platform sends a command to a specific node | Dashboard publishes to `a/cmd` or `b/cmd` | Not started |
| 4c | Platform sends a command to both nodes at once | Dashboard publishes to `all/cmd`; both nodes subscribe to it | Not started |
| 5 | ≥2 remotely controllable variables per node | Node A: `temp_setpoint`, `sample_period_ms`, `mode`, `actuator_state` (4). Node B: `moisture_setpoint`, `irrigation_seconds`, `sample_period_ms`, `mode`, `actuator_state` (5) — `docs/protocol.md` cmd schema, applied in each node's command handler | TBD |
| 6 | Node confirms reception/execution; platform shows real reported state, not just the sent command | `ack` message echoes `cmd_id` + `result` + actual value in effect; dashboard renders from `state`/`ack`, never from the command it sent, with pending/confirmed/rejected UI states | TBD |
| 7 | Detect loss of communication, show it, define safe behavior | Heartbeat every 2 s + 6 s timeout + MQTT LWT (`docs/decisions.md` → "Heartbeat timing"); safe states defined per actuator (`docs/decisions.md` → "Safe state on link loss"); dashboard shows offline indicator independently | TBD |
| 8 | External libs/frameworks allowed if explainable | Every dependency justified inline or in `docs/decisions.md`: ArduinoJson (serialization), PubSubClient or similar (MQTT client), MQTT.js (dashboard), mbedTLS SHA-256 (HMAC) | Ongoing |

## Optional extras attempted

| Extra | Where | Status |
|---|---|---|
| Message validation (HMAC + replay check) | `firmware/common/include/hmac_auth.h` | TBD |
| Historical logging | `platform/logger/` (Python → SQLite/CSV) | Not started |
| Alarms for out-of-range | events published + dashboard highlight | Not started |
| Auto reconnect/retry with backoff | MQTT client reconnect logic in each node | TBD |
| OTA firmware update | — | **Explicitly out of scope**, noted in README as a next step |
