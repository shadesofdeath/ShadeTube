#pragma once
// RFC 6238 TOTP used to sign Spotify's web-player token request (the "totp"/"totpServer" params).
// The secret is a Base32 string ("nuance") fetched at runtime; see Auth.h. HMAC-SHA1 via Windows CNG.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::spotify {

// Base32 (RFC 4648) decode; ignores padding, whitespace and case. Non-alphabet chars are skipped.
std::vector<uint8_t> base32Decode(std::string_view s);

// Raw HMAC-SHA1(key, data) (20 bytes). Throws std::runtime_error on a CNG failure.
std::vector<uint8_t> hmacSha1(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data);

// TOTP code for the given UNIX time (seconds). Spotify uses digits=6, period=30, SHA1.
// `unixSeconds` must be Spotify's server time (see Auth::serverTime), not the local clock.
std::string totp(std::string_view base32Secret, int64_t unixSeconds, int digits = 6, int period = 30);

} // namespace st::spotify
