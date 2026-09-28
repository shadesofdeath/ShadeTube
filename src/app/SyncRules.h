#pragma once
// Download sync ("Çevrimdışı kullanılabilir"): the collections the user keeps downloaded, and the pure planning logic
// behind it. The rules live in %LOCALAPPDATA%\ShadeTube\sync.json (their own file; downloads.json only marks a
// download that sync queued with "sy"). Standalone (core + catalog only) so tests compile it directly; the engine
// that lists the collections, feeds the download queue and draws the UI is app/DownloadSync.
//
// A rule is one collection: Liked Songs (Spotify's while logged in, else the ones on this PC), a playlist (Spotify or
// local) or an album (Spotify or MusicBrainz). Each sync of a rule lists the collection and remembers its track ids;
// the planner then decides what to queue (not downloaded yet, not blocked, the retry policy for failed downloads, the
// storage cap) and which sync downloads no rule wants any more. Downloads the user made themselves (or put in one of
// their download folders) are never cancelled or deleted by sync.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace st::app::sync {

enum class Kind { Liked, Playlist, Album };

// Why the last listing of a rule failed (translated only when shown).
enum class ListError { None, NotFound, RateLimited, Network };

// Rule id of the local Liked Songs (logged out). Spotify's Liked Songs use their collection URI.
inline constexpr const char* kLocalLikedId = "liked";
inline constexpr const char* kSpotifyLikedId = "spotify:collection:tracks";

struct Rule {
    std::string id;                        // kSpotifyLikedId | kLocalLikedId | playlist id | album id (spotify: URI / MBID)
    Kind kind = Kind::Playlist;
    std::string name;                      // shown in the Downloads page (refreshed by each listing)
    std::vector<catalog::Image> images;
    int64_t addedAt = 0;                   // unix seconds
    // Last listing.
    int64_t lastSync = 0;                  // unix seconds of the last successful listing (0 = never listed)
    std::vector<std::string> trackIds;     // the collection's downloadable tracks at that listing, in order
    ListError error = ListError::None;     // the last listing attempt failed (trackIds are kept from before)
    std::string errorDetail;               // technical detail for the log / tooltip
    int failures = 0;                      // consecutive failed listings
    int64_t retryAt = 0;                   // backoff: no listing before this (unix seconds)
    int64_t changedAt = 0;                 // the collection changed here after lastSync (not persisted)
};

// Automatic download attempts of one track by sync (the retry policy for failures).
struct Attempt {
    int count = 0;
    int64_t last = 0;                      // unix seconds of the last attempt
};

struct State {
    std::vector<Rule> rules;               // in the order the user added them (queue order)
    std::unordered_map<std::string, Attempt> attempts;   // track id -> attempts (only tracks that failed / are pending)
};

bool isSpotifyRule(const Rule& r);         // needs a Spotify session to list
const Rule* findRule(const State& s, const std::string& id);
Rule* findRule(State& s, const std::string& id);

// ---- Persistence ---------------------------------------------------------------------------------------------------
nlohmann::json toJson(const State& s);
State stateFromJson(const nlohmann::json& j);   // tolerant: unknown / broken entries are skipped
// Missing file = empty state. A damaged file is kept as sync.json.bad and an empty state returned.
State loadState(const std::filesystem::path& file);
bool saveState(const State& s, const std::filesystem::path& file);   // write-then-rename

// ---- What the download queue knows about a track ------------------------------------------------------------------
struct ItemInfo {
    enum class St { None, Queued, Downloading, Done, Failed };
    St state = St::None;
    bool synced = false;                   // queued / downloaded by sync (false = the user's own download)
    int64_t sizeBytes = 0;                 // Done: the file size
    int durationMs = 0;                    // for size estimates of queued items
};
using ItemLookup = std::function<ItemInfo(const std::string& trackId)>;

// Sync never queues these: no id, a radio station, a local file, a podcast episode, or a track the catalog marks as
// unplayable.
bool downloadable(const catalog::Track& t);

