#pragma once
// Song credits per track (docs/DEVELOPMENT.md, "Song credits"): the chain that finds them - Spotify's own credits for a
// spotify:track: while logged in, then MusicBrainz (the recording itself, or the one found by title / artist / length)
// - and the per-track cache (memory, then cache\credits\<id>.json). Nothing here runs on its own: Now Playing's
// Hakkında tab asks while it is open, the "Krediler" dialog when it opens.
// Standalone (catalog, spotify, musicbrainz, core): tests compile it directly; the UI is app/CreditsUi.
#include "catalog/Credits.h"
#include "catalog/Models.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st::spotify {
class Api;
}

namespace st::app::credits {

using CT = YoutubeExplode::CancellationToken;

// One source's answer: its credits (empty: it knows nobody), or nullopt when it failed (asked again next time).
using Provider = std::function<std::optional<catalog::Credits>(const CT&)>;

struct Request {
    std::string cacheKey;              // the track id
    std::vector<Provider> providers;   // in order: the first that names somebody wins
};

// Worker thread. The cached answer, else the providers in order until one names somebody. That answer is cached; an
// empty one ("nobody known") only when every provider answered (a failed one is asked again). nullopt = not cached
// and no provider answered; no providers at all = empty Credits (nothing cached). Throws only when `ct` is cancelled.
std::optional<catalog::Credits> fetch(const Request& r, const CT& ct = {});
// Any thread: the cached answer (memory, then disk) without asking anybody. nullopt = not cached.
std::optional<catalog::Credits> cached(const std::string& key);

// The real chain for a track. `api`: the logged-in Spotify Api captured on the UI thread (nullptr = logged out).
// MusicBrainz' answer takes the label of the track's own album (a release group MBID) as its source when the
// recording names none.
Request requestFor(spotify::Api* api, const catalog::Track& t);

// Now Playing's short list: up to `max` people, the first two of each group in group order, a person listed once with
// the roles of every group they appear in (and an artist id any of their entries has).
std::vector<catalog::Credit> summary(const catalog::Credits& c, size_t max = 5);

// How long an answer is kept on disk.
inline constexpr int64_t kFoundTtlSec = 30 * 24 * 3600;
inline constexpr int64_t kNoneTtlSec = 3 * 24 * 3600;
inline constexpr size_t kMemoryEntries = 48;

// The cache file's JSON (tests): {"v":1, "at":unix, "provider", "groups":[[[name, artistId, [roles]]...] x5],
// "sources":[...]}. fromJson: nullopt for a broken / other-version / expired entry (`now` in unix seconds).
std::string toJson(const catalog::Credits& c, int64_t now);
std::optional<catalog::Credits> fromJson(const std::string& text, int64_t now);
// Tests: the cache folder (default paths::cacheDir() / "credits") and forgetting the memory cache.
void setCacheDir(std::filesystem::path dir);
void clearMemory();

} // namespace st::app::credits
