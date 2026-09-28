#pragma once
// Opening pasted links (app/Links): the search box (Enter or the hint row) and Ctrl+V outside a text field. UI thread.
//   Spotify / MusicBrainz album, playlist, artist -> that page (Spotify needs a session: logged out it asks to connect)
//   Spotify track / MusicBrainz recording         -> plays in its album (from that song) and shows the album; alone
//                                                     when the album can't be loaded
//   YouTube video                                 -> plays as a single song pinned to exactly that video (never
//                                                     re-matched), titled from the video ("Artist - Title")
//   Unsupported                                   -> a toast; nothing is searched
#include "app/Links.h"

#include <string>

namespace st::app {

// What the hint row names the link ("Spotify albümü", "YouTube videosu"...). "" for Kind::None.
std::wstring linkLabel(const links::Link& link);
// What opening it does ("Albümü aç", "Şarkıyı çal", "Videoyu şarkı olarak çal"...). "" for None / Unsupported.
std::wstring linkAction(const links::Link& link);
// Icon for the hint row ("spotify-link", "youtube-source", "link").
const char* linkIcon(const links::Link& link);
// True when opening needs a Spotify session that isn't there.
bool linkNeedsSpotify(const links::Link& link);

// Opens `link` (see above). False only for Kind::None: the caller searches for the text instead.
bool openLink(const links::Link& link);
// Ctrl+V outside a text field: opens the link on the clipboard. False when the clipboard holds no link at all.
bool openClipboardLink();

} // namespace st::app
