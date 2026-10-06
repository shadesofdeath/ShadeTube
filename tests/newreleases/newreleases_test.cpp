// newreleases_test: offline checks of the "Yeni çıkanlar" model (app/NewReleases): release dates as day numbers, the
// week math, merging fetched releases (dedup by id and of clean / explicit twins, the keep window, first sighting kept),
// the Bu hafta / Geçen hafta / Bu ay grouping and the album / single filter, the store's JSON round trip on disk (a
// per-process temp folder), and the unseen / notification rules.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/NewReleases.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace st;
using namespace st::app;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static catalog::Album album(std::string id, std::string name, std::string type, std::string date,
                            std::string artistId = "spotify:artist:a1", std::string artistName = "Artist") {
    catalog::Album a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.primaryType = std::move(type);
    a.firstReleaseDate = std::move(date);
    a.artists.push_back({std::move(artistId), std::move(artistName)});
    a.images.push_back({"https://i.scdn.co/image/x", 640, 640});
    return a;
}

static void testDays() {
    std::printf("day numbers:\n");
    using releases::dayNumber;
    CHECK(dayNumber("1970-01-01") == 0);
    CHECK(dayNumber("1970-01-02") == 1);
    CHECK(dayNumber("2026-10-06") == 20732);
    CHECK(dayNumber("2026-10") == dayNumber("2026-10-01"));
    CHECK(dayNumber("2026") == dayNumber("2026-01-01"));
    CHECK(dayNumber("") == -1);
    CHECK(dayNumber("2026-13-01") == -1);
    CHECK(dayNumber("2026-02-30") == -1);
    CHECK(dayNumber("20x6-10-06") == -1);
    CHECK(dayNumber("2026/10/06") == -1);
    // 2026-10-05 is a Monday, 2026-10-11 a Sunday.
    const int64_t mon = dayNumber("2026-10-05");
    CHECK(releases::weekStart(mon) == mon);
    CHECK(releases::weekStart(dayNumber("2026-10-11")) == mon);
    CHECK(releases::weekStart(dayNumber("2026-10-04")) == mon - 7);
    CHECK(releases::weekStart(0) == -3);   // 1970-01-01 (Thursday) -> Monday 1969-12-29
    CHECK(releases::localToday() > dayNumber("2026-01-01"));
}

static void testMerge() {
    std::printf("merge:\n");
    const int64_t today = releases::dayNumber("2026-10-06");
    std::vector<std::string> added;
    auto items = releases::merge({}, {album("spotify:album:1", "Gece", "Single", "2026-10-02"),
                                      album("spotify:album:2", "Uzun", "Album", "2026-09-12", "spotify:artist:b", "B"),
                                      album("spotify:album:3", "Çok eski", "Album", "2026-06-01"),
                                      album("spotify:album:4", "Tarihsiz", "Album", ""),
                                      album("", "Kimliksiz", "Album", "2026-10-01")},
                                 today, 1000, &added);
    CHECK(items.size() == 2);   // too old, undated and id-less releases are dropped
    CHECK(added.size() == 2 && added[0] == "spotify:album:1" && added[1] == "spotify:album:2");
    CHECK(items.size() == 2 && items[0].album.id == "spotify:album:1" && items[1].album.id == "spotify:album:2");   // newest first
    CHECK(items.size() == 2 && items[0].foundAt == 1000 && items[0].day == releases::dayNumber("2026-10-02"));

    // Again later: the known release keeps its first sighting but takes fresh metadata; the explicit twin (another
    // URI, same title / artist / type, released a day later) is not listed twice; a new single is added.
    added.clear();
    auto renamed = album("spotify:album:1", "Gece", "Single", "2026-10-02");
    renamed.totalTracks = 1;
    items = releases::merge(std::move(items),
                            {renamed, album("spotify:album:1x", "GECE ", "Single", "2026-10-03"),
                             album("spotify:album:5", "Gece", "Album", "2026-10-03"),
                             album("spotify:album:6", "Gece", "Single", "2026-10-04", "spotify:artist:other", "Other")},
                            today, 2000, &added);
    CHECK(items.size() == 4);
    CHECK(added.size() == 2 && added[0] == "spotify:album:5" && added[1] == "spotify:album:6");
    for (const auto& i : items)
        if (i.album.id == "spotify:album:1") CHECK(i.foundAt == 1000 && i.album.totalTracks == 1);
    CHECK(items.size() == 4 && items[0].album.id == "spotify:album:6");

    // kKeepDays later only what was released since 2026-10-02 + ... is kept: here the window starts at 10-04.
    items = releases::merge(std::move(items), {}, releases::dayNumber("2026-10-04") + releases::kKeepDays, 3000);
    CHECK(items.size() == 1 && items[0].album.id == "spotify:album:6");
    items = releases::merge(std::move(items), {}, today + releases::kKeepDays, 4000);
    CHECK(items.empty());
}

