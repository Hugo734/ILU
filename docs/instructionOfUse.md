# Instructions of use — ILU (everything you run in a terminal)

Quick guide for demo day. Every command runs **from the repo root**:

```bash
cd ~/Documents/ILU
```

Normal order to bring everything up:

1. Check the laptop's IP against the one compiled into the nodes (section 1)
2. Check that Mosquitto is running (section 2)
3. Power the nodes and watch their messages arrive (section 3)
4. Start the dashboard (section 4)

---

## 1. The broker IP (check this first)

There is **no fixed IP**. The rule is:

> **The laptop's current IP must be the one in `MQTT_BROKER_URI` (`shared/net/secrets.h`)
> at the moment the nodes were compiled.**

The nodes have the broker address **baked into the firmware**. If the laptop's IP changes
(DHCP after a router reboot, switching from Ethernet to Wi-Fi, a different computer), the nodes
still join Wi-Fi but **never reach the broker**, and the dashboard shows them offline.
The dashboard itself is not affected: it talks to the broker on `localhost`.

### Compare the two IPs

```bash
ip -4 -br addr                              # the laptop's current IPs
grep MQTT_BROKER_URI shared/net/secrets.h   # the IP the nodes were built with
```

Look for the interface that is `UP` on `192.168.1.x` (the `CLARO_2.4GHz_768DE4` router). It can
be Ethernet (`enp…` / `eth…`) or Wi-Fi (`wlo1`); either works as long as it matches `secrets.h`.

> ⚠️ If `wlo1` shows something like `172.20.10.x`, the laptop is on the phone's hotspot, not on
> the router. The nodes only know the router → connect the laptop to the router first.

The rest of this guide uses `$BROKER` for that IP. Set it once per terminal from `secrets.h`:

```bash
BROKER=$(grep -oP 'mqtt://\K[0-9.]+' shared/net/secrets.h); echo $BROKER
```

### If they do not match

**Option A (best):** reserve the IP that is in `secrets.h` for the laptop in the router
(DHCP reservation) and reconnect. No firmware change.

**Option B:** put the laptop's current IP in the firmware:

```bash
nano shared/net/secrets.h
#   #define MQTT_BROKER_URI "mqtt://<NEW-IP>:1883"
```

Then **rebuild and flash both nodes** (section 5). Both use the same `secrets.h`.

### On a different computer

`shared/net/secrets.h` is git-ignored (it holds the Wi-Fi password), so a fresh clone does not
have it. Create it from the template and fill in the Wi-Fi credentials and **that computer's** IP:

```bash
cp shared/net/secrets.h.example shared/net/secrets.h
nano shared/net/secrets.h
```

Then rebuild and flash both nodes (section 5).

### Check that the broker is reachable at that IP

```bash
mosquitto_sub -h $BROKER -t 'ilu/#' -v
```

If it says `Connection refused`, see section 2.

---

## 2. MQTT broker (Mosquitto)

It runs as a system service and normally starts with the laptop.

```bash
systemctl status mosquitto          # is it active?
sudo systemctl restart mosquitto    # restart it
sudo systemctl start mosquitto      # if it was stopped
```

It must listen on `0.0.0.0:1883` (the whole network), not only on localhost:

```bash
ss -ltn | grep 1883
#   0.0.0.0:1883   → good
#   127.0.0.1:1883 → the repo config is missing
```

If the config is missing (for example after reinstalling Mosquitto):

```bash
sudo cp platform/mosquitto/ilu.conf /etc/mosquitto/conf.d/
sudo systemctl restart mosquitto
```

If the firewall is active (`sudo ufw status`), open the ports:

```bash
sudo ufw allow 1883/tcp    # MQTT from the nodes
sudo ufw allow 5000/tcp    # dashboard from another device
```

---

## 3. Watch the nodes' traffic

With the nodes powered (they take ~12–15 s to join Wi-Fi):

```bash
mosquitto_sub -h $BROKER -t 'ilu/#' -v
```

You should see ~1 message per second from each node:

```
ilu/a/state {"node":"a","up":...,"warm":true,"motion":false,...,"peer":true,...,"buzzer_enabled":1,"warmup_s":60,"publish_ms":1000}
ilu/b/state {"node":"b","up":...,"cm":40,"near":false,...,"peer":true,...,"near_cm":15,"repeat_ms":1000,"publish_ms":1000}
ilu/a/status online
ilu/b/status online
```

