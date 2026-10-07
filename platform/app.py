"""ILU platform: MQTT -> live dashboard.

One process, three jobs:
  1. MQTT client subscribed to ilu/# (paho runs it in its own thread).
  2. Holds the last state each node *reported*. Nothing shown in the UI is
     invented here; timeline events are differences between two reports.
  3. Pushes every change to the browser over Socket.IO, so the page is live
     without polling.

Run from the repo root:
  env -u PYTHONPATH platform/.venv/bin/python platform/app.py
then open http://localhost:5000 (or http://<laptop-ip>:5000 from the LAN).
"""

import json
import os
import threading
import time
from collections import deque

import paho.mqtt.client as mqtt
from flask import Flask, render_template
from flask_socketio import SocketIO, emit

BROKER_HOST = os.environ.get("ILU_BROKER", "localhost")
BROKER_PORT = 1883
TOPIC = "ilu/#"

# Nodes publish every 1 s; three missed messages means the node is gone.
OFFLINE_AFTER_S = 3.0
EVENT_HISTORY = 200

app = Flask(__name__)
# threading mode: paho already owns a thread, and no eventlet/gevent
# monkey-patching is needed to explain.
socketio = SocketIO(app, async_mode="threading")

# Re-entrant: add_event takes it, and is also called while it is already held.
lock = threading.RLock()
nodes = {
    n: {"state": None, "last_seen": None, "online": False, "msgs": 0}
    for n in ("a", "b")
}
events = deque(maxlen=EVENT_HISTORY)
broker = {"connected": False}

# (field, text when it becomes true, text when it becomes false, kind)
# `near` is left out on purpose: it flickers several times a second at the
# edge of the range and would bury everything else. The card still shows it.
WATCHED = {
    "a": [
        ("warm", "PIR warm-up finished, sensor trusted", None, "info"),
        ("motion", "Motion detected by PIR", "Motion cleared", "info"),
        ("alarm", "ALARM: buzzer on, MotionStarted sent to node_b", "Alarm off", "alarm"),
        ("access", "Access lease active: alarm disarmed (blue)", "Access closed: alarm armed", "access"),
    ],
    "b": [
        ("access", "Switch ON: access open", "Switch OFF: access closed", "access"),
        ("alert", "ALERT: intrusion reported by node_a (red blinking)", "Alert cleared by access switch", "alarm"),
    ],
}


def add_event(node, text, kind, ts_node=None):
    ev = {"ts_server": time.time(), "ts_node": ts_node, "node": node, "text": text, "kind": kind}
    with lock:
        events.appendleft(ev)
    socketio.emit("event", ev)


def node_payload(n):
    d = nodes[n]
    return {"node": n, "state": d["state"], "online": d["online"],
            "last_seen": d["last_seen"], "msgs": d["msgs"]}


def diff_events(n, old, new):
    up = new.get("up")
    if old is None:
        add_event(n, "First report received", "link", up)
        return
    if up is not None and old.get("up") is not None and up < old["up"]:
        add_event(n, "Node rebooted (uptime went backwards)", "link", up)
    for field, on_text, off_text, kind in WATCHED[n]:
        if field in new and old.get(field) != new[field]:
            text = on_text if new[field] else off_text
            if text:
                add_event(n, text, kind, up)


# --- MQTT (runs in paho's network thread) -----------------------------------

def on_connect(client, userdata, flags, reason_code, properties):
    broker["connected"] = not reason_code.is_failure
    socketio.emit("broker", broker)
    if broker["connected"]:
        client.subscribe(TOPIC)
        add_event(None, f"Platform connected to broker {BROKER_HOST}:{BROKER_PORT}", "link")


def on_disconnect(client, userdata, flags, reason_code, properties):
    broker["connected"] = False
    socketio.emit("broker", broker)
    add_event(None, "Platform lost the broker; paho is reconnecting", "link")


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")  # ilu/<node>/state
    if len(parts) != 3 or parts[2] != "state" or parts[1] not in nodes:
        return
    n = parts[1]
    try:
        state = json.loads(msg.payload)
    except ValueError:
        # A malformed report is shown, not silently dropped.
        add_event(n, f"Malformed JSON on {msg.topic}", "link")
        return

    with lock:
        d = nodes[n]
        was_online = d["online"]
        diff_events(n, d["state"], state)
        d["state"] = state
        d["last_seen"] = time.time()
        d["online"] = True
        d["msgs"] += 1
        if not was_online and d["msgs"] > 1:
            add_event(n, "Node back online", "link", state.get("up"))
        payload = node_payload(n)
    socketio.emit("node", payload)


def watchdog():
    # Silence, not a goodbye message, is what marks a node offline: a node
    # that loses power never gets to say anything.
    while True:
        socketio.sleep(0.5)
        now = time.time()
        with lock:
            for n, d in nodes.items():
                if d["online"] and d["last_seen"] and now - d["last_seen"] > OFFLINE_AFTER_S:
                    d["online"] = False
                    add_event(n, f"Node OFFLINE: no report for {OFFLINE_AFTER_S:.0f} s", "link")
                    socketio.emit("node", node_payload(n))


# --- Web ---------------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html")


@socketio.on("connect")
def on_browser_connect():
    # A browser that opens late still gets the full picture at once.
    with lock:
        snap = {"nodes": [node_payload(n) for n in nodes],
                "events": list(events), "broker": broker, "server_time": time.time()}
    emit("snapshot", snap)  # inside a handler: goes to this browser only


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ilu-platform")
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=5)
    # connect_async + loop_start: the web server comes up even if the broker
    # is down, and paho keeps retrying in the background.
    client.connect_async(BROKER_HOST, BROKER_PORT, keepalive=10)
    client.loop_start()

    socketio.start_background_task(watchdog)
    # allow_unsafe_werkzeug: Werkzeug is fine for a single-laptop demo; a
    # deployment would sit behind a production server instead.
    socketio.run(app, host="0.0.0.0", port=5000, allow_unsafe_werkzeug=True)


if __name__ == "__main__":
    main()
