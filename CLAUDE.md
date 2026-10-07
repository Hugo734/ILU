# CLAUDE.md — ILU IoT dual-node challenge

Read this first. Last updated: Tuesday 6 October 2026, ~23:40, end of session.
Everything below is committed (last commit `6f5ce37 Dashboard finished`).

Two ESP32 nodes (C++ on ESP-IDF v5.5.5, no Arduino) talk directly over ESP-NOW, and both publish
their state over Wi-Fi/MQTT to a Mosquitto broker on the laptop. A Flask + Socket.IO dashboard
shows it live. This is a job-application challenge: every line must be defensible live in a
technical evaluation. **Demo is Thursday.**

---

## Working rules (these govern every session)

- **Explain before acting.** Say what you are about to change and why, then do it.
- **One step at a time.** Propose, implement that step only, verify, stop and report.
- **Never commit.** Give the user the exact `git add` / `git commit` commands; they run them.
- **Never claim something works that has not been run.** Say explicitly what is untested.
  "It compiles" is not "it works". Hardware and browser checks need the user's eyes — ask.
- No unrequested scope (security layers, abstractions, "nice to haves"). Surface them as questions.
- Comments say **why**, not what, in **English**.
- `shared/core` must contain **no ESP-IDF headers**. It is built on the host to prove it.
- Check every `esp_err_t` that matters. Startup failures: `ESP_ERROR_CHECK` (fail loudly).
  Runtime failures (a send, a publish): log the error, do not reboot the node.
- More than 3 files in one step needs a stated reason.

---

## Hardware

| | node_a ("room") | node_b ("access point") |
|---|---|---|
| MAC (eFuse, verified) | `78:42:1c:68:44:98` | `f4:65:0b:c0:e0:a4` |
| USB adapter | CP2102, serial `0001` | CP2102N |
| Usual port | `/dev/ttyUSB0` | `/dev/ttyUSB1` |
| IP on the LAN (DHCP, seen tonight) | `192.168.1.190` | `192.168.1.189` |
| Sensor | PIR HC-SR501 on GPIO 32 | HC-SR04: TRIG 5, ECHO 18 (via 1 kΩ/2 kΩ divider) |
| Input | — | **3-pin access switch** on GPIO 4: middle → GPIO 4, one outer → GND, other unused |
| Actuator | active buzzer on GPIO 14 | — |
| RGB LED | R 25 / G 26 / B 27 | R 25 / G 26 / B 27 |

`ttyUSBn` numbers follow plug-in order and can swap. Flash by adapter, not by number:

```
node_a: /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
node_b: /dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_7a041be62e9cef118535af9061ce3355-if00-port0
```

When in doubt: `esptool.py -p <port> read_mac` — the MAC is burned in, it cannot lie.

### Network

- Wi-Fi: `CLARO_2.4GHz_768DE4`, 2.4 GHz, **channel 11**. Credentials live only in
  `shared/net/secrets.h` (git-ignored; template in `secrets.h.example`).
- Laptop: `192.168.1.110` (Ethernet, used as broker address) and `192.168.1.187` (Wi-Fi).
  **DHCP can change `.110` after a router reboot** — reserve it in the router before Thursday,
  or update `MQTT_BROKER_URI` in `secrets.h` and reflash both nodes.
- Mosquitto 2.0.11 runs as a system service. `platform/mosquitto/ilu.conf` (installed to
  `/etc/mosquitto/conf.d/`) makes it listen on `0.0.0.0:1883` with anonymous access —
  a deliberate, documented prototype shortcut (no auth, no TLS).

### Hardware lessons — do not relearn these

- Buzzer runs **straight off GPIO 14, no series resistor**. A 220 Ω resistor silenced it.
  Current never measured — the multimeter's mA fuse is suspect.
- **Servo and RC522 are permanently out of scope.** The MB-102 bench supply burned while the
  servo stalled. Both nodes run off the ESP32 board's own rails; do not reintroduce the MB-102.
- PIR jumper on **H** (repeat trigger). Output is 3.3 V even on 5 V supply → no divider.
- PIR needs a warm-up (`WARMUP_MS = 60000` in node_a); it false-triggers before that.
- Open question: the PIR fired every ~7–10 s in two logs, including during warm-up. Not yet
  confirmed whether someone was moving or it self-triggers. If it self-triggers, node_b will
  alert with nobody there — check before the demo.

---

## Build, flash, run

```bash
# Host build + unit tests for shared/core (no ESP-IDF)
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure

# Firmware (ESP-IDF env exported)
idf.py -C node_a build
idf.py -C node_a -p <node_a by-id path> flash
idf.py -C node_a -p /dev/ttyUSB0 monitor          # interactive: user runs it; Ctrl+] exits

# Watch raw MQTT traffic
mosquitto_sub -h 192.168.1.110 -t 'ilu/#' -v

# Dashboard (from the repo root). env -u PYTHONPATH keeps ROS Humble's packages,
# which the user's shell profile adds to PYTHONPATH, out of the venv.
env -u PYTHONPATH platform/.venv/bin/python platform/app.py
# → open http://localhost:5000  (or http://192.168.1.110:5000 from another device on the LAN)
```

