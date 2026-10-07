# CLAUDE.md — ILU IoT dual-node challenge

Read this first. Last updated: Tuesday 6 October 2026, end of session.

Two ESP32 nodes (C++ on ESP-IDF v5.5.5, no Arduino) talking over ESP-NOW, plus a platform
(Mosquitto + Flask + SQLite dashboard) that is **not started yet**. This is a job-application
challenge: every line must be defensible live in a technical evaluation.

---

## Working rules (these govern every session)

- **Explain before acting.** Say what you are about to change and why, then do it.
- **One step at a time.** Propose, implement that step only, verify, stop and report.
- **Never commit.** Give the user the exact `git add` / `git commit` commands; they run them.
- **Never claim something works that has not been run.** Say explicitly what is untested.
  "It compiles" is not "it works". Hardware checks need the user's eyes — ask for them.
- No unrequested scope (security layers, abstractions, "nice to haves"). Surface them as questions.
- Comments say **why**, not what, in **English**.
- `shared/core` must contain **no ESP-IDF headers**. It is built on the host to prove it.
- Check every `esp_err_t` that matters. Startup failures: `ESP_ERROR_CHECK` (fail loudly).
  Runtime failures (e.g. a send): return/log the error, do not reboot the node.
- More than 3 files in one step needs a stated reason.

---

## Hardware

| | node_a ("room") | node_b ("access point") |
|---|---|---|
| MAC (eFuse, verified) | `78:42:1c:68:44:98` | `f4:65:0b:c0:e0:a4` |
| USB adapter | CP2102, serial `0001` | CP2102N |
| Usual port | `/dev/ttyUSB0` | `/dev/ttyUSB1` |
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

### Hardware lessons — do not relearn these

- Buzzer runs **straight off GPIO 14, no series resistor**. A 220 Ω resistor silenced it
  (drop exceeded what 3.3 V could spare; its oscillator never started). Current never measured —
  the multimeter's mA fuse is suspect.
- **Servo and RC522 are permanently out of scope.** The MB-102 bench supply burned while the
  servo stalled. Both nodes run off the ESP32 board's own rails; do not reintroduce the MB-102.
- PIR jumper on **H** (repeat trigger). Its output is 3.3 V even on 5 V supply → no divider
  (unlike the HC-SR04 echo, which needs one).
- PIR needs a warm-up (`WARMUP_MS = 60000` in node_a); it false-triggers before that.
- Open question: in the step-5 log the PIR fired every ~7–10 s, including during warm-up.
  Not yet confirmed whether the user was waving or it self-triggers. If it self-triggers,
  node_b will alert with nobody there — check before the demo.

---

## Build, flash, test

```bash
# Host build + unit tests for shared/core (no ESP-IDF)
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure

# Firmware (ESP-IDF env must be exported)
idf.py -C node_a build
idf.py -C node_a -p <node_a by-id path> flash
idf.py -C node_a -p /dev/ttyUSB0 monitor      # interactive: user runs it; Ctrl+] exits
```

Both binaries are ~741 KB now that the Wi-Fi stack is linked: **29% of the app partition free**.
Enough for MQTT, but no longer generous — watch it.

---

## Code map

| Path | Role |
|---|---|
| `shared/core/protocol.{h,cpp}` | 10-byte packed `Frame` (version, type, src, event, seq, uptime_ms), `MsgType`, `EventId`, `protocol_parse()` (stateless validation, `memcpy` for unaligned buffers) |
| `shared/core/hal.h` | `IHalA` / `IHalB` interfaces, `Led { Off, Red, Yellow, Green, Blue }` |
| `shared/net/net.{h,cpp}` | `net_init()` (Wi-Fi STA, never connects to a router), `espnow_init(peer)`, `espnow_send_frame()`, `espnow_receive()` (FreeRTOS queue), `espnow_rx_counters()` |
| `test/test_protocol.cpp` | 15 host tests, one rule broken per case |
| `node_a/main/` | PIR + buzzer + LED, access lease, sends `MotionStarted` |
| `node_b/main/` | HC-SR04 + access switch + LED, sends access state, receives alerts |

**Receive path:** the ESP-NOW callback runs in the Wi-Fi task → drops frames whose sender MAC
is not the peer or that fail `protocol_parse()` → pushes onto an 8-slot queue with timeout 0
(never blocks; full queue increments a counter) → the main loop drains it. All control logic
stays in one task. **The MQTT event callback should reuse this exact pattern.**

`PEER_MAC` lives in each node's `main.cpp` (each node's peer is the other one).

---

