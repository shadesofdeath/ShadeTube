#pragma once
// "Yılın özeti" as a story (a Wrapped-style recap): a full-window view over the main window with up to ten cards
// that advance by themselves under a segmented progress bar: the year (or the last 12 months) at a glance, the
// minutes, the top songs, artists and albums, the genres (only when the artists' genres are known), when the music
// played (months, hours of the day), the longest streak and the busiest day, the discoveries, and a summary. Numbers
// count up, lists come in one after the other, the ground takes the colors of the card's cover; with "Hareketi azalt"
// or Windows' animation effects off everything stands still (the cards still advance).
//
// Data: app/YearReview on the listening history (ListenStats), locally; covers / artist pictures come from the Stats
// page's artwork caches and lookups (enrichStatsArtwork). Each card can be copied / saved as a 1080 x 1920 image
// (app/ShareCard), and "En çok dinlediklerini çal" plays the period's top songs: the smart list "<year> en iyileri"
// when there is one (app/SmartLists), else the period's 50 most streamed songs; "Liste olarak kaydet" saves them as a
// playlist.
//
// Controls: a click on the card's left third goes back, elsewhere forward; Left / Right, PageUp / PageDown, Home /
// End; Space pauses; Esc closes. The period picker at the top switches to another year that has enough listening.
#include "app/ListenStats.h"
#include "catalog/Models.h"
#include "core/Async.h"

#include <functional>
#include <string>
#include <vector>

namespace st::ui {
class Column;
}

namespace st::app {

namespace review {
// Opens the story for `year` (review::kRolling = the last 12 months; -1 = the current year when it qualifies, else
// the last 12 months, else the newest year that does). Toasts when no period has enough listening.
void open(int year = -1);
// The Stats page's invitation (nothing when no period qualifies): the page's year (`year`, -1 = as open(-1) picks).
void addBanner(ui::Column* c, int year);
} // namespace review

// Feature start (initFeatures): the dev hooks of the share cards (SHADETUBE_LYRICS_SHARE, SHADETUBE_REVIEW).
void initShareFeatures();

// Defined in app/StatsPage.cpp, which owns those caches: covers for top songs / albums that have none and the artists'
// pictures (`artistPictures`, parallel to `artists`), from the Stats page's caches; the missing ones are looked up as
// the page does and `landed` runs (UI thread) when answers arrive.
void enrichStatsArtwork(std::vector<StatsTrack>& tracks, std::vector<StatsAlbum>& albums, std::vector<StatsArtist>& artists,
                        std::vector<std::vector<catalog::Image>>& artistPictures, Lifetime::Ref owner, std::function<void()> landed);
// An artist's genres as the Stats page's picture lookup found them (MusicBrainz tags); empty = unknown.
std::vector<std::string> statsArtistGenres(const catalog::ArtistRef& artist);

} // namespace st::app
