#pragma once
// Year in review ("Yılın özeti"): one local calendar year, or the last 12 months, of the listening history
// (app/ListenStats: this PC's plays and the imported Spotify history) boiled down to what the story view
// (app/YearReviewView) shows: the time listened, the top songs / artists / albums, genres when they are known, when in
// the year and the day the music played, the longest streak, the busiest day and the discoveries.
//
// The rules are ListenStats': the listening time counts every play, everything else counts streams only (>= 30 s, or
// half of a short track); songs, artists and albums are the history's groups (merged by name across sources). Days,
// months and hours are on the local wall clock (LocalClock). A discovery is a song / artist whose first stream ever is
// in the period. The play that is still running is not included.
//
// Standalone: core + catalog + app/ListenStats only (review_test compiles it directly). Pure, any thread.
#include "app/ListenStats.h"
#include "catalog/Models.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st::app::review {

inline constexpr int kRolling = 0;          // Period::year of "the last 12 months"
inline constexpr int kMinStreams = 30;      // a period needs this many streams for a review
inline constexpr size_t kTopN = 5;          // songs / artists / albums on the cards
inline constexpr size_t kListN = 50;        // the songs "Play your top songs" takes (when there is no smart list)
inline constexpr size_t kMaxGenres = 5;
inline constexpr size_t kGenresPerArtist = 3;      // an artist's first (most used) tags count
inline constexpr double kMinGenreCoverage = 0.25;  // artists with known genres must hold this share of the streams
inline constexpr size_t kMinGenres = 3;            // ... and name at least this many genres

// A period on the local calendar: days [fromDay, toDay) (listen::dayNumber of local seconds).
struct Period {
    int year = kRolling;      // the calendar year, or kRolling
    int64_t fromDay = 0;
    int64_t toDay = 0;        // exclusive
    int64_t today = 0;        // the local day it was made on (days after it do not count yet)
};
// `year`'s period, or (kRolling) the 12 calendar months ending with the current one: 2026-11-23 -> 2025-12-01 ..
// 2026-11-23.
Period periodFor(int year, int64_t unixNow, const LocalClock& clock);

struct Genre {
    std::string name;         // as the first artist that had it spelled it
    double share = 0;         // of the streams of the artists with known genres (0..1)
};
struct MonthSlot {
    int year = 0, month = 0;  // month 1..12
    int64_t ms = 0;
};

struct Review {
    Period period;
    int64_t listenedMs = 0;              // every play
    int streams = 0;
    int days = 0;                        // days of the period so far (a running year: up to today)
    int activeDays = 0;                  // days with a stream
    int distinctTracks = 0, distinctArtists = 0, distinctAlbums = 0;
    std::vector<StatsTrack> topTracks;   // most streamed first (ties: more time, then more recent), kTopN
    std::vector<StatsArtist> topArtists;
    std::vector<StatsAlbum> topAlbums;
    std::vector<catalog::Track> topList; // the kListN most streamed songs
    std::vector<Genre> topGenres;        // empty: not enough genre data (the card is skipped)
    std::array<MonthSlot, 12> months{};  // oldest first (a year: January .. December)
    int topMonth = -1;                   // index into months, -1 = no listening
    std::array<int64_t, 24> hourMs{};    // listening per local hour (a play split across the hours it spans)
    int topHour = -1;
    std::array<int64_t, 4> daypartMs{};  // night 0-6, morning 6-12, afternoon 12-18, evening 18-24
    int topDaypart = -1;
    std::array<int64_t, 7> weekdayMs{};  // 0 = Monday
    int topWeekday = -1;
    std::vector<int64_t> weekMs;         // listening per 7 days from fromDay (the last block may be shorter)
    int longestStreak = 0;               // most consecutive days with a stream (the earliest such run)
    int64_t streakFromDay = 0, streakToDay = 0;
    int64_t busiestDay = -1;             // the day with the most listening, -1 = none
    int64_t busiestDayMs = 0;
    int busiestDayStreams = 0;
    int newTracks = 0, newArtists = 0;   // first streamed ever in this period
    std::optional<StatsArtist> topNewArtist;   // the most streamed of the new artists
    std::optional<StatsPlay> firstStream;      // the period's first stream
};

// Genres of an artist (its tags, the most used first); empty = unknown. Called once per artist with a stream.
using GenreLookup = std::function<std::vector<std::string>(const catalog::ArtistRef& artist)>;

Review build(const ListenStats::Index& index, const std::vector<ListenStats::Play>& plays, const Period& period,
             const LocalClock& clock, const GenreLookup& genres = {});
inline Review build(const ListenStats& stats, const Period& period, const LocalClock& clock, const GenreLookup& genres = {}) {
    return build(stats.index(), stats.plays(), period, clock, genres);
}

struct Available {
    int year = kRolling;      // kRolling = the last 12 months
    int streams = 0;
};
// The periods with at least kMinStreams streams: the last 12 months first, then the calendar years, newest first.
// Cheap: one pass over the plays, no clock call per play.
std::vector<Available> available(const ListenStats::Index& index, const std::vector<ListenStats::Play>& plays,
                                 int64_t unixNow, const LocalClock& clock);
inline std::vector<Available> available(const ListenStats& stats, int64_t unixNow, const LocalClock& clock) {
    return available(stats.index(), stats.plays(), unixNow, clock);
}

// Unix seconds (UTC) of the local midnight starting `day` (the instant the clock maps there; DST gaps move it to the
// first instant after). For the period's play range.
int64_t unixAtDay(int64_t day, const LocalClock& clock);

} // namespace st::app::review
