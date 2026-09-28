#include "Test.hpp"

#include <YoutubeExplode.hpp>

#include <chrono>

using namespace YoutubeExplode;
using namespace std::chrono;

namespace {

Search::VideoSearchResult video(const std::string& id, const std::string& title, const std::string& channel,
                                std::optional<TimeSpan> duration) {
    return Search::VideoSearchResult(Videos::VideoId(id), title, Common::Author("UC1234567890123456789012", channel),
                                     duration, {});
}

std::string bestId(const std::vector<Music::TrackCandidate>& ranked) { return ranked.front().video.id().value(); }

} // namespace

TEST(TrackMatcher_builds_queries_from_catalog_titles) {
    CHECK_EQ(Music::TrackMatcher::buildQuery({"Bohemian Rhapsody - Remastered 2011", {"Queen"}, seconds(355)}),
             std::string("Queen - Bohemian Rhapsody"));
    CHECK_EQ(Music::TrackMatcher::buildQuery({"Uptown Funk (feat. Bruno Mars)", {"Mark Ronson", "Bruno Mars"}, {}}),
             std::string("Mark Ronson - Uptown Funk"));
    CHECK_EQ(Music::TrackMatcher::buildQuery({"Song ft. Someone [Live]", {}, {}}), std::string("Song"));
}

TEST(TrackMatcher_prefers_official_audio_with_matching_duration) {
    Music::TrackMatcher matcher{YoutubeClient()};
    Music::TrackQuery query{"Around the World", {"Daft Punk"}, seconds(429)};
    auto ranked = matcher.rank(query, {
        video("aaaaaaaaaa1", "Daft Punk - Around The World (Official Music Video)", "Daft Punk", seconds(240)),
        video("aaaaaaaaaa2", "Daft Punk - Around the World (Live at Coachella)", "Fan", seconds(431)),
        video("aaaaaaaaaa3", "Daft Punk - Around the World (Official Audio)", "Daft Punk", seconds(430)),
        video("aaaaaaaaaa4", "Around the World 1 Hour Loop", "Loops", hours(1)),
        video("aaaaaaaaaa5", "Around the World (sped up)", "Edits", seconds(300)),
        video("aaaaaaaaaa6", "Daft Punk - Around the World LIVE", "Stream", std::nullopt),
    });
    CHECK_EQ(bestId(ranked), std::string("aaaaaaaaaa3"));
    CHECK(ranked.front().score > 90);
    // Live, loop and sped-up variants all end up far below the correct match.
    for (std::size_t i = 1; i < ranked.size(); ++i)
        CHECK(ranked[i].score < ranked.front().score - 30);
}

TEST(TrackMatcher_topic_channel_and_unicode_titles) {
    Music::TrackMatcher matcher{YoutubeClient()};
    Music::TrackQuery query{"Şımarık", {"Tarkan"}, seconds(234)};
    auto ranked = matcher.rank(query, {
        video("bbbbbbbbbb1", "Tarkan - Şımarık (Remix 2024)", "DJ", seconds(236)),
        video("bbbbbbbbbb2", "Şımarık", "Tarkan - Topic", seconds(235)),
        video("bbbbbbbbbb3", "Tarkan - Kuzu Kuzu", "Tarkan", seconds(234)),
    });
    CHECK_EQ(bestId(ranked), std::string("bbbbbbbbbb2"));
    CHECK_EQ(ranked.back().video.id().value(), std::string("bbbbbbbbbb3"));  // different song
}

TEST(TrackMatcher_keeps_variants_the_catalog_asked_for) {
    Music::TrackMatcher matcher{YoutubeClient()};
    Music::TrackQuery query{"Hotel California - Live on MTV, 1994", {"Eagles"}, seconds(427)};
    auto ranked = matcher.rank(query, {
        video("cccccccccc1", "Eagles - Hotel California (Official Audio)", "Eagles", seconds(391)),
        video("cccccccccc2", "Eagles - Hotel California (Live 1994)", "Eagles", seconds(428)),
    });
    CHECK_EQ(bestId(ranked), std::string("cccccccccc2"));  // "live" is not penalized when requested
}
