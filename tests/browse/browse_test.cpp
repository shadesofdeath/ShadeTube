// browse_test: the logic behind the Search and Artist pages (app/BrowseModel), offline.
//   * recent searches: queries and opened items, equality (case, Turkish İ, spacing; kind + id), order, the cap,
//     remove / clear, persistence, the version 1 file, broken files;
//   * the top result: exact / prefix / word matches, the artist of the top songs, kinds, ranks, Turkish folding;
//   * the discography: groups, date / name order, the newest release, release kinds.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/BrowseModel.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace st;
using namespace st::app::browse;
using namespace st::catalog;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static std::filesystem::path tempDir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    auto dir = std::filesystem::path(buf) / (L"st-browse-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    return dir;
}

static void write(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static RecentItem item(const char* kind, const char* id, const wchar_t* title) {
    RecentItem it;
    it.kind = kind;
    it.id = id;
    it.title = title;
    return it;
}

static void testRecentSearches(const std::filesystem::path& dir) {
    std::printf("recent searches:\n");
    const auto file = dir / L"recent-searches.json";
    std::filesystem::remove(file);
    {
        RecentSearches r(file);
        CHECK(r.items().empty());
        r.recordQuery(L"  duman   kufi ");
        r.recordQuery(L"   ");                 // blank: ignored
        CHECK(r.items().size() == 1 && r.items()[0].title == L"duman kufi" && r.items()[0].isQuery());
        r.recordQuery(L"DUMAN  KUFİ");          // the same query (case, İ, spacing): moves to the front, newest text
        CHECK(r.items().size() == 1 && r.items()[0].title == L"DUMAN KUFİ");
        r.recordQuery(L"İstanbul");
        CHECK(RecentSearches::sameQuery(L"istanbul", L"İSTANBUL"));
        CHECK(!RecentSearches::sameQuery(L"ısık", L"isik"));   // ı stays its own letter
        // Items: kind + id; a query and an item with the same text are different entries.
        RecentItem artist = item("artist", "spotify:artist:1", L"Duman");
        artist.images = {{"https://i.scdn.co/a.jpg", 640, 640}};
        r.record(artist);
        r.record(item("album", "spotify:album:1", L"Kufi"));
        r.record(item("artist", "", L"no id"));   // dropped
        CHECK(r.items().size() == 4);
        CHECK(r.items()[0].kind == "album" && r.items()[1].kind == "artist" && r.items()[2].title == L"İstanbul");
        r.record(item("artist", "spotify:artist:1", L"Duman"));   // to the front again
        CHECK(r.items().size() == 4 && r.items()[0].kind == "artist" && r.items()[0].images.empty());
        // A song keeps what plays.
        RecentItem song = item("track", "spotify:track:9", L"Bal");
        song.subtitle = L"Duman";
        Track t;
        t.id = "spotify:track:9";
        t.name = "Bal";
        t.artists = {{"spotify:artist:1", "Duman"}};
        t.durationMs = 312000;
        song.track = t;
        r.record(song);
        CHECK(r.items().size() == 5 && r.items()[0].track && r.items()[0].track->durationMs == 312000);
        // Removing.
        CHECK(r.remove(item("album", "spotify:album:1", L"other title")));
        CHECK(!r.remove(item("album", "spotify:album:1", L"Kufi")));
        CHECK(r.remove(item("query", "", L"istanbul")));
        CHECK(r.items().size() == 3);
    }
    {
        RecentSearches r(file);   // read back
        const auto& it = r.items();
        CHECK(it.size() == 3);
        if (it.size() == 3) {
            CHECK(it[0].kind == "track" && it[0].subtitle == L"Duman" && it[0].track && it[0].track->name == "Bal" &&
                  it[0].track->artists.size() == 1);
            CHECK(it[1].kind == "artist" && it[1].id == "spotify:artist:1" && it[1].title == L"Duman");
            CHECK(it[2].isQuery() && it[2].title == L"DUMAN KUFİ" && it[2].id.empty());
        }
        // The cap: the oldest go.
        for (int i = 0; i < 20; ++i) r.recordQuery(L"q" + std::to_wstring(i));
        CHECK(r.items().size() == RecentSearches::kMax);
        CHECK(r.items().front().title == L"q19" && r.items().back().title == L"q8");
        r.clear();
        CHECK(r.items().empty());
    }
    {
        RecentSearches r(file);
        CHECK(r.items().empty());   // the clear was saved
    }
    // Version 1 (0.10): queries only, read as queries (duplicates and blanks dropped).
    write(file, R"({"v":1,"queries":["sezen aksu","Sezen Aksu","  ","tarkan"]})");
    {
        RecentSearches r(file);
        CHECK(r.items().size() == 2 && r.items()[0].title == L"sezen aksu" && r.items()[1].title == L"tarkan");
        CHECK(r.items()[0].isQuery());
        r.record(item("artist", "mbid-1", L"Tarkan"));
    }
    {
        std::ifstream f(file, std::ios::binary);
        const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(s.find("\"v\":2") != std::string::npos && s.find("queries") == std::string::npos);   // rewritten as v2
    }
    // Broken and odd files: nothing / what is usable; unknown kinds stay (a newer build's result kinds).
    write(file, "{not json");
    CHECK(RecentSearches(file).items().empty());
    write(file, R"({"v":2,"items":[{"k":"track","id":"x","t":"no track"},{"k":"podcast","id":"p1","t":"Show"},{"k":"album","t":"no id"},7]})");
    {
        RecentSearches r(file);
        CHECK(r.items().size() == 1 && r.items()[0].kind == "podcast" && r.items()[0].title == L"Show");
    }
    std::filesystem::remove(file);
}

static TopCandidate cand(const char* kind, size_t index, const wchar_t* name, const char* id = "") {
    return {kind, index, name, id};
}

static void testTopResult() {
    std::printf("top result:\n");
    CHECK(pickTopResult(L"x", {}, {}) == -1);
    // An artist named like the query beats a song of that name only when... it is a tie otherwise: artist first.
    CHECK(pickTopResult(L"duman", {cand("artist", 0, L"Duman"), cand("track", 0, L"Duman")}, {}) == 0);
    // MusicBrainz homonyms: the Duman who sings the top songs.
    CHECK(pickTopResult(L"duman", {cand("artist", 0, L"Duman", "kz"), cand("artist", 1, L"Duman", "tr"),
                                   cand("track", 0, L"Halimiz Duman")},
                        {"tr"}) == 1);
    // "sezen": the singer of the top song over a song titled exactly "Sezen".
    CHECK(pickTopResult(L"sezen", {cand("artist", 0, L"Sezen Aksu", "sa"), cand("track", 0, L"Sezen Aksu Ninni"),
                                   cand("track", 1, L"Sezen")},
                        {"sa"}) == 0);
    // Without that: the exact song.
    CHECK(pickTopResult(L"sezen", {cand("artist", 0, L"Sezen Aksu", "sa"), cand("track", 0, L"Sezen")}, {"other"}) == 1);
    // A longer query: the song that holds every word.
    CHECK(pickTopResult(L"halimiz duman", {cand("artist", 0, L"Duman"), cand("track", 0, L"Halimiz Duman"),
                                           cand("album", 0, L"Eski Köprünün Altında")},
                        {}) == 1);
    // Ranks break ties within a kind.
    CHECK(pickTopResult(L"kufi", {cand("album", 3, L"Kufi"), cand("album", 0, L"Kufi")}, {}) == 1);
    // Case, accents and Turkish ı / İ fold.
    CHECK(pickTopResult(L"ISIK", {cand("track", 0, L"Gölge"), cand("track", 1, L"Işık")}, {}) == 1);
    CHECK(pickTopResult(L"sebnem ferah", {cand("artist", 0, L"Şebnem Ferah")}, {}) == 0);
    // Nothing matches: the first candidate (the source's own order).
    CHECK(pickTopResult(L"zzz", {cand("artist", 0, L"A"), cand("track", 0, L"B")}, {}) == 0);
}

static Album rel(const char* name, const char* date, const char* type = "Album") {
    Album a;
    a.id = std::string("id:") + name;
    a.name = name;
    a.firstReleaseDate = date;
    a.primaryType = type;
    return a;
}

static std::string names(const std::vector<Album>& v) {
    std::string s;
    for (const auto& a : v) s += (s.empty() ? "" : ",") + a.name;
    return s;
}

static void testDiscography() {
    std::printf("discography:\n");
    ArtistPage p;
    CHECK(groupsOf(p).empty());
    CHECK(!newestRelease(p));
    p.albums = {rel("B", "2002-10-16"), rel("a", "2009"), rel("c", "")};
    p.singles = {rel("S1", "2026-06-19", "Single"), rel("S2", "2026-06", "Single")};
    p.other = {rel("Live", "2030-01-01", "Compilation")};   // newer, but not an album / single
    p.appearsOn = {rel("Va", "2012")};
    const auto g = groupsOf(p);
    CHECK(g.size() == 4 && g[0] == Group::Albums && g[1] == Group::Singles && g[2] == Group::Compilations &&
          g[3] == Group::AppearsOn);
    CHECK(&releasesOf(p, Group::Compilations) == &p.other);
    p.popular = {rel("Hit", "2004")};
    CHECK(groupsOf(p).front() == Group::Popular);

    auto list = p.albums;
    list.push_back(rel("d", "2002"));
    list.push_back(rel("e", "2002-10"));
    sortReleases(list, Order::Newest);
    CHECK(names(list) == "a,B,e,d,c");   // precise dates after the bare year; undated last
    sortReleases(list, Order::Oldest);
    CHECK(names(list) == "d,e,B,a,c");
    std::vector<Album> named{rel("album 10", ""), rel("Zeta", ""), rel("album 2", ""), rel("beta", "")};
    sortReleases(named, Order::Name);
    CHECK(names(named) == "album 2,album 10,beta,Zeta");
    CHECK(compareDates("2002", "2002-01") < 0 && compareDates("2003", "2002-12-31") > 0 && compareDates("", "1999") < 0 &&
          compareDates("2002-10", "2002-10") == 0);

    // The newest release: the source's latest, else the newest dated album / single.
    auto n = newestRelease(p);
    CHECK(n && n->name == "S1");
    p.latest = rel("Latest", "2020", "EP");
    n = newestRelease(p);
    CHECK(n && n->name == "Latest");

    CHECK(releaseKind("SINGLE") == "Single" && releaseKind("Single") == "Single" && releaseKind("ep") == "EP" &&
          releaseKind("COMPILATION") == "Compilation" && releaseKind("") == "Album" && releaseKind("Broadcast") == "Album");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const auto dir = tempDir();
    testRecentSearches(dir);
    testTopResult();
    testDiscography();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (g_failures == 0) std::printf("\nAll browse checks passed.\n");
    else std::printf("\n%d check(s) FAILED.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
