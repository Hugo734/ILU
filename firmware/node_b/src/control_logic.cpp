#include "control_logic.h"

unsigned long effectiveIrrigationSeconds(const NodeBConfig &cfg, const NodeBInputs &in) {
  return in.peer_climate_alert ? cfg.irrigation_seconds / 2 : cfg.irrigation_seconds;
}

bool decidePumpOn(const NodeBConfig &cfg, const NodeBInputs &in, bool timerRunning) {
  if (!cfg.auto_mode) return cfg.manual_actuator_on;
  if (timerRunning) return true;
  return in.moisture_pct < cfg.moisture_setpoint;
}
