# Message protocol

This is the contract between Node A, Node B, and the dashboard. It's written before any firmware
code so all three sides implement against the same schema instead of drifting and getting patched
later. Reference implementation: `firmware/common/include/protocol.h` (structs + serialize/parse)
and `firmware/common/include/hmac_auth.h` (signing).

## Transport and topics

MQTT, QoS 1 (at-least-once — acceptable to receive a duplicate telemetry sample; not acceptable to
silently drop a command). Every topic is namespaced under a project prefix so a shared public
broker (`broker.hivemq.com`, used from Wokwi) doesn't mix this traffic with anyone else's:

```
gh/<prefix>/a/telemetry   Node A -> everyone   : raw sensor readings
gh/<prefix>/a/state       Node A -> everyone   : full reported state           (retained)
gh/<prefix>/a/cmd         dashboard -> Node A  : command
gh/<prefix>/a/ack         Node A -> dashboard  : command acknowledgement
gh/<prefix>/a/hb          Node A -> everyone   : heartbeat
gh/<prefix>/a/status      Node A -> everyone   : "online"/"offline" via MQTT LWT (retained)
gh/<prefix>/b/...         same set for Node B
gh/<prefix>/all/cmd       dashboard -> both nodes : broadcast command
gh/<prefix>/events        node -> node (and dashboard, as an observer) : climate_alert, irrigating
```

`<prefix>` is a short random-ish string (e.g. `hdc-gh-7f3a`) set once in `secrets.h` /
`.env` and shared by both nodes, the dashboard, and the logger. It is not a secret — it is
collision-avoidance on a broker other people also use, not access control (the HMAC below is what
prevents forged messages).

`state` and `status` are published with the MQTT **retained** flag: a dashboard that connects
after a node has been running for an hour still immediately sees its last known state, instead of
showing nothing until the next telemetry tick.

## Envelope

Every message, regardless of `type`, is one JSON object:

```json
{
  "v": 1,
  "src": "A",
  "seq": 1423,
  "ts": 88213,
  "type": "telemetry",
  "cmd_id": "",
  "data": { },
  "sig": "3f9a1c7be2d40a11"
}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version. Bump on any breaking schema change; receivers reject unknown major versions. |
| `src` | string | Origin: `"A"`, `"B"`, or `"PLATFORM"`. |
| `seq` | uint32 | Monotonic counter, per sender, incremented on every message that sender publishes. Used for loss detection (gaps) and replay/duplicate rejection (a `seq` at or below the last accepted one from that `src` is dropped). |
| `ts` | uint32 | Seconds since that device's boot (`millis()/1000` on the ESP32 — no NTP dependency for a prototype; the dashboard/logger timestamp on arrival instead of trusting node clocks for wall-clock time). |
| `type` | string | One of `telemetry`, `state`, `cmd`, `ack`, `hb`, `event`. |
| `cmd_id` | string | Present (non-empty) only on `cmd` and its matching `ack`. Client-generated, e.g. `"A-17"` (sender prefix + local counter), unique enough to match a command to its acknowledgement. |
| `data` | object | Payload, shape depends on `type` — see below. |
| `sig` | string | HMAC-SHA256 over the fields below, hex-encoded, **truncated to the first 16 hex chars (8 bytes)**. See "Message validation". |

### Why a truncated HMAC and not a full one

Full HMAC-SHA256 is 32 bytes / 64 hex chars. Truncating to 8 bytes keeps the signature short
relative to the small JSON payloads MQTT is carrying here, while still giving a 2^-64 forgery
chance per attempt — not a formal security guarantee (this is HMAC for message authentication and
integrity, not confidentiality: payloads are plaintext), but this challenge's own "optional
extras" list asks for "authentication, encryption, **or** message validation", and this is the
validation option. Full-strength encryption (TLS to the broker) is called out as the next step in
`docs/decisions.md` rather than attempted here, since a public test broker plus a from-scratch
ESP32 TLS handshake adds real complexity for a prototype-scale demo.

## Message validation (`sig`) — exact algorithm

Implemented once in `firmware/common/include/hmac_auth.h` and mirrored in the Python logger and
the JS dashboard so all three sides compute the same thing.

1. Build the **string to sign** by concatenating fields with `|` as separator, in this fixed
   order (never re-derived from JSON key order, to avoid any ambiguity across languages):

   ```
   to_sign = v + "|" + src + "|" + seq + "|" + ts + "|" + type + "|" + cmd_id + "|" + data_json
   ```

   where `data_json` is `data` serialized as **compact JSON with keys sorted alphabetically**
   (e.g. Python: `json.dumps(data, sort_keys=True, separators=(",", ":"))`). Sorting keys is what
   makes this reproducible without needing a canonical-JSON library on the ESP32. The firmware
   sorts `data`'s top-level keys in code before hashing (`buildSigningString()` in
   `firmware/common/src/protocol.cpp`) rather than relying on call sites always inserting keys
   alphabetically — a discipline that's one typo away from silently breaking every signature.

2. `sig = hex(HMAC-SHA256(key=shared_secret, message=to_sign))[:16]`.

3. Receiver recomputes `sig` the same way over the received fields and does a constant-time
   compare against the received `sig`. Mismatch → message dropped, not processed, not acked.

**Worked example** (secret = `"demo-secret"`, used only to prove the algorithm — the real shared
secret lives in `secrets.h`/`.env`, never in git):

```
data        = {"temp": 24.5}
data_json   = {"temp":24.5}
to_sign     = 1|A|42|1000|telemetry||{"temp":24.5}
sig (HMAC-SHA256, truncated 16 hex) = 6293f03ef6483e59
```

Verified: computed with Python's `hmac`/`hashlib` (`python3 -c "import hmac,hashlib,json; ..."`),
not hand-derived. `firmware/common/include/hmac_auth.h` and the logger/dashboard implementations
must reproduce this exact value for this input — that's the cross-language test to run once they
exist.

## `data` payload per type

### `telemetry` — raw sensor reading, sender's own values only
```json
{ "v": 1, "src": "A", "seq": 8, "ts": 4021, "type": "telemetry", "cmd_id": "",
  "data": { "humidity": 61.2, "temp_c": 26.8 }, "sig": "..." }
