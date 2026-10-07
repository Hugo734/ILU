// Host-only test: if this stops compiling, ESP-IDF has leaked into shared/core.
#include "protocol.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

static Frame valid_event()
{
    Frame f{};
    f.version   = PROTOCOL_VERSION;
    f.type      = static_cast<uint8_t>(MsgType::Event);
    f.src       = 'A';
    f.event     = static_cast<uint8_t>(EventId::MotionStarted);
    f.seq       = 42;
    f.uptime_ms = 123456;
    return f;
}

// Each case starts from a valid frame and breaks exactly one rule, so a pass
// proves that rule alone causes the rejection.
static bool parses(const Frame &f)
{
    Frame out;
    return protocol_parse(reinterpret_cast<const uint8_t *>(&f), sizeof f, &out);
}

int main()
{
    {
        Frame in = valid_event(), out{};
        check(protocol_parse(reinterpret_cast<const uint8_t *>(&in), sizeof in, &out),
              "valid event is accepted");
        check(memcmp(&in, &out, sizeof in) == 0, "accepted frame is copied intact");
    }
    {
        Frame f = valid_event();
        f.type = static_cast<uint8_t>(MsgType::Heartbeat); f.event = 0; f.src = 'B';
        check(parses(f), "valid heartbeat from B is accepted");
    }

    // The receive buffer is not aligned; parse from an odd offset.
    {
        uint8_t buf[sizeof(Frame) + 1];
        Frame in = valid_event(), out{};
        memcpy(buf + 1, &in, sizeof in);
        check(protocol_parse(buf + 1, sizeof in, &out) && out.uptime_ms == 123456,
              "unaligned buffer is parsed correctly");
    }

    Frame f, out;
    f = valid_event();
    check(!protocol_parse(reinterpret_cast<const uint8_t *>(&f), sizeof f - 1, &out), "short frame rejected");
    uint8_t longer[sizeof(Frame) + 1] = {};
    memcpy(longer, &f, sizeof f);
    check(!protocol_parse(longer, sizeof longer, &out), "long frame rejected");
    check(!protocol_parse(nullptr, sizeof f, &out), "null data rejected");

    f = valid_event(); f.version = PROTOCOL_VERSION + 1; check(!parses(f), "wrong version rejected");
    f = valid_event(); f.type = 0;   check(!parses(f), "type 0 rejected");
    f = valid_event(); f.type = 3;   check(!parses(f), "unknown type rejected");
    f = valid_event(); f.src = 'C';  check(!parses(f), "unknown src rejected");
    f = valid_event(); f.event = 0;  check(!parses(f), "event frame with no event rejected");
    f = valid_event(); f.event = 3;  check(!parses(f), "unknown event rejected");
    f = valid_event(); f.type = static_cast<uint8_t>(MsgType::Heartbeat);
    check(!parses(f), "heartbeat carrying an event rejected");

    printf(failures ? "%d failure(s)\n" : "all protocol tests passed\n", failures);
    return failures ? 1 : 0;
}
