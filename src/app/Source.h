#pragma once
// The catalog "source" seam. ShadeTube shows Spotify data when the user is logged in and an open catalog otherwise:
// MusicBrainz or YouTube Music (Settings::openCatalog, picked on the connect screen or in Ayarlar). Pages don't branch
// on this themselves: they capture the active Spotify Api* on the UI thread (source::activeApi()) and call these
// dispatchers on a worker thread. Detail lookups route by id — a "spotify:*" URI goes to Spotify, a "ytm:*" id to
// YouTube Music, an MBID to MusicBrainz — so mixed histories keep working whichever catalog is active.
#include "catalog/Models.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <string>

namespace st::spotify {
class Api;
}

namespace st::app::source {

using CT = YoutubeExplode::CancellationToken;

enum class Catalog { Spotify, MusicBrainz, YouTubeMusic };

// UI-thread helpers.
bool loggedIn();               // a Spotify session is active
spotify::Api* activeApi();     // the logged-in Api, or nullptr; capture this before dispatching to a worker
Catalog active();              // Spotify while logged in, else the open catalog
bool ytmMode();                // logged out with YouTube Music as the open catalog
std::wstring sourceLabel();    // titlebar tag: "SPOTIFY · <name>", "AÇIK KATALOG · MUSICBRAINZ" / "· YOUTUBE MUSIC"
std::wstring catalogName();    // the open catalog's name: "MusicBrainz" / "YouTube Music"
// Settings::openCatalog changed (or was read at startup): mirrors it for the worker-thread dispatchers. A switch
// while logged out reopens the current page against the new catalog.
void applyOpenCatalog(bool reopen = false);

// Worker-thread dispatchers. `api` is the value captured by the page (nullptr = logged out => the open catalog,
// read from the mirror applyOpenCatalog() keeps).
catalog::SearchResults search(spotify::Api* api, const std::string& query, int limit, const CT& ct = {});
// Search as you type (the command palette): the same, except that YouTube Music asks one request instead of five.
catalog::SearchResults quickSearch(spotify::Api* api, const std::string& query, int limit, const CT& ct = {});
// Whether the open catalog is YouTube Music, for worker threads (the mirror applyOpenCatalog() keeps).
bool ytmCatalog();
catalog::Album album(spotify::Api* api, const std::string& id, const CT& ct = {});
catalog::ArtistPage artistPage(spotify::Api* api, const std::string& id, const CT& ct = {});
// The whole of one part of an artist's discography when artistPage gave only its first releases (its *Total > 0);
// empty for a source whose lists are already complete.
std::vector<catalog::Album> artistReleases(spotify::Api* api, const std::string& artistId, catalog::ReleaseGroup group,
                                           const CT& ct = {});

// True if this id belongs to Spotify (a "spotify:" URI) rather than an open catalog.
bool isSpotifyId(const std::string& id);
// True for a YouTube Music collection ("ytm:album:", "ytm:artist:", "ytm:playlist:").
bool isYtmId(const std::string& id);
// The web link of an album / artist / playlist id ("" when it has none: local lists) and its "copy" menu label.
std::wstring webLink(const std::string& kind, const std::string& id, const std::string& albumPlaylist = {});
std::wstring copyLinkLabel(const std::string& id);

} // namespace st::app::source
