#pragma once
// Last-resort audio source: the same song on SoundCloud, when neither YouTube nor a backup server plays it.
//
// Used by MatchService only while the backup source is on and Settings::altSoundCloud is set. No API key: like other
// players (yt-dlp, NewPipe, Spotube plugins) it reads the public web client's client_id from soundcloud.com's
// scripts (kept for 12 hours; fetched again once when the API answers 401 / 403).
//
//   GET https://api-v2.soundcloud.com/search/tracks?q=<artist title>&limit=20&client_id=<id>
//   GET <transcoding.url>?client_id=<id>&track_authorization=<t>      -> {"url": signed CDN URL}
//
// Only full tracks qualify: policy "ALLOW" (Go+ / label tracks are "SNIP", 30 s previews), a progressive MP3
// transcoding that is not snipped, a duration within 3 s of the catalog track's, and a TrackMatcher score (the YouTube
// ranking: title / artist / duration / unwanted variants such as cover, remix, slowed) of at least 70, which in practice
// requires the artist in the title or the uploader name, and a title that names at most two words beyond the song, its
// artists and the usual decorations ("Official Audio", a year): re-uploads like "A - Song / Other Song / Third" are not
// the song. Matches are re-uploads by users, so they are stand-ins for one play only. HLS-only tracks are skipped: the engine plays progressive
// streams with Range requests (seeking), and the CDN URL must pass alt::probeStream (206, MP3 data).
//
// Blocking and thread-safe: call from worker threads.
#include "catalog/Models.h"
#include "youtube/AltSource.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Music/TrackMatcher.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::youtube::soundcloud {

struct Candidate {
    int64_t id = 0;
    std::string title;
    std::string user;                 // uploader's display name
    std::string permalink;            // https://soundcloud.com/<user>/<track>
    int durationMs = 0;
    std::string policy;               // "ALLOW" | "SNIP" | "BLOCK" | "MONETIZE"
    std::string progressiveUrl;       // the progressive MP3 transcoding's API URL ("" = none / snipped)
    std::string trackAuthorization;
};

// ---- Pure parsing and matching (no network; unit-tested) -------------------------------------------------------
std::vector<std::string> scriptUrls(std::string_view html);            // https://a-v2.sndcdn.com/assets/*.js, in order
std::optional<std::string> parseClientId(std::string_view script);     // client_id:"<32 alphanumerics>"
std::vector<Candidate> parseSearch(std::string_view json);             // throws alt::AltSourceError
// The best full-length candidate for the track, or nullptr (see the rules above). `ranker` scores like TrackMatcher.
const Candidate* pick(const std::vector<Candidate>& candidates, const catalog::Track& track,
                      const YoutubeExplode::Music::TrackMatcher& ranker);

struct Served {
    alt::AudioStream stream;          // mimeType "audio/mpeg", codec "mp3"
    Candidate track;
};

class SoundCloud {
public:
    // Searches "<artist> <title>", picks a candidate and resolves its stream. Throws alt::AltSourceError (no match,
    // only previews, API / CDN failures) or OperationCanceledException.
    Served find(const catalog::Track& track, const YoutubeExplode::Music::TrackMatcher& ranker,
                const YoutubeExplode::CancellationToken& ct = {});

private:
    std::string clientId(bool refresh, std::chrono::steady_clock::time_point deadline, const YoutubeExplode::CancellationToken& ct);
    // GET of a client_id URL; a 401 / 403 refreshes the client_id once.
    alt::HttpResult api(const std::string& urlWithoutId, std::chrono::steady_clock::time_point deadline,
                        const YoutubeExplode::CancellationToken& ct);

    std::mutex mutex_;
    std::string clientId_;
    std::chrono::steady_clock::time_point clientIdAt_{};
};

} // namespace st::youtube::soundcloud
