#include "hmac_auth.h"
#include <mbedtls/md.h>

String hmacSign(const String &toSign, const String &secret) {
  const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t fullHash[32];  // SHA-256 output is 32 bytes; only the first 8 are used after hex-encoding

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mdInfo, 1 /* 1 = HMAC mode, not plain hash */);
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const uint8_t *>(secret.c_str()), secret.length());
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t *>(toSign.c_str()), toSign.length());
  mbedtls_md_hmac_finish(&ctx, fullHash);
  mbedtls_md_free(&ctx);

  static const char hexDigits[] = "0123456789abcdef";
  String hex;
  hex.reserve(16);
  for (int i = 0; i < 8; i++) {  // 8 bytes -> 16 hex chars, per docs/protocol.md truncation
    hex += hexDigits[(fullHash[i] >> 4) & 0x0F];
    hex += hexDigits[fullHash[i] & 0x0F];
  }
  return hex;
}

bool constantTimeEquals(const String &a, const String &b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) {
    diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
  }
  return diff == 0;
}
