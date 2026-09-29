#pragma once
// HMAC-SHA256 message signing (docs/protocol.md -> "Message validation").
//
// Why mbedtls and not a separate crypto library: mbedtls ships inside the ESP32 Arduino core
// already (it's what the chip's own TLS stack is built on), so using it here adds zero extra
// dependencies -- just a header include -- instead of pulling in a whole separate SHA-256
// implementation for one function.

#include <Arduino.h>

// Returns HMAC-SHA256(secret, toSign) as lowercase hex, truncated to 16 chars (8 bytes),
// exactly as specified in docs/protocol.md's signing algorithm.
String hmacSign(const String &toSign, const String &secret);

// Constant-time string comparison. A naive `a == b` short-circuits on the first mismatching
// character, which leaks (via timing) how many leading characters of a guess were correct --
// this walks the whole string regardless of where the mismatch is.
bool constantTimeEquals(const String &a, const String &b);
