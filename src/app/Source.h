#pragma once
// The catalog "source" seam. ShadeTube shows Spotify data when the user is logged in and MusicBrainz's open
// catalog otherwise. Pages don't branch on this themselves: they capture the active Spotify Api* on the UI
// thread (source::activeApi()) and call these dispatchers on a worker thread. Detail lookups route by id —
// a "spotify:*" URI goes to Spotify, an MBID goes to MusicBrainz — so mixed histories keep working.
#include "catalog/Models.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <string>

namespace st::spotify {
class Api;
}

namespace st::app::source {

using CT = YoutubeExplode::CancellationToken;

// UI-thread helpers.
bool loggedIn();               // a Spotify session is active
spotify::Api* activeApi();     // the logged-in Api, or nullptr; capture this before dispatching to a worker
std::wstring sourceLabel();    // titlebar tag: "SPOTIFY · <name>" or "AÇIK KATALOG · MUSICBRAINZ"

// Worker-thread dispatchers. `api` is the value captured by the page (nullptr = logged out => MusicBrainz).
catalog::SearchResults search(spotify::Api* api, const std::string& query, int limit, const CT& ct = {});
catalog::Album album(spotify::Api* api, const std::string& id, const CT& ct = {});
catalog::ArtistPage artistPage(spotify::Api* api, const std::string& id, const CT& ct = {});

// True if this id belongs to Spotify (a "spotify:" URI) rather than a MusicBrainz MBID.
bool isSpotifyId(const std::string& id);

} // namespace st::app::source
