"""Stand-ins for node_a and node_b on MQTT, to test the platform without boards.

They speak the same MQTT contract as the firmware: state reports on
ilu/<n>/state, commands on ilu/<n>/cmd and ilu/all/cmd, acks
(received -> applied | rejected) on ilu/<n>/ack, and a retained
online/offline on ilu/<n>/status with the Last Will.

This tests the platform, not the firmware: the node logic here is a copy
of the rules, kept small on purpose. The firmware's own command handling is
tested in test_command.cpp.

Standalone:  python test/sim_nodes.py [broker-host] [port]
"""

import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

# Same names and ranges as VARS in node_a/main/main.cpp and node_b/main/main.cpp.
VARS = {
    "a": {"buzzer_enabled": (0, 1, 1), "warmup_s": (0, 300, 60), "publish_ms": (250, 2000, 1000),
          "access": (0, 1, 0), "buzzer_on": (0, 1, 0)},
    "b": {"near_cm": (5, 100, 15), "repeat_ms": (200, 1000, 1000), "publish_ms": (250, 2000, 1000),
          "access": (0, 1, 0)},
}
REQUIRED = ("id", "var", "value")


class SimNode:
    def __init__(self, node, host="localhost", port=1883, keepalive=5):
        self.node = node
        self.cfg = {k: v[2] for k, v in VARS[node].items()}
        self.t0 = time.time()
        self.extra = {}            # test hook: fields merged into the state
        self.answer = True         # test hook: False = receive commands, never apply
        self.access_by = "switch"
        self._stop = threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-node-{node}-{id(self)}")
        self.c.will_set(f"ilu/{node}/status", "offline", qos=1, retain=True)
        self.c.on_connect = self._on_connect
        self.c.on_message = self._on_message
        self.c.connect(host, port, keepalive=keepalive)

    def _on_connect(self, client, userdata, flags, rc, props):
        client.subscribe([(f"ilu/{self.node}/cmd", 1), ("ilu/all/cmd", 1)])
        client.publish(f"ilu/{self.node}/status", "online", qos=1, retain=True)

    def _ack(self, cid, var, stage, reason=None, value=None):
        ack = {"node": self.node, "id": cid, "var": var, "stage": stage}
        if reason is not None:
            ack["reason"] = reason
        if value is not None:
            ack["value"] = value
        self.c.publish(f"ilu/{self.node}/ack", json.dumps(ack, separators=(",", ":")), qos=1)

    def _on_message(self, client, userdata, msg):
        try:
            cmd = json.loads(msg.payload)
            if not isinstance(cmd, dict) or set(cmd) != set(REQUIRED) or not isinstance(cmd["value"], int):
                raise ValueError
        except ValueError:
            self._ack("", "", "rejected", "malformed")
            return
        cid, var, value = cmd["id"], cmd["var"], cmd["value"]
        self._ack(cid, var, "received")
        if not self.answer:
            return
        spec = VARS[self.node].get(var)
        if spec is None:
            self._ack(cid, var, "rejected", "unknown variable")
        elif not spec[0] <= value <= spec[1]:
            self._ack(cid, var, "rejected", "out of range", self.cfg[var])
        else:
            self.cfg[var] = value
            if var == "access":
                self.access_by = "platform"
            self._ack(cid, var, "applied", None, value)
            self.publish_state()  # a change goes out at once, like the firmware

    def state(self):
        up = int((time.time() - self.t0) * 1000)
        if self.node == "a":
            s = {"node": "a", "up": up, "warm": True, "motion": False, "alarm": False, "buzzer": 0,
                 "near": False, "peer": True, "led": "red"}
        else:
            s = {"node": "b", "up": up, "cm": 40, "near": False, "access_by": self.access_by,
                 "alert": False, "peer": True, "led": "red"}
        s.update(self.cfg)
        # The firmware reports access and the manual buzzer as booleans/pin state.
        s["access"] = bool(s["access"])
        if self.node == "a":
            s["buzzer"] = s["buzzer_on"]
        if s["access"]:
            s["led"] = "blue"
        s.update(self.extra)
        return s

    def publish_state(self):
        self.c.publish(f"ilu/{self.node}/state", json.dumps(self.state(), separators=(",", ":")), qos=0)

    def _run(self):
        while not self._stop.is_set():
            self.publish_state()
            self._stop.wait(self.cfg["publish_ms"] / 1000)

    def start(self):
        self.c.loop_start()
        threading.Thread(target=self._run, daemon=True).start()
        return self

    def stop_reports(self):
        """Goes silent but keeps the TCP session: the platform's watchdog must notice."""
        self._stop.set()

    def die(self):
        """Drops off the network without a DISCONNECT, like a board losing power:
        the broker must publish the Last Will once the keepalive runs out."""
        self._stop.set()
        self.c.loop_stop()
        try:
            self.c.socket().close()
        except Exception:
            pass


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
    a, b = SimNode("a", host, port).start(), SimNode("b", host, port).start()
    print(f"simulating node_a and node_b on {host}:{port}; Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
