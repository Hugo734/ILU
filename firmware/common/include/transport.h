#pragma once
// Transport abstraction: control logic in each node's main.cpp talks only to this interface,
// never directly to PubSubClient/MQTT. Why this exists: docs/decisions.md ("Why two
// transports") plans to add an ESP-NOW implementation of this same interface once hardware
// exists, for the node-to-node events/heartbeat path, without touching main.cpp's control logic
// at all. Until that implementation is written, MqttTransport (transport_mqtt.h) is the only one.

#include <ArduinoJson.h>
#include <functional>

using MessageHandler = std::function<void(const JsonDocument &env)>;

// Logical channels the control logic cares about. A concrete transport maps these onto whatever
// the underlying medium actually calls them (MQTT topics today; ESP-NOW has no topics, so that
// future implementation would filter on the envelope's own "type"/"src" fields instead). This
// indirection is what lets the inter-node link move from MQTT to ESP-NOW by swapping one .cpp
// file, per docs/architecture.md.
enum class Channel : uint8_t {
  TELEMETRY_OUT,  // this node's own telemetry, published outward
  STATE_OUT,      // this node's own state, published outward (retained on MQTT)
  CMD_IN,         // commands addressed to this node specifically
  ALL_CMD_IN,     // commands addressed to both nodes
  ACK_OUT,        // this node's acks, published outward
  HB_OUT,         // this node's heartbeat, published outward
  EVENTS_IO,      // inter-node events -- both published and subscribed on this channel
  PEER_HB_IN,     // the other node's heartbeat, subscribed (for link-loss detection)
};

class Transport {
 public:
  virtual ~Transport() = default;

  // Connects Wi-Fi/broker (or a radio, for a future ESP-NOW implementation) and subscribes to
  // everything this node needs to hear. Call once from setup(), after all subscribe() calls.
  virtual bool begin() = 0;

  // Pumps the underlying client -- must be called every loop() iteration or incoming messages
  // are never actually read and handlers never fire.
  virtual void loop() = 0;

  virtual bool connected() = 0;

  // Publishes a fully-built, already-signed envelope on the given logical channel.
  virtual bool publish(Channel ch, const JsonDocument &env) = 0;

  // Registers `handler` to be called whenever a message arrives on `ch`. One handler per channel
  // -- this firmware never needs fan-out, so keeping it 1:1 keeps the interface simple.
  virtual void subscribe(Channel ch, MessageHandler handler) = 0;
};
