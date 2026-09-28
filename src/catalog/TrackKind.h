#pragma once
// Track ids that are no songs. Podcast episodes (app/Podcasts) play straight from their feed's media URL: no YouTube
// match, SponsorBlock, lyrics, scrobbling, listening stats, radio / autoplay seeds or crossfade.
// (Internet radio stations have their own prefix, app/InternetRadio.h: radio::isStationId.)
#include <string_view>

namespace st::catalog {

inline constexpr std::string_view kPodcastIdPrefix = "podcast:";

inline bool isPodcastId(std::string_view trackId) {
    return trackId.size() > kPodcastIdPrefix.size() && trackId.starts_with(kPodcastIdPrefix);
}

} // namespace st::catalog
