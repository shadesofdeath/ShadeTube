// Developer test for the year in review ("Yılın özeti", app/YearReview) on fixed synthetic histories (offline):
//   periods     a calendar year, the last 12 months (month slots, days so far), a running year
//   numbers     a hand-made year: time, streams, tops and their order, months, weekdays, hours (a play across
//               midnight split), dayparts, weekly blocks, the streak, the busiest day, discoveries, the first stream
//   zones       local days on another time zone's clock (a New Year's Eve play belongs to the next year)
//   genres      shares, coverage and minimum rules, duplicate / case-folded tags
//   available   which periods qualify (minimum streams), order, an empty history
//   timing      300 000 plays over 20 000 songs
//   review_test
#include "app/YearReview.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace st;
using namespace st::app;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

constexpr int64_t kDay = 86'400;

int64_t utc(int y, int m, int d, int hh = 0, int mm = 0) { return listen::daysFromCivil(y, m, d) * kDay + hh * 3600 + mm * 60; }
int64_t day(int y, int m, int d) { return listen::daysFromCivil(y, m, d); }

catalog::Track song(const std::string& name, std::vector<std::string> artists, const std::string& album, int durationMs = 200'000) {
    catalog::Track t;
    t.id = "mb-" + name;
    t.name = name;
    for (auto& a : artists) t.artists.push_back({"id-" + a, a});
    t.album.id = "al-" + album;
    t.album.name = album;
    t.durationMs = durationMs;
    return t;
}

// A per-process scratch file (never written: the plays are added in memory).
std::filesystem::path scratchFile() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::filesystem::path(tmp) / (L"st-review-test-" + std::to_wstring(GetCurrentProcessId())) / L"listening.json";
}

const LocalClock kUtc = [](int64_t u) { return u; };

// ---- periods ------------------------------------------------------------------------------------------------------

void testPeriods() {
    std::printf("periods\n");
    const review::Period y = review::periodFor(2025, utc(2026, 11, 23, 12), kUtc);
    CHECK(y.year == 2025);
    CHECK(y.fromDay == day(2025, 1, 1));
    CHECK(y.toDay == day(2026, 1, 1));
    CHECK(y.today == day(2026, 11, 23));
    const review::Period r = review::periodFor(review::kRolling, utc(2026, 11, 23, 12), kUtc);
    CHECK(r.year == review::kRolling);
    CHECK(r.fromDay == day(2025, 12, 1));
    CHECK(r.toDay == day(2026, 11, 24));
    const review::Period jan = review::periodFor(review::kRolling, utc(2026, 1, 5), kUtc);
    CHECK(jan.fromDay == day(2025, 2, 1));
    const review::Period dec = review::periodFor(review::kRolling, utc(2026, 12, 31, 23), kUtc);
    CHECK(dec.fromDay == day(2026, 1, 1));

    ListenStats empty(scratchFile());
    const review::Review e = review::build(empty, r, kUtc);
    CHECK(e.streams == 0 && e.listenedMs == 0);
    CHECK(e.days == static_cast<int>(day(2026, 11, 24) - day(2025, 12, 1)));
    CHECK(e.months[0].year == 2025 && e.months[0].month == 12);
    CHECK(e.months[1].year == 2026 && e.months[1].month == 1);
    CHECK(e.months[11].year == 2026 && e.months[11].month == 11);
    CHECK(e.topMonth == -1 && e.topHour == -1 && e.topWeekday == -1 && e.topDaypart == -1);
    CHECK(e.busiestDay == -1 && e.longestStreak == 0 && !e.firstStream && !e.topNewArtist);
    CHECK(e.topTracks.empty() && e.topArtists.empty() && e.topAlbums.empty() && e.topList.empty());
    CHECK(e.weekMs.size() == static_cast<size_t>((e.days + 6) / 7));
    CHECK(review::available(empty, utc(2026, 11, 23), kUtc).empty());

    // A running year counts its days up to today.
    const review::Review running = review::build(empty, review::periodFor(2026, utc(2026, 3, 10, 8), kUtc), kUtc);
    CHECK(running.days == 31 + 28 + 10);
    const review::Review past = review::build(empty, review::periodFor(2024, utc(2026, 3, 10), kUtc), kUtc);
    CHECK(past.days == 366);
    const review::Review future = review::build(empty, review::periodFor(2027, utc(2026, 3, 10), kUtc), kUtc);
    CHECK(future.days == 0);
}

