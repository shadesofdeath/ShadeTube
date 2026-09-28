#pragma once
// Podcast pieces other screens use (implemented in app/PodcastsPage.cpp): the episode menu, the episode's show page,
// the show notes Now Playing draws in the lyrics' place and the new-episode count of the sidebar entry.
#include "app/Components.h"
#include "catalog/Models.h"

namespace st::app {

// Menu of a podcast episode (play, queue, download, played, notes, the show). Tracks from lists that mix songs and
// episodes go through showTrackMenu(), which sends a lone episode here.
void showEpisodeMenu(const catalog::Track& episode, gfx::Point windowPos);
// Opens the show an episode belongs to.
void openEpisodeShow(const catalog::Track& episode);
// Now Playing: the playing episode's show notes (a mono label and the text, wrapped and faded out at the bottom).
void paintEpisodeNotes(Canvas& c, const Rect& r, const catalog::Track& episode);
// Episodes of the subscriptions published since the user last looked (sidebar badge).
int podcastNewEpisodeCount();
// Dev (--play-episode "<feed URL>[#n][@sec]"): plays episode n (0 = newest) of a feed, seeking to sec once it plays.
void devPlayEpisode(const std::string& spec);

} // namespace st::app
