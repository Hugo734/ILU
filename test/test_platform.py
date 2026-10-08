"""End-to-end test of the platform: broker + platform/app.py + simulated nodes.

Starts its own MQTT broker (amqtt, pure Python) on a spare port, runs the
platform against it with a throw-away database, drives it through Socket.IO
exactly as the dashboard does, and checks every answer.

    pip install -r platform/requirements.txt "python-socketio[client]" amqtt
    python test/test_platform.py

What it covers: commands to one node and to both (#4), variables on each node
(#5), reception and execution acks with the value the node reports (#6), and
loss detection by silence and by the broker's Last Will (#7), plus the SQLite
history and its CSV export. What it does not cover: the firmware. The nodes
here are stand-ins (sim_nodes.py).
"""

import csv
import io
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

import socketio

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from sim_nodes import SimNode  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(pred, timeout=5.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def wait_port(port, timeout=15.0):
    return wait_for(lambda: socket.socket().connect_ex(("127.0.0.1", port)) == 0, timeout, 0.2)


def main():
    tmp = tempfile.mkdtemp(prefix="ilu-test-")
    mqtt_port, http_port = free_port(), free_port()
    conf = os.path.join(tmp, "broker.yaml")
    with open(conf, "w") as f:
        f.write(f"listeners:\n  default:\n    type: tcp\n    bind: 127.0.0.1:{mqtt_port}\n"
                "plugins:\n  amqtt.plugins.authentication.AnonymousAuthPlugin:\n    allow_anonymous: true\n")

    scripts = os.path.dirname(sys.executable)
    amqtt = os.path.join(scripts, "amqtt.exe" if os.name == "nt" else "amqtt")
    procs = []
    try:
        procs.append(subprocess.Popen([amqtt, "-c", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        check(wait_port(mqtt_port), f"broker listening on {mqtt_port}")

        env = dict(os.environ, ILU_BROKER="127.0.0.1", ILU_BROKER_PORT=str(mqtt_port),
                   ILU_PORT=str(http_port), ILU_DB=os.path.join(tmp, "history.db"))
        log = open(os.path.join(tmp, "platform.log"), "w")
        procs.append(subprocess.Popen([sys.executable, os.path.join(ROOT, "platform", "app.py")],
                                      env=env, stdout=log, stderr=subprocess.STDOUT))
        check(wait_port(http_port), f"platform serving on {http_port}")

        # Short keepalive so the Last Will fires within the test (1.5 x 2 s).
        a = SimNode("a", "127.0.0.1", mqtt_port, keepalive=2).start()
        b = SimNode("b", "127.0.0.1", mqtt_port, keepalive=2).start()

        nodes, cmds, events = {}, {}, []
        sio = socketio.Client()
        sio.on("snapshot", lambda s: [nodes.__setitem__(p["node"], p) for p in s["nodes"]])
        sio.on("node", lambda p: nodes.__setitem__(p["node"], p))
        sio.on("command", lambda c: cmds.__setitem__(c["id"], c))
        sio.on("event", lambda e: events.append(e))
        sio.connect(f"http://127.0.0.1:{http_port}", wait_timeout=10)

        online = lambda n: nodes.get(n, {}).get("online")
        reported = lambda n, k: ((nodes.get(n) or {}).get("state") or {}).get(k)
        check(wait_for(lambda: online("a") and online("b"), 10), "both nodes shown online")

        def send(target, var, value):
            res = sio.call("command", {"target": target, "var": var, "value": value}, timeout=5)
            return res

        def final(cid, n):
            c = cmds.get(cid)
            return c and c["nodes"][n]["stage"] in ("applied", "rejected", "timeout")

        # --- #4 + #5 + #6: one node --------------------------------------------
        r = send("a", "buzzer_enabled", 0)
        check(r["ok"], "command to node_a accepted by the platform")
        cid = r["id"]
        check(wait_for(lambda: final(cid, "a")), "node_a answered")
        slot = cmds[cid]["nodes"]["a"]
        check(slot["stage"] == "applied" and slot["value"] == 0, "node_a applied buzzer_enabled = 0")
        check(slot["t_received"] is not None, "reception was acknowledged before execution")
        check(list(cmds[cid]["nodes"]) == ["a"], "a node_a command expects no answer from node_b")
        check(wait_for(lambda: reported("a", "buzzer_enabled") == 0), "node_a's state report shows the new value")

        r = send("b", "near_cm", 20)
        check(wait_for(lambda: final(r["id"], "b")) and cmds[r["id"]]["nodes"]["b"]["stage"] == "applied",
              "node_b applied near_cm = 20")
        r = send("b", "repeat_ms", 500)
        check(wait_for(lambda: final(r["id"], "b")) and cmds[r["id"]]["nodes"]["b"]["value"] == 500,
              "node_b applied repeat_ms = 500 (second variable on node_b)")
        r = send("a", "warmup_s", 10)
        check(wait_for(lambda: final(r["id"], "a")) and cmds[r["id"]]["nodes"]["a"]["value"] == 10,
              "node_a applied warmup_s = 10 (second variable on node_a)")

        # --- Rejection is the node's answer, with the value still in effect ----
        r = send("b", "near_cm", 500)
        check(r["ok"], "out-of-range value is forwarded, not filtered by the platform")
        check(wait_for(lambda: final(r["id"], "b")), "node_b answered the out-of-range command")
        slot = cmds[r["id"]]["nodes"]["b"]
        check(slot["stage"] == "rejected" and slot["reason"] == "out of range" and slot["value"] == 20,
              "node_b rejected near_cm = 500 and reports 20 still in effect")
        check(reported("b", "near_cm") == 20, "node_b's state still reports 20")

        # --- #4: both at once ---------------------------------------------------
        r = send("all", "publish_ms", 500)
        cid = r["id"]
        check(wait_for(lambda: final(cid, "a") and final(cid, "b")), "both nodes answered one broadcast")
        check(all(cmds[cid]["nodes"][n]["stage"] == "applied" for n in "ab"), "both applied publish_ms = 500")
        check(wait_for(lambda: reported("a", "publish_ms") == 500 and reported("b", "publish_ms") == 500),
              "both state reports show publish_ms = 500")

        r = send("all", "near_cm", 30)
        cid = r["id"]
        check(wait_for(lambda: final(cid, "a") and final(cid, "b")), "both nodes answered a broadcast for node_b's variable")
        check(cmds[cid]["nodes"]["a"]["stage"] == "rejected" and cmds[cid]["nodes"]["a"]["reason"] == "unknown variable",
              "node_a rejected a variable it does not have")
        check(cmds[cid]["nodes"]["b"]["stage"] == "applied", "node_b applied the same broadcast")

        # --- Platform-side input checks -----------------------------------------
        check(not send("c", "x", 1)["ok"], "unknown target refused by the platform")
        check(not send("a", "near_cm", "abc")["ok"], "non-integer value refused by the platform")
        check(not send("a", 'x"}', 1)["ok"], "variable name with JSON characters refused")

        # --- #6: received but never executed -> timed out -----------------------
        a.answer = False
        r = send("a", "warmup_s", 20)
        cid = r["id"]
        check(wait_for(lambda: cmds[cid]["nodes"]["a"]["stage"] == "received", 2), "reception ack shown while waiting")
        check(wait_for(lambda: final(cid, "a"), 6) and cmds[cid]["nodes"]["a"]["stage"] == "timeout",
              "no execution ack -> timed out")
        check(reported("a", "warmup_s") == 10, "node_a still reports the old warmup_s")
        a.answer = True

        # --- #7: silence watchdog ------------------------------------------------
        b.stop_reports()
        check(wait_for(lambda: not online("b"), 6), "silent node_b marked offline by the watchdog")
        check(online("a"), "node_a stays online")
        check(any(e["node"] == "b" and "OFFLINE" in e["text"] for e in events), "offline event in the timeline")

        # --- #7: a peer-link loss reported by a node reaches the timeline ------
        a.extra = {"peer": False}
        check(wait_for(lambda: any("link to node_b LOST" in e["text"] for e in events), 4),
              "node_a's report of a lost ESP-NOW link becomes a timeline event")

        # --- #7: Last Will -------------------------------------------------------
        b.die()
        check(wait_for(lambda: (nodes.get("b") or {}).get("lwt") == "offline", 10),
              "broker published node_b's Last Will and the platform shows it")
        r = send("b", "near_cm", 25)
        check(wait_for(lambda: final(r["id"], "b"), 6) and cmds[r["id"]]["nodes"]["b"]["stage"] == "timeout",
              "command to a dead node times out")

        # --- History --------------------------------------------------------------
        def table(name):
            with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/export/{name}.csv", timeout=5) as resp:
                return list(csv.DictReader(io.StringIO(resp.read().decode())))
        cmd_rows, ack_rows, tel_rows, ev_rows = (table(t) for t in ("commands", "acks", "telemetry", "events"))
        check(len(cmd_rows) == 9, f"commands table holds every command sent ({len(cmd_rows)})")
        check(any(x["stage"] == "rejected" and x["reason"] == "out of range" for x in ack_rows), "rejections are stored")
        check(sum(1 for x in ack_rows if x["stage"] == "received") >= 9, "reception acks are stored")
        check(len(tel_rows) > 10 and {x["node"] for x in tel_rows} == {"a", "b"}, "telemetry from both nodes is stored")
        check(any("TIMED OUT" in x["text"] for x in ev_rows), "events are stored")
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{http_port}/export/sqlite_master.csv", timeout=5)
            check(False, "export refuses tables outside the whitelist")
        except urllib.error.HTTPError as e:
            check(e.code == 404, "export refuses tables outside the whitelist")

        sio.disconnect()
        a.die()
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()

    print(f"{failures} failure(s)" if failures else "all platform tests passed")
    print("platform log:", os.path.join(tmp, "platform.log"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
