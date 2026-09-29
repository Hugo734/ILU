#include "protocol.h"
#include "hmac_auth.h"

#include <algorithm>
#include <vector>

const char *msgTypeToStr(MsgType t) {
  switch (t) {
    case MsgType::TELEMETRY: return "telemetry";
    case MsgType::STATE:     return "state";
    case MsgType::CMD:       return "cmd";
    case MsgType::ACK:       return "ack";
    case MsgType::HB:        return "hb";
    case MsgType::EVENT:     return "event";
    default:                 return "unknown";
  }
}

MsgType msgTypeFromStr(const String &s) {
  if (s == "telemetry") return MsgType::TELEMETRY;
  if (s == "state") return MsgType::STATE;
  if (s == "cmd") return MsgType::CMD;
  if (s == "ack") return MsgType::ACK;
  if (s == "hb") return MsgType::HB;
  if (s == "event") return MsgType::EVENT;
  return MsgType::UNKNOWN;
}

void makeEnvelope(JsonDocument &env, const char *src, uint32_t seq, uint32_t ts, MsgType type,
                   const char *cmdId) {
  env.clear();
  env["v"] = PROTOCOL_VERSION;
  env["src"] = src;
  env["seq"] = seq;
  env["ts"] = ts;
  env["type"] = msgTypeToStr(type);
  env["cmd_id"] = cmdId;
  env["data"].to<JsonObject>();  // ensure "data" exists as an (initially empty) object
}

// Serializes a single JSON value compactly -- the same way every time, which is what makes the
// signature reproducible across languages: each data field's value must round-trip to identical
// bytes whether it's this firmware, the Python logger, or the JS dashboard doing the signing.
static String serializeCompact(JsonVariantConst v) {
  String out;
  serializeJson(v, out);
  return out;
}

String buildSigningString(const JsonDocument &env) {
  // Sort data's top-level keys alphabetically in code before building the string to sign, rather
  // than trusting every call site in main.cpp to insert keys in alphabetical order -- see
  // docs/protocol.md "Message validation" for why relying on manual discipline was rejected.
  JsonObjectConst data = env["data"].as<JsonObjectConst>();
  std::vector<String> keys;
  for (JsonPairConst kv : data) keys.push_back(String(kv.key().c_str()));
  std::sort(keys.begin(), keys.end());

  String dataJson = "{";
  for (size_t i = 0; i < keys.size(); i++) {
    if (i > 0) dataJson += ",";
    dataJson += "\"" + keys[i] + "\":" + serializeCompact(data[keys[i].c_str()]);
  }
  dataJson += "}";

  String s;
  s.reserve(64 + dataJson.length());
  s += String(static_cast<int>(env["v"]));
  s += "|";
  s += env["src"].as<String>();
  s += "|";
  s += String(static_cast<uint32_t>(env["seq"]));
  s += "|";
  s += String(static_cast<uint32_t>(env["ts"]));
  s += "|";
  s += env["type"].as<String>();
  s += "|";
  s += env["cmd_id"].as<String>();
  s += "|";
  s += dataJson;
  return s;
}

void signEnvelope(JsonDocument &env, const String &secret) {
  env["sig"] = hmacSign(buildSigningString(env), secret);
}

bool verifyEnvelope(const JsonDocument &env, const String &secret) {
  if (!env["sig"].is<const char *>()) return false;
  String expected = hmacSign(buildSigningString(env), secret);
  return constantTimeEquals(expected, env["sig"].as<String>());
}

bool parseEnvelope(const String &json, JsonDocument &out) {
  DeserializationError err = deserializeJson(out, json);
  return !err;
}
