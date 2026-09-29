# Architecture

## Scenario

Distributed greenhouse. Node A owns climate (temperature/humidity → fan), Node B owns soil
(moisture → irrigation). Chosen because it gives each node a sensor/actuator pair that is
naturally coupled to the *other* node's readings, which is what requirement 3 (inter-node
interaction without the platform) needs to be honest rather than contrived.

## Topology

```
        Wi-Fi                                   Wi-Fi
 Node A ───────┐                         ┌─────────── Node B
 (ESP32)       │                         │             (ESP32)
               ▼                         ▼
          ┌─────────────────────────────────────┐
          │            MQTT broker               │
          │  native: local Mosquitto (1883/9001) │
          │  Wokwi:  broker.hivemq.com (public)   │
          └──────────────┬────────────┬──────────┘
                          │            │
                 dashboard (WS)   logger (TCP)
```

Both nodes connect to the *same* broker. There is no gateway process and no custom server:
the broker is the only shared infrastructure, and it is doing exactly one job (pub/sub message
routing) that it is designed for. This is the "broker" topology explicitly allowed by the
challenge, as opposed to point-to-point or a custom gateway.

**Phase 2 change (post-hardware):** the node-to-node event/heartbeat path moves to ESP-NOW
(direct, no broker, no Wi-Fi router needed), while telemetry/commands to the dashboard keep using
MQTT. See `docs/decisions.md` → "Why two transports" for the justification, and
`firmware/common/include/transport.h` for the interface that makes the swap a one-file change.

## Data flow

**Telemetry (node → world):**
1. Node samples its sensor on `sample_period_ms` (a controllable variable).
2. Node runs its control state machine (compare reading to setpoint, decide actuator state,
   factor in any cross-node event currently in effect).
3. Node drives the actuator GPIO/PWM.
4. Node publishes `telemetry` (raw reading) and `state` (full reported state, retained) to MQTT.

**Command (dashboard → node):**
1. Dashboard publishes a `cmd` message to `a/cmd`, `b/cmd`, or `all/cmd`.
2. Node validates it (HMAC, sequence/replay check, range check on the value).
3. Node applies it to its own config, or rejects it with a reason.
4. Node publishes an `ack` referencing the command's `cmd_id`, with the *actual* resulting value.
5. Node's next `state` message reflects the new reality — this is what the dashboard renders,
   not the command it sent (requirement 6).

**Inter-node event (node → node, platform not involved):**
1. Node A's control loop crosses a threshold (temp > setpoint) → publishes `climate_alert` to
   `events`.
2. Node B is subscribed to `events` independently of the dashboard. It updates its own control
   state (shorter irrigation interval) and continues operating even if the dashboard is closed.
3. Symmetric case: Node B publishes `irrigating`, Node A pauses its fan.

This is demoed by physically triggering one node's sensor and observing the *other* node's
actuator, dashboard closed — see `docs/demo-script.md`.

**Link loss:**
1. Each node publishes `hb` every 2 s and also sets MQTT LWT on its `status` topic.
2. Each node tracks the other's last-seen heartbeat; >6 s silence → internal `LINK_LOST`.
3. Isolated node: keeps running on last-known-good config, drives actuator to its defined safe
   state, sets status LED red, logs the event, and reports "was isolated" once reconnected.
4. Dashboard independently watches both nodes' `hb`/`status` and shows an offline indicator —
   it does not rely on the nodes telling each other's story.

## Why this and not X

Short version — full versions with rejected alternatives are in `docs/decisions.md`:
- MQTT over a custom TCP protocol: broker gives pub/sub, LWT, and retained state for free.
- JSON over a binary format: inspectable live during evaluation, worth the extra bytes at this
  message rate.
- Static HTML dashboard over a framework: nothing to install, every line explainable.
- ESP-NOW for phase 2 inter-node link, not phase 1: Wokwi cannot simulate it between separate
  projects (unverified — flagged in `sim/README.md`), so it can't be developed before hardware.
