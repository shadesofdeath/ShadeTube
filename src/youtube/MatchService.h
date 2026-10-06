#pragma once
// Catalog track -> YouTube video -> playable audio stream.
//
// Layers (fastest first):
//   1. stream cache   videoId -> StreamInfo, in memory, until the googlevideo URL's `expire` - 10 min
//   2. match cache    trackId -> Match, persistent (cache/matches.json); manual picks are pinned forever
//   3. TrackMatcher   YouTube search + ranking (YoutubeExplode::Music), ~1 request
// A cached match costs a single getManifest() request on the next play, plus a 1 KB range request: the chosen stream
// URL is tried before it is handed to the engine. YouTube refuses the URLs of a whole session now and then (HTTP 403;
// seen on 2026-10-06 for every stream of one run while a restart played fine); a refused URL starts a new YouTube
// session (fresh visitor data and cookies) and resolves once more, and so does a URL that fails in the engine shortly
// after it was handed out. Only then the backup sources are asked.
//
// Backup source (Settings::altSource, off by default): when YoutubeExplode cannot deliver a stream or a search, the
// Piped / Invidious instances (youtube/AltSource; the chosen kind first, then the other, by remembered health) are
// asked instead; their search results are ranked by the same TrackMatcher scorer. When none plays the song either,
// resolveForPlayback() looks for it on SoundCloud (youtube/SoundCloud, Settings::altSoundCloud): a full-length upload
// with the track's duration, played for this time only. StreamInfo::source tells which one served a stream and is
// logged for every track. A YouTube 429 leaves YouTube alone for 3 minutes while the backup is on. Turning the backup
// off / changing it drops every stream it served. A stand-in found after a failure that may pass is played but not
// persisted as the track's match.
//
// Blocking and thread-safe: call from worker threads.
#include "catalog/Models.h"
#include "youtube/AltSource.h"
#include "youtube/SoundCloud.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Music/TrackMatcher.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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
    std::optional<double> loudnessDb;   // YouTube's loudness of the stream vs. its -14 LUFS reference (dB); none = unknown
    std::chrono::system_clock::time_point expires{};
    std::string source;        // "youtube" | "piped <host>" | "invidious <host>" | "soundcloud"
    std::chrono::system_clock::time_point fetched{};
};

struct Resolved {
    Match match;
    StreamInfo stream;
};

struct NoMatchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// YouTube handed out a stream URL and then refused it (HTTP 403 / 404 / 410), also after a new session.
struct StreamRefusedError : std::runtime_error {
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

    // The player's resolve: like resolve(), and with refresh the cached match's stream is fetched anew (its URL failed
    // in the engine). When nothing plays and the backup source is on, a SoundCloud stand-in (Match::videoId empty).
    Resolved resolveForPlayback(const catalog::Track& track, bool allowWebm, bool lowQuality, bool refresh, const CT& ct = {});

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
    void setSoundCloud(bool on) { soundCloudOn_ = on; }   // Settings::altSoundCloud (only with the backup source on)
    alt::AltSource& altSource() { return alt_; }
    // Test switch (also env SHADETUBE_FORCE_ALT=1): skip YoutubeExplode entirely while a backup source is set, so the
    // fallback path runs. Off by default.
    void forceAltSource(bool on) { forceAlt_ = on; }
    // Test hook (set before use): replaces the check of a YouTube stream URL; (url, content length) -> HTTP status, 0 =
    // could not tell.
    void setUrlCheck(std::function<int(const std::string&, int64_t)> check) { urlCheck_ = std::move(check); }

private:
    void load();
    StreamInfo youtubeStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct);
    StreamInfo pickYoutubeStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct);
    // Starts a new YouTube session unless one started after `failedAt` (that one is new enough) or in the last 20 s.
    // True when the session is newer than `failedAt`, i.e. resolving again is worth it.
    bool renewYoutubeSession(std::chrono::system_clock::time_point failedAt, const std::string& why);
    Resolved soundCloud(const catalog::Track& track, const CT& ct);
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
    soundcloud::SoundCloud soundCloud_;
    std::atomic<bool> soundCloudOn_{true};
    std::atomic<bool> forceAlt_{false};
    std::function<int(const std::string&, int64_t)> urlCheck_;
    std::mutex sessionMutex_;
    std::chrono::system_clock::time_point sessionStarted_{};   // last renewYoutubeSession (epoch = the first session)
    std::atomic<std::chrono::steady_clock::rep> youtubeLimitedUntil_{0};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Match> matches_;
    std::unordered_map<std::string, StreamInfo> streams_;
    bool dirty_ = false;
};

} // namespace st::youtube
