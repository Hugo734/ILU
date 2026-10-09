#pragma once
#include <stddef.h>
#include <stdint.h>

// A command from the platform, as it arrives on ilu/<node>/cmd or ilu/all/cmd:
//   {"id":"143005-12","var":"near_cm","value":20}
// The node, not the platform, decides whether it is valid: the platform does
// not check ranges, so a rejection is always the node's own answer.
struct Command {
    char    id[16];   // chosen by the platform, echoed in every ack
    char    var[24];
    int32_t value;
};

// Parses one command from a buffer that is not NUL-terminated (an MQTT
// payload). Accepts a flat JSON object with exactly the fields id, var and
// value, in any order. Strings may not contain escapes; value must be an
// integer. Returns nullptr on success, or a short reason the node can send
// back in a rejection. On failure `out->id` still holds the id if it was read
// before the error, so the platform can match the rejection to its command.
const char *command_parse(const char *json, size_t len, Command *out);

// One remotely controllable variable and the range the node accepts.
struct VarSpec {
    const char *name;
    int32_t     min;
    int32_t     max;
};

// Index of `name` in `vars`, or -1.
int command_find_var(const VarSpec *vars, size_t n, const char *name);

enum class AckStage : uint8_t { Received, Applied, Rejected };

struct ApplyResult {
    AckStage    stage;   // Applied or Rejected
    int         index;   // the variable, or -1 if the name is unknown
    const char *reason;  // null when applied
};

// Checks the command against `vars` and, only if it passes, writes the value
// into `values[index]`. A rejected command changes nothing.
ApplyResult command_apply(const VarSpec *vars, int32_t *values, size_t n, const Command &c);

// Formats an acknowledgement:
//   {"node":"b","id":"143005-12","var":"near_cm","stage":"applied","value":20}
// `reason` may be null. `value` is written only when has_value, and is the
// value now in effect, which after a rejection is the old one.
// Returns false if it did not fit in `size`.
bool ack_format(char *buf, size_t size, char node, const char *id, const char *var,
                AckStage stage, const char *reason, bool has_value, int32_t value);
