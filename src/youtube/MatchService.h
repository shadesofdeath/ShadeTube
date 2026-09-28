#pragma once
// Catalog track -> YouTube video -> playable audio stream.
//
// Layers (fastest first):
//   1. stream cache   videoId -> StreamInfo, in memory, until the googlevideo URL's `expire` - 10 min
//   2. match cache    trackId -> Match, persistent (cache/matches.json); manual picks are pinned forever
//   3. TrackMatcher   YouTube search + ranking (YoutubeExplode::Music), ~1 request
// A cached match costs a single getManifest() request on the next play.
//
// Backup source (Settings::altSource, off by default): when YoutubeExplode cannot deliver a stream or a search, the
// Piped / Invidious instance (youtube/AltSource) is asked instead; its search results are ranked by the same
// TrackMatcher scorer. StreamInfo::source tells which one served a stream (logs only). A YouTube 429 leaves YouTube
// alone for 3 minutes while the backup is on. Turning the backup off / changing it drops every stream it served.
// A stand-in found after a failure that may pass is played but not persisted as the track's match.
//
// Blocking and thread-safe: call from worker threads.
#include "catalog/Models.h"
#include "youtube/AltSource.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Music/TrackMatcher.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::youtube {

using CT = YoutubeExplode::CancellationToken;

struct Match {
    std::string videoId;
    std::string title;
    std::string channel;
    int durationSec = 0;
    double score = 0;          // TrackMatcher score (0..100); 100 for manual picks
    bool manual = false;       // pinned by the user via "Yanlış eşleşme?"
    std::string thumbnailUrl;
};

struct StreamInfo {
    std::string url;
    int64_t contentLength = 0;
    std::string mimeType;      // "audio/mp4" | "audio/webm"
    std::string codec;         // "mp4a.40.2" | "opus"
    int bitrateKbps = 0;
    int itag = 0;
    std::chrono::system_clock::time_point expires{};
    std::string source;        // "youtube" | "piped <host>" | "invidious <host>"
    std::chrono::system_clock::time_point fetched{};
};

struct Resolved {
    Match match;
    StreamInfo stream;
};

struct NoMatchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class MatchService {
public:
    // Reads the backup source from Settings (construct on the UI thread) and SHADETUBE_FORCE_ALT.
    explicit MatchService(YoutubeExplode::YoutubeClient client);
    ~MatchService();

    // allowWebm: pick Opus/WebM when it has the best bitrate (only when the engine can decode it).
    // lowQuality: cap to <= 128 kbps AAC (data saver).
    Resolved resolve(const catalog::Track& track, bool allowWebm, bool lowQuality, const CT& ct = {});

    // Stream for an explicit video (manual pick / re-resolve after URL expiry).
    StreamInfo stream(const std::string& videoId, bool allowWebm, bool lowQuality, bool bypassCache, const CT& ct = {});

    // Candidates for the "wrong match?" picker (best first; includes the current match).
    std::vector<Match> candidates(const catalog::Track& track, const CT& ct = {});

    void pin(const std::string& trackId, Match match);
    std::optional<Match> cachedMatch(const std::string& trackId) const;
    void forgetStream(const std::string& videoId);

    // Persist the match cache if it changed (called by App on a debounce and at exit).
    void flush();

    YoutubeExplode::YoutubeClient& client() { return client_; }

    // Backup source: Settings::altSource ("off" | "piped" | "invidious") + altSourceInstance ("" = built-in list).
    // Any thread; the Settings page calls it on change. Cached streams served by a backup server are dropped.
    void setAltSource(const std::string& kind, const std::string& instance);
    alt::AltSource& altSource() { return alt_; }
    // Test switch (also env SHADETUBE_FORCE_ALT=1): skip YoutubeExplode entirely while a backup source is set, so the
    // fallback path runs. Off by default.
    void forceAltSource(bool on) { forceAlt_ = on; }

private:
    void load();
    StreamInfo youtubeStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct);
    // `generation`: the AltSource configuration the stream was fetched under.
    StreamInfo altStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct, uint64_t& generation);
    bool youtubeLimited() const;   // a 429 was seen in the last few minutes
    void noteYoutubeLimited();
    // TrackMatcher::find with the backup source's search as the fallback (same ranking).
    std::optional<YoutubeExplode::Music::TrackMatch> find(const YoutubeExplode::Music::TrackQuery& query,
                                                          const YoutubeExplode::Music::TrackMatcherOptions& options,
                                                          const CT& ct);
    std::optional<YoutubeExplode::Music::TrackMatch> altFind(const YoutubeExplode::Music::TrackQuery& query,
                                                             const YoutubeExplode::Music::TrackMatcherOptions& options,
                                                             const CT& ct);
    YoutubeExplode::YoutubeClient client_;
    alt::AltSource alt_;
    std::atomic<bool> forceAlt_{false};
    std::atomic<std::chrono::steady_clock::rep> youtubeLimitedUntil_{0};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Match> matches_;
    std::unordered_map<std::string, StreamInfo> streams_;
    bool dirty_ = false;
};

} // namespace st::youtube