`ilu/<node>/status` is retained: the node publishes `online` when it connects, and the broker
publishes `offline` (the node's MQTT Last Will) about 7.5–10 s after the node stops answering.

If nothing shows up: IP (section 1), broker (section 2), or the serial monitor (section 6).

`Ctrl+C` to quit.

---

## 4. Start the dashboard

```bash
env -u PYTHONPATH platform/.venv/bin/python platform/app.py
```

Then open:

- On the laptop: <http://localhost:5000>
- From another device on the same network: `http://<LAPTOP-IP>:5000`

`Ctrl+C` to stop it.

**Why `env -u PYTHONPATH`?** The shell profile adds ROS Humble's packages to `PYTHONPATH` and
they get mixed with the venv's. This removes them for this one command only.

**If port 5000 is busy** (an old server was left running):

```bash
ss -ltnp | grep 5000      # which process holds it
pkill -f platform/app.py  # kill the old server
```

**The dashboard connects to the broker on `localhost`** by default (the broker runs on the same
laptop, so it does not depend on the IP). If the broker ever ran on another machine:

```bash
ILU_BROKER=<BROKER-IP> env -u PYTHONPATH platform/.venv/bin/python platform/app.py
```

### The two buttons

The dashboard's **Control** panel has two big buttons:

- **Open access / Close access**: one command on `ilu/all/cmd`, so both nodes receive it and both
  answer. node_b opens or closes; node_a starts or ends its lease. Opening also stops a latched
  alarm. The physical switch still works: whichever acted last wins, and the switch acts when it
  is **flipped**. After a dashboard Close with the switch left on, flip it off and on to reopen.
- **Turn buzzer ON / OFF**: node_a's buzzer by hand, with or without an alarm. A latched alarm
  keeps sounding until access opens, whatever this button says.

Each button shows what the node reported and offers the opposite. If the label does not change,
the node did not confirm it; look at the chips below.

What the LEDs mean (both nodes always show the same colour):

| LED | Meaning |
|---|---|
| blue | access open |
| red | closed, nobody at the door |
| green | closed, someone near node_b |
| red blinking (both) | alarm: motion while closed; buzzer sounds until access opens |
| blue blinking | that node lost the other one (3 s without ESP-NOW frames) |
| yellow (node_a only) | PIR warming up (first `warmup_s` seconds) |

### Other commands (Advanced panel)

Under **Advanced** in the Control panel: pick the target (node_a, node_b or Both), the variable
and the value, then **Send**. The table below the buttons shows each node's answer, for the
buttons too:

| Chip | Meaning |
|---|---|
| `pending` | sent, the node has not answered yet |
| `received` | the node confirmed reception (it read and parsed the command) |
| `applied = X` | the node executed it; X is the value it is using now |
| `rejected: reason · still X` | the node refused it; it keeps using X |
| `timed out` | no final answer within 3 s (node off, or the message was lost) |

The small number on each chip is the round trip in milliseconds. On the real boards it is
~100–350 ms.

| Variable | Node | Range | Effect |
|---|---|---|---|
| `access` | A and B | 0–1 | what the Open/Close button sends |
| `buzzer_on` | A | 0–1 | what the buzzer button sends |
| `buzzer_enabled` | A | 0–1 | 0 = silent alarm (LED red and node_b alerted, no sound) |
| `warmup_s` | A | 0–300 | PIR warm-up in seconds |
| `near_cm` | B | 5–100 | upper edge of the "near" (yellow) zone |
| `repeat_ms` | B | 200–1000 | how often node_b repeats the access state to node_a |
| `publish_ms` | A and B | 250–2000 | how often the node publishes its state |

To show a rejection: send `near_cm = 500` to node_b, or `near_cm` to **Both** (node_a has no such
variable and answers `unknown variable`). Values return to the defaults when a node reboots; the
dashboard shows that, because it always displays what the node reports.

A command can also be sent by hand, without the dashboard. Malformed ones are answered with a
`rejected` ack and the parser's reason:

```bash
mosquitto_pub -h $BROKER -t ilu/b/cmd -m '{"id":"manual-1","var":"near_cm","value":25}'
mosquitto_pub -h $BROKER -t ilu/all/cmd -m '{"id":"manual-2","var":"publish_ms","value":500}'
mosquitto_pub -h $BROKER -t ilu/b/cmd -m '{"id":"manual-3","var":"near_cm","value":2.5}'   # rejected
mosquitto_sub -h $BROKER -t 'ilu/+/ack' -v      # watch the answers
```

Commands sent by hand show up in the dashboard timeline as "Ack for unknown command", because the
platform did not send them.

### History

Everything that arrives is stored in `platform/history.db` (SQLite): reports, events, commands
and acks. The dashboard has links to download each table as CSV, or query it directly:

```bash
sqlite3 platform/history.db "SELECT * FROM acks ORDER BY id DESC LIMIT 10;"
```

### Recreate the Python environment (if `platform/.venv` is missing or broken)

```bash
rm -rf platform/.venv
/usr/bin/python3 -m venv platform/.venv
platform/.venv/bin/pip install -r platform/requirements.txt
```

---

## 5. Rebuild and flash the nodes (after any change)

### 5.1 Activate ESP-IDF (once per terminal)

```bash
get_idf
```

(An alias for `. $HOME/esp/esp-idf/export.sh`, defined in `~/.bashrc`.)
Without it, `idf.py` does not exist in that terminal.

### 5.2 Identify the ports

`ttyUSB0` / `ttyUSB1` swap depending on plug-in order. Always use the per-adapter path:

```bash
ls /dev/serial/by-id/
```

```bash
PORT_A=/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
PORT_B=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_7a041be62e9cef118535af9061ce3355-if00-port0
```

If in doubt which board is which, the MAC does not lie:

```bash
esptool.py -p $PORT_A read_mac    # node_a → 78:42:1c:68:44:98
esptool.py -p $PORT_B read_mac    # node_b → f4:65:0b:c0:e0:a4
```

### 5.3 Build

```bash
idf.py -C node_a build
idf.py -C node_b build
```

The end of each build shows how much space is left in the app partition (~14 % today).
If it says the image does not fit, see "Flash budget" in `CLAUDE.md`.

### 5.4 Flash

```bash
idf.py -C node_a -p $PORT_A flash
idf.py -C node_b -p $PORT_B flash
```

`flash` also builds when needed, so `build` + `flash` can be one step.

### 5.5 What do I rebuild after changing what?

| I changed… | Rebuild and flash |
|---|---|
| `shared/net/secrets.h` (broker IP, Wi-Fi) | **both nodes** |
| `shared/core/*` or `shared/net/*` | **both nodes** |
| `node_a/main/*` | node_a only |
| `node_b/main/*` | node_b only |
| `platform/*` (dashboard) | no firmware; just restart `app.py` (`Ctrl+C` and run it again) |
| `platform/mosquitto/ilu.conf` | copy it to `/etc/mosquitto/conf.d/` and restart Mosquitto |

### 5.6 If the build acts strange

```bash
idf.py -C node_a fullclean
idf.py -C node_a build
```

### 5.7 Common flashing problems

- `Permission denied` on the port → the user is not in the `dialout` group:
  `sudo usermod -aG dialout $USER`, then log out and back in.
- `Failed to connect to ESP32` → hold **BOOT** on the board when flashing starts.
- `Device or resource busy` → a `monitor` is open on that port; close it (`Ctrl+]`).

---

## 6. Serial monitor (a node's logs)

```bash
idf.py -C node_a -p $PORT_A monitor
idf.py -C node_b -p $PORT_B monitor
```

- `Ctrl+]` to quit.
- `Ctrl+T` then `Ctrl+R` resets the board without unplugging it.

Flash and open the monitor in one command:

```bash
idf.py -C node_a -p $PORT_A flash monitor
```

What to look for in the logs:

- Wi-Fi connection (takes ~12–15 s; 2–3 retries are normal)
- `MQTT connected` ~0.4 s after getting an IP
- Wi-Fi OK but never MQTT → the broker IP is wrong (section 1)

---

## 7. Shared-core tests (no boards needed)

Builds `shared/core` on the PC (no ESP-IDF) and runs the tests for the ESP-NOW frame parser
and for the command parser:

```bash
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

## 7b. Platform test (no boards needed)

Starts its own broker (amqtt, pure Python), runs `app.py` against it with two simulated nodes
(`test/sim_nodes.py`), and checks commands, rejections, timeouts, offline detection, Last Will
and history (46 checks):

```bash
env -u PYTHONPATH platform/.venv/bin/pip install "python-socketio[client]" amqtt   # first time only
env -u PYTHONPATH platform/.venv/bin/python test/test_platform.py
```

The simulated nodes can also show the dashboard without boards:

```bash
env -u PYTHONPATH platform/.venv/bin/python test/sim_nodes.py localhost
```

Careful: if the boards are powered at the same time, the simulated nodes publish on the same
topics.

---

## 8. Quick checklist before the demo

```bash
cd ~/Documents/ILU
ip -4 -br addr                                   # laptop's current IP
grep MQTT_BROKER_URI shared/net/secrets.h        # same IP as above?
BROKER=$(grep -oP 'mqtt://\K[0-9.]+' shared/net/secrets.h)
systemctl is-active mosquitto                    # → active
ss -ltn | grep 1883                              # → 0.0.0.0:1883
mosquitto_sub -h $BROKER -t 'ilu/#' -v           # messages from a and b? (Ctrl+C)
env -u PYTHONPATH platform/.venv/bin/python platform/app.py   # → http://localhost:5000
```

### If the new version fails: go back to the verified one

Branch `v2.0` has the commands; `main` is the version verified before them.

```bash
git switch main
idf.py -C node_a -p $PORT_A flash
idf.py -C node_b -p $PORT_B flash
# and restart app.py: main's version does not know about commands
```

Recommended terminals during the demo:

1. `app.py` (dashboard)
2. `mosquitto_sub` (raw MQTT traffic, a good visual proof for the evaluators)
3. (optional) `monitor` on one node, to show the ESP-NOW logs
