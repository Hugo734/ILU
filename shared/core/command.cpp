#include "command.h"

#include <stdio.h>
#include <string.h>

// A hand-written parser for one flat object instead of a JSON library: the
// format is fixed and small, and every rule below can be read and tested.

namespace {

struct Cursor {
    const char *p;
    const char *end;
};

void skip_ws(Cursor &c)
{
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) c.p++;
}

bool eat(Cursor &c, char ch)
{
    skip_ws(c);
    if (c.p < c.end && *c.p == ch) {
        c.p++;
        return true;
    }
    return false;
}

// Copies a string into dst, which must have room for the terminator.
// Escapes are refused rather than decoded: no valid id or variable name needs
// one, and refusing them guarantees the text can be echoed back in an ack
// without escaping it again.
const char *read_string(Cursor &c, char *dst, size_t size)
{
    skip_ws(c);
    if (c.p >= c.end || *c.p != '"') return "expected a string";
    c.p++;
    size_t n = 0;
    while (c.p < c.end && *c.p != '"') {
        unsigned char ch = static_cast<unsigned char>(*c.p);
        if (ch == '\\') { dst[0] = '\0'; return "escapes not supported"; }
        if (ch < 0x20)  { dst[0] = '\0'; return "control character in string"; }
        if (n + 1 >= size) { dst[0] = '\0'; return "string too long"; }
        dst[n++] = static_cast<char>(ch);
        c.p++;
    }
    if (c.p >= c.end) { dst[0] = '\0'; return "unterminated string"; }
    c.p++;  // closing quote
    dst[n] = '\0';
    return nullptr;
}

const char *read_int(Cursor &c, int32_t *out)
{
    skip_ws(c);
    bool neg = false;
    if (c.p < c.end && *c.p == '-') {
        neg = true;
        c.p++;
    }
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') return "value must be an integer";
    int64_t v = 0;
    int digits = 0;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        // Eleven digits always exceed int32; stopping there also keeps v far
        // from overflowing int64.
        if (++digits > 10) return "value out of int32 range";
        v = v * 10 + (*c.p - '0');
        c.p++;
    }
    // 1.5 or 1e3 means the sender meant something that is not an integer;
    // truncating it would apply a value nobody asked for.
    if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) return "value must be an integer";
    if (neg) v = -v;
    if (v < INT32_MIN || v > INT32_MAX) return "value out of int32 range";
    *out = static_cast<int32_t>(v);
    return nullptr;
}

}  // namespace

const char *command_parse(const char *json, size_t len, Command *out)
{
    if (out == nullptr) return "no output";
    memset(out, 0, sizeof *out);
    if (json == nullptr) return "no data";

    Cursor c{json, json + len};
    if (!eat(c, '{')) return "expected an object";

    bool have_id = false, have_var = false, have_value = false;
    if (!eat(c, '}')) {
        do {
            char key[8];
            if (read_string(c, key, sizeof key) != nullptr) return "unknown field";
            if (!eat(c, ':')) return "expected ':'";

            const char *err = nullptr;
            if (strcmp(key, "id") == 0) {
                if (have_id) return "duplicate field";
                err = read_string(c, out->id, sizeof out->id);
                have_id = true;
            } else if (strcmp(key, "var") == 0) {
                if (have_var) return "duplicate field";
                err = read_string(c, out->var, sizeof out->var);
                have_var = true;
            } else if (strcmp(key, "value") == 0) {
                if (have_value) return "duplicate field";
                err = read_int(c, &out->value);
                have_value = true;
            } else {
                // Rejecting unknown fields turns a typo such as "valeu" into a
                // visible rejection instead of a silent "missing value".
                return "unknown field";
            }
            if (err != nullptr) return err;
        } while (eat(c, ','));
        if (!eat(c, '}')) return "expected '}'";
    }

    skip_ws(c);
    if (c.p != c.end) return "trailing data";
    if (!have_id || out->id[0] == '\0') return "missing id";
    if (!have_var || out->var[0] == '\0') return "missing var";
    if (!have_value) return "missing value";
    return nullptr;
}

int command_find_var(const VarSpec *vars, size_t n, const char *name)
{
    if (vars == nullptr || name == nullptr) return -1;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(vars[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

ApplyResult command_apply(const VarSpec *vars, int32_t *values, size_t n, const Command &c)
{
    int i = command_find_var(vars, n, c.var);
    if (i < 0) return {AckStage::Rejected, -1, "unknown variable"};
    if (c.value < vars[i].min || c.value > vars[i].max) return {AckStage::Rejected, i, "out of range"};
    values[i] = c.value;
    return {AckStage::Applied, i, nullptr};
}

bool ack_format(char *buf, size_t size, char node, const char *id, const char *var,
                AckStage stage, const char *reason, bool has_value, int32_t value)
{
    static const char *const STAGE[] = {"received", "applied", "rejected"};
    if (buf == nullptr || size == 0) return false;

    int n = snprintf(buf, size, "{\"node\":\"%c\",\"id\":\"%s\",\"var\":\"%s\",\"stage\":\"%s\"",
                     node, id ? id : "", var ? var : "", STAGE[static_cast<int>(stage)]);
    if (n < 0 || static_cast<size_t>(n) >= size) return false;
    size_t used = static_cast<size_t>(n);

    if (reason != nullptr) {
        n = snprintf(buf + used, size - used, ",\"reason\":\"%s\"", reason);
        if (n < 0 || static_cast<size_t>(n) >= size - used) return false;
        used += static_cast<size_t>(n);
    }
    if (has_value) {
        n = snprintf(buf + used, size - used, ",\"value\":%ld", static_cast<long>(value));
        if (n < 0 || static_cast<size_t>(n) >= size - used) return false;
        used += static_cast<size_t>(n);
    }
    n = snprintf(buf + used, size - used, "}");
    return n == 1 && used + 1 < size;
}
