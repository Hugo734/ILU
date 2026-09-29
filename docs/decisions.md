# Decisions log

Every entry: the decision, the alternative(s) rejected, and why. This is the file to study before
the interview — if asked "why did you do X and not Y", the answer is here.

---

## Scenario: distributed greenhouse

**Chosen:** Node A = climate (DHT22 + fan), Node B = soil (moisture + pump).

**Rejected:** two-tank/pump-transfer scenario — mechanically simpler to build but the inter-node
coupling (tank A level affects tank B) is less demonstrable with LEDs standing in for water, and
less relatable to explain live. Lighting-in-two-rooms — technically the easiest, but the two
directions of inter-node interaction (A→B, B→A) feel forced (why would room 1's light depend on
room 2's occupancy?). Greenhouse gives a physically real reason for both directions: heat dries
soil faster (A→B), and you don't want to blow wet soil dry right after irrigating (B→A).

## Transport: MQTT (phase 1), MQTT + ESP-NOW (phase 2)

**Chosen:** MQTT over Wi-Fi for everything now; add ESP-NOW for the inter-node path once hardware
exists, keep MQTT for the dashboard link.

**Rejected — MQTT only, permanently:** would work and is simpler to justify, but ties the
safety-relevant node-to-node behavior (fan-off-while-irrigating, alarm response) to the same
router and broker the dashboard depends on. A Wi-Fi outage should not be able to take down the
one interaction the challenge explicitly wants to see working *without* the platform.

**Rejected — ESP-NOW only, no broker:** would satisfy requirement 3 more purely, but then the
dashboard needs a gateway node bridging ESP-NOW to Wi-Fi/MQTT, which is extra firmware complexity
and a new single point of failure, for a prototype that doesn't need that complexity.

**Rejected — nRF24/LoRa:** LoRa's bitrate/latency is wrong for this update rate and short range;
nRF24 needs an extra radio module and SPI wiring neither ESP32 needs since it already has
onboard Wi-Fi/ESP-NOW capability. Adding a second radio chip is complexity with no matching
requirement.

**Why the split is defensible:** the challenge explicitly separates "nodes communicate wirelessly
with each other" (req. 3) from "platform observes/controls" (req. 4) as different requirements
with different failure tolerances. Two transports map cleanly onto that split. The cost is a
transport abstraction layer (`firmware/common/include/transport.h`) so the control logic never
calls an MQTT or ESP-NOW function directly — it calls `transport_send_event()` etc., and the
active implementation is chosen at build time. Swapping phase 1 → phase 2 should touch one file.

**Verification note:** Wokwi's ESP-NOW support between two *separate* simulated boards is
unverified as of writing — flagged in `sim/README.md`. If it isn't simulable, phase 2 is
developed and demoed only once real hardware arrives Wednesday.

## Message format: JSON

**Chosen:** JSON body for every MQTT message.

**Rejected — a packed binary struct:** smaller and faster to parse, but unreadable without a
custom decoder — a real cost when the evaluator wants to see a message on the wire. At this
message rate (heartbeats every 2 s, telemetry on the order of seconds) bandwidth is not a
constraint worth the opacity. Binary is exactly what phase-2 ESP-NOW might switch to later if
payload size becomes a real constraint over the air, but that's a separate decision to make with
real numbers, not now.

**Rejected — CSV/plain key=value:** no nesting, awkward for `data` payloads that differ by
message `type`, and no real parsing library maturity advantage over JSON on ESP32
(ArduinoJson handles both equally well).

## Dashboard: static HTML + vanilla JS + MQTT.js over WebSockets

**Chosen:** single `index.html`, no build step, no framework.

**Rejected — React/Vue + bundler:** more idiomatic for a "real" product, but adds a build
toolchain, node_modules, and abstraction (components, state management library) that has to be
explained in an evaluation about IoT, not frontend architecture. A static page keeps every line
attributable to "this does X because Y", which is the actual grading criterion (see the
challenge's "expected level" section).

**Rejected — a Python (Flask/FastAPI) backend serving the dashboard:** would add a server process
and a second place commands could get lost or mistranslated between the browser and MQTT. The
browser can talk to the broker directly via MQTT-over-WebSockets — no backend needed for this
scope. A backend earns its place if/when auth, multi-tenant access, or an HTTP API is needed;
none of that is in scope here.

## Message validation: shared-secret HMAC (truncated SHA-256) + sequence check

**Chosen:** every message body is signed with an HMAC using a shared secret (kept out of git via
`secrets.h`/`.env`, `.example` files committed instead); receivers reject bad signatures, stale
`seq` (replay), and out-of-order duplicates.

**Rejected — full TLS to the broker:** meaningful in production, but adds certificate handling
that a public test broker (HiveMQ) doesn't uniformly support for MQTT-over-WS from a browser
without extra setup, and ESP32 TLS handshakes are memory- and time-costly for a prototype that
just needs to show "I understand message authentication," which HMAC demonstrates just as well
architecturally. Noted as the natural next step for a production version.

**Rejected — no validation:** technically satisfies the mandatory requirements (validation is
optional/extra-credit), but a public broker means anyone can publish to a guessable topic;
skipping it would mean a stranger could trivially fake a command. Namespacing the topic prefix
mitigates most of this already, but HMAC is cheap insurance and directly hits an "optional extra"
line in the brief.

## Safe state on link loss

**Node A (fan):** safe state = fan OFF. Rationale: an unattended fan running indefinitely wastes
power and can over-ventilate/dry the greenhouse with nobody able to correct it; OFF is the
lower-risk failure for a ventilation actuator with no direct spoilage risk over the isolation
window.

**Node B (pump/valve):** safe state = pump OFF (never leave it running blind). Rationale: this is
the one that can actually cause damage — a stuck-open valve with no oversight floods the bed
long after the person who'd notice is gone. Erring toward under-watering during an isolation
window is trivially recoverable; erring toward flooding is not.

Both nodes keep their last valid config in RAM (and could persist to flash in a future revision —
not needed for a prototype-length isolation window) and resume normal control on reconnect,
reporting `was_isolated: true` in their next `state` message so the dashboard/operator knows a gap
happened even if they weren't watching live.

## Heartbeat timing: 2 s interval, 6 s timeout

**Chosen:** publish every 2 s, declare `LINK_LOST` after 6 s of silence (3 missed beats).

**Rejected — 1 missed beat = lost:** too sensitive to normal Wi-Fi/MQTT jitter on a shared public
broker; would flap the link status during the live demo, which is worse than a slightly slower
detection.

**Rejected — 30 s+ interval:** technically fine for greenhouse dynamics (nothing here changes in
seconds), but a multi-tens-of-seconds detection window looks weak in a live evaluation where the
evaluator will watch the clock after unplugging a node. 6 s is fast enough to *see* on a
projector, slow enough not to false-positive.
