#pragma once

#include <stdint.h>

#include "hal.h"
#include "driver/gpio.h"

// Why the last measurement ended the way it did. Diagnostics only: it lives on
// the concrete class and not on IHalB, so the interface the control logic
// depends on does not grow a debugging surface.
enum class EchoResult {
    Ok,          // a plausible acoustic echo was timed
    ShortPulse,  // the line pulsed, but too briefly to be sound
    NoRise,      // the line never went high: the sensor did not answer
    NoFall,      // the line stayed high: out of range, or stuck
};

// ESP32 implementation of IHalB. This is the only file in node B that knows
// which physical pin each device is on.
class HalBEsp32 : public IHalB {
public:
    HalBEsp32(gpio_num_t trig, gpio_num_t echo);

    void init();

    uint32_t nowMs() const override;
    uint16_t distanceCm() override;

    EchoResult lastResult()  const { return last_result_; }
    uint32_t   lastWidthUs() const { return last_width_us_; }

private:
    gpio_num_t trig_;
    gpio_num_t echo_;
    EchoResult last_result_   = EchoResult::NoRise;
    uint32_t   last_width_us_ = 0;
};