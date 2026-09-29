#pragma once
// "Senin için listeler" (app/SmartLists) in the app: keeping the lists current, their generated covers, the rows on the
// Library and Home pages and the list page (Route{RouteKind::SmartList, <list id>}, "#play" appended = start playing).
//
// When: the lists are built on a worker from a copy of the listening history (app/ListenStats) and the liked songs,
// at the first use of the day, after the history changed (at most every 20 minutes), after a login / logout, and the
// blocklist is applied again whenever it changes. The newest build is kept in %LOCALAPPDATA%\ShadeTube\smart-lists.json
// (with the day it was built for), so a restart shows the same lists at once.
// Fresh songs for the daily mixes: logged in, Spotify's radio of the mix's lead song (read-only, two requests a mix);
// logged out, ListenBrainz's similar artists of the lead artist and their popular songs. Fetched once a day per lead
// artist and kept in the cache file. Logged in, the liked songs are Spotify's (the newest 500, fetched once a day);
// logged out, the local library's.
//
// UI thread only.
#include "app/SmartLists.h"
#include "core/Async.h"
#include "gfx/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::gfx {
class Canvas;
}
namespace st::ui {
class Column;
}

namespace st::app {

class Page;

namespace smart {

// The lists as last built, blocklist applied, in display order (empty until the first build / cache load).
const std::vector<List>& lists();
const List* find(const std::string& id);
// Loads the cache / rebuilds when due (see above). Cheap to call often.
void ensure();
// Changes (a build landed, covers found, the blocklist changed). Dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

std::wstring title(const List& l);      // "Günün karışımı 1", "2025 en iyileri"
std::wstring subtitle(const List& l);   // card line: "Tarkan, Sezen Aksu ve daha fazlası" / "42 şarkı"
std::wstring why(const List& l);        // page line: what it is made of and how often it changes

// The generated cover: a mosaic of the list's covers above a band with its name, in the list's tile color.
void drawCover(gfx::Canvas& c, const List& l, const gfx::Rect& r, float radius);

// A "Senin için listeler" row (section header + cards, `maxRows` rows of cards; 0 = all) that follows the lists by
// itself; hidden while there is none.
void addSection(ui::Column* c, int maxRows);

} // namespace smart

std::unique_ptr<Page> makeSmartListPage(const std::string& id);

// Covers for songs that have none (the imported Spotify history): filled from the Stats page's artwork cache, and up to
// `lookups` of the missing ones looked up (one request at a time); `landed` runs on the UI thread as answers arrive.
// Defined in app/StatsPage.cpp (it owns that cache).
void enrichTrackArtwork(std::vector<catalog::Track>& tracks, size_t lookups, Lifetime::Ref owner, std::function<void()> landed);

} // namespace st::app
