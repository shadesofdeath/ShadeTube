#pragma once
// Video mode's official-video decisions, per song (app/OfficialVideo): which music video plays against the song's
// audio-only upload and how its time relates (the alignment's segments), or that none does. Kept in
// cache\officialvideos.json so a song is searched and aligned once: the next play shows its video at once.
//
// A decision belongs to the song's own upload it was aligned against (audioVideoId): after "Yanlış eşleşme?" picked
// another upload it is made again (the user's rejections stay). An automatic "none" is retried after 30 days (7 when no
// candidate was found at all: a video may have appeared); a decision of an older alignment version is made again. The user's own choices ("Yanlış video
// mu?": a pick, "Video gösterme", rejections) are kept as long as the song's upload stays the same.
//
// Standalone (tests/video compiles it): core and video/AudioAlign only. UI thread.
#include "video/AudioAlign.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app {

struct OfficialDecision {
    static constexpr int kVersion = 1;             // bump when the alignment changes: older decisions are made again

    std::string audioVideoId;                      // the song's own upload the alignment was made against
    std::string videoId;                           // the music video ("" = none: the cover shows)
    std::string title, channel, thumbnailUrl;
    int durationSec = 0;
    std::vector<video::AlignSegment> segments;     // song -> video (video::TimeMap)
    double score = 0, coverage = 0;
    std::string verdict;                           // the alignment's verdict, or why there is none ("no candidates")
    bool manual = false;                           // picked by the user (also "Video gösterme": manual with no video)
    std::vector<std::string> rejected;             // turned down by the user ("Bu videoyu gösterme")
    std::vector<std::pair<std::string, std::string>> tried;   // video id -> verdict of the candidates aligned so far
    int version = kVersion;
    int64_t decidedAt = 0;                         // unix seconds

    bool hasVideo() const { return !videoId.empty() && !segments.empty(); }
    std::string verdictOf(const std::string& id) const;   // "" = not tried
};

class OfficialVideoCache {
public:
    explicit OfficialVideoCache(std::filesystem::path file);

    void load();
    // Writes the file when something changed (atomic: temporary file + rename).
    void flush();

    // The decision for a song if one is valid for its current upload (see the header comment); `nowUnix` for the
    // expiry of an automatic "none".
    std::optional<OfficialDecision> find(const std::string& trackId, const std::string& audioVideoId, int64_t nowUnix) const;
    // Any stored decision of the song (its rejections / picks survive a new search).
    const OfficialDecision* raw(const std::string& trackId) const;
    void put(const std::string& trackId, OfficialDecision d);
    void erase(const std::string& trackId);
    size_t size() const { return entries_.size(); }

    static constexpr int64_t kNoneTtlSec = 30LL * 24 * 3600;        // candidates that didn't match
    static constexpr int64_t kNoCandidatesTtlSec = 7LL * 24 * 3600;  // nothing found
    static constexpr size_t kMaxEntries = 3000;   // the oldest go first beyond this

private:
    std::filesystem::path file_;
    std::unordered_map<std::string, OfficialDecision> entries_;
    bool dirty_ = false;
};

} // namespace st::app
