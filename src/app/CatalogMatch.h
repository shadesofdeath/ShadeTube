#pragma once
// Songs read from another service (app/ServiceImport) -> songs of the active catalog (docs/DEVELOPMENT.md, "Import from
// Deezer, Apple Music and TIDAL").
//
//   Spotify        an ISRC search first ("isrc:<code>", exact), else a search for title + artist; ~100 ms apart.
//   YouTube Music  a songs search for title + artist (YouTube Music has no ISRCs), four at a time.
//   MusicBrainz    the ISRCs 20 per search (mb::recordingsByIsrc), then a search by name for the rest (at most
//                  kMaxNameSearches: MusicBrainz allows one request a second).
// Every candidate is scored against the song (score(): title without its version / featuring noise, artists, length);
// a search hit counts from kAccept, an ISRC hit needs only to look like the song (kAcceptIsrc: the code is exact, the
// spelling may differ). A song no catalog result fits stays a song known by name ("import:<hash>", like a CSV row:
// MatchService finds its video when it plays).
//
// Standalone (core + catalog + spotify + ytmusic + musicbrainz + app/PlaylistIO): tests/services compiles it.
#include "app/ServiceImport.h"
#include "catalog/Models.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace st::spotify {
class Api;
}

namespace st::app::catmatch {

using CT = YoutubeExplode::CancellationToken;

enum class Catalog { Spotify, YouTubeMusic, MusicBrainz };
const wchar_t* catalogName(Catalog c);   // L"Spotify", L"YouTube Music", L"MusicBrainz"

inline constexpr double kAccept = 70;      // a search hit
inline constexpr double kAcceptIsrc = 40;  // an ISRC hit
inline constexpr int kMaxNameSearches = 150;   // MusicBrainz, per import

// ---- Scoring (pure) ---------------------------------------------------------------------------------------------------
// 0..100: title 50 (core words, Dice), artists 30 (the best pair, words of the smaller name found in the other), length
// 20 (exact within 2 s, nothing beyond 20 s; unknown = 12). A wrong title halves it, a wrong artist takes 40 %, a
// live / remix / acoustic / instrumental / karaoke / cover / sped-up version on one side only costs 35.
double score(const svc::Song& want, const catalog::Track& candidate);
// The best candidate scoring at least `minScore`; -1 when none. `bestScore` gets its score (or the best seen).
int best(const svc::Song& want, const std::vector<catalog::Track>& candidates, double minScore, double* bestScore = nullptr);
// Albums: title 60 + artist 40 (same rules), at least kAccept.
double albumScore(const std::string& title, const std::string& artist, const catalog::Album& candidate);

// Words of a title / name: folded (case, accents, Turkish I), split at everything that isn't a letter or digit.
// coreWords drops bracketed parts and " - " suffixes that only say which edition it is (remastered, feat., radio edit,
// version, year...) and keeps the words that make it another recording (live, remix...) in `flags`.
std::vector<std::wstring> words(std::string_view text);
std::vector<std::wstring> coreWords(std::string_view title, std::vector<std::wstring>* flags = nullptr);

// A song known by name only: "import:<hash of title + artist>" with its artists, album, cover and length.
catalog::Track nameOnly(const svc::Song& s);

// ---- Lookups (blocking, worker threads) ---------------------------------------------------------------------------------
struct Result {
    catalog::Track track;   // the catalog's song, or nameOnly() when not found
    bool found = false;
    bool byIsrc = false;
};

struct Counters {           // read by the UI while matching runs
    std::atomic<int> done{0};
    std::atomic<int> found{0};
    std::atomic<int> byIsrc{0};
};

// `api`: the logged-in Spotify Api (Catalog::Spotify only). Results in the songs' order. A lookup that fails (network)
// leaves its song not found; after many failures in a row the rest are given up the same way (`interrupted`).
std::vector<Result> matchAll(const std::vector<svc::Song>& songs, Catalog catalog, spotify::Api* api, Counters& counters,
                             const CT& ct, bool* interrupted = nullptr);

// The active catalog's album for one read from a service (by barcode where the catalog can, else by title and artist):
// its id ("spotify:album:…", "ytm:album:…", a release-group MBID), "" when none fits.
std::string findAlbum(const svc::List& album, Catalog catalog, spotify::Api* api, const CT& ct);

} // namespace st::app::catmatch
