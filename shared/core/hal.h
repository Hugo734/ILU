#pragma once

#include <stdint.h>

// Hardware interface for node B.
//
// No ESP-IDF headers here on purpose: this file has to compile on the laptop
// too. That is what lets the control logic be unit-tested against a mock,
// without a board attached.
class IHalB {
public:
    virtual ~IHalB() = default;

    // Milliseconds since boot.
    virtual uint32_t nowMs() const = 0;

    // Measured distance in centimetres.
    // Returns 0 when no echo arrived within the timeout: out of range, sensor
    // disconnected, or a surface that scatters the ultrasonic burst.
    virtual uint16_t distanceCm() = 0;
};