`platform/.venv` is git-ignored. Recreate it with:
`/usr/bin/python3 -m venv platform/.venv && platform/.venv/bin/pip install -r platform/requirements.txt`

**Flash budget:** both binaries are ~875 KB in a 1 MB app partition — **14% free**. Enough for D3,
but tight. If it runs out, switch to the "Single factory app (large)" partition table in
`menuconfig` (the module has 4 MB of flash). Not done yet.

---

## Code map

| Path | Role |
|---|---|
| `shared/core/protocol.{h,cpp}` | 10-byte packed `Frame` (version, type, src, event, seq, uptime_ms); `MsgType`; `EventId` = MotionStarted 1, MotionStopped 2, AccessOpen 3, AccessClosed 4; `protocol_parse()` (stateless, `memcpy` for unaligned buffers) |
| `shared/core/hal.h` | `IHalA` / `IHalB`; `Led { Off, Red, Yellow, Green, Blue }`; `accessSwitchOn()` (level) |
| `shared/net/net.{h,cpp}` | `net_init(node_id)`: Wi-Fi STA join + auto-retry, starts esp-mqtt on GOT_IP; `mqtt_publish_state(json)` (QoS 0, skipped while disconnected); `espnow_init(peer)`, `espnow_send_frame()`, `espnow_receive()` (queue), `espnow_rx_counters()` |
| `shared/net/secrets.h` | git-ignored credentials + `MQTT_BROKER_URI` |
| `test/test_protocol.cpp` | 15 host tests, one rule broken per case |
| `node_a/main/` | PIR + buzzer + LED, access lease, sends `MotionStarted`, publishes `ilu/a/state` |
| `node_b/main/` | HC-SR04 + switch + LED, sends access state, receives alerts, publishes `ilu/b/state` |
| `platform/app.py` | MQTT subscriber → state store → Socket.IO push; timeline + offline watchdog |
| `platform/templates/index.html` | The dashboard page |
| `platform/static/socket.io.min.js` | Vendored Socket.IO 4.7.5 client (works without internet) |
| `platform/mosquitto/ilu.conf` | Broker config |

**ESP-NOW receive path:** callback (Wi-Fi task) drops frames from a non-peer MAC or that fail
`protocol_parse()` → pushes onto an 8-slot queue with timeout 0 (never blocks; full → counter) →
main loop drains it. All control logic stays in one task. **Reuse this pattern for incoming MQTT
commands in D3** (the esp-mqtt event handler runs in its own task).

`PEER_MAC` lives in each node's `main.cpp` (each node's peer is the other one).

---

## Behaviour

| node_b switch | node_b LED | node_a |
|---|---|---|
| **Off** = access closed (default) | red; yellow when something is 4–15 cm away | motion (after warm-up) → buzzer + red, sends `MotionStarted` on the rising edge |
| Off, after `MotionStarted` received | **red blinking until the switch is turned on** | — |
| **On** = access open | green; clears any alert | **blue, buzzer silent, sends nothing** |

**Fail-safe access lease:** node_b sends `AccessOpen`/`AccessClosed` on every switch change and
repeats it every 1 s. node_a counts access as open only while an `AccessOpen` from the last 3 s
backs it. A dead node_b, lost frame or broken link all end **armed**, never disarmed. Every
message is idempotent, so no duplicate dropping; `seq` gaps are only logged.

### MQTT state messages (each node: every 1 s, and at once when anything but uptime/distance changes)

```
ilu/a/state {"node":"a","up":26480,"warm":true,"motion":false,"alarm":false,"buzzer":0,"access":false,"led":"green"}
ilu/b/state {"node":"b","up":26700,"cm":40,"near":false,"access":false,"alert":false,"led":"red"}
```

`buzzer` is read back from the pin (`alarmActive()`), not taken from `alarm`. `led` values:
`off|red|yellow|green|blue|red_blink`. node_b's `cm` is excluded from change detection (it jitters).

### Dashboard (D2)