## Behaviour implemented (cross-node interaction, requirement #3 and beyond)

| node_b switch | node_b LED | node_a |
|---|---|---|
| **Off** = access closed (default) | red; yellow when something is 4–15 cm away | motion (after warm-up) → buzzer + red, sends `MotionStarted` on the rising edge |
| Off, after `MotionStarted` received | **red blinking until the switch is turned on** | — |
| **On** = access open | green; clears any alert | **blue, buzzer silent, sends nothing** |

**Fail-safe access lease:** node_b sends `AccessOpen`/`AccessClosed` on every switch change and
repeats it every 1 s (`STATE_REPEAT_MS`). node_a treats access as open only while an `AccessOpen`
from the last 3 s backs it (`ACCESS_LEASE_MS`). A dead node_b, lost frame or broken link all end
**armed**, never disarmed. Every message is idempotent, so no duplicate dropping is needed;
`seq` gaps are only logged.

node_a's alarm is `warm && motion && !access`; the frame is sent on its rising edge only.

---

## Status — verified vs not

### Verified on hardware (run and observed)
- node_b baseline after servo removal: LED sequence, distance, button (before the switch change).
- node_b MAC read from eFuse.
- Step 5: node_a sends exactly one `MotionStarted` per edge (seq 0,1,2), nothing during warm-up,
  no flooding during sustained motion.
- Final firmware boots on both boards; both radios come up with the right peer.
- node_b → node_a **link-layer ACKs**: ~9 access-state frames in 15 s, all ACKed except the first
  (sent before node_a's radio was up). This proves node_a's radio receives; **not** that node_a acts on it.
- Host: 15/15 protocol tests pass, and a deliberately broken rule makes them fail.

### NOT yet verified — do this first tomorrow
The user has not reported the results of these checks (two monitors, wait for the 60 s warm-up):
1. **Blue** shows in the boot LED test on both boards.
2. **Switch on** → node_b green, node_a logs `access OPEN: alarm disarmed`, goes blue; waving gives
   no buzzer and no `TX` line. **Switch off** → node_a `access CLOSED: alarm armed`.
3. **Switch off, wave** → node_a `TX MotionStarted seq N`; node_b `RX MotionStarted seq N: ALERT`,
   red blinking, continues after motion clears; **switch on** → `alert cleared by access switch`.
4. **Fail-safe:** switch on, node_a blue, unplug node_b → within ~3 s node_a logs
   `access CLOSED: alarm armed (lease expired, node_b silent)`.

Then **film check 3 with the router off** (~2 min of phone footage) as insurance for Thursday.

---

## Next steps, in order

1. Run the 4 hardware checks above; fix anything that fails.
2. **Rewrite `docs/requirements.md`.** ⚠️ It currently describes the **scrapped** firmware
   (PlatformIO, DHT22, HMAC, `firmware/` paths, MQTT for requirement #3). None of it exists.
   Record #3 as met in `node_a/main/main.cpp` (send on edge) and `node_b/main/main.cpp`
   (receive, alert, access state), once check 3 passes on hardware.
3. Review `docs/decisions.md`, `docs/protocol.md`, `docs/architecture.md`, `docs/demo-script.md` and
   README against the real code — likely also describe old designs. Add the decisions made today:
   frame format, queue pattern, access lease, switch instead of button, blue = access on node_a.
4. **MQTT on both nodes** (esp-mqtt to Mosquitto). Watch for the **ESP-NOW channel trap**: once a
   node joins the router it moves to the router's channel; ESP-NOW only works if both nodes share
   it. Most likely cause if the link "mysteriously breaks" after adding Wi-Fi.
5. **Dashboard — not started.** `platform/` contains only `templates/.gitkeep`. Planned per README:
   Python 3 · Flask · Flask-SocketIO · paho-mqtt, SQLite logger, server-rendered HTML with
   WebSocket push. Must show state reported by nodes (requirement #6), command one node and both
   at once (#4), ≥2 remote variables per node (#5), link-loss indicator (#7).
6. **Requirement #7** (link loss): the 1 s access-state repeat is already a heartbeat from B→A;
   A→B still needs one. README reserves blue for `DEGRADED`, but blue now means "access open" on
   node_a — pick a different indication or resolve the conflict.

### Small known issues
- When the peer is off, `net: TX got no link-layer ACK` is logged once per second (node_b's
  state repeat). Informative but noisy.
- node_b logs on change only now (it used to print every 200 ms); loop period is 100 ms.
- One commit message (`fcc978f`) starts with a leading space and reads as a fragment — cosmetic.