// Expected file size of a download (MP3 at `mp3Kbps`, or the original ~128 kbps AAC for 0) incl. cover + tag.
int64_t estimateBytes(int durationMs, int mp3Kbps);

// Retry policy: a failed sync download is queued again after 1 h, then 6 h; after kMaxAttempts it waits for the user
// ("Hataları yeniden dene" clears the attempts).
inline constexpr int kMaxAttempts = 3;
bool retryDue(const Attempt& a, int64_t now);

struct Limits {
    int64_t capBytes = 0;                  // 0 = no cap
    int64_t usedBytes = 0;                 // sync downloads on disk + the estimate of the queued ones
    int mp3Kbps = 320;
};

struct Plan {
    std::vector<catalog::Track> toQueue;   // new or retryable tracks, in collection order, each once
    int skippedCap = 0;                    // would be queued, but the storage cap is reached
    int waitingRetry = 0;                  // failed, retried later
    int gaveUp = 0;                        // failed kMaxAttempts times
    int64_t queuedBytes = 0;               // estimate of toQueue
};
// `tracks` = the listed collection. Already downloaded / queued (by anyone) and blocked tracks are left out.
Plan plan(const std::vector<catalog::Track>& tracks, const ItemLookup& lookup,
          const std::function<bool(const catalog::Track&)>& blocked,
          const std::unordered_map<std::string, Attempt>& attempts, const Limits& limits, int64_t now);

// The ids a listing keeps for a rule: downloadable tracks, each once, in order.
std::vector<std::string> ruleTrackIds(const std::vector<catalog::Track>& tracks);

// Every track id some rule wants (the union of their last listings).
std::unordered_set<std::string> wantedIds(const std::vector<Rule>& rules);

// Sync downloads no rule wants any more. `cancel`: queued / downloading / failed ones (just dropped from the queue);
// `remove`: downloaded files, only when `deleteFiles` (Ayarlar: "Listeden çıkan şarkıları sil"). The user's own
// downloads are never listed.
struct Drop {
    std::vector<std::string> cancel;
    std::vector<std::string> remove;
};
Drop dropped(const std::vector<std::pair<std::string, ItemInfo>>& items, const std::unordered_set<std::string>& wanted,
             bool deleteFiles);

// A listing that suddenly came back empty after holding tracks is not trusted for deleting files (an API hiccup must
// never wipe the user's offline music); the next listing decides.
bool suspiciousShrink(size_t before, size_t after);

// ---- Progress of a rule --------------------------------------------------------------------------------------------
struct Progress {
    int total = 0;                         // tracks in the last listing
    int done = 0;                          // downloaded (by sync or by the user)
    int active = 0;                        // queued or downloading
    int failed = 0;                        // failed downloads
};
Progress progress(const Rule& r, const ItemLookup& lookup);

// ---- Scheduling ----------------------------------------------------------------------------------------------------
inline constexpr int64_t kStartupDelaySec = 45;     // first pass after the app started
inline constexpr int64_t kChangeDebounceSec = 5;    // a like / playlist edit is synced this long after it happened
inline constexpr int64_t kPendingRelistSec = 30 * 60;   // tracks still missing: list again after this

// How often a rule is listed again with no change seen: playlists 45 min, Spotify Liked Songs 6 h (likes made here
// are seen at once through the library snapshot), albums 24 h, local collections 45 min (free: no network).
int64_t relistInterval(const Rule& r);
// Least time between two listings when changes keep coming (Spotify Liked Songs: 5 min, as a listing pages them all;
// other Spotify lists: 1 min; local: none).
int64_t minRelistGap(const Rule& r);
// Whether a rule should be listed now. `pending` = some of its tracks are neither downloaded nor queued and could be.
bool ruleDue(const Rule& r, int64_t now, bool pending);
// Backoff after `failures` consecutive failed listings: 1, 5, 15, 60 min (capped); rate limited (HTTP 429): at least
// 10 min.
int64_t backoffSeconds(int failures, bool rateLimited);

} // namespace st::app::sync
