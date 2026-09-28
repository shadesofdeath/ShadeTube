#pragma once
// Open music catalog client: MusicBrainz (metadata), Cover Art Archive (covers), ListenBrainz (popularity,
// similar artists, fresh releases) and Wikidata/Wikimedia Commons (artist photos). All public APIs, no keys.
//
// Etiquette (enforced here):
//  - MusicBrainz: max 1 request/second per client (global rate limiter shared by all threads), a meaningful
//    User-Agent "ShadeTube/<ver> ( <contact> )", 503 -> back off and retry.
//  - Responses are cached on disk (cache/mb/<hash>.json) with TTLs (lookups 7 days, searches 1 day,
//    ListenBrainz stats 12 hours) so the app feels instant and stays far below the limits.
//
// Blocking and thread-safe: call from worker threads (st::async).
#include "catalog/Models.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace st::mb {

using CT = YoutubeExplode::CancellationToken;
using namespace st::catalog;

struct ApiError : std::runtime_error {
    int status = 0;
    ApiError(int s, const std::string& what) : std::runtime_error(what), status(s) {}
};

// --- MusicBrainz ------------------------------------------------------------------------------------------
// Search (Lucene query built from free text). Albums = release groups (official Album/EP/Single).
SearchResults search(const std::string& query, int limit = 12, const CT& ct = {});
std::vector<Artist> searchArtists(const std::string& query, int limit = 20, const CT& ct = {});
std::vector<Album> searchAlbums(const std::string& query, int limit = 20, const CT& ct = {});
std::vector<Track> searchTracks(const std::string& query, int limit = 30, const CT& ct = {});

Artist artist(const std::string& artistId, const CT& ct = {});           // + tags, + Commons image
// All release groups of an artist (paged internally), newest first, split by type in ArtistPage.
std::vector<Album> artistReleaseGroups(const std::string& artistId, const CT& ct = {});
// Release group + tracklist of its most representative official release (prefers the earliest official
// release in the user's region "XW"/"TR"/any, CD/Digital over vinyl, fewest bonus tracks).
Album album(const std::string& releaseGroupId, const CT& ct = {});
// The release group of one release (a pasted musicbrainz.org/release/ link). "" when MusicBrainz names none.
std::string releaseGroupOfRelease(const std::string& releaseId, const CT& ct = {});
// One recording as a Track: title, artist credit, length and its best (official, album first) release group as the
// album. Throws ApiError (404 for an unknown MBID).
Track recording(const std::string& recordingId, const CT& ct = {});

// --- ListenBrainz ----------------------------------------------------------------------------------------
std::vector<Track> topTracksForArtist(const std::string& artistId, int limit = 10, const CT& ct = {});
std::vector<Artist> similarArtists(const std::string& artistId, int limit = 12, const CT& ct = {});
std::vector<Artist> trendingArtists(const std::string& range = "week", int limit = 20, const CT& ct = {});
std::vector<Album> trendingAlbums(const std::string& range = "week", int limit = 20, const CT& ct = {});
std::vector<Track> trendingTracks(const std::string& range = "week", int limit = 30, const CT& ct = {});
std::vector<Album> freshReleases(int days = 14, int limit = 30, const CT& ct = {});

// Optional ListenBrainz user token (https://listenbrainz.org/settings/). Since 2026 LB requires a token for
// /1/popularity/top-recordings-for-artist; without one topTracksForArtist() ranks the artist's MusicBrainz
// recordings by LB's public bulk popularity endpoint instead. Also read from env ST_LISTENBRAINZ_TOKEN.
void setListenBrainzToken(std::string token);

// --- Composite ------------------------------------------------------------------------------------------
ArtistPage artistPage(const std::string& artistId, const CT& ct = {});

} // namespace st::mb
