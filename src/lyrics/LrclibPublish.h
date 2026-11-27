#pragma once
// Publishing lyrics to LRCLIB (https://lrclib.net/docs, "Publish a new lyrics"): the synchronizer's "LRCLIB'e gönder",
// only ever on the user's explicit request (the lyrics become public there).
//
// POST /api/publish needs a one-time Publish Token "<prefix>:<nonce>": POST /api/request-challenge gives a prefix and a
// target (64 hex digits, valid 5 minutes); the nonce is a decimal number for which SHA-256(prefix + nonce), read as a
// big-endian number, is at most the target (LRCGET's solver: src-tauri/src/lrclib/challenge_solver.rs). With the
// target LRCLIB hands out (000000FF…) that is about 2^24 hashes, so solve() runs on several threads.
//
// Blocking: call from a worker thread. Standalone (core only): tests compile it directly (lyrictools_test), with a fake
// transport for the requests.
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace st::lyrics::lrclib {

using Hash = std::array<uint8_t, 32>;

// SHA-256 (FIPS 180-4), a plain portable implementation (the solver hashes millions of short messages).
Hash sha256(std::string_view data);
std::string toHex(const Hash& h);   // upper case, like LRCLIB's target

// The target's 64 hex digits (either case). False when it isn't that.
bool parseTarget(std::string_view hex, Hash& out);
// hash <= target, both read as big-endian numbers.
bool meetsTarget(const Hash& hash, const Hash& target);

struct Challenge {
    std::string prefix;
    std::string target;   // 64 hex digits
};
// /api/request-challenge's JSON {"prefix": "...", "target": "..."}. nullopt when either is missing or malformed.
std::optional<Challenge> parseChallenge(const std::string& body);

// A nonce for the challenge: n with sha256(prefix + decimal(n)) <= target. `threads` 0 = the machine's cores (at most
// 8); with 1 thread the smallest such n is found (what LRCGET finds). nullopt when cancelled, when `maxTries` (0 = no
// limit) numbers were tried without one, or when the target is malformed.
std::optional<uint64_t> solve(const Challenge& c, const YoutubeExplode::CancellationToken& ct = {}, unsigned threads = 0,
                              uint64_t maxTries = 0);
// "<prefix>:<nonce>".
std::string publishToken(const Challenge& c, uint64_t nonce);

// What goes public: the track's signature and its lyrics (LRC text and the same lines as plain text).
struct Submission {
    std::string title, artist, album;
    int durationSec = 0;
    std::string plainLyrics;
    std::string syncedLyrics;
};
// The publish request's JSON body (camelCase keys as the API documents them; the duration in whole seconds).
std::string publishBody(const Submission& s);
// The signature LRCLIB requires (title, artist and a duration of 1..3600 s) and some lyrics.
bool publishable(const Submission& s);

using Transport = std::function<YoutubeExplode::Http::HttpResponse(const YoutubeExplode::Http::HttpRequest&,
                                                                    const YoutubeExplode::CancellationToken&)>;

struct Outcome {
    enum class Kind {
        Published,      // 201 Created
        BadToken,       // 400 IncorrectPublishTokenError (expired / used): a new challenge would be needed
        Rejected,       // another 4xx: the request itself was refused (message from the server)
        Failed,         // network error, 5xx, 429, or no challenge
        Cancelled,
    };
    Kind kind = Kind::Failed;
    int status = 0;
    std::string message;   // the server's "message" when it sent one
};

// The whole flow: request a challenge, solve it, POST the lyrics with the token. `transport` unset = the app's shared
// HTTP client. `onSolving` (optional) is called once the challenge arrived, before the solving starts.
Outcome publish(const Submission& s, const YoutubeExplode::CancellationToken& ct = {}, const Transport& transport = {},
                const std::function<void()>& onSolving = {});

inline constexpr const char* kBaseUrl = "https://lrclib.net";
// Where publish() sends its requests: kBaseUrl, or SHADETUBE_LRCLIB_URL (dev: a local mock server, so the whole flow can
// be run from the app without publishing anything).
std::string baseUrl();

} // namespace st::lyrics::lrclib
