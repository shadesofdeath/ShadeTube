#pragma once
// YouTube Music as a catalog (docs/DEVELOPMENT.md, "YouTube Music catalog"): search, albums, artists, playlists, the
// Home shelves, moods & genres and radios from music.youtube.com, as the anonymous web client (WEB_REMIX, no Google
// account), through the app's YoutubeClient session (YoutubeExplode::Music::MusicClient: its cookies, visitor data and
// retries). Every track carries its video id ("yt:<id>", Track::videoId), so playback is pinned to exactly that
// recording: MatchService never searches for it.
//
// Answers are parsed by ytmusic/Parse (tolerant of shape changes). Requests use hl=en, so the few English words the
// parsers read are predictable; only the Home shelves and the mood tiles are asked for in the UI language (their titles
// are shown as they come). Parsed pages are kept in memory for a while (albums 30 min, artists / playlists 15 min,
// searches 10 min), so Back and the page transitions don't wait for the network again.
//
// Blocking and thread-safe: call from worker threads (st::async). Throws ApiError (HTTP status, 429 = rate limited),
// the YoutubeExplode transport exceptions and OperationCanceledException.
#include "catalog/Models.h"
#include "ytmusic/Parse.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace st::ytm {

using CT = YoutubeExplode::CancellationToken;

struct ApiError : std::runtime_error {
    int status = 0;
    ApiError(int s, const std::string& what) : std::runtime_error(what), status(s) {}
};

// The session every request goes through (the app passes MatchService's client; tests their own). Call before the
// first request; until then requests use a client of their own.
void setClient(YoutubeExplode::YoutubeClient client);

// Songs, albums, artists and playlists (featured ones first, then community lists), `limit` of each at most. The five
// filtered searches YouTube Music's own chips make run in parallel; one that fails leaves its kind empty (all failing
// throws the first error).
catalog::SearchResults search(const std::string& query, int limit = 20, const CT& ct = {});
// One filtered search (a single request): artwork / id lookups that need one kind only.
enum class SearchKind { Songs, Albums, Artists, Playlists };
catalog::SearchResults searchKind(const std::string& query, SearchKind kind, const CT& ct = {});
// "ytm:album:<id>": header + tracks (Album::releaseId = its audio playlist).
catalog::Album album(const std::string& id, const CT& ct = {});
// "ytm:artist:<id>": top songs (the first 20 of the artist's songs by popularity: a second request), albums, singles &
// EPs, related artists.
catalog::ArtistPage artistPage(const std::string& id, const CT& ct = {});
// "ytm:playlist:<id>": header + the first rows (about 100); playlistMore() continues with page.continuation.
PlaylistPage playlist(const std::string& id, const CT& ct = {});
PlaylistPage playlistMore(const PlaylistPage& previous, const CT& ct = {});
// Every row of a playlist (pages followed), at most `maxTracks` (download sync, imports, radio seeds).
std::vector<catalog::Track> playlistTracks(const std::string& id, size_t maxTracks = 5000, const CT& ct = {});
// Home: YouTube Music's home feed (two more pages of it), "New albums & singles" / trending of explore and the charts'
// playlists and top artists, titled in the UI language. Shelves that failed are left out; all failing throws.
std::vector<catalog::Shelf> home(const CT& ct = {});
// "Moods & genres" tiles (UI language) and the playlists of one of them.
std::vector<Mood> moods(const CT& ct = {});
std::vector<catalog::Shelf> moodShelves(const std::string& params, const CT& ct = {});
// The radio of a song ("yt:<id>"), an album, an artist or a playlist ("ytm:*"): YouTube Music's automix queue, the seed
// song first, up to `maxTracks` (continuations followed). Empty when YouTube Music has none.
std::vector<catalog::Track> radio(const std::string& seedId, size_t maxTracks = 100, const CT& ct = {});

// Test / diagnostics: requests sent since start and memory-cache hits.
struct Stats {
    int requests = 0;
    int cacheHits = 0;
};
Stats stats();
void clearCache();

} // namespace st::ytm
