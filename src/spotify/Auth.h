#pragma once
// Spotube-style Spotify web-player token acquisition.
// Given the user's `sp_dc` cookie (captured by the WebView login), mint a short-lived Bearer access token
// that authorizes the internal Pathfinder GraphQL API. The request is signed with a TOTP whose secret
// ("nuance") is fetched at runtime (Spotify rotates it) with a baked-in fallback. See Totp.h.
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstdint>
#include <string>

namespace st::spotify {

using CT = YoutubeExplode::CancellationToken;

// A realistic desktop Chrome UA, reused for the token request and every GraphQL call.
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/137.0.0.0 Safari/537.36";

struct TokenBundle {
    std::string accessToken;
    int64_t expiresAtMs = 0; // accessTokenExpirationTimestampMs (unix ms)
    bool anonymous = false;

    bool valid() const { return !accessToken.empty(); }
    // Treat as expired a minute early to avoid racing the boundary.
    bool expired(int64_t nowMs) const { return expiresAtMs != 0 && nowMs >= expiresAtMs - 60'000; }
};

// The rotating TOTP secret + its version, from the maintainers' gist (with a compiled-in fallback).
struct Nuance {
    int version = 0;
    std::string secret; // Base32
};

// Blocking; call from a worker thread. Throws std::runtime_error on failure (bad cookie, network, etc.).
// Runs the full flow: nuance -> server-time -> TOTP -> GET /api/token. Retries once with a cache-busted
// nuance if the first attempt is rejected (covers a freshly rotated secret).
TokenBundle fetchAccessToken(const std::string& spDc, const CT& ct = {});

// Exposed for tests / diagnostics.
Nuance fetchNuance(bool bustCache, const CT& ct);
int64_t fetchServerTimeSeconds(const CT& ct);

} // namespace st::spotify