// ---- a hand-made year ---------------------------------------------------------------------------------------------

// 2024: B twice (so B and its artist Yara are not discoveries in 2025).
// 2025: A (Xan, album "First") 20 days in a row, Jan 1..20 at 10:00, 200 s each.
//       B (Yara, album "Second") 10 times on Mar 5 from 21:00, 180 s each (the busiest day).
//       C (Xan + Yara, album "First") 5 times, one a week from Jun 2 (Mondays) at 23:58, 240 s each (across midnight).
//       D (Zed) one 10 s skip on Jan 2 (listening time, not a stream).
// 2026: A 3 times (outside the year).
ListenStats& handMade() {
    static ListenStats s(scratchFile());
    static bool built = false;
    if (built) return s;
    built = true;
    const auto A = song("Alpha", {"Xan"}, "First"), B = song("Bravo", {"Yara"}, "Second", 180'000),
               C = song("Charlie", {"Xan", "Yara"}, "First", 240'000), D = song("Delta", {"Zed"}, "Fourth");
    s.addPlay(B, utc(2024, 6, 1, 12), 180'000);
    s.addPlay(B, utc(2024, 6, 2, 12), 180'000);
    for (int i = 0; i < 20; ++i) s.addPlay(A, utc(2025, 1, 1 + i, 10), 200'000);
    for (int i = 0; i < 10; ++i) s.addPlay(B, utc(2025, 3, 5, 21) + i * 200, 180'000);
    for (int i = 0; i < 5; ++i) s.addPlay(C, utc(2025, 6, 2 + 7 * i, 23, 58), 240'000);
    s.addPlay(D, utc(2025, 1, 2, 15), 10'000);
    for (int i = 0; i < 3; ++i) s.addPlay(A, utc(2026, 2, 1 + i, 9), 200'000);
    return s;
}

void testNumbers() {
    std::printf("numbers\n");
    const ListenStats& s = handMade();
    const review::Review r = review::build(s, review::periodFor(2025, utc(2026, 11, 23), kUtc), kUtc);
    const int64_t total = 20 * 200'000 + 10 * 180'000 + 5 * 240'000 + 10'000;
    CHECK(r.listenedMs == total);
    CHECK(r.streams == 35);
    CHECK(r.days == 365);
    CHECK(r.distinctTracks == 3);
    CHECK(r.distinctArtists == 2);
    CHECK(r.distinctAlbums == 2);
    // Tops (most streams first).
    CHECK(r.topTracks.size() == 3);
    CHECK(r.topTracks.size() == 3 && r.topTracks[0].track.name == "Alpha" && r.topTracks[0].streams == 20);
    CHECK(r.topTracks.size() == 3 && r.topTracks[1].track.name == "Bravo" && r.topTracks[1].streams == 10);
    CHECK(r.topTracks.size() == 3 && r.topTracks[2].track.name == "Charlie" && r.topTracks[2].listenedMs == 5 * 240'000);
    CHECK(r.topList.size() == 3 && r.topList[0].name == "Alpha");
    CHECK(r.topArtists.size() == 2);
    CHECK(r.topArtists.size() == 2 && r.topArtists[0].artist.name == "Xan" && r.topArtists[0].streams == 25);
    CHECK(r.topArtists.size() == 2 && r.topArtists[1].artist.name == "Yara" && r.topArtists[1].streams == 15);
    CHECK(r.topAlbums.size() == 2 && r.topAlbums[0].album.name == "First" && r.topAlbums[0].streams == 25);
    CHECK(r.topAlbums.size() == 2 && r.topAlbums[1].album.name == "Second" && r.topAlbums[1].artist == "Yara");
    // When: months, weekdays, hours, dayparts, weeks.
    CHECK(r.months[0].year == 2025 && r.months[0].month == 1);
    CHECK(r.months[0].ms == 20 * 200'000 + 10'000);
    CHECK(r.months[2].ms == 10 * 180'000);
    CHECK(r.months[5].ms == 5 * 240'000);   // the June 30 play spills into July 1 but stays on its start day
    CHECK(r.topMonth == 0);
    // 2025-01-01 was a Wednesday: Jan 1..20 = Wed x3, Thu x3, Fri x3, Sat x3, Sun x3, Mon x3, Tue x2.
    CHECK(listen::weekday(day(2025, 1, 1)) == 2);
    CHECK(listen::weekday(day(2025, 3, 5)) == 2 && listen::weekday(day(2025, 6, 2)) == 0);
    CHECK(r.weekdayMs[2] == 3 * 200'000 + 10 * 180'000);   // Wednesdays: Alpha 3x + Bravo's day
    CHECK(r.weekdayMs[0] == 3 * 200'000 + 5 * 240'000);    // Mondays: Alpha 3x + Charlie every Monday
    CHECK(r.weekdayMs[3] == 3 * 200'000 + 10'000);         // Thursday Jan 2: + the skip
    CHECK(r.weekdayMs[1] == 2 * 200'000);
    CHECK(r.topWeekday == 2);
    CHECK(r.hourMs[10] == 20 * 200'000);
    CHECK(r.hourMs[21] == 10 * 180'000);
    CHECK(r.hourMs[23] == 5 * 120'000);   // 23:58 + 240 s: 2 minutes before midnight...
    CHECK(r.hourMs[0] == 5 * 120'000);    // ...and 2 after
    CHECK(r.hourMs[15] == 10'000);
    CHECK(r.topHour == 10);
    CHECK(r.daypartMs[0] == 5 * 120'000);
    CHECK(r.daypartMs[1] == 20 * 200'000);
    CHECK(r.daypartMs[2] == 10'000);
    CHECK(r.daypartMs[3] == 10 * 180'000 + 5 * 120'000);
    CHECK(r.topDaypart == 1);
    CHECK(r.weekMs.size() == 53);
    CHECK(r.weekMs.size() == 53 && r.weekMs[0] == 7 * 200'000 + 10'000);
    CHECK(r.weekMs.size() == 53 && r.weekMs[2] == 6 * 200'000);
    int64_t weeks = 0;
    for (int64_t w : r.weekMs) weeks += w;
    CHECK(weeks == total);
    // Days.
    CHECK(r.activeDays == 20 + 1 + 5);
    CHECK(r.longestStreak == 20);
    CHECK(r.streakFromDay == day(2025, 1, 1) && r.streakToDay == day(2025, 1, 20));
    CHECK(r.busiestDay == day(2025, 3, 5));
    CHECK(r.busiestDayMs == 10 * 180'000);
    CHECK(r.busiestDayStreams == 10);
    // Discoveries: Alpha and Charlie are new, Bravo was heard in 2024; Xan is new, Yara is not, Zed never streamed.
    CHECK(r.newTracks == 2);
    CHECK(r.newArtists == 1);
    CHECK(r.topNewArtist && r.topNewArtist->artist.name == "Xan" && r.topNewArtist->streams == 25);
    CHECK(r.firstStream && r.firstStream->track.name == "Alpha" && r.firstStream->startedAt == utc(2025, 1, 1, 10));
    CHECK(r.topGenres.empty());   // no lookup given

    // 2024 alone: Bravo twice, below the minimum but still summarized.
    const review::Review y24 = review::build(s, review::periodFor(2024, utc(2026, 11, 23), kUtc), kUtc);
    CHECK(y24.streams == 2 && y24.newTracks == 1 && y24.newArtists == 1);
    CHECK(y24.longestStreak == 2 && y24.streakFromDay == day(2024, 6, 1));
    // The last 12 months from 2026-11-23: Alpha's 2026 plays only.
    const review::Review roll = review::build(s, review::periodFor(review::kRolling, utc(2026, 11, 23), kUtc), kUtc);
    CHECK(roll.streams == 3 && roll.newTracks == 0 && roll.newArtists == 0 && !roll.topNewArtist);
    CHECK(roll.months[2].year == 2026 && roll.months[2].month == 2 && roll.months[2].ms == 3 * 200'000);
    CHECK(roll.topMonth == 2);
}

// ---- time zones -----------------------------------------------------------------------------------------------------

void testZones() {
    std::printf("zones\n");
    const LocalClock berlin = timeZoneClock(L"W. Europe Standard Time");
    ListenStats s(scratchFile());
    const auto A = song("Alpha", {"Xan"}, "First");
    s.addPlay(A, utc(2025, 12, 31, 23, 30), 200'000);   // 00:30 on 2026-01-01 in Berlin
    s.addPlay(A, utc(2025, 12, 31, 22, 30), 200'000);   // 23:30 on 2025-12-31 in Berlin
    s.addPlay(A, utc(2025, 7, 1, 21, 30), 200'000);     // 23:30 in summer (UTC+2)
    const review::Review y25 = review::build(s, review::periodFor(2025, utc(2026, 3, 1), berlin), berlin);
    const review::Review y26 = review::build(s, review::periodFor(2026, utc(2026, 3, 1), berlin), berlin);
    CHECK(y25.streams == 2);
    CHECK(y26.streams == 1);
    CHECK(y25.hourMs[23] == 2 * 200'000);
    CHECK(y26.hourMs[0] == 200'000);
    CHECK(y25.months[11].ms == 200'000 && y25.months[6].ms == 200'000);
    CHECK(y26.newTracks == 0);   // first heard on 2025-12-31 local
    // Day boundaries back to UTC.
    CHECK(review::unixAtDay(day(2026, 1, 1), berlin) == utc(2025, 12, 31, 23));
    CHECK(review::unixAtDay(day(2026, 7, 1), berlin) == utc(2026, 6, 30, 22));
    CHECK(review::unixAtDay(day(2026, 7, 1), kUtc) == utc(2026, 7, 1));
}

// ---- genres ---------------------------------------------------------------------------------------------------------

void testGenres() {
    std::printf("genres\n");
    const ListenStats& s = handMade();
    const review::Period p = review::periodFor(2025, utc(2026, 11, 23), kUtc);
    // Xan: 25 streams over rock / indie / Rock (a duplicate) / folk (a fourth tag: only the first three count);
    // Yara: 15 streams of pop.
    const review::GenreLookup all = [](const catalog::ArtistRef& a) -> std::vector<std::string> {
        if (a.name == "Xan") return {"rock", "indie", "Rock", "folk", "jazz"};
        if (a.name == "Yara") return {"pop"};
        return {};
    };
    const review::Review r = review::build(s, p, kUtc, all);
    CHECK(r.topGenres.size() == 4);
    if (r.topGenres.size() == 4) {
        CHECK(r.topGenres[0].name == "pop");
        CHECK(std::abs(r.topGenres[0].share - 15.0 / 40.0) < 1e-9);
        CHECK(r.topGenres[1].name == "rock");   // ties keep the order the tags came in
        CHECK(std::abs(r.topGenres[1].share - 25.0 / 3.0 / 40.0) < 1e-9);
        CHECK(r.topGenres[2].name == "indie");
        CHECK(r.topGenres[3].name == "folk");
        double sum = 0;
        for (const auto& g : r.topGenres) sum += g.share;
        CHECK(std::abs(sum - 1.0) < 1e-9);
    }
    // Only Yara known: 15 / 40 streams covered, but one genre is too few.
    const review::Review few = review::build(s, p, kUtc, [](const catalog::ArtistRef& a) -> std::vector<std::string> {
        if (a.name == "Yara") return {"pop"};
        return {};
    });
    CHECK(few.topGenres.empty());
    // Only Yara known, with three genres: enough coverage (37.5 %).
    const review::Review yara = review::build(s, p, kUtc, [](const catalog::ArtistRef& a) -> std::vector<std::string> {
        if (a.name == "Yara") return {"pop", "dance", "electronic"};
        return {};
    });
    CHECK(yara.topGenres.size() == 3);
    // Below the coverage: a fringe artist of a bigger history.
    ListenStats big(scratchFile());
    for (int i = 0; i < 90; ++i) big.addPlay(song("Main", {"Main"}, "M"), utc(2025, 2, 1) + i * 600, 200'000);
    for (int i = 0; i < 10; ++i) big.addPlay(song("Side", {"Side"}, "S"), utc(2025, 3, 1) + i * 600, 200'000);
    const review::Review fringe = review::build(big, p, kUtc, [](const catalog::ArtistRef& a) -> std::vector<std::string> {
        if (a.name == "Side") return {"a", "b", "c"};
        return {};
    });
    CHECK(fringe.topGenres.empty());
}

// ---- available ------------------------------------------------------------------------------------------------------

void testAvailable() {
    std::printf("available\n");
    ListenStats s(scratchFile());
    const auto A = song("Alpha", {"Xan"}, "First");
    for (int i = 0; i < 40; ++i) s.addPlay(A, utc(2023, 5, 1) + i * kDay, 200'000);   // 2023: 40 streams
    for (int i = 0; i < 29; ++i) s.addPlay(A, utc(2024, 5, 1) + i * kDay, 200'000);   // 2024: 29, too few
    for (int i = 0; i < 60; ++i) s.addPlay(A, utc(2024, 5, 1, 12) + i * 60, 5'000);    // + skips: still too few
    for (int i = 0; i < 35; ++i) s.addPlay(A, utc(2025, 12, 1) + i * kDay, 200'000);  // Dec 2025 .. Jan 2026
    const auto a = review::available(s, utc(2026, 6, 1), kUtc);
    // Last 12 months (from 2025-07-01): 35; 2026: 4; 2025: 31; 2024: 29; 2023: 40.
    CHECK(a.size() == 3);
    CHECK(a.size() == 3 && a[0].year == review::kRolling && a[0].streams == 35);
    CHECK(a.size() == 3 && a[1].year == 2025 && a[1].streams == 31);
    CHECK(a.size() == 3 && a[2].year == 2023 && a[2].streams == 40);
    // A year later the rolling window holds nothing.
    const auto later = review::available(s, utc(2027, 6, 1), kUtc);
    CHECK(later.size() == 2 && later[0].year == 2025);
}

// ---- timing ---------------------------------------------------------------------------------------------------------

void testTiming() {
    std::printf("timing\n");
    // Built straight as an index + plays (ListenStats keeps only the newest 100 000 plays of this PC; imported
    // histories are this large).
    ListenStats::Index index;
    std::vector<uint32_t> songs;
    for (int i = 0; i < 20'000; ++i)
        songs.push_back(index.intern(ListenStats::fromCatalog(
            song("Song " + std::to_string(i), {"Artist " + std::to_string(i % 1500)}, "Album " + std::to_string(i % 4000)))));
    std::vector<ListenStats::Play> plays;
    uint64_t x = 88172645463325252ull;
    auto next = [&x] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    const int64_t start = utc(2022, 1, 1);
    std::vector<int64_t> times(300'000);
    for (auto& t : times) t = start + static_cast<int64_t>(next() % static_cast<uint64_t>(4 * 365 * kDay));
    std::sort(times.begin(), times.end());
    for (int64_t t : times)
        plays.push_back({songs[next() % songs.size()], t, 60'000 + static_cast<int64_t>(next() % 180'000), true});
    const LocalClock berlin = timeZoneClock(L"W. Europe Standard Time");
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    const auto a = review::available(index, plays, utc(2026, 1, 15), berlin);
    const double availMs = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    t0 = clk::now();
    const review::Review r = review::build(index, plays, review::periodFor(2024, utc(2026, 1, 15), berlin), berlin,
                                           [](const catalog::ArtistRef& ar) -> std::vector<std::string> {
                                               return {ar.name.size() % 2 ? "rock" : "pop", "indie"};
                                           });
    const double buildMs = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    std::printf("  available() %.1f ms, build() %.1f ms for 300 000 plays\n", availMs, buildMs);
    CHECK(a.size() >= 4);
    CHECK(r.streams > 70'000 && r.topTracks.size() == review::kTopN && r.topList.size() == review::kListN);
    CHECK(r.topGenres.size() == 3);
    CHECK(availMs < 2000 && buildMs < 3000);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testPeriods();
    testNumbers();
    testZones();
    testGenres();
    testAvailable();
    testTiming();
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall review checks passed\n");
    return 0;
}
