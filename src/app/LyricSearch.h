#pragma once
// "Sözden şarkı bul" in the app (docs/DEVELOPMENT.md, "Lyrics tools"): a remembered line of lyrics -> songs of the
// active catalog. lyrics::searchGenius names the songs and the line each was found by; every one is then looked up in
// the active catalog (Spotify / YouTube Music / MusicBrainz, by title and artist) so it plays and has its menu like any
// search result. When Genius can't be asked (offline, a bot check), YouTube Music's song search for the line is the
// fallback (it resolves lyric fragments too, without naming the line). The Search page shows the hits in its "Sözler"
// tab; the command palette's "Sözden şarkı bul" opens that tab.
#include "catalog/Models.h"
#include "lyrics/LyricSearch.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <functional>
#include <string>
#include <vector>

namespace st::spotify {
class Api;
}
namespace st::ui {
class Column;
class Widget;
}

namespace st::app::lyricsearch {

// The Search page's route id prefix that opens the "Sözler" tab: Route{Search, "lyrics:<query>"}.
inline constexpr const char* kRoutePrefix = "lyrics:";

// A query that reads like a line of lyrics rather than a name (lyrics::looksLikeLyrics): the "Sözler" tab is offered.
inline bool looksLikeLyrics(const std::wstring& query) { return lyrics::looksLikeLyrics(query); }

struct Hit {
    enum class State {
        Found,       // `track` is the song in the active catalog
        Pending,     // its catalog lookup is still to come (resolve())
        Missing,     // looked up: the catalog doesn't have it
        NotLooked,   // not looked up (MusicBrainz' first four only): a catalog search may still find it
    };
    // The song in the active catalog; otherwise the source's title, artist and cover.
    catalog::Track track;
    State state = State::Pending;
    bool found() const { return state == State::Found; }
    lyrics::LyricMatch match;   // what the source matched (title, artist, the line and its highlighted words)
};
struct Answer {
    std::vector<Hit> hits;      // the source's order, at most 8
    std::string source;         // lyrics::kSourceGenius, or "ytmusic" (the fallback)
    bool failed = false;        // no source could be asked
    bool pending() const;       // some hits still wait for their catalog lookup
};

// Worker thread, in two steps so the page can show the songs before the catalog answers (MusicBrainz allows a request
// a second): search() asks the source (Genius, else YouTube Music) and gives its songs, Pending; resolve() looks them
// up in the active catalog in parallel (on MusicBrainz only the first four) and drops a second entry of one catalog
// song. `api`: the Spotify session captured on the UI thread (nullptr = the open catalog). Both throw
// OperationCanceledException when `ct` fires.
Answer search(const std::string& query, const YoutubeExplode::CancellationToken& ct = {});
void resolve(spotify::Api* api, Answer& a, const YoutubeExplode::CancellationToken& ct = {});

// The run's answers (12 queries, per catalog). UI thread.
const Answer* cached(const std::wstring& query);
void remember(const std::wstring& query, const Answer& a);

// The "Sözler" tab's content: a header (the source), then a row per hit - cover, title, artist, the line the words were
// found in with them highlighted - or the empty / failed state. Rows go to `nav` (arrow-key stops); Enter / a click
// plays the hit (with the other found hits after it), the context menu is the song's; a hit the catalog doesn't have
// runs `searchCatalog("<artist> <title>")`, a normal search for it.
void addResults(ui::Column* c, const std::wstring& query, const Answer& a, const std::function<void(ui::Widget*)>& nav,
                const std::function<void(const std::wstring&)>& searchCatalog, const std::function<void()>& retry);
// Placeholders in the rows' shape while the answer is on its way.
void addLoading(ui::Column* c);

} // namespace st::app::lyricsearch