Header with broker status; topology diagram where a dot pulses along a node's MQTT wire on each
report, and the ESP-NOW link is drawn but labelled *not seen by the platform* (requirement #3);
one card per node (mirrored LED incl. blink, online pill, "last report x s ago", every field,
node_b distance bar with the 4–15 cm zone and a 60 s sparkline, collapsible raw JSON); event
timeline with filters All/Alarm/Access/Link/Info, server time + node uptime per event.

Server-side rules: the UI shows only what nodes reported; timeline events are differences between
two consecutive reports; `near` is deliberately not a timeline event (flickers); offline = 3 s of
silence (checked every 0.5 s); `up` going backwards = "Node rebooted".

---

## Status — verified vs not

### Verified (run and observed)
- node_b baseline after servo removal; both MACs from eFuse.
- node_a sends exactly one `MotionStarted` per edge, nothing during warm-up, no flooding.
- Both boards boot the current firmware; ESP-NOW peers correct.
- **Wi-Fi + MQTT (D1):** both join on channel 11 (~12–15 s, with 2–3 auth/assoc retries each),
  MQTT connects ~0.4 s after IP, broker received ~1 msg/s per node in the expected format.
- **ESP-NOW survives Wi-Fi with the router ON:** switch-off on node_b reached node_a in ~100 ms
  with both on channel 11; later the server snapshot showed node_a `access:true, led:blue`
  while node_b reported `access:true` — the B→A access link works with both on Wi-Fi.
- **Dashboard server (D2):** page and JS return 200; a hand-driven Socket.IO session received a
  snapshot with broker connected, both nodes online, 11 msgs each, real state, first events.
- Host: 15/15 protocol tests pass; a deliberately broken rule makes them fail.

### NOT yet verified — do these first
1. **The dashboard in a real browser.** Only the data path was checked; the rendering (cards,
   LED glow/blink, pulses, sparkline, timeline filters) has not been seen by anyone. The user
   committed it as "Dashboard finished" but never reported opening it.
2. **Motion alert (A→B) with Wi-Fi on:** switch off, wave → node_b blinks red, dashboard shows
   `ALERT` on node_b and in the timeline.
3. **Blue LED** at boot on both boards (6a), and node_a blue/silent while the switch is on.
4. **Lease fail-safe:** switch on, unplug node_b → node_a `access CLOSED ... (lease expired)`
   within ~3 s, and the dashboard marks node_b offline.
5. **ROUTER OFF test — the biggest risk.** With the router off, each node's Wi-Fi retry loop
   scans channels, which can pull the radio off channel 11 and drop ESP-NOW frames. During the
   connect phase tonight node_b logged 6 un-ACKed frames and one access change took 1.1 s instead
   of 100 ms. Test: router off, wave 3–4 times (count alerts on node_b), flip switch on (does
   node_a go blue and **stay** blue, or flicker?). If it fails, the fix is in `on_wifi_event`
   (`shared/net/net.cpp`): back off between `esp_wifi_connect()` retries so the radio sits on
   channel 11 most of the time. Possibly also `esp_wifi_set_ps(WIFI_PS_NONE)` — modem sleep is
   known to hurt ESP-NOW reception. Neither is implemented; only add if the test shows a need.
6. Film the demo with the router off once 5 passes.

---

## Next steps, in order

1. The checks above (browser first — it is quick).
2. **D3 — commands + acknowledgements** (requirements #4, #5, #6). Needs the user's choice of
   ≥2 remotely controllable variables per node. Proposed, not yet confirmed:
   node_a: buzzer enabled on/off, PIR warm-up time; node_b: near threshold (`CERCA_MAX`),
   access-state repeat interval. Design sketch: topics `ilu/a/cmd`, `ilu/b/cmd`, `ilu/all/cmd`;
   each command carries a `cmd_id`; node replies on `ilu/<node>/ack` with result
   (APPLIED / REJECTED + reason) and the value **actually in effect**; dashboard shows
   pending → confirmed / rejected / timed out (2 s). Incoming commands go through a queue to the
   main loop, same pattern as ESP-NOW. Watch the 14% flash margin.
3. **D4 — link loss + history.** MQTT Last Will on `ilu/<node>/status` (requirement #7), SQLite
   logger (`platform/history.db`, schema in README: telemetry, events, commands, acks,
   link_status) with node and server timestamps. A→B ESP-NOW heartbeat is still missing for #7
   (B→A already has the 1 s access-state repeat). README reserves blue for `DEGRADED`, but blue now
   means "access open" on node_a — resolve that conflict.
4. **Docs.** ⚠️ `docs/requirements.md` still describes the **scrapped** firmware (PlatformIO,
   DHT22, HMAC, `firmware/` paths, MQTT for #3). Rewrite it against the real code. Also review
   `docs/decisions.md`, `protocol.md`, `architecture.md`, `demo-script.md` and README. Decisions to
   record: frame format, queue pattern, access lease, switch instead of button, blue = access on
   node_a, MQTT state-on-change + 1 s, dashboard derives events from state diffs, anonymous broker.
   Requirement #3 lives in `node_a/main/main.cpp` (send on edge) and `node_b/main/main.cpp`
   (receive + alert + access state).

### Small known issues
- When the peer is off, `net: TX got no link-layer ACK` is logged once per second.
- node_b logs on change only (it used to print every 200 ms); loop period is 100 ms.
- The dashboard server uses Werkzeug's dev server (`allow_unsafe_werkzeug=True`) — fine for a
  one-laptop demo, worth saying so if asked.
- Commit `fcc978f`'s message starts with a space and reads as a fragment — cosmetic.
