#pragma once

#include <stdint.h>

enum class Led { Off, Red, Yellow, Green };

// Nodo B - Acceso
class IHalB {
public:
    virtual ~IHalB() = default;
    virtual uint32_t nowMs() const = 0;
    virtual uint16_t distanceCm() = 0;      // 0 = sin eco
    virtual bool     buttonPressed() = 0;
    virtual void     setLed(Led c) = 0;
    virtual void     setServoAngle(uint8_t deg) = 0;   // 0 cerrado, 90 abierto
};

// Nodo A - Cuarto
class IHalA {
public:
    virtual ~IHalA() = default;
    virtual uint32_t nowMs() const = 0;
    virtual bool     motionDetected() = 0;
};