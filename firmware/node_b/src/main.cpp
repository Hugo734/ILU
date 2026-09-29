// Node B -- Soil. Simulated sensor: potentiometer standing in for a capacitive soil probe until
// it arrives (docs/architecture.md). Actuator: pump/valve relay, plus an RGB status LED
// (green=normal, amber=degraded, red=link lost).
//
// Same structure as node_a/src/main.cpp: sensor read -> control logic (control_logic.h, pure
// functions, unit-tested natively in test/) -> actuator drive -> telemetry/state publish ->
// command handling -> heartbeat + link watchdog.
//
// NOT YET BUILT OR RUN -- no PlatformIO/ESP32 toolchain in this environment yet. See
// sim/README.md and the top-level README's progress checklist for what has actually been
// verified vs. what is written-but-unverified.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "control_logic.h"
#include "protocol.h"
#include "transport_mqtt.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy firmware/node_b/include/secrets.h.example to secrets.h and fill in real values."
#endif

// ---- Pin assignments -- placeholders until real hardware is wired Wednesday. ----
constexpr int PIN_POT = 34;   // ADC1 input -- soil moisture stand-in (docs/decisions.md)
constexpr int PIN_PUMP = 26;
constexpr int PIN_LED_R = 25;
constexpr int PIN_LED_G = 27;
constexpr int PIN_LED_B = 14;

constexpr uint32_t HB_INTERVAL_MS = 2000;        // docs/decisions.md "Heartbeat timing"
constexpr uint32_t LINK_TIMEOUT_MS = 6000;       // 3 missed heartbeats
constexpr uint32_t PEER_EVENT_STALE_MS = 10000;  // docs/protocol.md "Republishing, not edge-triggering"

MqttTransport transport("B", TOPIC_PREFIX, WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER_HOST,
                         MQTT_BROKER_PORT, HMAC_SHARED_SECRET);

NodeBConfig cfg;  // current effective values of the 5 controllable variables (req. 5)

bool peerClimateAlert = false;
uint32_t lastClimateAlertMs = 0;

bool linkToA_ok = true;
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
bool lastPumpOn = false;
bool pumpTimerRunning = false;
uint32_t pumpTimerEndsMs = 0;