```
Node B's equivalent uses `"moisture_pct"` instead of `temp_c`/`humidity`.

### `state` — full reported configuration + derived status, retained
```json
{ "v": 1, "src": "A", "seq": 9, "ts": 4023, "type": "state", "cmd_id": "",
  "data": {
    "temp_setpoint": 28.0,
    "sample_period_ms": 5000,
    "mode": "AUTO",
    "actuator_state": "OFF",
    "fan_paused_by_b": false,
    "link_to_b": "OK",
    "was_isolated": false
  }, "sig": "..." }
```
This — not the last command sent — is what the dashboard renders. Node B's `state.data` carries
`moisture_setpoint`, `irrigation_seconds`, `sample_period_ms`, `mode`, `actuator_state`,
`shortened_by_a_alert`, `link_to_a`, `was_isolated`.

### `cmd` — dashboard → node (or both, on `all/cmd`)
```json
{ "v": 1, "src": "PLATFORM", "seq": 55, "ts": 1727500000, "type": "cmd", "cmd_id": "P-101",
  "data": { "set": "temp_setpoint", "value": 27.5 }, "sig": "..." }
```
One variable per command message (simpler to ack unambiguously than a batch). `set` must be one
of the node's controllable-variable names (`docs/requirements.md` #5); anything else is rejected.

### `ack` — node → dashboard, always in response to a `cmd_id`
```json
{ "v": 1, "src": "A", "seq": 10, "ts": 4025, "type": "ack", "cmd_id": "P-101",
  "data": { "result": "applied", "reason": "", "value": 27.5 }, "sig": "..." }
```
`result` is one of `applied` / `rejected` / `error`. `value` is **the value actually in effect
after applying it** — e.g. if a requested `sample_period_ms` was clamped to a firmware-enforced
minimum, `value` reports the clamped number, not the requested one. `reason` is populated only
when `result != "applied"` (e.g. `"unknown variable"`, `"out of range"`, `"bad signature"`).

### `hb` — heartbeat, no meaningful `data`
```json
{ "v": 1, "src": "B", "seq": 201, "ts": 6000, "type": "hb", "cmd_id": "", "data": {}, "sig": "..." }
```

### `event` — inter-node signal, also observable by the dashboard
```json
{ "v": 1, "src": "A", "seq": 11, "ts": 4030, "type": "event", "cmd_id": "",
  "data": { "name": "climate_alert", "temp_c": 29.1 }, "sig": "..." }
```
```json
{ "v": 1, "src": "B", "seq": 205, "ts": 6010, "type": "event", "cmd_id": "",
  "data": { "name": "irrigating", "seconds_remaining": 12 }, "sig": "..." }
```
Both nodes subscribe to `events` directly — this is what makes requirement 3 (node-to-node
interaction without the platform) true even with the dashboard closed.

**Republishing, not edge-triggering:** both `climate_alert` and `irrigating` are republished on
every control cycle for as long as the condition holds (not sent once on the rising edge). The
receiving node treats the condition as active until ~10 s pass without a fresh event ("stale"),
then clears it — this is simpler than defining a second "condition cleared" event type, at the
cost of a small tail where the effect lingers slightly after the cause stops. Known limitation:
this assumes the sender's `sample_period_ms` stays well under the 10 s staleness window; pushing
it much higher via a command would make the receiver's flag clear too early. Acceptable for a
prototype; a production version would have the event payload carry the sender's own period so the
receiver could size the staleness window dynamically instead of using a fixed constant.

## Sequence / replay handling

Strict `seq > last_seq[src]` enforcement (after signature verification passes) is applied only to
`cmd` messages, where replaying a stale command would actually cause harm — e.g. a captured
"set temp_setpoint=20" replayed after the operator changed it to 25 would silently revert a
deliberate change. `hb` and `event` are deliberately *not* seq-gated: both are idempotent,
level-style signals already designed to be republished every control cycle while their condition
holds (see "Republishing, not edge-triggering" above), so replaying an old one just extends a
liveness/condition signal slightly — a non-issue functionally, and gating them would introduce a
reboot deadlock (a receiver that only trusts a fresh, higher `seq` can't distinguish "peer
rebooted, counter restarted" from "replay attack" without a separate reset signal, and the
heartbeat is exactly the message that would need to arrive to trigger that reset in the first
place). Implemented in `firmware/node_a/src/main.cpp` / `firmware/node_b/src/main.cpp`'s
`handleCmd()` via a per-node `lastSeqPlatform` counter. Known limitation: a dashboard session that
restarts (its own `seq` resetting to 0) would have its first few commands rejected until its
counter passes the node's high-water mark — acceptable for a prototype's command volume, and
avoids the same reboot-deadlock problem the heartbeat case has.
