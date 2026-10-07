#pragma once
#include <stdint.h>

#include "hal.h"
#include "driver/gpio.h"

class HalAEsp32 : public IHalA {
public:
    HalAEsp32(gpio_num_t pir_out, gpio_num_t buzz, gpio_num_t r, gpio_num_t g, gpio_num_t b);
    void init();

    uint32_t nowMs() const override;
    bool    motionDetected() override;
    void    setBuzzer(bool on) override;
    bool    alarmActive() override;
    void    setLed(Led c) override;

private:
    gpio_num_t pir_out_, buzz_, r_, g_, b_;
};