#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr uint8_t PROTOCOL_VERSION = 1;

enum class MsgType : uint8_t { Heartbeat = 1, Event = 2 };
// AccessOpen/AccessClosed are repeated every second by node_b, so a lost
// frame is corrected by the next one instead of leaving node_a disarmed.
enum class EventId : uint8_t {
    MotionStarted = 1,
    MotionStopped = 2,
    AccessOpen    = 3,
    AccessClosed  = 4,
};

// Packed so both binaries agree on field offsets; padding would silently
// shift every field after the first. Fields are raw uint8_t rather than the
// enums so a corrupt byte can be inspected instead of being undefined.
struct __attribute__((packed)) Frame {
    uint8_t  version;     // PROTOCOL_VERSION
    uint8_t  type;        // MsgType
    uint8_t  src;         // 'A' or 'B'
    uint8_t  event;       // EventId, 0 for a heartbeat
    uint16_t seq;         // detect losses and drop duplicates
    uint32_t uptime_ms;
};
static_assert(sizeof(Frame) == 10, "Frame is a wire format; its size must not drift");

// Copies into `out` only if the bytes are a frame this firmware understands.
// Stateless on purpose: duplicate/loss detection by seq is receiver state.
bool protocol_parse(const uint8_t *data, size_t len, Frame *out);
