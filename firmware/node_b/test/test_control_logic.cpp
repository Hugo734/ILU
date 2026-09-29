// Native unit test for control_logic.cpp -- no Arduino/ESP32 toolchain needed.
// Run with:  g++ -std=c++17 -I../src test_control_logic.cpp ../src/control_logic.cpp -o /tmp/test_b && /tmp/test_b
#include "../src/control_logic.h"

#include <cstdio>

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
  NodeBConfig cfg;  // defaults: moisture_setpoint 40.0, irrigation_seconds 10, AUTO

  // AUTO, moisture above setpoint, no timer running -> pump stays off
  CHECK(decidePumpOn(cfg, {60.0f, false}, false) == false, "AUTO wet soil, no timer -> pump off");

  // AUTO, moisture below setpoint, no timer yet -> pump should start
  CHECK(decidePumpOn(cfg, {20.0f, false}, false) == true, "AUTO dry soil -> pump on");

  // Once a timer is running, pump stays on regardless of the instantaneous reading (it completes
  // its irrigation_seconds cycle rather than flickering off mid-cycle if moisture ticks up)
  CHECK(decidePumpOn(cfg, {60.0f, false}, true) == true, "timer running -> pump stays on");

  // Effective irrigation duration: full length normally, halved during a climate_alert (req. 3,
  // A->B direction) -- soil dries faster when it's hot, per docs/decisions.md.
  CHECK(effectiveIrrigationSeconds(cfg, {20.0f, false}) == 10, "no alert -> full duration");
  CHECK(effectiveIrrigationSeconds(cfg, {20.0f, true}) == 5, "climate_alert -> halved duration");

  // MANUAL mode: pump follows manual_actuator_on directly, ignoring moisture and timer state
  cfg.auto_mode = false;
  cfg.manual_actuator_on = true;
  CHECK(decidePumpOn(cfg, {90.0f, false}, false) == true, "MANUAL on, wet soil -> pump still on");
  cfg.manual_actuator_on = false;
  CHECK(decidePumpOn(cfg, {5.0f, false}, true) == false, "MANUAL off, dry soil -> pump still off");

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "SOME FAILED", failures,
              failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
