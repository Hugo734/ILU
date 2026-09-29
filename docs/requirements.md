# Mandatory requirements — where each one is met

Table kept up to date as the implementation lands, so nothing is shipped with a requirement
quietly unmet. "TBD" means designed but not yet implemented in code.

| # | Requirement | Where it's met | Status |
|---|---|---|---|
| 1 | Two independent physical devices, own MCU, C/C++ | `firmware/node_a/`, `firmware/node_b/` — two separate PlatformIO (Arduino/C++) projects, each targeting its own ESP32. Both build clean with `pio run`. | Firmware done; real hardware pending Wednesday |
| 2a | ≥1 sensor per node, ≥1 real in the system | Node A: DHT22 (real) — read in `firmware/node_a/src/main.cpp` `loop()`. Node B: potentiometer standing in for soil probe (simulated, documented in `readMoisturePct()`, `firmware/node_b/src/main.cpp`) | Done in firmware; untested on hardware |
| 2b | ≥1 actuator with observable physical output | Node A: fan relay/MOSFET (`PIN_FAN`) + RGB status LED. Node B: pump/valve relay (`PIN_PUMP`) + RGB status LED. Driven in each `main.cpp` `loop()`. | Done in firmware; untested on hardware |
| 3 | Nodes communicate wirelessly; ≥1 interaction happens node-to-node without the platform | `climate_alert` (A→B) and `irrigating` (B→A) events over the MQTT `events` topic — published in each node's `loop()`, handled in `handleEvent()`. Both nodes subscribe directly to each other via `firmware/common/include/transport.h`'s `EVENTS_IO`/`PEER_HB_IN` channels, independent of any dashboard. | Done in firmware; unverified end-to-end (needs Wokwi or hardware) |
| 4a | Platform visualizes state/readings of both nodes | `platform/dashboard/index.html` (planned: subscribes to `a/state`, `a/telemetry`, `b/state`, `b/telemetry`) | Not started |
| 4b | Platform sends a command to a specific node | Dashboard publishes to `a/cmd` or `b/cmd`; node-side handling already implemented (`handleCmd()` in each `main.cpp`) | Node side done; dashboard not started |
| 4c | Platform sends a command to both nodes at once | Dashboard publishes to `all/cmd`; both nodes already subscribe to `Channel::ALL_CMD_IN` and route it to the same `handleCmd()` | Node side done; dashboard not started |
| 5 | ≥2 remotely controllable variables per node | Node A: `temp_setpoint`, `sample_period_ms`, `mode`, `actuator_state` (4) — `NodeAConfig` in `firmware/node_a/src/control_logic.h`, applied in `handleCmd()`. Node B: `moisture_setpoint`, `irrigation_seconds`, `sample_period_ms`, `mode`, `actuator_state` (5) — `NodeBConfig` in `firmware/node_b/src/control_logic.h` | Done in firmware |
| 6 | Node confirms reception/execution; platform shows real reported state, not just the sent command | `publishAck()` echoes `cmd_id` + `result` + the actual value in effect (e.g. clamped `sample_period_ms`); `publishState()` is called right after, so the dashboard's next view of reality comes from the node, not from the command — both in each node's `main.cpp` | Node side done; dashboard pending/confirmed/rejected UI not started |
| 7 | Detect loss of communication, show it, define safe behavior | Heartbeat every 2 s (`HB_INTERVAL_MS`), 6 s timeout (`LINK_TIMEOUT_MS`) in each `main.cpp` `loop()`; MQTT LWT set in `MqttTransport::ensureMqttConnected()` (`firmware/common/src/transport_mqtt.cpp`); safe states (`docs/decisions.md` → "Safe state on link loss") enforced in `loop()` — Node B forcibly zeroes the pump timer when isolated | Node side done; dashboard offline indicator not started |
| 8 | External libs/frameworks allowed if explainable | ArduinoJson (JSON, see `protocol.h`), PubSubClient (MQTT, see `transport_mqtt.h`), mbedTLS's `mbedtls/md.h` (HMAC-SHA256, already bundled in the ESP32 Arduino core — see `hmac_auth.cpp`), Adafruit DHT sensor library (Node A only). Every choice justified inline and in `docs/decisions.md`. | Ongoing — MQTT.js for the dashboard still to add |

## Optional extras attempted

| Extra | Where | Status |
|---|---|---|
| Message validation (HMAC + replay check) | `firmware/common/include/hmac_auth.h`, `verifyEnvelope()` in `protocol.cpp`, called in every `handleCmd()`/`handleEvent()`. Replay rejection (`acceptSeq()`) applied to `cmd` messages in each node's `handleCmd()` — see `docs/protocol.md` "Sequence / replay handling" for why `hb`/`event` are deliberately not seq-gated (would deadlock on a peer reboot). | Done |
| Historical logging | `platform/logger/` (Python → SQLite/CSV) | Not started |
| Alarms for out-of-range | events published + dashboard highlight | Not started |
| Auto reconnect/retry with backoff | `MqttTransport::ensureMqttConnected()` retries on every `loop()` call | Basic reconnect done; no backoff (retries immediately each loop iteration) — acceptable for a prototype, worth noting as a known simplification |
| OTA firmware update | — | **Explicitly out of scope**, noted in README as a next step |
