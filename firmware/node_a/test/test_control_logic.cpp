// Native unit test for control_logic.cpp -- no Arduino/ESP32 toolchain needed.
// Run with:  g++ -std=c++17 -I../src test_control_logic.cpp ../src/control_logic.cpp -o /tmp/test_a && /tmp/test_a
#include "../src/control_logic.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond, label)                                        \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL: %s\n", label);                            \
      failures++;                                                  \
    } else {                                                       \
      std::printf("ok:   %s\n", label);                            \
    }                                                               \
  } while (0)

int main() {
  NodeAConfig cfg;  // defaults: setpoint 28.0, AUTO

  // AUTO, below setpoint, peer not irrigating -> fan off, no alert
  CHECK(decideFanOn(cfg, {25.0f, false}) == false, "AUTO below setpoint -> fan off");
  CHECK(isClimateAlertActive(cfg, {25.0f, false}) == false, "below setpoint -> no alert");

  // AUTO, above setpoint -> fan on, alert active
  CHECK(decideFanOn(cfg, {30.0f, false}) == true, "AUTO above setpoint -> fan on");
  CHECK(isClimateAlertActive(cfg, {30.0f, false}) == true, "above setpoint -> alert active");

  // AUTO, above setpoint, but peer irrigating -> fan paused regardless (req. 3, B->A)
  CHECK(decideFanOn(cfg, {30.0f, true}) == false, "peer irrigating overrides fan -> off");
  // The alert condition itself is independent of the peer -- only the fan actuator is paused.
  CHECK(isClimateAlertActive(cfg, {30.0f, true}) == true, "alert still active while peer irrigates");

  // MANUAL mode: fan follows manual_actuator_on, ignoring temperature
  cfg.auto_mode = false;
  cfg.manual_actuator_on = true;
  CHECK(decideFanOn(cfg, {10.0f, false}) == true, "MANUAL on, cold -> fan still on");
  cfg.manual_actuator_on = false;
  CHECK(decideFanOn(cfg, {40.0f, false}) == false, "MANUAL off, hot -> fan still off");

  // MANUAL, forced on, but peer irrigating -> still overridden off (safety takes priority)
  cfg.manual_actuator_on = true;
  CHECK(decideFanOn(cfg, {40.0f, true}) == false, "MANUAL on overridden by peer irrigating");

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "SOME FAILED", failures,
              failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
