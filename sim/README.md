# Running without hardware

Two independent ways to exercise the firmware before Wednesday: Wokwi (simulates the ESP32 +
peripherals) and a native build against a local broker (compiles the control logic for real, but
without simulating GPIO/sensors at all — see `firmware/*/test/` for that side instead).

## Status: what has and hasn't been verified

- **Native unit tests for the pure control logic** (`firmware/node_a/test/`,
  `firmware/node_b/test/`) — verified, actually compiled and run with `g++` in this environment.
  All pass.
- **Full PlatformIO build of the firmware** — attempted; see the top-level README's progress
  checklist and this session's notes for the actual result, since it depends on downloading the
  ESP32 toolchain, which may or may not have finished by the time you read this.
- **Wokwi simulation (`diagram.json` files in this folder)** — written to Wokwi's documented
  schema, but **not opened in Wokwi yet**. You need to load them in the Wokwi web editor or VS
  Code extension and confirm they simulate correctly before trusting them.
- **ESP-NOW in Wokwi between two separate projects** — **unverified, and I could not confirm
  either way from this environment.** Wokwi's own docs describe ESP-NOW support for a *single*
  simulation containing multiple ESP32 parts on one canvas, not necessarily two independently
  running `wokwi-cli`/browser projects for Node A and Node B. Since this repo intentionally keeps
  Node A and Node B as two separate PlatformIO projects (mirroring two separate physical boards),
  check this yourself before relying on it: try opening both diagrams as tabs in one Wokwi
  project, or search Wokwi's current docs for "ESP-NOW multi-instance". If it doesn't work,
  that's fine — ESP-NOW is phase 2, exercised once real hardware exists; phase 1 (MQTT) already
  covers requirement 3 in simulation.

## Option 1: Wokwi (recommended for now — needs no broker of your own)

1. Build each node: `cd firmware/node_a && pio run`, `cd firmware/node_b && pio run`.
2. In VS Code with the Wokwi extension installed, open `sim/wokwi/node_a/diagram.json` — it
   should pick up the build via `wokwi.toml` pointing at `.pio/build/esp32dev/firmware.bin`.
   Repeat for `sim/wokwi/node_b/diagram.json` in a second window/tab.
3. Copy `firmware/node_a/include/secrets.h.example` to `secrets.h` (same for node_b) and fill in
   a Wi-Fi SSID/password — Wokwi's simulated ESP32 needs *some* Wi-Fi network to associate with;
   its default virtual network (`Wokwi-GUEST`, no password) works and has real internet access,
   which is what lets it reach a public MQTT broker like `broker.hivemq.com`. Set
   `WIFI_SSID="Wokwi-GUEST"` and leave `WIFI_PASSWORD=""` for simulation.
4. Start both simulations. Both nodes should connect to `broker.hivemq.com` under your
   `TOPIC_PREFIX` and start exchanging heartbeats/telemetry — watch each node's Serial Monitor.
5. To see the cross-node interaction (req. 3), warm up the simulated DHT22 (Wokwi lets you drag
   its temperature slider) past `temp_setpoint` and watch Node B's serial output react to the
   `climate_alert` event, and vice versa by moving Node B's potentiometer below the moisture
   threshold and watching Node A's fan pause.

## Option 2: native build against a local Mosquitto broker

Useful for testing the dashboard/logger against real MQTT traffic without any ESP32 at all —
run the firmware's *logic* natively isn't possible directly (it depends on Arduino/ESP32 headers),
but a local broker plus `mosquitto_pub`/`mosquitto_sub` lets you hand-craft messages against
`docs/protocol.md`'s schema and watch the dashboard/logger (once built) react.

```
docker run -it -p 1883:1883 -p 9001:9001 eclipse-mosquitto
```
Needs a `mosquitto.conf` enabling the websocket listener on 9001 for the browser dashboard to
connect — not yet written; flagged as a TODO for the dashboard session.
