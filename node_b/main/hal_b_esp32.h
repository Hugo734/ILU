#pragma once

#include <stdint.h>

#include "hal.h"
#include "driver/gpio.h"

class HalBEsp32 : public IHalB {
public:
    HalBEsp32(gpio_num_t trig, gpio_num_t echo, gpio_num_t btn,
              gpio_num_t servo, gpio_num_t r, gpio_num_t g, gpio_num_t b);

    void init();

    uint32_t nowMs() const override;
    uint16_t distanceCm() override;
    bool     buttonPressed() override;
    void     setLed(Led c) override;
    void     setServoAngle(uint8_t deg) override;

private:
    gpio_num_t trig_, echo_, btn_, servo_, r_, g_, b_;
};