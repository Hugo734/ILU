#include "protocol.h"

#include <string.h>

static bool event_matches_type(uint8_t type, uint8_t event)
{
    switch (static_cast<MsgType>(type)) {
    case MsgType::Heartbeat:
        return event == 0;
    case MsgType::Event:
        return event == static_cast<uint8_t>(EventId::MotionStarted) ||
               event == static_cast<uint8_t>(EventId::MotionStopped) ||
               event == static_cast<uint8_t>(EventId::AccessOpen)    ||
               event == static_cast<uint8_t>(EventId::AccessClosed);
    }
    return false;  // unknown type
}

bool protocol_parse(const uint8_t *data, size_t len, Frame *out)
{
    // A short buffer would be read past its end; a long one is a different
    // wire format that happens to share our first bytes.
    if (data == nullptr || out == nullptr || len != sizeof(Frame)) return false;

    // memcpy instead of casting: the radio buffer has no alignment guarantee,
    // and reading uptime_ms through a cast pointer would be undefined.
    Frame f;
    memcpy(&f, data, sizeof f);

    if (f.version != PROTOCOL_VERSION) return false;
    if (f.src != 'A' && f.src != 'B') return false;
    if (!event_matches_type(f.type, f.event)) return false;

    *out = f;
    return true;
}
