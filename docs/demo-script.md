# Live demo script (~3 minutes)

Draft — will be refined once the dashboard exists and a rehearsal has actually been run. Nothing
here is claimed to have been executed yet.

## Setup (before the evaluators arrive)
1. Both nodes powered, connected to Wi-Fi, dashboard open, both showing green/online.
2. Broker reachable (local Mosquitto if on-site network, or the public broker if remote).

## Script

**0:00 – Overview (20s)**
"Two independent ESP32 nodes, a greenhouse split into climate and soil. They talk to each other
directly over MQTT today — ESP-NOW once hardware allows — and a dashboard lets me observe and
override both."

**0:20 – Requirement 4: platform control (40s)**
- Send a command to Node A only (e.g. change `temp_setpoint`) → point out the pending → confirmed
  transition in the UI, and that the displayed value came from Node A's own `ack`/`state`, not
  from the command just sent.
- Send a broadcast command to both nodes (`all/cmd`) → both update.

**1:00 – Requirement 3: inter-node interaction without the platform (60s)**
- Close/hide the dashboard, or point out it's not required for this step.
- Heat the DHT22 (hand, hair dryer, whatever's on hand) past `temp_setpoint` on Node A.
- Show Node A's fan responding, then show Node B's irrigation interval shortening
  (`climate_alert` received) — visible on Node B's LED/serial output even with the dashboard
  closed.
- Reverse direction: trigger Node B's irrigation (potentiometer/manual override), show Node A's
  fan pausing while `irrigating` is active.

**2:00 – Requirement 7: link loss and safe state (40s)**
- Power off (or disconnect Wi-Fi on) Node B mid-irrigation.
- Show the dashboard flipping Node B to offline within ~6 s.
- Show Node B's actuator going to its safe state (pump off) and status LED turning red, on the
  node itself, not just on the dashboard.
- Reconnect Node B, show it reports `was_isolated: true` and resumes normal operation.

**2:40 – Wrap (20s)**
"That's the full loop: sensing, cross-node reaction, remote control, confirmed state, and safe
failure — all in under three minutes." Point to `docs/decisions.md` for anything they want to dig
into.

## Fallback if hardware misbehaves live
Have the Wokwi simulation running in a second window as backup, with the same script rehearsed
there. State clearly which one is running if switching.