void setRgb(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

// ESP32's ADC1 is 12-bit (0..4095). Wiring choice: potentiometer fully clockwise reads as driest
// (raw high -> 0%), documented here since it's arbitrary and easy to get backwards on the bench.
float readMoisturePct() {
  int raw = analogRead(PIN_POT);
  return 100.0f - (raw / 4095.0f) * 100.0f;
}

void publishState() {
  JsonDocument env;
  makeEnvelope(env, "B", nextSeq(), millis() / 1000, MsgType::STATE);
  JsonObject data = env["data"];
  data["actuator_state"] = lastPumpOn ? "ON" : "OFF";
  data["irrigation_seconds"] = cfg.irrigation_seconds;
  data["link_to_a"] = linkToA_ok ? "OK" : "LOST";
  data["mode"] = cfg.auto_mode ? "AUTO" : "MANUAL";
  data["moisture_setpoint"] = cfg.moisture_setpoint;
  data["sample_period_ms"] = cfg.sample_period_ms;
  data["shortened_by_a_alert"] = peerClimateAlert;
  data["was_isolated"] = wasIsolated;
  signEnvelope(env, HMAC_SHARED_SECRET);
  transport.publish(Channel::STATE_OUT, env);
}

void publishAck(const String &cmdId, const char *result, const char *reason,
                JsonVariantConst value) {
  JsonDocument env;
  makeEnvelope(env, "B", nextSeq(), millis() / 1000, MsgType::ACK, cmdId.c_str());
  env["data"]["reason"] = reason;
  env["data"]["result"] = result;
  env["data"]["value"] = value;
  signEnvelope(env, HMAC_SHARED_SECRET);
  transport.publish(Channel::ACK_OUT, env);
}

void handleCmd(const JsonDocument &env) {
  if (!verifyEnvelope(env, HMAC_SHARED_SECRET)) return;
  if (!acceptSeq(lastSeqPlatform, env)) return;  // stale/replayed command -- dropped
  String cmdId = env["cmd_id"].as<String>();
  String set = env["data"]["set"].as<String>();
  JsonVariantConst value = env["data"]["value"];

  if (set == "moisture_setpoint") {
    float v = value.as<float>();
    if (v < 0 || v > 100) {
      publishAck(cmdId, "rejected", "out of range", value);
      return;
    }
    cfg.moisture_setpoint = v;
  } else if (set == "irrigation_seconds") {
    unsigned long v = value.as<unsigned long>();
    if (v < 1) v = 1;
    cfg.irrigation_seconds = v;
  } else if (set == "sample_period_ms") {
    unsigned long v = value.as<unsigned long>();
    if (v < 500) v = 500;
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
  if (set == "moisture_setpoint") actual.set(cfg.moisture_setpoint);
  else if (set == "irrigation_seconds") actual.set(cfg.irrigation_seconds);
  else if (set == "sample_period_ms") actual.set(cfg.sample_period_ms);
  else if (set == "mode") actual.set(cfg.auto_mode ? "AUTO" : "MANUAL");
  else actual.set(cfg.manual_actuator_on ? "ON" : "OFF");
  publishAck(cmdId, "applied", "", actual.as<JsonVariantConst>());
  publishState();  // req. 6: reality comes from here, not from echoing the command
}

void handleEvent(const JsonDocument &env) {
  if (!verifyEnvelope(env, HMAC_SHARED_SECRET)) return;
  if (env["data"]["name"].as<String>() == "climate_alert") {
    peerClimateAlert = true;
    lastClimateAlertMs = millis();
  }
}

void handlePeerHb(const JsonDocument & /*env*/) { lastPeerHbMs = millis(); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  analogReadResolution(12);

  transport.subscribe(Channel::CMD_IN, handleCmd);
  transport.subscribe(Channel::ALL_CMD_IN, handleCmd);
  transport.subscribe(Channel::EVENTS_IO, handleEvent);
  transport.subscribe(Channel::PEER_HB_IN, handlePeerHb);
  transport.begin();

  lastPeerHbMs = millis();
}

void loop() {
  transport.loop();
  uint32_t now = millis();

  // --- link watchdog (req. 7) ---
  bool linkNowOk = (now - lastPeerHbMs) < LINK_TIMEOUT_MS;
  if (!linkNowOk && linkToA_ok) wasIsolated = true;
  linkToA_ok = linkNowOk;

  if (!linkToA_ok) peerClimateAlert = false;  // stop trusting stale info from an unreachable peer
  if (peerClimateAlert && (now - lastClimateAlertMs > PEER_EVENT_STALE_MS)) peerClimateAlert = false;

  // --- safe state on isolation (req. 7): never leave the pump running blind ---
  if (!linkToA_ok && pumpTimerRunning) {
    pumpTimerRunning = false;
    lastPumpOn = false;
    digitalWrite(PIN_PUMP, LOW);
  }

  // --- sensor + control, on the controllable sample_period_ms (req. 5) ---
  if (now - lastSampleMs >= cfg.sample_period_ms) {
    lastSampleMs = now;
    float moisture = readMoisturePct();
    NodeBInputs in{moisture, peerClimateAlert};

    if (pumpTimerRunning && static_cast<int32_t>(now - pumpTimerEndsMs) >= 0) {
      pumpTimerRunning = false;  // irrigation cycle elapsed naturally
    }

    bool pumpOn = decidePumpOn(cfg, in, pumpTimerRunning);
    if (pumpOn && !pumpTimerRunning && cfg.auto_mode) {
      pumpTimerRunning = true;
      pumpTimerEndsMs = now + effectiveIrrigationSeconds(cfg, in) * 1000UL;
    }
    lastPumpOn = pumpOn;
    digitalWrite(PIN_PUMP, pumpOn ? HIGH : LOW);

    bool degraded = !cfg.auto_mode;
    if (!linkToA_ok) setRgb(true, false, false);
    else if (degraded) setRgb(true, true, false);
    else setRgb(false, true, false);

    JsonDocument telemetry;
    makeEnvelope(telemetry, "B", nextSeq(), now / 1000, MsgType::TELEMETRY);
    telemetry["data"]["moisture_pct"] = moisture;
    signEnvelope(telemetry, HMAC_SHARED_SECRET);
    transport.publish(Channel::TELEMETRY_OUT, telemetry);

    // Republished every cycle while true -- see docs/protocol.md "Republishing, not
    // edge-triggering".
    if (pumpOn) {
      JsonDocument ev;
      makeEnvelope(ev, "B", nextSeq(), now / 1000, MsgType::EVENT);
      ev["data"]["name"] = "irrigating";
      unsigned long remaining = pumpTimerRunning ? (pumpTimerEndsMs - now) / 1000 : 0;
      ev["data"]["seconds_remaining"] = remaining;
      signEnvelope(ev, HMAC_SHARED_SECRET);
      transport.publish(Channel::EVENTS_IO, ev);
    }

    publishState();
  }

  // --- heartbeat (req. 7) ---
  if (now - lastHbMs >= HB_INTERVAL_MS) {
    lastHbMs = now;
    JsonDocument hb;
    makeEnvelope(hb, "B", nextSeq(), now / 1000, MsgType::HB);
    signEnvelope(hb, HMAC_SHARED_SECRET);
    transport.publish(Channel::HB_OUT, hb);
  }
}
