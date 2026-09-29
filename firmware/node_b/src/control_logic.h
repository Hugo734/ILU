#pragma once
// Pure control logic for Node B (Soil). Same rationale as node_a/src/control_logic.h: no
// Arduino/hardware calls here, so it's testable with a plain g++ independent of the ESP32
// toolchain. Timer state (is the pump mid-cycle right now) lives in main.cpp, not here, because
// it's stateful across calls and this file stays pure/stateless -- decidePumpOn() is told the
// timer's current state rather than tracking it itself.

struct NodeBConfig {
  float moisture_setpoint = 40.0f;         // controllable variable (req. 5): irrigate below this %
  unsigned long irrigation_seconds = 10;   // controllable variable (req. 5)
  unsigned long sample_period_ms = 5000;   // controllable variable (req. 5)
  bool auto_mode = true;                   // controllable variable (req. 5)
  bool manual_actuator_on = false;         // controllable variable (req. 5): used only in MANUAL
};

struct NodeBInputs {
  float moisture_pct;
  bool peer_climate_alert;  // true while Node A reports "climate_alert" (req. 3, A->B direction)
};

// Effective irrigation duration right now: halved while the peer's climate_alert is active,
// because hot conditions dry soil faster (docs/decisions.md's scenario rationale). Halved rather
// than a separate tunable so the demo shows the exact same irrigation_seconds value get cut in
// half live, instead of a second unexplained magic number.
unsigned long effectiveIrrigationSeconds(const NodeBConfig &cfg, const NodeBInputs &in);

// Decides whether the pump should be ON right now.
// AUTO: ON if a cycle is already running (timerRunning), or a new cycle should start because
//       moisture_pct has dropped below moisture_setpoint.
// MANUAL: follows manual_actuator_on directly.
bool decidePumpOn(const NodeBConfig &cfg, const NodeBInputs &in, bool timerRunning);
