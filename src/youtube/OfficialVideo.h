#pragma once
// Video mode's official video (app/OfficialVideo): for a song that plays from an Art Track or an audio upload (no
// picture of its own), the artist's music video of the same song, found here and then aligned with the song's audio
// (video/AudioAlign) before it is ever shown.
//
// Where candidates come from:
//   1. YouTube Music's video search (music.youtube.com/youtubei/v1/search, WEB_REMIX client, the "Videos" filter):
//      every result carries YouTube Music's own classification, MUSIC_VIDEO_TYPE_OMV ("official music video", what its
//      Song / Video switch plays) or _UGC (anyone's upload), and the artist runs. The direct song -> video counterpart
//      of the Song / Video switch (the watch-next panel's `counterpart`) is not in anonymous responses (checked
//      2026-11-26), so the search is the way in.
//   2. YouTube's own search ("<artist> - <title> official video") when (1) finds nothing usable.
// Ranking (rankOfficialVideos, pure): the song's title words in the video's title, the primary artist in YouTube
// Music's artists / the channel / the title; refused: " - Topic" channels (Art Tracks), lyric / audio / visualizer
// uploads, live, cover, remix, acoustic, reaction, karaoke, sped up / slowed, compilations, teasers, translations ...
// (unless the catalog title itself says so), and durations outside [song - max(20 s, 12 %), song + max(3 min, 75 %)].
// A non-OMV upload must look official (the artist's or a VEVO channel, "Official Video" in its title). OMV first,
// then the strength of those signals.
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace st::youtube {

struct OfficialCandidate {
    std::string videoId;
    std::string title;
    std::string channel;                 // the upload's channel (YouTube) or the first artist run (YouTube Music)
    std::vector<std::string> artists;    // YouTube Music's artist runs
    int durationSec = 0;
    std::string thumbnailUrl;
    bool omv = false;                    // YouTube Music: MUSIC_VIDEO_TYPE_OMV
    bool ugc = false;                    // YouTube Music: MUSIC_VIDEO_TYPE_UGC
    double score = 0;                    // rankOfficialVideos
};

struct OfficialQuery {
    std::string title;                   // as the catalog shows it ("Blinding Lights", "bad guy - Live")
    std::vector<std::string> artists;    // the first is the primary artist
    int durationSec = 0;                 // 0 = unknown
    std::string album;
};

// The list items of a YouTube Music search response body (any layout of shelves; items without a video id are
// skipped; malformed JSON gives none).
std::vector<OfficialCandidate> parseMusicVideoSearch(std::string_view body);

// The acceptable candidates, best first (refused ones are left out; see the header comment).
std::vector<OfficialCandidate> rankOfficialVideos(const OfficialQuery& query, std::vector<OfficialCandidate> candidates);

// Network (worker threads): YouTube Music's video search, then YouTube's when that finds nothing; ranked. `exclude`:
// video ids never offered (the song's own upload, the user's rejections). Only the query text is sent.
std::vector<OfficialCandidate> findOfficialVideos(YoutubeExplode::YoutubeClient& client, const OfficialQuery& query,
                                                  const std::vector<std::string>& exclude, const std::string& region,
                                                  const YoutubeExplode::CancellationToken& ct = {});

} // namespace st::youtube
