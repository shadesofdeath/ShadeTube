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
// searches 10 min), so Back and the page transitions don't wait for the network again, and the answers of album,
// artist and playlist pages on disk (see clearDiskCache).
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

enum class SearchKind { Songs, Albums, Artists, Playlists };

// The Search page: the unfiltered answer (its top result: SearchResults::topResult) and the first page of each kind
// (songs, albums, artists, playlists: the featured lists, then the community's), `limit` of each at most, in parallel.
// A kind whose search fails takes the overview's rows of it (everything failing throws the first error).
catalog::SearchResults search(const std::string& query, int limit = 20, const CT& ct = {});
// One request: the unfiltered answer only (a few rows of each kind and the top result), for live search as you type
// (the command palette).
catalog::SearchResults searchQuick(const std::string& query, int limit = 6, const CT& ct = {});
// Rows [offset, offset + count) of one kind, its pages followed (kept for 10 minutes per query, so paging on asks only
// for what is new). hasMore: the kind has rows after these.
struct SearchBatch {
    catalog::SearchResults results;
    bool hasMore = false;
};
SearchBatch searchMore(const std::string& query, SearchKind kind, int offset, int count, const CT& ct = {});
// One filtered search (a single request): artwork / id lookups that need one kind only.
catalog::SearchResults searchKind(const std::string& query, SearchKind kind, const CT& ct = {});
// "ytm:album:<id>": header + tracks (Album::releaseId = its audio playlist).
catalog::Album album(const std::string& id, const CT& ct = {});
// "ytm:artist:<id>": top songs (the first 20 of the artist's songs by popularity: a second request), the albums and
// singles & EPs the page shows (about ten each; albumsTotal / singlesTotal one more than shown when the page links the
// whole group), appears on, fans also like, featured playlists, the banner, monthly audience, subscribers and the
// description (Wikipedia's summary, credited, or the artist's own).
catalog::ArtistPage artistPage(const std::string& id, const CT& ct = {});
// The whole group from the artist's discography page ("More" of its albums / singles carousel; its pages followed).
// Compilations and appears-on have no such page: empty.
std::vector<catalog::Album> artistReleases(const std::string& id, catalog::ReleaseGroup group, const CT& ct = {});
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

// Test / diagnostics: requests sent since start, memory- and disk-cache hits.
struct Stats {
    int requests = 0;
    int cacheHits = 0;
    int diskHits = 0;
};
Stats stats();
void clearCache();       // the memory cache
// The disk cache (cache\ytmusic: albums 7 days, artists / discographies 1 day, playlists' first pages 6 hours, mood
// tiles 1 day; at most 32 MB, oldest first): cleared with the other caches in Ayarlar.
void clearDiskCache();
uint64_t diskCacheBytes();

} // namespace st::ytm
