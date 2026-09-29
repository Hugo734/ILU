#include "transport_mqtt.h"
#include "protocol.h"

#include <WiFi.h>

MqttTransport *MqttTransport::s_instance = nullptr;

MqttTransport::MqttTransport(const char *nodeId, const char *topicPrefix, const char *wifiSsid,
                              const char *wifiPassword, const char *brokerHost,
                              uint16_t brokerPort, const char *hmacSecret)
    : nodeId_(nodeId),
      prefix_(topicPrefix),
      ssid_(wifiSsid),
      password_(wifiPassword),
      brokerHost_(brokerHost),
      brokerPort_(brokerPort),
      secret_(hmacSecret),
      mqtt_(wifiClient_) {
  peerId_ = (nodeId_ == "A") ? "B" : "A";
  s_instance = this;  // only one Transport instance exists per node -- see header comment
}

String MqttTransport::lower(const String &s) const {
  String out = s;
  out.toLowerCase();
  return out;
}

String MqttTransport::topicFor(Channel ch) const {
  String base = "gh/" + prefix_ + "/";
  switch (ch) {
    case Channel::TELEMETRY_OUT: return base + lower(nodeId_) + "/telemetry";
    case Channel::STATE_OUT:     return base + lower(nodeId_) + "/state";
    case Channel::CMD_IN:        return base + lower(nodeId_) + "/cmd";
    case Channel::ALL_CMD_IN:    return base + "all/cmd";
    case Channel::ACK_OUT:       return base + lower(nodeId_) + "/ack";
    case Channel::HB_OUT:        return base + lower(nodeId_) + "/hb";
    case Channel::EVENTS_IO:     return base + "events";
    case Channel::PEER_HB_IN:    return base + lower(peerId_) + "/hb";
  }
  return "";  // unreachable -- Channel is a fully-covered enum class, kept for compiler warnings
}

void MqttTransport::mqttCallbackTrampoline(char *topic, uint8_t *payload, unsigned int length) {
  if (s_instance) s_instance->handleIncoming(topic, payload, length);
}

void MqttTransport::handleIncoming(char *topic, uint8_t *payload, unsigned int length) {
  String json;
  json.reserve(length);
  for (unsigned int i = 0; i < length; i++) json += static_cast<char>(payload[i]);

  JsonDocument env;
  if (!parseEnvelope(json, env)) return;  // malformed JSON -- silently dropped

  String topicStr(topic);
  // Match against the (small, fixed) set of topics this node actually subscribes to. Each
  // handler is responsible for calling verifyEnvelope() itself (see main.cpp) -- this layer's
  // job is routing, not validation, so validation always happens at one clear call site.
  if (topicStr == topicFor(Channel::CMD_IN) && handlers_[static_cast<int>(Channel::CMD_IN)]) {
    handlers_[static_cast<int>(Channel::CMD_IN)](env);
  } else if (topicStr == topicFor(Channel::ALL_CMD_IN) &&
             handlers_[static_cast<int>(Channel::ALL_CMD_IN)]) {
    handlers_[static_cast<int>(Channel::ALL_CMD_IN)](env);
  } else if (topicStr == topicFor(Channel::EVENTS_IO) &&
             handlers_[static_cast<int>(Channel::EVENTS_IO)]) {
    handlers_[static_cast<int>(Channel::EVENTS_IO)](env);
  } else if (topicStr == topicFor(Channel::PEER_HB_IN) &&
             handlers_[static_cast<int>(Channel::PEER_HB_IN)]) {
    handlers_[static_cast<int>(Channel::PEER_HB_IN)](env);
  }
}

void MqttTransport::ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(ssid_.c_str(), password_.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);  // blocking on purpose: nothing useful to do before Wi-Fi is up at boot
  }
}

void MqttTransport::ensureMqttConnected() {
  if (mqtt_.connected()) return;
  ensureWifiConnected();
  if (WiFi.status() != WL_CONNECTED) return;

  String clientId = "greenhouse-" + nodeId_;
  String statusTopic = "gh/" + prefix_ + "/" + lower(nodeId_) + "/status";

  // Last Will and Testament: if this node disconnects uncleanly, the BROKER itself publishes
  // "offline" (retained) on our behalf -- this is what lets the dashboard notice a dead node even
  // faster than waiting out the heartbeat timeout, per docs/architecture.md's link-loss design.
  bool ok = mqtt_.connect(clientId.c_str(), nullptr, nullptr, statusTopic.c_str(),
                           1 /* will QoS */, true /* will retain */, "offline");
  if (!ok) return;

  mqtt_.publish(statusTopic.c_str(), "online", true /* retained */);
  mqtt_.subscribe(topicFor(Channel::CMD_IN).c_str(), 1);
  mqtt_.subscribe(topicFor(Channel::ALL_CMD_IN).c_str(), 1);
  mqtt_.subscribe(topicFor(Channel::EVENTS_IO).c_str(), 1);
  mqtt_.subscribe(topicFor(Channel::PEER_HB_IN).c_str(), 1);
}

bool MqttTransport::begin() {
  mqtt_.setServer(brokerHost_.c_str(), brokerPort_);
  mqtt_.setCallback(mqttCallbackTrampoline);
  ensureMqttConnected();
  return mqtt_.connected();
}

void MqttTransport::loop() {
  ensureMqttConnected();  // cheap no-op when already connected; retries reconnection otherwise
  mqtt_.loop();
}

bool MqttTransport::connected() { return mqtt_.connected(); }

bool MqttTransport::publish(Channel ch, const JsonDocument &env) {
  if (!mqtt_.connected()) return false;
  String out;
  serializeJson(env, out);
  bool retained = (ch == Channel::STATE_OUT);  // per docs/protocol.md: state is retained
  return mqtt_.publish(topicFor(ch).c_str(), out.c_str(), retained);
}

void MqttTransport::subscribe(Channel ch, MessageHandler handler) {
  handlers_[static_cast<int>(ch)] = handler;
}
