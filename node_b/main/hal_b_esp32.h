#pragma once
#include <stdint.h>

#include "hal.h"
#include "driver/gpio.h"

class HalBEsp32 : public IHalB {
public:
    HalBEsp32(gpio_num_t trig, gpio_num_t echo, gpio_num_t sw, gpio_num_t r, gpio_num_t g, gpio_num_t b);

    void init();

    uint32_t nowMs() const override;
    uint16_t distanceCm() override;
    bool     accessSwitchOn() override;
    void     setLed(Led c) override;

private:
    gpio_num_t trig_, echo_, sw_, r_, g_, b_;
};