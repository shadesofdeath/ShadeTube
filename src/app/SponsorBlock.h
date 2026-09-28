#pragma once
// SponsorBlock: skips the non-music / sponsor parts of the YouTube video that plays the current track,
// using the community database at sponsor.ajay.app (the same data Spotube uses).
//
// Standalone (depends on st_core only). UI-thread object: setVideo()/check() are called on the UI thread,
// the lookup runs on the worker pool and its result is applied on the UI thread (st::async continuation,
// guarded by an own Lifetime that setVideo() renews, so a late result for a previous video is dropped).
//
// Privacy: the lookup uses the k-anonymity endpoint /api/skipSegments/<first 4 hex of sha256(videoId)>,
// so the server never learns which video is playing; the response (all videos in that hash bucket,
// ~8 KB gzipped) is filtered locally. There is deliberately no fallback to the plain
// /api/skipSegments?videoID=... lookup (it would leak the id); a failed lookup is retried after 30 s.
//
// Skip semantics (per segment "armed" flag):
//   - A segment is skipped when the playhead is inside it while it is armed; skipping disarms it, so the
//     stale positions reported while the seek is in flight (or a seek landing slightly early) never loop.
//   - It re-arms when the playhead is seen before its start (the user seeked back / the track restarted),
//     or on a backward jump that lands within its first second (restart: "previous" -> seek(0),
//     repeat-one). A deliberate seek into the MIDDLE of an already-skipped segment plays it (like
//     SponsorBlock's "unskip").
//   - No skip at position <= 0 (Player reports 0 while tracks switch; the first real tick is ~200 ms later).
#include "core/Async.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app {

class SponsorBlock {
public:
    struct Segment {
        int64_t startMs = 0;
        int64_t endMs = 0;
        std::string category;          // "music_offtopic", "sponsor", ... (first one of a merged run)
        int64_t videoDurationMs = 0;   // video length when submitted (0 = unknown)
    };
    enum class State { Idle, Loading, Ready, Failed };

    SponsorBlock();
    ~SponsorBlock();
    SponsorBlock(const SponsorBlock&) = delete;
    SponsorBlock& operator=(const SponsorBlock&) = delete;

    // Switches to another YouTube video; "" = none (offline/local file, not resolved yet). A different id
    // drops the old segments and (when enabled) looks up the new ones in the background. The same id again
    // is a no-op, so this may be called on every Player::onChanged. videoDurationMs (0 = unknown) is the
    // real length of the video: segments submitted for a length that differs by > 3 s (re-edited upload)
    // are ignored, so outdated timestamps cannot cut into the music.
    void setVideo(const std::string& videoId, int64_t videoDurationMs = 0);

    // Call every ~200-250 ms while playing. Returns the position (ms) to seek to when positionMs is inside
    // an armed segment (its end; overlapping/adjacent segments are merged into one), else nullopt.
    // Also starts the lookup lazily when `enabled` or `categories` changed since setVideo().
    std::optional<int64_t> check(int64_t positionMs);

    int segmentCount() const { return static_cast<int>(segments_.size()); }
    const std::vector<Segment>& segments() const { return segments_; }   // e.g. seek-bar markers
    const std::string& videoId() const { return videoId_; }
    State state() const { return state_; }
    // The segment returned by the last successful check() (for a toast), or nullptr.
    const Segment* lastSkipped() const;

    bool enabled = true;
    std::vector<std::string> categories;   // SponsorBlock category ids; defaults to defaultCategories()

    // music_offtopic, sponsor, selfpromo, interaction: never music by definition. intro/outro/preview/filler
    // are left out because in music videos they often overlap real music (song intro under a logo, outro
    // over the credits, a chorus teaser, cinematic scenes with score).
    static std::vector<std::string> defaultCategories();

    // --- building blocks (public for tests) -------------------------------------------------------------
    // Blocking network lookup (worker thread only). Returns the "skip" segments of videoId in the given
    // categories, un-normalised. 404 = no segments = empty result. Throws on network/HTTP/JSON errors.
    // hashPrefix = true (the app): k-anonymity endpoint only; false: direct ?videoID= lookup (tests).
    static std::vector<Segment> fetchSegments(const std::string& videoId, const std::vector<std::string>& categories,
                                              bool hashPrefix = true);
    // Parses either response shape: [{segment,category,actionType,...}] (direct) or
    // [{videoID, segments:[...]}] (hash prefix; only videoId's entry is used). Keeps actionType "skip" only.
    // Throws on malformed JSON.
    static std::vector<Segment> parseResponse(std::string_view json, const std::string& videoId);
    // Sorts, drops outdated (duration mismatch) / invalid ones, merges overlapping or near-adjacent
    // (gap <= 500 ms) ones and finally drops those shorter than 1 s (a seek costs more than it saves).
    static std::vector<Segment> normalize(std::vector<Segment> segments, int64_t videoDurationMs = 0);
    static std::string sha256Hex(std::string_view data);   // CNG, lowercase hex
    // Replaces the current video's segments (normalised, all armed) without a lookup: tests / injection.
    void setSegments(std::vector<Segment> raw);

private:
    struct CacheEntry {
        std::string key;               // videoId + '\n' + categories
        std::vector<Segment> raw;
    };

    void ensureFetched();
    void apply(const std::vector<Segment>& raw);
    const std::vector<Segment>* findCache(const std::string& key) const;
    void storeCache(std::string key, std::vector<Segment> raw);

    std::string videoId_;
    int64_t videoDurationMs_ = 0;
    std::string requestKey_;           // categories the current segments/lookup are for ("" = none yet)
    std::vector<Segment> segments_;    // normalised, sorted by start
    std::vector<unsigned char> armed_; // parallel to segments_
    int lastSkipped_ = -1;
    int64_t lastPos_ = -1;             // previous check() position (-1 = none since setVideo)
    State state_ = State::Idle;
    int64_t failedAtMs_ = 0;           // steady clock; a failed lookup is retried after 30 s
    std::vector<CacheEntry> cache_;    // small session cache (replays, repeat-one): no refetch
    Lifetime life_;
};

} // namespace st::app
