#pragma once
// MQTT implementation of Transport, following docs/protocol.md's topic layout exactly.
//
// Why PubSubClient: a small, dependency-light MQTT client that's been the standard choice on
// Arduino/ESP32 for years. Its source is short enough to read end to end, which matters because
// I need to be able to explain exactly what happens on connect/subscribe/publish, not just that
// it works. Trade-off accepted for this prototype: its internal receive buffer defaults to 256
// bytes, too small for a full `state` message once JSON keys and the HMAC signature are added,
// so MQTT_MAX_PACKET_SIZE is raised via a build_flag in each node's platformio.ini.

#include "transport.h"

#include <PubSubClient.h>
#include <WiFiClient.h>

class MqttTransport : public Transport {
 public:
  MqttTransport(const char *nodeId, const char *topicPrefix, const char *wifiSsid,
                const char *wifiPassword, const char *brokerHost, uint16_t brokerPort,
                const char *hmacSecret);

  bool begin() override;
  void loop() override;
  bool connected() override;
  bool publish(Channel ch, const JsonDocument &env) override;
  void subscribe(Channel ch, MessageHandler handler) override;

 private:
  String topicFor(Channel ch) const;
  String lower(const String &s) const;
  void ensureWifiConnected();
  void ensureMqttConnected();
  void handleIncoming(char *topic, uint8_t *payload, unsigned int length);

  // PubSubClient's callback is a plain C function pointer, so it can't be a member function
  // directly. This static instance pointer + trampoline is the usual workaround for that.
  static MqttTransport *s_instance;
  static void mqttCallbackTrampoline(char *topic, uint8_t *payload, unsigned int length);

  String nodeId_;      // "A" or "B" -- used as the envelope "src" and to derive this node's topics
  String peerId_;      // the other node's id, derived from nodeId_ in the constructor
  String prefix_;
  String ssid_;
  String password_;
  String brokerHost_;
  uint16_t brokerPort_;
  String secret_;
  WiFiClient wifiClient_;
  PubSubClient mqtt_;

  static constexpr int kNumChannels = 8;  // must match the number of Channel enumerators
  MessageHandler handlers_[kNumChannels];
};
