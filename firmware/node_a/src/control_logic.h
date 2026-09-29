#pragma once
// Pure control logic for Node A (Climate). No Arduino/hardware calls in this file on purpose --
// it takes plain values in, returns plain values out, so it compiles and can be unit-tested with
// a stock g++ on a dev machine, independent of the ESP32 toolchain. main.cpp is the only file
// that touches GPIO/DHT/MQTT; this file is what main.cpp calls to decide what to do.

struct NodeAConfig {
  float temp_setpoint = 28.0f;            // controllable variable (req. 5)
  unsigned long sample_period_ms = 5000;  // controllable variable (req. 5)
  bool auto_mode = true;                  // controllable variable (req. 5): AUTO vs MANUAL
  bool manual_actuator_on = false;        // controllable variable (req. 5): used only in MANUAL
};

struct NodeAInputs {
  float temp_c;
  bool peer_irrigating;  // true while Node B reports "irrigating" (req. 3, B->A direction)
};

// Decides whether the fan should be ON right now.
// AUTO: ON when temp_c exceeds temp_setpoint, UNLESS the peer is irrigating.
// MANUAL: follows manual_actuator_on directly, still overridden by peer_irrigating -- a
//         deliberately-forced-on fan while soil is being watered would defeat the same
//         "don't blow the water off" rationale (docs/decisions.md) just as much as in AUTO, so
//         the override applies in both modes rather than only in AUTO.
bool decideFanOn(const NodeAConfig &cfg, const NodeAInputs &in);

// True while the current reading is above the setpoint -- this is the condition Node A publishes
// as the "climate_alert" event (req. 3, A->B direction). Publishing is the caller's job (main.cpp
// republishes on every cycle while this is true, per docs/protocol.md); this function only
// answers "is the condition true right now".
bool isClimateAlertActive(const NodeAConfig &cfg, const NodeAInputs &in);
