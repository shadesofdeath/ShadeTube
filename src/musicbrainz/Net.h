#pragma once
// Internal transport for the open-catalog client: polite rate limiting per service, retries, and a two-level
// response cache (in-memory LRU of body strings + disk under cacheDir()/mb). Not part of the public API.
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace st::mb::net {

enum class Service { MusicBrainz, ListenBrainz, Wikidata };

namespace ttl {
inline constexpr std::chrono::seconds lookup{7 * 24 * 3600};
inline constexpr std::chrono::seconds search{24 * 3600};
inline constexpr std::chrono::seconds listenBrainz{12 * 3600};
inline constexpr std::chrono::seconds wikidata{30 * 24 * 3600};
} // namespace ttl

struct Request {
    Service service = Service::MusicBrainz;
    std::string url;
    std::string method = "GET";
    std::string body;                  // POST body (JSON)
    std::chrono::seconds ttl = ttl::lookup;
    bool cacheOnly = false;            // never touch the network; nullopt on miss
    bool sendToken = false;            // ListenBrainz: add "Authorization: Token <t>" when a token is configured
    bool store = true;                 // false: don't cache the raw body (caller caches a derived value)
};

// Parsed JSON response. Throws ApiError(status) on non-2xx after retries, transport exceptions as-is,
// OperationCanceledException on cancellation. Returns nullopt only for cacheOnly misses.
std::optional<nlohmann::json> fetch(const Request& request, const YoutubeExplode::CancellationToken& ct);

// Raw cache access for derived values (e.g. the Commons file name extracted from a large Wikidata entity).
std::optional<std::string> cacheGet(const std::string& key, std::chrono::seconds ttl);
void cachePut(const std::string& key, const std::string& value);

void setListenBrainzToken(std::string token);
bool hasListenBrainzToken();

uint64_t fnv1a(std::string_view s);

// Diagnostics for tests.
struct Stats {
    int networkRequests = 0;
    int memoryHits = 0;
    int diskHits = 0;
};
Stats stats();
void clearMemoryCache();

} // namespace st::mb::net
