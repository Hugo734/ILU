// Host-only test: if this stops compiling, ESP-IDF has leaked into shared/core.
#include "command.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

static const char *parse(const char *json, Command *out)
{
    return command_parse(json, strlen(json), out);
}

// The reason a broken command is rejected for, or "ok".
static const char *why(const char *json)
{
    Command c;
    const char *err = parse(json, &c);
    return err ? err : "ok";
}

static bool same(const char *a, const char *b) { return strcmp(a, b) == 0; }

static const VarSpec VARS[] = {
    {"near_cm",   5,   100},
    {"repeat_ms", 200, 1000},
};

int main()
{
    // --- Parsing: the valid forms --------------------------------------------
    {
        Command c;
        check(parse("{\"id\":\"c1\",\"var\":\"near_cm\",\"value\":20}", &c) == nullptr, "valid command is accepted");
        check(same(c.id, "c1") && same(c.var, "near_cm") && c.value == 20, "fields are copied");
    }
    {
        // Byte for byte what platform/app.py publishes: json.dumps with
        // separators=(",", ":") and an id of the form HHMMSS-counter.
        Command c;
        check(parse("{\"id\":\"231504-12\",\"var\":\"publish_ms\",\"value\":500}", &c) == nullptr &&
              same(c.id, "231504-12") && same(c.var, "publish_ms") && c.value == 500,
              "the platform's exact payload is accepted");
    }
    {
        Command c;
        check(parse(" {\n \"value\" : -7 , \"var\":\"x\", \"id\" : \"abc\" }\r\n", &c) == nullptr,
              "fields in any order, with whitespace, are accepted");
        check(c.value == -7, "negative value is parsed");
    }
    {
        // An MQTT payload is not NUL-terminated: only `len` bytes may be read.
        const char buf[] = "{\"id\":\"c2\",\"var\":\"v\",\"value\":5}GARBAGE";
        Command c;
        check(command_parse(buf, strlen(buf) - 7, &c) == nullptr && c.value == 5,
              "parser stops at len and ignores the bytes after it");
    }
    {
        Command c;
        check(parse("{\"id\":\"c3\",\"var\":\"v\",\"value\":2147483647}", &c) == nullptr && c.value == 2147483647,
              "INT32_MAX is accepted");
        check(parse("{\"id\":\"c3\",\"var\":\"v\",\"value\":-2147483648}", &c) == nullptr && c.value == -2147483647 - 1,
              "INT32_MIN is accepted");
    }

    // --- Parsing: one rule broken per case -----------------------------------
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":2147483648}"), "value out of int32 range"), "INT32_MAX + 1 rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":99999999999}"), "value out of int32 range"), "11 digits rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1.5}"), "value must be an integer"), "fraction rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1e3}"), "value must be an integer"), "exponent rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":\"5\"}"), "value must be an integer"), "quoted number rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":true}"), "value must be an integer"), "boolean rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\"}"), "missing value"), "missing value rejected");
    check(same(why("{\"id\":\"c\",\"value\":1}"), "missing var"), "missing var rejected");
    check(same(why("{\"var\":\"v\",\"value\":1}"), "missing id"), "missing id rejected");
    check(same(why("{\"id\":\"\",\"var\":\"v\",\"value\":1}"), "missing id"), "empty id rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"\",\"value\":1}"), "missing var"), "empty var rejected");
    check(same(why("{}"), "missing id"), "empty object rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1,\"value\":2}"), "duplicate field"), "duplicate field rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"valeu\":1}"), "unknown field"), "misspelt field rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1,\"extra\":1}"), "unknown field"), "extra field rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\\\"x\",\"value\":1}"), "escapes not supported"), "escape rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"a\tb\",\"value\":1}"), "control character in string"), "control char rejected");
    check(same(why("{\"id\":\"0123456789abcdef\",\"var\":\"v\",\"value\":1}"), "string too long"), "16-char id rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1} x"), "trailing data"), "trailing data rejected");
    check(same(why("{\"id\":\"c\",\"var\":\"v\",\"value\":1"), "expected '}'"), "unclosed object rejected");
    check(same(why("{\"id\":\"c"), "unterminated string"), "unterminated string rejected");
    check(same(why("[1,2]"), "expected an object"), "array rejected");
    check(same(why(""), "expected an object"), "empty payload rejected");
    {
        Command c;
        check(command_parse(nullptr, 10, &c) != nullptr, "null data rejected");
    }
    {
        // The id survives a later error, so the platform can match the rejection.
        Command c;
        check(parse("{\"id\":\"c9\",\"var\":\"v\",\"value\":\"x\"}", &c) != nullptr && same(c.id, "c9"),
              "id is kept when a later field is bad");
    }
    {
        // A 15-character id is the longest that fits.
        Command c;
        check(parse("{\"id\":\"0123456789abcde\",\"var\":\"v\",\"value\":1}", &c) == nullptr, "15-char id accepted");
    }

    // --- Applying ---------------------------------------------------------------
    {
        int32_t values[] = {15, 1000};
        Command c{};
        strcpy(c.id, "c1"); strcpy(c.var, "near_cm"); c.value = 20;
        ApplyResult r = command_apply(VARS, values, 2, c);
        check(r.stage == AckStage::Applied && r.index == 0 && values[0] == 20, "in-range value is applied");

        c.value = 4;
        r = command_apply(VARS, values, 2, c);
        check(r.stage == AckStage::Rejected && r.index == 0 && same(r.reason, "out of range"), "below min rejected");
        check(values[0] == 20, "rejected value leaves the old one in effect");

        c.value = 101;
        r = command_apply(VARS, values, 2, c);
        check(r.stage == AckStage::Rejected && values[0] == 20, "above max rejected");

        c.value = 5;
        check(command_apply(VARS, values, 2, c).stage == AckStage::Applied, "min itself is accepted");
        c.value = 100;
        check(command_apply(VARS, values, 2, c).stage == AckStage::Applied, "max itself is accepted");

        strcpy(c.var, "warmup_s");
        r = command_apply(VARS, values, 2, c);
        check(r.stage == AckStage::Rejected && r.index == -1 && same(r.reason, "unknown variable"),
              "variable this node lacks is rejected");
        check(values[0] == 100 && values[1] == 1000, "unknown variable changes nothing");
    }

    // --- Ack format -------------------------------------------------------------
    {
        char buf[160];
        check(ack_format(buf, sizeof buf, 'b', "c1", "near_cm", AckStage::Applied, nullptr, true, 20) &&
              same(buf, "{\"node\":\"b\",\"id\":\"c1\",\"var\":\"near_cm\",\"stage\":\"applied\",\"value\":20}"),
              "applied ack is formatted");
        check(ack_format(buf, sizeof buf, 'a', "c2", "x", AckStage::Received, nullptr, false, 0) &&
              same(buf, "{\"node\":\"a\",\"id\":\"c2\",\"var\":\"x\",\"stage\":\"received\"}"),
              "received ack carries no value");
        check(ack_format(buf, sizeof buf, 'a', "c3", "warmup_s", AckStage::Rejected, "out of range", true, 60) &&
              same(buf, "{\"node\":\"a\",\"id\":\"c3\",\"var\":\"warmup_s\",\"stage\":\"rejected\",\"reason\":\"out of range\",\"value\":60}"),
              "rejected ack carries the reason and the value still in effect");

        const char *full = "{\"node\":\"b\",\"id\":\"c1\",\"var\":\"near_cm\",\"stage\":\"applied\",\"value\":20}";
        char small[80];
        size_t exact = strlen(full) + 1;
        check(ack_format(small, exact, 'b', "c1", "near_cm", AckStage::Applied, nullptr, true, 20), "ack fits in exactly its size");
        check(!ack_format(small, exact - 1, 'b', "c1", "near_cm", AckStage::Applied, nullptr, true, 20), "one byte short is reported");
    }

    printf(failures ? "%d failure(s)\n" : "all command tests passed\n", failures);
    return failures ? 1 : 0;
}
