#pragma once
// Envelope helpers built on ArduinoJson, matching docs/protocol.md exactly. Read that file
// first -- this is just the implementation of the schema defined there.
//
// Why ArduinoJson: the de-facto standard JSON library for Arduino/ESP32, actively maintained,
// and it handles the fiddly parts (buffer growth, string escaping) that are easy to get subtly
// wrong writing JSON by hand with String concatenation. Version 7's JsonDocument has no
// compile-time size template (unlike v6's StaticJsonDocument<N>), which matters here because
// envelope size varies a lot by message type (a bare heartbeat vs. a full state dump) -- v7
// grows its internal buffer as needed instead of requiring one fixed capacity guess per type.

#include <Arduino.h>
#include <ArduinoJson.h>

constexpr int PROTOCOL_VERSION = 1;

enum class MsgType : uint8_t { TELEMETRY, STATE, CMD, ACK, HB, EVENT, UNKNOWN };

const char *msgTypeToStr(MsgType t);
MsgType msgTypeFromStr(const String &s);

// Resets `env` and fills in the envelope header fields plus an empty "data" object. Caller then
// does env["data"]["key"] = value; for each payload field (see docs/protocol.md's per-type
// tables), then calls signEnvelope() last, once "data" is fully populated.
void makeEnvelope(JsonDocument &env, const char *src, uint32_t seq, uint32_t ts, MsgType type,
                   const char *cmdId = "");

// Computes "sig" over env's current contents (docs/protocol.md's algorithm) and writes it into
// env["sig"]. Must be called after "data" is fully populated -- anything added afterward isn't
// covered by the signature.
void signEnvelope(JsonDocument &env, const String &secret);

// Recomputes the expected "sig" and compares it (constant-time) to env["sig"]. False on any
// mismatch or on a missing "sig" field.
bool verifyEnvelope(const JsonDocument &env, const String &secret);

// Parses `json` into `out`. Returns false on malformed JSON. Deliberately does NOT check the
// signature -- callers must call verifyEnvelope() explicitly, so a message can never be acted on
// by accidentally skipping validation.
bool parseEnvelope(const String &json, JsonDocument &out);

// Builds the exact string that gets HMAC'd: "v|src|seq|ts|type|cmd_id|<sorted-key data JSON>".
// Not static so it stays independently testable and so the schema in docs/protocol.md points at
// one authoritative implementation instead of a description that could drift from the code.
String buildSigningString(const JsonDocument &env);
