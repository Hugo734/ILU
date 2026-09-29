#include "control_logic.h"

bool decideFanOn(const NodeAConfig &cfg, const NodeAInputs &in) {
  if (in.peer_irrigating) return false;  // safety/quality override: never blow while wet
  if (!cfg.auto_mode) return cfg.manual_actuator_on;
  return in.temp_c > cfg.temp_setpoint;
}

bool isClimateAlertActive(const NodeAConfig &cfg, const NodeAInputs &in) {
  return in.temp_c > cfg.temp_setpoint;
}
