// Node A -- Climate. Real sensor: DHT22 (temperature + humidity). Actuator: fan relay/MOSFET,
// plus an RGB status LED (green=normal, amber=degraded, red=link lost -- docs/architecture.md).
//
// Structure, per the project plan: sensor read -> control logic (control_logic.h, pure
// functions, unit-tested natively in test/) -> actuator drive -> telemetry/state publish ->
// command handling -> heartbeat + link watchdog. This file is the only place that touches
// GPIO/DHT/MQTT directly; every decision is made in control_logic.h so it stays testable
// without hardware.
//
// NOT YET BUILT OR RUN -- no PlatformIO/ESP32 toolchain in this environment yet. See
// sim/README.md and the top-level README's progress checklist for what has actually been
// verified vs. what is written-but-unverified.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>

#include "control_logic.h"
#include "protocol.h"
#include "transport_mqtt.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy firmware/node_a/include/secrets.h.example to secrets.h and fill in real values."
#endif

// ---- Pin assignments -- placeholders until real hardware is wired Wednesday; see
// docs/decisions.md for the safe-state reasoning behind what each pin drives. ----
constexpr int PIN_DHT22 = 4;
constexpr int PIN_FAN = 26;
constexpr int PIN_LED_R = 25;
constexpr int PIN_LED_G = 27;
constexpr int PIN_LED_B = 14;

constexpr uint32_t HB_INTERVAL_MS = 2000;          // docs/decisions.md "Heartbeat timing"
constexpr uint32_t LINK_TIMEOUT_MS = 6000;         // 3 missed heartbeats
constexpr uint32_t PEER_EVENT_STALE_MS = 10000;    // docs/protocol.md "Republishing, not edge-triggering"

DHT dht(PIN_DHT22, DHT22);
MqttTransport transport("A", TOPIC_PREFIX, WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER_HOST,
                         MQTT_BROKER_PORT, HMAC_SHARED_SECRET);

NodeAConfig cfg;  // current effective values of the 4 controllable variables (req. 5)

bool peerIrrigating = false;
uint32_t lastIrrigatingEventMs = 0;

bool linkToB_ok = true;
bool wasIsolated = false;
uint32_t lastPeerHbMs = 0;

uint32_t seqCounter = 0;
uint32_t nextSeq() { return ++seqCounter; }

// Replay protection for commands only -- see docs/protocol.md "Sequence / replay handling" for
// why hb/event are intentionally not gated the same way (it would deadlock on a peer reboot).
uint32_t lastSeqPlatform = 0;
bool acceptSeq(uint32_t &lastSeq, const JsonDocument &env) {
  uint32_t seq = env["seq"].as<uint32_t>();
  if (seq <= lastSeq) return false;  // stale or replayed
  lastSeq = seq;
  return true;
}

uint32_t lastSampleMs = 0;
uint32_t lastHbMs = 0;
bool lastFanOn = false;

