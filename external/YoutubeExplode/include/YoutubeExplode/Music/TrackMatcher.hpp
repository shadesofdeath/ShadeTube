#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Common/Time.hpp>
#include <YoutubeExplode/Search/SearchResults.hpp>
#include <YoutubeExplode/Videos/Streams/StreamInfo.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::Music {

/// A track described by an external catalog (e.g. Spotify): what the user sees on screen.
struct TrackQuery {
    /// Track title as shown by the catalog, e.g. "Around the World - Radio Edit".
    std::string title;
    /// Artist names; the first one is treated as the primary artist.
    std::vector<std::string> artists;
    /// Track duration. Strongly recommended: it is the most reliable matching signal.
    std::optional<TimeSpan> duration;
    /// Album name (optional, only used as a weak signal).
    std::string album;
};

/// A YouTube video scored against a TrackQuery.
struct TrackCandidate {
    Search::VideoSearchResult video;
    /// Match score (higher is better). Roughly 0..100; anything above ~60 is a confident match.
    double score = 0;
    /// Human-readable explanation of the score, useful for debugging and "why this video?" UI.
    std::vector<std::string> reasons;
};

/// Result of a match: the best candidate plus the other candidates (best first) so the UI can
/// offer "choose another source".
struct TrackMatch {
    TrackCandidate best;
    std::vector<TrackCandidate> alternatives;
};

/// A matched video together with its playable audio stream.
struct ResolvedTrack {
    TrackCandidate match;
    /// Best audio-only stream; falls back to a muxed stream (audio + video, e.g. itag 18) when
    /// YouTube offers no audio-only stream for the video. Either can be played as audio.
    std::shared_ptr<const Videos::Streams::IAudioStreamInfo> audio;
    /// Remaining candidates, best first (excludes `match`).
    std::vector<TrackCandidate> alternatives;
};

struct TrackMatcherOptions {
    /// Number of search results to consider.
    std::size_t maxSearchResults = 15;
    /// Candidates scoring below this are not considered a match.
    double minimumScore = 40;
    /// Run a second, differently phrased search when the first one yields no confident match.
    bool enableFallbackSearch = true;
    /// How many candidates resolve() tries before giving up (unplayable / region-locked videos).
    std::size_t maxResolveAttempts = 3;
};

/// Finds the YouTube video that corresponds to a catalog track (the "Spotube" approach):
/// search YouTube, then rank results by title/artist similarity, duration difference,
/// channel type (official "Artist - Topic" / VEVO uploads) and unwanted variants
/// (live, cover, remix, sped up, 1 hour loop, ...) that are not part of the requested track.
///
///   Music::TrackMatcher matcher(youtube);
///   auto track = matcher.resolve({"Around the World", {"Daft Punk"}, std::chrono::seconds(429)});
///   if (track) play(track->audio->url());
///
/// Cache "catalog track id -> track->match.video.id()" on your side: it makes subsequent plays
/// cost a single request (getManifest) and keeps you well below YouTube's rate limits.
class TrackMatcher {
public:
    explicit TrackMatcher(YoutubeClient youtube, TrackMatcherOptions options = {})
        : youtube_(std::move(youtube)), options_(options) {}

    /// Searches YouTube and returns the best match, or nothing if no candidate is good enough.
    std::optional<TrackMatch> find(const TrackQuery& query, const CancellationToken& cancellationToken = {}) const;

    /// Like find(), but also resolves the audio stream. If the best video turns out to be
    /// unplayable, the next candidates are tried. `container` restricts the audio container
    /// ("webm" = opus, "mp4" = AAC); empty = highest bitrate of any container.
    std::optional<ResolvedTrack> resolve(const TrackQuery& query, const std::string& container = {},
                                         const CancellationToken& cancellationToken = {}) const;

    std::future<std::optional<ResolvedTrack>> resolveAsync(TrackQuery query, std::string container = {},
                                                           CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, query, container, cancellationToken] {
            return self.resolve(query, container, cancellationToken);
        });
    }

    /// Scores and sorts arbitrary search results (no network access). Exposed for testing and
    /// for apps that want to re-rank results they already have.
    std::vector<TrackCandidate> rank(const TrackQuery& query, const std::vector<Search::VideoSearchResult>& results) const;

    /// Search query used for a track, e.g. "Daft Punk - Around the World".
    static std::string buildQuery(const TrackQuery& query);

private:
    YoutubeClient youtube_;
    TrackMatcherOptions options_;
};

} // namespace YoutubeExplode::Music
