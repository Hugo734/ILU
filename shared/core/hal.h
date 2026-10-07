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
};

// Nodo A - Cuarto
class IHalA {
public:
    virtual ~IHalA() = default;
    virtual uint32_t nowMs() const = 0;
    virtual bool    motionDetected() = 0;
    virtual void    setBuzzer(bool on) = 0;
    virtual bool    alarmActive() = 0;  // read back from the pin, not from the flag
    virtual void    setLed(Led c) = 0;
};