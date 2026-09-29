#pragma once
// Akıllı listeler ("Senin için listeler"): playlists computed on this PC from what the user really listened to
// (app/ListenStats: this PC's plays and the imported Spotify history) and their liked songs. The pure part lives here:
// build() turns a copy of the history into lists, mixFresh() blends recommendations into a daily mix. The app side
// (app/SmartListsPage.cpp: when to rebuild, the recommendations from Spotify / ListenBrainz, the cache file, the pages)
// calls them on a worker.
//
// The lists (a list with fewer songs than its minimum is left out):
//   month        Bu ayın favorileri       streamed at least twice in the last 30 days, most streamed first
//   best         Tüm zamanların en iyileri every stream counts, recent ones more (half-life one year)
//   rediscover   Tekrar keşfet            streamed 4+ times, not in the last 90 days (a new pick every day)
//   discoveries  Yeni keşiflerin          first heard in the last 30 days and streamed again since
//   forgotten    Unutulan beğeniler       liked songs streamed at most once and not in the last 60 days
//   mix:1..3     Günün karışımı N         an artist the user listens to now + the artists they play it with (the
//                                         same listening sessions), their familiar songs in a daily order; fresh
//                                         songs are added by the app (mixFresh)
//   year:<Y>     <Y> en iyileri           the most streamed songs of a local calendar year
// Only streams count (ListenStats' rule: >= 30 s, or half of a short track). Podcast episodes, radio stations and
// unnamed rows never appear. Songs are the history's distinct tracks (merged by name + first artist across sources),
// so a song shows up once. Orders that are not a ranking are shuffled with a seed of the local day: a list is stable
// for the day and changes the next.
//
// Standalone: core + catalog + app/ListenStats only (tests compile it directly). Thread-agnostic (pure).
#include "app/ListenStats.h"
#include "catalog/Models.h"

#include <cstdint>
#include <string>
#include <vector>

namespace st::app::smart {

enum class Kind { Month, Best, Rediscover, Discoveries, Forgotten, Mix, Year };

struct List {
    std::string id;                        // "month", "best", "rediscover", "discoveries", "forgotten", "mix:2", "year:2025"
    Kind kind = Kind::Month;
    int number = 0;                        // mix: 1..3; year: the year
    std::vector<catalog::Track> tracks;
    std::vector<bool> fresh;               // parallel to tracks (mixes): a recommendation, not heard before
    std::vector<std::string> artists;      // mix: its artists (the lead one first), for the title line
    std::string seedTrackId;               // mix: the lead artist's most streamed song (a Spotify radio seed when it is one)
    std::string seedArtistId;              // mix: the lead artist's id (a ListenBrainz seed when it is an MBID)
    int streams = 0;                       // the streams behind the list (the "why" line)
    int64_t day = 0;                       // local day it was built for (listen::dayNumber)
};

// Minimum songs for a list to be shown, and the most it holds.
inline constexpr size_t kMinSongs = 8;
inline constexpr size_t kMinDiscoveries = 5;
inline constexpr size_t kMaxSongs = 50;
inline constexpr size_t kMaxBest = 100;
inline constexpr size_t kMaxMixes = 3;
inline constexpr size_t kMixFamiliar = 30;     // familiar songs a mix takes at most
inline constexpr size_t kMixMinFamiliar = 10;  // a mix needs at least this many
inline constexpr size_t kMixFresh = 15;        // recommendations mixFresh() blends in at most
inline constexpr size_t kMaxYears = 6;         // newest years that get a list
inline constexpr size_t kMinYearSongs = 10;

struct Input {
    std::vector<ListenStats::Track> tracks;   // the history's distinct tracks (ListenStats::tracks())
    std::vector<ListenStats::Play> plays;     // oldest first (ListenStats::plays())
    std::vector<catalog::Track> liked;        // the user's liked songs (Spotify's or the local library's)
    int64_t now = 0;                          // unix seconds
    LocalClock clock;                         // local wall clock (days, years); empty = UTC
};

// Every list the history supports right now, in display order (months first, the years last). Songs are not
// filtered by the blocklist here: the caller does that (it lives on the UI thread) and then calls trim().
std::vector<List> build(const Input& in);

// Seed of a list for a day (stable within the day, different per list).
uint64_t seedFor(const std::string& listId, int64_t day);

// Adds up to kMixFresh `candidates` to a mix: songs the history doesn't know and the mix doesn't hold (by name + first
// artist), one after every two familiar songs, in a seeded order. `known` = ListenStats::keysFor(..).track of every
// song in the history (the caller builds it once).
void mixFresh(List& mix, const std::vector<catalog::Track>& candidates, const std::vector<std::wstring>& knownKeys);

// Caps a list after the caller dropped songs (blocklist): false when it fell below its minimum (hide it).
bool trim(List& list);

// Folded name + first artist key of a catalog track (ListenStats' song identity).
std::wstring songKey(const catalog::Track& t);

} // namespace st::app::smart
