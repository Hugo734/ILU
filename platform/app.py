"""ILU platform: MQTT <-> live dashboard, commands and history.

One process, five jobs:
  1. MQTT client subscribed to ilu/# (paho runs it in its own thread).
  2. Holds the last state each node *reported*. Nothing shown in the UI is
     invented here; timeline events are differences between two reports.
  3. Sends commands to one node (ilu/<node>/cmd) or to both at once
     (ilu/all/cmd), and tracks each one per node:
       pending -> received -> applied | rejected, or timed out.
     Ranges are not checked here: the node is the authority, so every
     rejection the UI shows is the node's own answer.
  4. Pushes every change to the browser over Socket.IO, so the page is live
     without polling.
  5. Logs reports, events, commands and acks to SQLite (platform/history.db),
     exportable as CSV.

Run from the repo root:
  env -u PYTHONPATH platform/.venv/bin/python platform/app.py
then open http://localhost:5000 (or http://<laptop-ip>:5000 from the LAN).
"""

import csv
import io
import json
import os
import re
import sqlite3
import threading
import time
from collections import OrderedDict, deque

import paho.mqtt.client as mqtt
from flask import Flask, Response, abort, render_template
from flask_socketio import SocketIO, emit

BROKER_HOST = os.environ.get("ILU_BROKER", "localhost")
BROKER_PORT = int(os.environ.get("ILU_BROKER_PORT", "1883"))
HTTP_PORT = int(os.environ.get("ILU_PORT", "5000"))
DB_PATH = os.environ.get("ILU_DB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "history.db"))
TOPIC = "ilu/#"

# Nodes publish every 1 s by default; three missed messages means the node is gone.
OFFLINE_AFTER_S = 3.0
# A command with no final ack after this long is shown as timed out. A late
# ack still updates it afterwards, marked as late: the node's answer wins.
CMD_TIMEOUT_S = 3.0
EVENT_HISTORY = 200
COMMAND_HISTORY = 50

# Fits the node's 15-character id buffer: HHMMSS of this run plus a counter,
# so an ack left over from a previous run can never match a new command.
SESSION = time.strftime("%H%M%S")
VAR_RE = re.compile(r"^[a-z_][a-z0-9_]{0,22}$")

app = Flask(__name__)
# threading mode: paho already owns a thread, and no eventlet/gevent
# monkey-patching is needed to explain.
socketio = SocketIO(app, async_mode="threading")

# Re-entrant: add_event takes it, and is also called while it is already held.
# It also serialises every write to the SQLite connection.
lock = threading.RLock()
nodes = {
    n: {"state": None, "last_seen": None, "online": False, "msgs": 0, "lwt": None}
    for n in ("a", "b")
}
events = deque(maxlen=EVENT_HISTORY)
commands = OrderedDict()
broker = {"connected": False}
cmd_counter = 0
mqttc = None

# (field, text when it becomes true, text when it becomes false, kind)
# `near` is left out on purpose: it flickers several times a second at the
# edge of the range and would bury everything else. The card still shows it.
WATCHED = {
    "a": [
        ("warm", "PIR warm-up finished, sensor trusted", None, "info"),
        ("motion", "Motion detected by PIR", "Motion cleared", "info"),
        ("alarm", "ALARM: MotionStarted sent to node_b", "Alarm off", "alarm"),
        ("access", "Access lease active: alarm disarmed (blue)", "Access closed: alarm armed", "access"),
        ("peer", "ESP-NOW link to node_b up", "ESP-NOW link to node_b LOST: node_a stays armed", "link"),
    ],
    "b": [
        ("access", "Switch ON: access open", "Switch OFF: access closed", "access"),
        ("alert", "ALERT: intrusion reported by node_a (red blinking)", "Alert cleared by access switch", "alarm"),
        ("peer", "ESP-NOW link to node_a up", "ESP-NOW link to node_a LOST: LED blue, room not watched", "link"),
    ],
}

# Remotely controllable variables, as each node reports them in its state.
SETTINGS = {
    "a": ("buzzer_enabled", "warmup_s", "publish_ms"),
    "b": ("near_cm", "repeat_ms", "publish_ms"),
}


# --- SQLite -------------------------------------------------------------------

SCHEMA = """
CREATE TABLE IF NOT EXISTS telemetry (
    id INTEGER PRIMARY KEY, ts_server REAL NOT NULL, node TEXT NOT NULL,
    up_ms INTEGER, payload TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY, ts_server REAL NOT NULL, node TEXT,
    up_ms INTEGER, kind TEXT NOT NULL, text TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS commands (
    id INTEGER PRIMARY KEY, ts_server REAL NOT NULL, cmd_id TEXT NOT NULL,
    target TEXT NOT NULL, var TEXT NOT NULL, value INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS acks (
    id INTEGER PRIMARY KEY, ts_server REAL NOT NULL, cmd_id TEXT, node TEXT NOT NULL,
    stage TEXT, var TEXT, value INTEGER, reason TEXT);
CREATE INDEX IF NOT EXISTS acks_cmd ON acks(cmd_id);
"""
TABLES = ("telemetry", "events", "commands", "acks")

# isolation_level=None: autocommit, so every row is on disk as soon as it is
# written and a crash loses nothing already shown on screen.
db = sqlite3.connect(DB_PATH, check_same_thread=False, isolation_level=None)
db.execute("PRAGMA journal_mode=WAL")  # CSV export reads while paho writes
db.executescript(SCHEMA)


def db_insert(table, **row):
    cols = ", ".join(row)
    marks = ", ".join("?" for _ in row)
    with lock:
        db.execute(f"INSERT INTO {table} ({cols}) VALUES ({marks})", tuple(row.values()))


# --- State and events -----------------------------------------------------------

def add_event(node, text, kind, ts_node=None):
    ev = {"ts_server": time.time(), "ts_node": ts_node, "node": node, "text": text, "kind": kind}
    with lock:
        events.appendleft(ev)
        db_insert("events", ts_server=ev["ts_server"], node=node, up_ms=ts_node, kind=kind, text=text)
    socketio.emit("event", ev)


def node_payload(n):
    d = nodes[n]
    return {"node": n, "state": d["state"], "online": d["online"],
            "last_seen": d["last_seen"], "msgs": d["msgs"], "lwt": d["lwt"]}


def diff_events(n, old, new):
    up = new.get("up")
    if old is None:
        add_event(n, "First report received", "link", up)
        return
    if up is not None and old.get("up") is not None and up < old["up"]:
        add_event(n, "Node rebooted (uptime went backwards); settings back to defaults", "link", up)
    for field, on_text, off_text, kind in WATCHED[n]:
        if field in new and old.get(field) != new[field]:
            text = on_text if new[field] else off_text
            if text:
                add_event(n, text, kind, up)
    # What the node now reports is the proof a command took effect; the ack
    # alone is the node's word, this is its behaviour.
    for field in SETTINGS[n]:
        if field in new and field in old and old[field] != new[field]:
            add_event(n, f"Node reports {field} = {new[field]} (was {old[field]})", "cmd", up)


def handle_state(n, payload):
    try:
        state = json.loads(payload)
    except ValueError:
        # A malformed report is shown, not silently dropped.
        add_event(n, "Malformed JSON on state topic", "link")
        return
    if not isinstance(state, dict):
        add_event(n, "State report is not a JSON object", "link")
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
        db_insert("telemetry", ts_server=d["last_seen"], node=n, up_ms=state.get("up"),
                  payload=json.dumps(state, separators=(",", ":")))
        payload_out = node_payload(n)
    socketio.emit("node", payload_out)


def handle_status(n, payload, retained):
    # The node publishes "online" (retained) on connect; the broker publishes
    # the Last Will "offline" when the node stops answering keepalives. This is
    # a second detector next to the 3 s silence watchdog, and a slower one.
    text = payload.decode(errors="replace").strip()
    with lock:
        nodes[n]["lwt"] = text
        out = node_payload(n)
    if text == "offline":
        add_event(n, "Broker reports node offline (MQTT Last Will)" + (" — retained from before" if retained else ""), "link")
    elif text == "online" and not retained:
        add_event(n, "Node connected to the broker", "link")
    socketio.emit("node", out)


# --- Commands -------------------------------------------------------------------

FINAL = ("applied", "rejected")


def command_view(cmd):
    return {k: cmd[k] for k in ("id", "ts", "target", "var", "value", "nodes")}


def handle_ack(n, payload):
    try:
        ack = json.loads(payload)
    except ValueError:
        add_event(n, "Malformed JSON on ack topic", "link")
        return
    if not isinstance(ack, dict):
        return
    cid, stage = ack.get("id"), ack.get("stage")
    now = time.time()
    value = ack.get("value") if isinstance(ack.get("value"), int) else None

    with lock:
        db_insert("acks", ts_server=now, cmd_id=cid, node=n, stage=stage, var=ack.get("var"),
                  value=value, reason=ack.get("reason"))
        cmd = commands.get(cid)
        slot = cmd["nodes"].get(n) if cmd else None
        if slot is None:
            # No matching command: a malformed payload the node could not read
            # an id from, or a command from another tool. Shown, not dropped.
            add_event(n, f"Ack for unknown command '{cid}': {stage}"
                         + (f" ({ack.get('reason')})" if ack.get("reason") else ""), "cmd")
            return
        if stage == "received":
            if slot["stage"] == "pending":
                slot["stage"] = "received"
                slot["t_received"] = round(now - cmd["ts"], 3)
        elif stage in FINAL and slot["stage"] not in FINAL:
            slot["late"] = slot["stage"] == "timeout"
            slot["stage"] = stage
            slot["reason"] = ack.get("reason")
            slot["value"] = value
            slot["t_final"] = round(now - cmd["ts"], 3)
            if stage == "applied":
                add_event(n, f"Command {cid} applied: {cmd['var']} = {value}"
                             + (" (late, after timeout)" if slot["late"] else ""), "cmd")
            else:
                still = f", {cmd['var']} still {value}" if value is not None else ""
                add_event(n, f"Command {cid} REJECTED: {ack.get('reason')}{still}", "cmd")
        out = command_view(cmd)
    socketio.emit("command", out)


def send_command(target, var, value):
    """Returns (command_view, None) or (None, error)."""
    global cmd_counter
    if target not in ("a", "b", "all"):
        return None, "target must be a, b or all"
    if not isinstance(var, str) or not VAR_RE.match(var):
        return None, "variable name must be lowercase letters, digits and _"
    try:
        value = int(value)
    except (TypeError, ValueError):
        return None, "value must be an integer"
    if not -2**31 <= value < 2**31:
        return None, "value must fit in 32 bits"
    if not broker["connected"]:
        return None, "platform is not connected to the broker"

    with lock:
        cmd_counter += 1
        cid = f"{SESSION}-{cmd_counter}"
        targets = ("a", "b") if target == "all" else (target,)
        cmd = {"id": cid, "ts": time.time(), "target": target, "var": var, "value": value,
               "nodes": {n: {"stage": "pending", "reason": None, "value": None, "late": False,
                             "t_received": None, "t_final": None} for n in targets}}
        commands[cid] = cmd
        while len(commands) > COMMAND_HISTORY:
            commands.popitem(last=False)
        db_insert("commands", ts_server=cmd["ts"], cmd_id=cid, target=target, var=var, value=value)
        out = command_view(cmd)

    # One publish on ilu/all/cmd reaches both nodes at once: that is what
    # "both simultaneously" means here, not two separate sends.
    body = json.dumps({"id": cid, "var": var, "value": value}, separators=(",", ":"))
    mqttc.publish(f"ilu/{target}/cmd", body, qos=1)
    socketio.emit("command", out)
    add_event(None, f"Command {cid} sent to {'both nodes' if target == 'all' else 'node_' + target}: "
                    f"{var} = {value}", "cmd")
    return out, None


# --- MQTT (runs in paho's network thread) ---------------------------------------

def on_connect(client, userdata, flags, reason_code, properties):
    broker["connected"] = not reason_code.is_failure
    socketio.emit("broker", broker)
    if broker["connected"]:
        client.subscribe(TOPIC, qos=1)
        add_event(None, f"Platform connected to broker {BROKER_HOST}:{BROKER_PORT}", "link")


def on_disconnect(client, userdata, flags, reason_code, properties):
    broker["connected"] = False
    socketio.emit("broker", broker)
    add_event(None, "Platform lost the broker; paho is reconnecting", "link")


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")  # ilu/<node>/<kind>
    if len(parts) != 3 or parts[1] not in nodes:
        return  # includes ilu/all/cmd, which this process publishes itself
    n, kind = parts[1], parts[2]
    if kind == "state":
        handle_state(n, msg.payload)
    elif kind == "ack":
        handle_ack(n, msg.payload)
    elif kind == "status":
        handle_status(n, msg.payload, msg.retain)


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
            for cmd in commands.values():
                changed = False
                for n, slot in cmd["nodes"].items():
                    if slot["stage"] in ("pending", "received") and now - cmd["ts"] > CMD_TIMEOUT_S:
                        heard = " (reception confirmed, no execution ack)" if slot["stage"] == "received" else ""
                        slot["stage"] = "timeout"
                        changed = True
                        add_event(n, f"Command {cmd['id']} TIMED OUT after {CMD_TIMEOUT_S:.0f} s{heard}", "cmd")
                if changed:
                    socketio.emit("command", command_view(cmd))


# --- Web --------------------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/export/<table>.csv")
def export(table):
    if table not in TABLES:
        abort(404)
    # A separate read connection: WAL lets it read while paho keeps writing.
    con = sqlite3.connect(DB_PATH)
    try:
        cur = con.execute(f"SELECT * FROM {table} ORDER BY id")
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow([c[0] for c in cur.description])
        w.writerows(cur)
    finally:
        con.close()
    return Response(buf.getvalue(), mimetype="text/csv",
                    headers={"Content-Disposition": f"attachment; filename=ilu_{table}.csv"})


@socketio.on("connect")
def on_browser_connect():
    # A browser that opens late still gets the full picture at once.
    with lock:
        snap = {"nodes": [node_payload(n) for n in nodes],
                "events": list(events), "broker": broker, "server_time": time.time(),
                "commands": [command_view(c) for c in commands.values()],
                "cmd_timeout": CMD_TIMEOUT_S}
    emit("snapshot", snap)  # inside a handler: goes to this browser only


@socketio.on("command")
def on_browser_command(msg):
    # The return value is the Socket.IO acknowledgement to that browser.
    if not isinstance(msg, dict):
        return {"ok": False, "error": "bad request"}
    cmd, err = send_command(msg.get("target"), msg.get("var"), msg.get("value"))
    return {"ok": True, "id": cmd["id"]} if cmd else {"ok": False, "error": err}


def main():
    global mqttc
    mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ilu-platform-{SESSION}")
    mqttc.on_connect = on_connect
    mqttc.on_disconnect = on_disconnect
    mqttc.on_message = on_message
    mqttc.reconnect_delay_set(min_delay=1, max_delay=5)
    # connect_async + loop_start: the web server comes up even if the broker
    # is down, and paho keeps retrying in the background.
    mqttc.connect_async(BROKER_HOST, BROKER_PORT, keepalive=10)
    mqttc.loop_start()

    socketio.start_background_task(watchdog)
    # allow_unsafe_werkzeug: Werkzeug is fine for a single-laptop demo; a
    # deployment would sit behind a production server instead.
    socketio.run(app, host="0.0.0.0", port=HTTP_PORT, allow_unsafe_werkzeug=True)


if __name__ == "__main__":
    main()