void setRgb(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void publishState() {
  JsonDocument env;
  makeEnvelope(env, "A", nextSeq(), millis() / 1000, MsgType::STATE);
  JsonObject data = env["data"];
  data["actuator_state"] = lastFanOn ? "ON" : "OFF";
  data["fan_paused_by_b"] = peerIrrigating;
  data["link_to_b"] = linkToB_ok ? "OK" : "LOST";
  data["mode"] = cfg.auto_mode ? "AUTO" : "MANUAL";
  data["sample_period_ms"] = cfg.sample_period_ms;
  data["temp_setpoint"] = cfg.temp_setpoint;
  data["was_isolated"] = wasIsolated;
  signEnvelope(env, HMAC_SHARED_SECRET);
  transport.publish(Channel::STATE_OUT, env);
}

void publishAck(const String &cmdId, const char *result, const char *reason,
                JsonVariantConst value) {
  JsonDocument env;
  makeEnvelope(env, "A", nextSeq(), millis() / 1000, MsgType::ACK, cmdId.c_str());
  env["data"]["reason"] = reason;
  env["data"]["result"] = result;
  env["data"]["value"] = value;
  signEnvelope(env, HMAC_SHARED_SECRET);
  transport.publish(Channel::ACK_OUT, env);
}

// Applies one `cmd` message. Every branch reports the value actually in effect afterward (req.
// 6) -- e.g. a too-small sample_period_ms is clamped, and the ack reports the clamped value, not
// the one that was requested.
void handleCmd(const JsonDocument &env) {
  if (!verifyEnvelope(env, HMAC_SHARED_SECRET)) return;  // bad signature -- dropped, not acked
  if (!acceptSeq(lastSeqPlatform, env)) return;           // stale/replayed command -- dropped
  String cmdId = env["cmd_id"].as<String>();
  String set = env["data"]["set"].as<String>();
  JsonVariantConst value = env["data"]["value"];

  if (set == "temp_setpoint") {
    float v = value.as<float>();
    if (v < 0 || v > 60) {
      publishAck(cmdId, "rejected", "out of range", value);
      return;
    }
    cfg.temp_setpoint = v;
  } else if (set == "sample_period_ms") {
    unsigned long v = value.as<unsigned long>();
    if (v < 500) v = 500;  // firmware-enforced floor; ack below reports this clamped value
    cfg.sample_period_ms = v;
  } else if (set == "mode") {
    String m = value.as<String>();
    if (m != "AUTO" && m != "MANUAL") {
      publishAck(cmdId, "rejected", "unknown mode", value);
      return;
    }
    cfg.auto_mode = (m == "AUTO");
  } else if (set == "actuator_state") {
    cfg.manual_actuator_on = (value.as<String>() == "ON");
  } else {
    publishAck(cmdId, "rejected", "unknown variable", value);
    return;
  }

  JsonDocument actual;
  if (set == "temp_setpoint") actual.set(cfg.temp_setpoint);
  else if (set == "sample_period_ms") actual.set(cfg.sample_period_ms);
  else if (set == "mode") actual.set(cfg.auto_mode ? "AUTO" : "MANUAL");
  else actual.set(cfg.manual_actuator_on ? "ON" : "OFF");
  publishAck(cmdId, "applied", "", actual.as<JsonVariantConst>());
  publishState();  // req. 6: the dashboard's next view of reality comes from here, not the cmd
}

void handleEvent(const JsonDocument &env) {
  if (!verifyEnvelope(env, HMAC_SHARED_SECRET)) return;
  if (env["data"]["name"].as<String>() == "irrigating") {
    peerIrrigating = true;
    lastIrrigatingEventMs = millis();
  }
}

void handlePeerHb(const JsonDocument & /*env*/) { lastPeerHbMs = millis(); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  dht.begin();

  transport.subscribe(Channel::CMD_IN, handleCmd);
  transport.subscribe(Channel::ALL_CMD_IN, handleCmd);
  transport.subscribe(Channel::EVENTS_IO, handleEvent);
  transport.subscribe(Channel::PEER_HB_IN, handlePeerHb);
  transport.begin();

  lastPeerHbMs = millis();  // don't declare LINK_LOST before we've had a chance to hear B at all
}

void loop() {
  transport.loop();
  uint32_t now = millis();

  // --- link watchdog (req. 7) ---
  bool linkNowOk = (now - lastPeerHbMs) < LINK_TIMEOUT_MS;
  if (!linkNowOk && linkToB_ok) wasIsolated = true;  // latched; reported once reconnected
  linkToB_ok = linkNowOk;

  // A node we can't hear from can't be trusted to still be sending "irrigating" -- stop trusting
  // the stale flag rather than pausing the fan forever based on old information.
  if (!linkToB_ok) peerIrrigating = false;
  if (peerIrrigating && (now - lastIrrigatingEventMs > PEER_EVENT_STALE_MS)) peerIrrigating = false;

  // --- sensor + control, on the controllable sample_period_ms (req. 5) ---
  if (now - lastSampleMs >= cfg.sample_period_ms) {
    lastSampleMs = now;
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    bool sensorOk = !(isnan(temp) || isnan(hum));
    bool degraded = !cfg.auto_mode || !sensorOk;

    if (!linkToB_ok) setRgb(true, false, false);        // red: link lost (highest priority)
    else if (degraded) setRgb(true, true, false);       // amber: manual mode or sensor glitch
    else setRgb(false, true, false);                    // green: normal

    if (!sensorOk) {
      publishState();  // still report reality (link/was_isolated) even without a fresh reading
      return;
    }

    NodeAInputs in{temp, peerIrrigating};
    bool fanOn = decideFanOn(cfg, in);
    lastFanOn = fanOn;
    digitalWrite(PIN_FAN, fanOn ? HIGH : LOW);

    JsonDocument telemetry;
    makeEnvelope(telemetry, "A", nextSeq(), now / 1000, MsgType::TELEMETRY);
    telemetry["data"]["humidity"] = hum;
    telemetry["data"]["temp_c"] = temp;
    signEnvelope(telemetry, HMAC_SHARED_SECRET);
    transport.publish(Channel::TELEMETRY_OUT, telemetry);

    // Republished every cycle while true, not edge-triggered -- see docs/protocol.md
    // "Republishing, not edge-triggering" for why (lets Node B detect the condition clearing
    // without a second event type).
    if (isClimateAlertActive(cfg, in)) {
      JsonDocument ev;
      makeEnvelope(ev, "A", nextSeq(), now / 1000, MsgType::EVENT);
      ev["data"]["name"] = "climate_alert";
      ev["data"]["temp_c"] = temp;
      signEnvelope(ev, HMAC_SHARED_SECRET);
      transport.publish(Channel::EVENTS_IO, ev);
    }

    publishState();
  }

  // --- heartbeat (req. 7) ---
  if (now - lastHbMs >= HB_INTERVAL_MS) {
    lastHbMs = now;
    JsonDocument hb;
    makeEnvelope(hb, "A", nextSeq(), now / 1000, MsgType::HB);
    signEnvelope(hb, HMAC_SHARED_SECRET);
    transport.publish(Channel::HB_OUT, hb);
  }
}
