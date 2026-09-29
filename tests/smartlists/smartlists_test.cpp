// Developer test for the smart lists ("Senin için listeler", app/SmartLists), on synthetic histories (offline):
//   rules      each list's selection (month, discoveries, rediscover, forgotten likes, best, the years), minimum sizes,
//              only streams count, podcast / radio ids never appear
//   mixes      artists heard in the same sessions form one mix, mixes don't share artists, the lead's top song seeds it
//   seeds      a list is the same all day and changes with the day
//   fresh      mixFresh skips known songs and duplicates, blends one new song after every two, flags stay aligned
//   trim       a list emptied by the blocklist is hidden, caps hold
//   timing     300 000 plays over 20 000 songs build in time
//   smartlists_test
#include "app/SmartLists.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace st;
using namespace st::app;
using smart::Kind;
using smart::List;

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
constexpr int64_t kNow = 1'790'000'000;   // 2026-09-21 (UTC), well after the plays below

struct History {
    smart::Input in;
    uint32_t song(const std::string& artist, const std::string& name, const std::string& id = {}) {
        ListenStats::Track t;
        t.id = id.empty() ? "mb-" + artist + "-" + name : id;
        t.name = name;
        t.artists = {{"a-" + artist, artist}};
        t.albumName = artist + " album";
        t.durationMs = 200'000;
        in.tracks.push_back(t);
        return static_cast<uint32_t>(in.tracks.size() - 1);
    }
    // `times` streams of 3 minutes, one a day going back from `daysAgo`.
    void play(uint32_t track, int times, int64_t daysAgo, int64_t spacingDays = 1) {
        for (int i = 0; i < times; ++i) in.plays.push_back({track, kNow - (daysAgo + i * spacingDays) * kDay, 180'000, false});
    }
    void finish() {
        std::sort(in.plays.begin(), in.plays.end(), [](const auto& a, const auto& b) { return a.startedAt < b.startedAt; });
        in.now = kNow;
    }
};

const List* byId(const std::vector<List>& lists, const std::string& id) {
    for (const auto& l : lists)
        if (l.id == id) return &l;
    return nullptr;
}

bool has(const List& l, const std::string& name) {
    return std::any_of(l.tracks.begin(), l.tracks.end(), [&](const catalog::Track& t) { return t.name == name; });
}

void testRules() {
    std::printf("rules\n");
    CHECK(smart::build({}).empty());
    History h;
    // This month: 10 songs streamed 2+ times (the more the higher; each also once long ago, so not a new discovery),
    // one heard only once, one only skipped.
    for (int i = 0; i < 10; ++i) {
        const uint32_t s = h.song("Month", "m" + std::to_string(i));
        h.play(s, 2 + i, 1);
        h.play(s, 1, 200);
    }
    h.play(h.song("Month", "once"), 1, 2);
    const uint32_t skipped = h.song("Month", "skipped");
    for (int i = 0; i < 5; ++i) h.in.plays.push_back({skipped, kNow - (i + 1) * kDay, 10'000, false});
    // New discoveries: first heard 10 days ago, replayed (6 of them).
    for (int i = 0; i < 6; ++i) h.play(h.song("New", "n" + std::to_string(i)), 2, 3, 4);
    // Old favorites: 5+ streams, all more than 100 days ago (12 of them).
    for (int i = 0; i < 12; ++i) h.play(h.song("Old", "o" + std::to_string(i)), 5 + i, 120);
    // Not songs.
    const uint32_t podcast = h.song("Pod", "Episode", "podcast:1234567890abcdef");
    h.play(podcast, 20, 1);
    const uint32_t station = h.song("Radio", "Station", "radio:abc");
    h.play(station, 20, 1);
    h.finish();
    // Liked: 9 never played, one played a lot (not forgotten), a duplicate and an unplayable one.
    for (int i = 0; i < 9; ++i) {
        catalog::Track t;
        t.id = "spotify:track:liked" + std::to_string(i);
        t.name = "liked " + std::to_string(i);
        t.artists = {{"", "Likes"}};
        h.in.liked.push_back(t);
    }
    h.in.liked.push_back(h.in.liked[0]);
    catalog::Track loved;
    loved.id = "spotify:track:loved";
    loved.name = "m9";   // the same song as a history row (name + first artist)
    loved.artists = {{"spotify:artist:x", "Month"}};
    h.in.liked.push_back(loved);
    catalog::Track gone = h.in.liked[1];
    gone.name = "unplayable";
    gone.playable = false;
    h.in.liked.push_back(gone);

    const auto lists = smart::build(h.in);
    const List* month = byId(lists, "month");
    CHECK(month && month->tracks.size() == 16);                // + the new discoveries (streamed twice lately)
    CHECK(month && month->tracks.front().name == "m9");      // most streamed first
    CHECK(month && !has(*month, "once") && !has(*month, "skipped"));
    const List* disc = byId(lists, "discoveries");
    CHECK(disc && disc->tracks.size() == 6);
    CHECK(disc && !has(*disc, "m0"));   // first heard 200 days ago
    const List* red = byId(lists, "rediscover");
    CHECK(red && red->tracks.size() == 12);
    CHECK(red && std::all_of(red->tracks.begin(), red->tracks.end(), [](const auto& t) { return t.name[0] == 'o'; }));
    const List* forgot = byId(lists, "forgotten");
    CHECK(forgot && forgot->tracks.size() == 9);              // the duplicate once, the streamed one and the unplayable out
    CHECK(forgot && !has(*forgot, "m9") && !has(*forgot, "unplayable"));
    const List* best = byId(lists, "best");
    CHECK(best != nullptr);
    CHECK(best && !has(*best, "once"));
    bool clean = true;
    for (const auto& l : lists)
        for (const auto& t : l.tracks) clean = clean && t.id.rfind("podcast:", 0) != 0 && t.id.rfind("radio:", 0) != 0;
    CHECK(clean);
    // Recency: two songs with the same streams, the recent one ranks higher among the best.
    History r;
    const uint32_t oldSong = r.song("A", "old"), newSong = r.song("A", "new");
    r.play(oldSong, 10, 700);
    r.play(newSong, 10, 5);
    for (int i = 0; i < 10; ++i) r.play(r.song("B", "filler" + std::to_string(i)), 3, 300);
    r.finish();
    const auto rl = smart::build(r.in);
    const List* rb = byId(rl, "best");
    CHECK(rb && rb->tracks.size() >= 2 && rb->tracks[0].name == "new" && rb->tracks[1].name == "old");
    // Too little history: nothing but what reaches its minimum.
    History few;
    few.play(few.song("X", "a"), 3, 1);
    few.finish();
    CHECK(smart::build(few.in).empty());
}

void testMixes() {
    std::printf("mixes\n");
    History h;
    // Two listening worlds: A, B, C always in the same sessions; D, E, F in other sessions. G alone.
    std::vector<uint32_t> world1, world2;
    for (const char* a : {"A", "B", "C"})
        for (int i = 0; i < 6; ++i) world1.push_back(h.song(a, std::string(a) + std::to_string(i)));
    for (const char* a : {"D", "E", "F"})
        for (int i = 0; i < 6; ++i) world2.push_back(h.song(a, std::string(a) + std::to_string(i)));
    for (int day = 1; day <= 60; ++day) {
        const auto& w = day % 2 ? world1 : world2;
        int64_t t = kNow - day * kDay;
        for (size_t k = 0; k < 8; ++k) {
            const uint32_t s = w[(day * 7 + k * 5) % w.size()];
            h.in.plays.push_back({s, t, 180'000, false});
            t += 190;
        }
    }
    const uint32_t lead = world1[0];   // A0: A's top song, heard more (inside world 1's sessions: odd days)
    h.play(lead, 15, 1, 2);
    h.finish();
    const auto lists = smart::build(h.in);
    const List* m1 = byId(lists, "mix:1");
    const List* m2 = byId(lists, "mix:2");
    CHECK(m1 && m2);
    if (m1 && m2) {
        CHECK(m1->artists.size() == 3 && m1->artists[0] == "A");
        std::set<std::string> a1(m1->artists.begin(), m1->artists.end()), a2(m2->artists.begin(), m2->artists.end());
        CHECK((a1 == std::set<std::string>{"A", "B", "C"}));
        CHECK((a2 == std::set<std::string>{"D", "E", "F"}));
        CHECK(m1->tracks.size() >= smart::kMixMinFamiliar && m1->tracks.size() <= smart::kMixFamiliar);
        CHECK(m1->seedTrackId == h.in.tracks[lead].id);
        CHECK(m1->seedArtistId == "a-A");
        // Same artist rarely twice in a row.
        int repeats = 0;
        for (size_t i = 1; i < m1->tracks.size(); ++i)
            repeats += m1->tracks[i].artists[0].name == m1->tracks[i - 1].artists[0].name ? 1 : 0;
        CHECK(repeats <= 2);
    }
    CHECK(byId(lists, "mix:3") == nullptr);   // no third world
}

void testSeeds() {
    std::printf("seeds\n");
    History h;
    for (int i = 0; i < 40; ++i) h.play(h.song("Old", "o" + std::to_string(i)), 5 + i % 7, 150);
    h.finish();
    auto order = [](const std::vector<List>& ls) {
        std::vector<std::string> v;
        if (const List* l = byId(ls, "rediscover"))
            for (const auto& t : l->tracks) v.push_back(t.name);
        return v;
    };
    const auto a = order(smart::build(h.in));
    const auto b = order(smart::build(h.in));
    CHECK(!a.empty() && a == b);   // same day: same list
    h.in.now += kDay;
    const auto c = order(smart::build(h.in));
    CHECK(!c.empty() && c != a);   // the next day: another pick
    CHECK(smart::seedFor("mix:1", 1) != smart::seedFor("mix:2", 1));
    CHECK(smart::seedFor("mix:1", 1) != smart::seedFor("mix:1", 2));
}

void testYears() {
    std::printf("years\n");
    History h;
    // 2025 (Jan-Jun): 12 songs; 2024: 11 songs; 2023: 3 songs (too few). Timestamps in UTC.
    const int64_t y2025 = 1'735'689'600;   // 2025-01-01
    const int64_t y2024 = 1'704'067'200;   // 2024-01-01
    const int64_t y2023 = 1'672'531'200;   // 2023-01-01
    for (int i = 0; i < 12; ++i) {
        const uint32_t s = h.song("Y", "y25-" + std::to_string(i));
        for (int k = 0; k <= i; ++k) h.in.plays.push_back({s, y2025 + (i * 10 + k) * kDay, 180'000, false});
    }
    for (int i = 0; i < 11; ++i) h.in.plays.push_back({h.song("Y", "y24-" + std::to_string(i)), y2024 + i * kDay, 180'000, false});
    for (int i = 0; i < 3; ++i) h.in.plays.push_back({h.song("Y", "y23-" + std::to_string(i)), y2023 + i * kDay, 180'000, false});
    h.finish();
    const auto lists = smart::build(h.in);
    const List* l25 = byId(lists, "year:2025");
    for (const auto& l : lists)
        std::printf("        %s: %zu songs, first %s\n", l.id.c_str(), l.tracks.size(), l.tracks.empty() ? "-" : l.tracks[0].name.c_str());
    const List* l24 = byId(lists, "year:2024");
    CHECK(l25 && l25->number == 2025 && l25->tracks.size() == 12 && l25->tracks[0].name == "y25-11");
    CHECK(l24 && l24->tracks.size() == 11);
    CHECK(byId(lists, "year:2023") == nullptr);
    // Newest year first among the year lists.
    size_t i25 = 0, i24 = 0;
    for (size_t i = 0; i < lists.size(); ++i) {
        if (lists[i].id == "year:2025") i25 = i;
        if (lists[i].id == "year:2024") i24 = i;
    }
    CHECK(i25 < i24);
}

void testFreshAndTrim() {
    std::printf("fresh + trim\n");
    List mix;
    mix.id = "mix:1";
    mix.kind = Kind::Mix;
    mix.day = 20000;
    for (int i = 0; i < 12; ++i) {
        catalog::Track t;
        t.id = "f" + std::to_string(i);
        t.name = "familiar " + std::to_string(i);
        t.artists = {{"", "A"}};
        mix.tracks.push_back(t);
        mix.fresh.push_back(false);
    }
    std::vector<catalog::Track> cands;
    auto cand = [&](const std::string& name, const std::string& artist, const std::string& id = "sp") {
        catalog::Track t;
        t.id = id;
        t.name = name;
        t.artists = {{"", artist}};
        cands.push_back(t);
    };
    cand("familiar 3", "A");          // already in the mix
    cand("Known song", "B");          // in the history
    for (int i = 0; i < 20; ++i) cand("new " + std::to_string(i), "C");
    cand("new 0", "C");               // duplicate
    cand("Episode", "P", "podcast:1111111111111111");
    std::vector<std::wstring> known{smart::songKey(cands[1])};
    smart::mixFresh(mix, cands, known);
    size_t freshCount = 0;
    std::set<std::string> names;
    bool aligned = mix.fresh.size() == mix.tracks.size();
    for (size_t i = 0; i < mix.tracks.size(); ++i) {
        names.insert(mix.tracks[i].name);
        if (mix.fresh[i]) {
            ++freshCount;
            aligned = aligned && mix.tracks[i].name.rfind("new ", 0) == 0;
        }
    }
    CHECK(aligned);
    CHECK(freshCount == smart::kMixFresh);
    CHECK(names.size() == mix.tracks.size());   // no duplicates
    CHECK(!names.contains("Known song") && !names.contains("Episode"));
    CHECK(!mix.fresh[0] && !mix.fresh[1] && mix.fresh[2]);   // two familiar, then a new one
    // Trim: a mix needs its familiar songs; a list below its minimum is hidden.
    List small = mix;
    small.tracks.resize(20);
    small.fresh.resize(20);
    CHECK(smart::trim(small));
    List tiny;
    tiny.kind = Kind::Month;
    tiny.tracks.resize(smart::kMinSongs - 1);
    CHECK(!smart::trim(tiny));
    List big;
    big.kind = Kind::Month;
    big.tracks.resize(80);
    CHECK(smart::trim(big) && big.tracks.size() == smart::kMaxSongs && big.fresh.size() == smart::kMaxSongs);
}

void testTiming() {
    std::printf("timing\n");
    History h;
    constexpr int kSongs = 20'000, kPlays = 300'000;
    for (int i = 0; i < kSongs; ++i) h.song("Artist" + std::to_string(i % 1500), "Song " + std::to_string(i));
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < kPlays; ++i) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        const uint32_t s = static_cast<uint32_t>(x % 7 == 0 ? x % kSongs : x % 400);   // a popular head + a long tail
        h.in.plays.push_back({s, kNow - static_cast<int64_t>(kPlays - i) * 300, 150'000, false});
    }
    h.finish();
    const auto t0 = std::chrono::steady_clock::now();
    const auto lists = smart::build(h.in);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("        %zu lists from %d plays in %.0f ms\n", lists.size(), kPlays, ms);
    CHECK(!lists.empty());
#ifdef NDEBUG
    CHECK(ms < 1000);
#else
    CHECK(ms < 8000);   // Debug (no optimizations, checked iterators)
#endif
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testRules();
    testMixes();
    testSeeds();
    testYears();
    testFreshAndTrim();
    testTiming();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