static void testGroups() {
    std::printf("groups and filters:\n");
    const int64_t today = releases::dayNumber("2026-10-07");   // a Wednesday; this week since Monday 10-05
    auto items = releases::merge({},
                                 {album("spotify:album:a", "Bu hafta tekli", "Single", "2026-10-05"),
                                  album("spotify:album:b", "Bu hafta albüm", "Album", "2026-10-06"),
                                  album("spotify:album:c", "Geçen hafta EP", "EP", "2026-10-04"),
                                  album("spotify:album:d", "Geçen hafta pazartesi", "Album", "2026-09-28"),
                                  album("spotify:album:e", "Bu ay", "Compilation", "2026-09-27"),
                                  album("spotify:album:f", "Ayın başı", "Single", "2026-09-06"),
                                  album("spotify:album:g", "Daha önce", "Album", "2026-09-05"),
                                  album("spotify:album:h", "Gelecek", "Single", "2026-10-09")},
                                 today, 1);
    using releases::Bucket;
    CHECK(releases::bucketOf(releases::dayNumber("2026-10-09"), today) == Bucket::ThisWeek);
    CHECK(releases::bucketOf(releases::dayNumber("2026-10-05"), today) == Bucket::ThisWeek);
    CHECK(releases::bucketOf(releases::dayNumber("2026-10-04"), today) == Bucket::LastWeek);
    CHECK(releases::bucketOf(releases::dayNumber("2026-09-28"), today) == Bucket::LastWeek);
    CHECK(releases::bucketOf(releases::dayNumber("2026-09-27"), today) == Bucket::ThisMonth);
    CHECK(releases::bucketOf(releases::dayNumber("2026-09-06"), today) == Bucket::ThisMonth);
    CHECK(releases::bucketOf(releases::dayNumber("2026-09-05"), today) == Bucket::Earlier);

    auto all = releases::group(items, releases::Filter::All, today);
    CHECK(all.size() == 4);
    if (all.size() == 4) {
        CHECK(all[0].bucket == Bucket::ThisWeek && all[0].items.size() == 3);
        CHECK(all[0].items[0]->album.id == "spotify:album:h");   // newest first inside a group
        CHECK(all[1].bucket == Bucket::LastWeek && all[1].items.size() == 2);
        CHECK(all[2].bucket == Bucket::ThisMonth && all[2].items.size() == 2);
        CHECK(all[3].bucket == Bucket::Earlier && all[3].items.size() == 1);
    }
    auto albums = releases::group(items, releases::Filter::Albums, today);
    size_t n = 0;
    for (const auto& g : albums)
        for (const auto* i : g.items) {
            ++n;
            CHECK(i->album.primaryType == "Album" || i->album.primaryType == "Compilation");
        }
    CHECK(n == 4);
    auto shorts = releases::group(items, releases::Filter::SinglesAndEps, today);
    n = 0;
    for (const auto& g : shorts) n += g.items.size();
    CHECK(n == 4 && shorts.size() == 3);   // nothing short in "Daha önce"
    CHECK(releases::group({}, releases::Filter::All, today).empty());
}

static void testStore() {
    std::printf("store, seen and notifications:\n");
    const fs::path dir = fs::temp_directory_path() / ("shadetube_newreleases_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "new-releases.json";

    const int64_t today = releases::dayNumber("2026-10-06");
    releases::Store s;
    s.user = "spotify:user:berkay";
    // First refresh: nothing notified, the last week counts as new.
    std::vector<std::string> added;
    s.items = releases::merge({}, {album("spotify:album:1", "Yeni", "Single", "2026-10-05"),
                                   album("spotify:album:2", "Geçen ay", "Album", "2026-09-10")},
                              today, 100, &added);
    s.fetchedAt = 100;
    releases::seedSeen(s, today);
    CHECK(s.seen.count("spotify:album:2") == 1 && s.seen.count("spotify:album:1") == 0);
    CHECK(releases::unseenCount(s, today) == 1);

    // Round trip (the seen id of a release that left the list is not written).
    s.seen.insert("spotify:album:gone");
    s.retryAt = 4242;
    CHECK(releases::save(s, file));
    CHECK(fs::exists(file) && !fs::exists(fs::path(file).concat(".tmp")));
    releases::Store back = releases::load(file);
    CHECK(back.user == s.user && back.fetchedAt == 100 && back.retryAt == 4242);
    CHECK(back.items.size() == 2);
    if (back.items.size() == 2) {
        CHECK(back.items[0].album.id == "spotify:album:1" && back.items[0].album.name == "Yeni");
        CHECK(back.items[0].album.primaryType == "Single" && back.items[0].album.firstReleaseDate == "2026-10-05");
        CHECK(back.items[0].album.artists.size() == 1 && back.items[0].album.artists[0].id == "spotify:artist:a1");
        CHECK(back.items[0].album.images.size() == 1 && back.items[0].album.images[0].width == 640);
        CHECK(back.items[0].foundAt == 100 && back.items[0].day == today - 1);
    }
    CHECK(back.seen.size() == 1 && back.seen.count("spotify:album:2") == 1);

    // A later refresh: a fresh release is notifiable, an old one Spotify surfaced late is not.
    added.clear();
    back.items = releases::merge(std::move(back.items), {album("spotify:album:3", "Bugün", "EP", "2026-10-06"),
                                                         album("spotify:album:4", "Geç gelen", "Album", "2026-09-20")},
                                 today, 200, &added);
    CHECK(added.size() == 2);
    const auto note = releases::notifiable(back, added, today);
    CHECK(note.size() == 1 && note[0]->album.id == "spotify:album:3");
    CHECK(releases::unseenCount(back, today) == 2);   // 1 and 3 (4 is older than kRecentDays)
    releases::markAllSeen(back);
    CHECK(releases::unseenCount(back, today) == 0);
    CHECK(releases::notifiable(back, added, today).empty());

    // Missing / broken files load as an empty store.
    CHECK(releases::load(dir / "missing.json").items.empty());
    { std::FILE* f = _wfopen((dir / L"broken.json").c_str(), L"wb"); if (f) { std::fputs("{nope", f); std::fclose(f); } }
    CHECK(releases::load(dir / "broken.json").user.empty());
    back.clear();
    CHECK(back.items.empty() && back.user.empty() && back.fetchedAt == 0);
    fs::remove_all(dir, ec);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testDays();
    testMerge();
    testGroups();
    testStore();
    if (g_failures == 0) std::printf("\nAll new releases checks passed.\n");
    else std::printf("\n%d check(s) FAILED.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
