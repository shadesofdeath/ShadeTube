// Developer test for the Library page's views (app/LibrarySort), offline:
//   ids        settings values of tabs / sorts / views, the sorts each tab offers, fallbacks for unknown values
//   recents    recentsTime(): local items, Spotify items with and without Spotify's played times
//   sorts      every key (Son çalınanlar, Son eklenenler, Alfabetik, Oluşturan / Sanatçı), pinned items, undated items
//              in source order, stable ties, Turkish collation, digits as numbers
//   folders    in place under Recents / Added (empty folders last), first under Alpha / Creator, a folder's own level,
//              nesting, expanded folders (sidebar), unknown folders, a broken parent chain
//   filter     Turkish İ / ı, accents and case, word starts for short queries, creator matches rank lower, ties in sort
//              order, everything under the folder (folders included)
//   store      library-recents.json: tracked contexts, newer times only, round trip, garbage, the cap, the file
//   history    PlayRecency: albums by id and by name + artist, artists by name, streams only
//   timing     10 000 items (200 folders): model, every sort, filters
//   library_test
#include "app/LibrarySort.h"
#include "core/Utf.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace st;
using namespace st::app;
using namespace st::app::libview;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
fs::path g_dir;

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
constexpr int64_t kNow = 1'794'000'000;   // 2026-11-06 (UTC)

Item playlist(std::string id, std::wstring name, int64_t recent = 0, int64_t added = 0, std::wstring owner = {},
              std::string folder = {}) {
    Item it;
    it.kind = Item::Kind::Playlist;
    it.id = std::move(id);
    it.name = std::move(name);
    it.recent = recent;
    it.added = added;
    it.creator = std::move(owner);
    it.folder = std::move(folder);
    return it;
}

Item folderItem(std::string id, std::wstring name, std::string parent = {}) {
    Item it;
    it.kind = Item::Kind::Folder;
    it.id = std::move(id);
    it.name = std::move(name);
    it.folder = std::move(parent);
    return it;
}

// The rows' ids, space separated (folders as "[id]").
std::string ids(const Model& m, const std::vector<Row>& rows) {
    std::string s;
    for (const auto& r : rows) {
        const auto& it = m.items()[r.item];
        if (!s.empty()) s += ' ';
        s += it.kind == Item::Kind::Folder ? "[" + it.id + "]" : it.id;
    }
    return s;
}

std::string ordered(const Model& m, Sort s, std::wstring filter = {}, std::string root = {}) {
    Query q;
    q.sort = s;
    q.filter = std::move(filter);
    q.root = std::move(root);
    return ids(m, m.order(q));
}

void testIds() {
    std::printf("ids\n");
    for (Sort s : {Sort::Recents, Sort::Added, Sort::Alpha, Sort::Creator}) CHECK(parseSort(sortId(s)) == s);
    CHECK(parseView(viewId(View::List)) == View::List && parseView(viewId(View::Grid)) == View::Grid);
    CHECK(std::string(tabId(Tab::Playlists)) == "playlists" && std::string(tabId(Tab::Albums)) == "albums" &&
          std::string(tabId(Tab::Artists)) == "artists");
    CHECK(sortsFor(Tab::Playlists).size() == 4 && sortsFor(Tab::Albums).size() == 4);
    CHECK(sortsFor(Tab::Artists).size() == 3);
    CHECK(sortFor(Tab::Artists, "creator") == Sort::Recents);   // not offered there
    CHECK(sortFor(Tab::Albums, "creator") == Sort::Creator);
    CHECK(sortFor(Tab::Playlists, "") == Sort::Recents && sortFor(Tab::Playlists, "bogus") == Sort::Recents);
    CHECK(viewFor("list") == View::List && viewFor("") == View::Grid && viewFor("tiles") == View::Grid);
}

void testRecentsTime() {
    std::printf("recents\n");
    Signals local;
    local.playedHere = 100;
    local.added = 300;
    CHECK(recentsTime(local) == 300);   // a local playlist created after its last play: its creation counts
    local.added = 50;
    CHECK(recentsTime(local) == 100);
    Signals sp;
    sp.spotifyItem = true;
    sp.added = 500;
    sp.playedHere = 0;
    CHECK(recentsTime(sp) == 0);        // no Spotify times: libraryV3's order decides, not the added date
    sp.playedHere = 200;
    CHECK(recentsTime(sp) == 200);
    sp.spotifyTimes = true;
    sp.playedSpotify = 400;
    CHECK(recentsTime(sp) == 500);      // with Spotify's times: the latest of all three
    sp.added = 10;
    CHECK(recentsTime(sp) == 400);
}

void testSorts() {
    std::printf("sorts\n");
    std::vector<Item> items{
        playlist("liked", L"Beğenilen Şarkılar", 0, 0),
        playlist("b", L"Bravo", kNow - 5 * kDay, kNow - 100 * kDay, L"Zeynep"),
        playlist("u1", L"Undated one", 0, kNow - 2 * kDay, L"ali"),
        playlist("c", L"charlie", kNow - 1 * kDay, 0, L"Ali"),
        playlist("u2", L"alpha", 0, 0, L""),
        playlist("d", L"Delta", kNow - 5 * kDay, kNow - 50 * kDay, L"zeynep"),   // same time as "b": source order
    };
    items[0].pinned = true;
    const Model m(items, L"tr-TR");
    CHECK(ordered(m, Sort::Recents) == "liked c b d u1 u2");
    CHECK(ordered(m, Sort::Added) == "liked u1 d b c u2");
    CHECK(ordered(m, Sort::Alpha) == "liked u2 b c d u1");                 // pinned first, case-insensitive
    // Owners case-insensitively (Ali / ali, Zeynep / zeynep), ties by name, no owner last.
    CHECK(ordered(m, Sort::Creator) == "liked c u1 b d u2");
    // Same input, same order (no hidden state between calls).
    CHECK(ordered(m, Sort::Recents) == "liked c b d u1 u2");

    // Stable: equal keys keep the source order, whatever the input size.
    std::vector<Item> same;
    for (int i = 0; i < 50; ++i) same.push_back(playlist("p" + std::to_string(i), L"Same", 0, 0));
    const Model ms(same, L"tr-TR");
    bool stable = true;
    for (Sort s : {Sort::Recents, Sort::Added, Sort::Alpha, Sort::Creator}) {
        Query q;
        q.sort = s;
        const auto rows = ms.order(q);
        for (size_t i = 0; i < rows.size(); ++i) stable = stable && rows[i].item == i;
    }
    CHECK(stable);

    // Turkish collation (Windows' tr-TR tables, as Explorer sorts): Ç after C, Ş after S, Ö after O, digits as numbers.
    // Windows weighs I / ı / İ / i as one letter (the next letters decide), between H and J.
    std::vector<Item> tr;
    for (const wchar_t* n : {L"Dans", L"Çay", L"Cam", L"Tuz", L"Şeker", L"Su", L"Pul", L"Ölü", L"Oda", L"Jale",
                             L"İnci", L"Işık", L"Hal", L"Mix 10", L"Mix 2"})
        tr.push_back(playlist(toUtf8(n), n));
    const Model mt(tr, L"tr-TR");
    const std::string alpha = ordered(mt, Sort::Alpha);
    std::printf("        tr-TR: %s\n", alpha.c_str());
    CHECK(alpha.rfind("Cam Çay Dans Hal ", 0) == 0);
    CHECK(alpha.find("Jale Mix 2 Mix 10 Oda Ölü Pul Su Şeker Tuz") != std::string::npos);
    CHECK(alpha.find("Işık") > alpha.find("Hal") && alpha.find("İnci") > alpha.find("Hal") &&
          alpha.find("Işık") < alpha.find("Jale") && alpha.find("İnci") < alpha.find("Jale"));
    // Case never decides ("ali" and "Ali" tie: their source order), dotted capitals fold like small letters.
    std::vector<Item> cases{playlist("1", L"ali"), playlist("2", L"Ali"), playlist("3", L"ALİ"), playlist("4", L"Ahmet")};
    CHECK(ordered(Model(cases, L"tr-TR"), Sort::Alpha) == "4 1 2 3");

    // Albums under "Sanatçı": the artist, then the release (oldest first, undated after), then the name.
    std::vector<Item> albums;
    auto album = [&](std::string id, std::wstring name, std::wstring artist, std::string released) {
        Item it;
        it.kind = Item::Kind::Album;
        it.id = std::move(id);
        it.name = std::move(name);
        it.creator = std::move(artist);
        it.released = std::move(released);
        albums.push_back(it);
    };
    album("x3", L"Third", L"Sezen Aksu", "1991");
    album("x1", L"First", L"Sezen Aksu", "1978-06-01");
    album("y", L"Alone", L"Barış Manço", "1975");
    album("x0", L"Undated", L"sezen aksu", "");
    album("z", L"No artist", L"", "1970");
    album("x2", L"Second", L"Sezen Aksu", "1984");
    const Model ma(albums, L"tr-TR");
    CHECK(ordered(ma, Sort::Creator) == "y x1 x2 x3 x0 z");
}

void testFolders() {
    std::printf("folders\n");
    std::vector<Item> items{
        playlist("liked", L"Liked", 0, 0),
        playlist("top1", L"Top one", kNow - 10 * kDay, kNow - 10 * kDay),
        playlist("in1", L"Inside one", kNow - 2 * kDay, kNow - 30 * kDay, {}, "F"),
        playlist("in2", L"Inside two", kNow - 20 * kDay, kNow - 1 * kDay, {}, "F"),
        playlist("deep", L"Deep", kNow - 1 * kDay, kNow - 40 * kDay, {}, "G"),   // G is inside F
        playlist("top2", L"another", kNow - 3 * kDay, kNow - 3 * kDay),
        playlist("lost", L"Lost", kNow - 4 * kDay, 0, {}, "nope"),                // unknown folder: top level
        folderItem("F", L"Zeta folder"),
        folderItem("G", L"Gamma", "F"),
        folderItem("E", L"Empty"),
    };
    items[0].pinned = true;
    const Model m(items, L"en-US");
    // Recents: F sits where "deep" (its most recent, two levels down) would be; the empty folder goes last.
    CHECK(ordered(m, Sort::Recents) == "liked [F] top2 lost top1 [E]");
    // Added: F by "in2" (1 day), above top2 (3 days).
    CHECK(ordered(m, Sort::Added) == "liked [F] top2 top1 lost [E]");
    // Alpha / Creator: folders first, alphabetically, after the pinned items.
    CHECK(ordered(m, Sort::Alpha) == "liked [E] [F] top2 lost top1");
    CHECK(ordered(m, Sort::Creator) == "liked [E] [F] top2 lost top1");
    // A folder's own level.
    CHECK(ordered(m, Sort::Recents, {}, "F") == "[G] in1 in2");
    CHECK(ordered(m, Sort::Alpha, {}, "F") == "[G] in1 in2");
    CHECK(ordered(m, Sort::Recents, {}, "G") == "deep");
    CHECK(ordered(m, Sort::Recents, {}, "E").empty());
    // Expanded folders (the sidebar): contents follow their folder, one level deeper.
    Query q;
    q.sort = Sort::Recents;
    q.expanded = [](const std::string& id) { return id == "F" || id == "G"; };
    const auto rows = m.order(q);
    CHECK(ids(m, rows) == "liked [F] [G] deep in1 in2 top2 lost top1 [E]");
    CHECK(rows.size() == 10 && rows[1].depth == 0 && rows[2].depth == 1 && rows[3].depth == 2 && rows[4].depth == 1);
    q.expanded = [](const std::string& id) { return id == "G"; };   // G open but F closed: G's contents stay hidden
    CHECK(ids(m, m.order(q)) == "liked [F] top2 lost top1 [E]");

    // A broken chain (two folders naming each other as parent) is never reached and never loops.
    std::vector<Item> broken{playlist("p", L"P"), folderItem("A", L"A", "B"), folderItem("B", L"B", "A"),
                             playlist("q", L"Q", 0, 0, {}, "A")};
    const Model mb(broken, L"en-US");
    CHECK(ordered(mb, Sort::Recents) == "p");
    CHECK(ordered(mb, Sort::Recents, {}, "A") == "[B] q");
    CHECK(ordered(mb, Sort::Recents, L"q", "A") == "q");
}

void testFilter() {
    std::printf("filter\n");
    std::vector<Item> items{
        playlist("ist", L"İSTANBUL Geceleri", kNow - 9 * kDay),
        playlist("isik", L"ışık yılı", kNow - 8 * kDay),
        playlist("muz", L"Müzik Kutusu", kNow - 7 * kDay),
        playlist("cay", L"Çay Saati", kNow - 6 * kDay),
        playlist("bazar", L"Bazar", kNow - 5 * kDay),
        playlist("arab", L"Arabesk", kNow - 4 * kDay),
        playlist("own", L"Road trip", kNow - 3 * kDay, 0, L"Arda"),        // "ar" only in the owner's name
        playlist("rock2", L"Rock Klasikleri", kNow - 2 * kDay),
        playlist("rock1", L"Rock", kNow - 1 * kDay),
        playlist("night", L"Gece Yarısı", kNow, 0, L"Rockçı"),             // "rock" only in the owner's name
        playlist("jz1", L"Jazz", kNow - 11 * kDay, kNow - 30 * kDay),
        playlist("jz2", L"Jazz", kNow - 12 * kDay, kNow - 20 * kDay),
        playlist("deep", L"İstanbul derinlerde", kNow - 10 * kDay, 0, {}, "F"),
        folderItem("F", L"Eski İstanbul"),
    };
    const Model m(items, L"tr-TR");
    // Prefix matches (shorter text first), then the folder's word-start match; folders and their contents included.
    CHECK(ordered(m, Sort::Recents, L"istanbul") == "ist deep [F]");
    CHECK(ordered(m, Sort::Recents, L"İSTANBUL") == "ist deep [F]");
    CHECK(ordered(m, Sort::Recents, L"ıstanbul") == "ist deep [F]");
    CHECK(ordered(m, Sort::Recents, L"IŞIK") == "isik");
    CHECK(ordered(m, Sort::Recents, L"muzik") == "muz");
    CHECK(ordered(m, Sort::Recents, L"CAY SAAT") == "cay");
    // One or two letters: word starts only ("Bazar" holds "ar" mid-word, "Arda" only as the owner, counted lower).
    CHECK(ordered(m, Sort::Recents, L"ar") == "arab");
    CHECK(ordered(m, Sort::Recents, L"arda") == "own");   // longer: the owner's name matches too
    // The whole name beats a prefix, a name match beats the owner's (though "night" was played last).
    CHECK(ordered(m, Sort::Recents, L"rock") == "rock1 rock2 night");
    // Equal scores keep the sort's order.
    CHECK(ordered(m, Sort::Recents, L"jazz") == "jz1 jz2");
    CHECK(ordered(m, Sort::Added, L"jazz") == "jz2 jz1");
    // Inside a folder: only its contents.
    CHECK(ordered(m, Sort::Recents, L"istanbul", "F") == "deep");
    CHECK(ordered(m, Sort::Recents, L"   ") == ordered(m, Sort::Recents));   // blanks = no filter
    CHECK(ordered(m, Sort::Recents, L"zzzz").empty());
    // The score itself: no match / below the threshold is -1.
    CHECK(filterScore(L"ab", L"xaby", L"") == -1);
    CHECK(filterScore(L"rock", L"rock", L"") > filterScore(L"rock", L"rock klasikleri", L""));
}

void testStore() {
    std::printf("store\n");
    CHECK(Recents::tracked("playlist:spotify:playlist:1") && Recents::tracked("album:x") && Recents::tracked("artist:y") &&
          Recents::tracked("liked"));
    CHECK(!Recents::tracked("search") && !Recents::tracked("smart:mix:1") && !Recents::tracked("playlist:") &&
          !Recents::tracked("history") && !Recents::tracked(""));
    Recents r;
    CHECK(r.record("playlist:a", 100));
    CHECK(!r.record("playlist:a", 90));    // older: kept
    CHECK(r.record("playlist:a", 120));
    CHECK(!r.record("search", 500));
    CHECK(!r.record("album:b", 0));
    CHECK(r.record("album:b", 110) && r.record("liked", 130));
    CHECK(r.at("playlist:a") == 120 && r.at("album:b") == 110 && r.at("liked") == 130 && r.at("nope") == 0);
    const auto back = Recents::fromJson(r.toJson());
    CHECK(back.size() == 3 && back.at("playlist:a") == 120 && back.at("liked") == 130);
    CHECK(r.toJson().find("\"liked\"") < r.toJson().find("\"playlist:a\""));   // newest first
    CHECK(Recents::fromJson("not json").size() == 0);
    CHECK(Recents::fromJson(R"({"v":1,"recent":[["album:x",5],["bad"],[1,2],["search",9],["artist:y","x"]]})").size() == 1);
    // The cap: the oldest go.
    Recents big;
    for (int i = 0; i < static_cast<int>(Recents::kMax) + 50; ++i) big.record("album:" + std::to_string(i), 1000 + i);
    CHECK(big.size() == Recents::kMax);
    CHECK(big.at("album:0") == 0 && big.at("album:49") == 0 && big.at("album:50") == 1050);
    // The file (in this test's own profile folder).
    const fs::path file = g_dir / L"library-recents.json";
    CHECK(r.save(file));
    const auto loaded = Recents::load(file);
    CHECK(loaded.size() == 3 && loaded.at("album:b") == 110);
    CHECK(Recents::load(g_dir / L"missing.json").size() == 0);
}

void testHistory() {
    std::printf("history\n");
    ListenStats stats(g_dir / L"listening.json");
    auto track = [](std::string id, std::string name, std::string artist, std::string albumId, std::string album) {
        catalog::Track t;
        t.id = std::move(id);
        t.name = std::move(name);
        t.artists = {{"", std::move(artist)}};
        t.album.id = std::move(albumId);
        t.album.name = std::move(album);
        t.durationMs = 200'000;
        return t;
    };
    stats.addPlay(track("spotify:track:1", "Gülümse", "Sezen Aksu", "spotify:album:G", "Gülümse"), kNow - 10 * kDay, 180'000);
    stats.addPlay(track("spotify:track:2", "Hadi Bakalım", "Sezen Aksu", "spotify:album:G", "Gülümse"), kNow - 3 * kDay, 150'000);
    // Imported history: no album id, only names.
    stats.addPlay(track("import:1", "Halo", "Beyoncé", "", "I Am... Sasha Fierce"), kNow - 2 * kDay, 200'000);
    // Skipped after 5 s: not a stream, doesn't count.
    stats.addPlay(track("spotify:track:3", "Kış Güneşi", "Tarkan", "spotify:album:K", "Karma"), kNow - 1 * kDay, 5'000);
    PlayRecency pr;
    pr.build(stats);
    CHECK(pr.album("spotify:album:G", "Gülümse", "Sezen Aksu") == kNow - 3 * kDay);
    CHECK(pr.album("", "GÜLÜMSE", "sezen aksu") == kNow - 3 * kDay);   // by name + first artist
    CHECK(pr.album("spotify:album:other", "I Am... Sasha Fierce", "Beyonce") == kNow - 2 * kDay);
    CHECK(pr.album("spotify:album:K", "Karma", "Tarkan") == 0);
    CHECK(pr.artist("SEZEN AKSU") == kNow - 3 * kDay);
    CHECK(pr.artist("beyonce") == kNow - 2 * kDay);
    CHECK(pr.artist("Tarkan") == 0 && pr.artist("") == 0);
    PlayRecency empty;
    CHECK(empty.album("x", "y", "z") == 0 && empty.artist("x") == 0);
}

void testTiming() {
    std::printf("timing\n");
    std::vector<Item> items;
    items.reserve(10'000);
    const wchar_t* words[] = {L"Rock", L"Çay", L"İstanbul", L"Gece", L"Yolculuk", L"Müzik", L"Klasik", L"Şarkı",
                              L"Sabah", L"Akşam", L"Jazz", L"Ölümsüz"};
    for (int i = 0; i < 200; ++i) items.push_back(folderItem("f" + std::to_string(i), L"Klasör " + std::to_wstring(i),
                                                             i % 4 == 3 ? "f" + std::to_string(i - 1) : std::string()));
    for (int i = 0; items.size() < 10'000; ++i) {
        std::wstring name = std::wstring(words[i % 12]) + L" " + words[(i / 12) % 12] + L" " + std::to_wstring(i);
        Item it = playlist("p" + std::to_string(i), name, i % 3 ? kNow - (i % 997) * 3600 : 0, kNow - (i % 1009) * 7200,
                           L"Owner " + std::to_wstring(i % 37), i % 5 == 0 ? "f" + std::to_string(i % 200) : std::string());
        it.pinned = i < 2;
        items.push_back(std::move(it));
    }
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a) { return std::chrono::duration<double, std::milli>(clock::now() - a).count(); };
    auto t0 = clock::now();
    const Model m(std::move(items), L"tr-TR");
    const double build = ms(t0);
    double worst = 0, total = build;
    size_t top = 0;
    for (Sort s : {Sort::Recents, Sort::Added, Sort::Alpha, Sort::Creator}) {
        Query q;
        q.sort = s;
        t0 = clock::now();
        top = m.order(q).size();
        const double d = ms(t0);
        std::printf("        %-8s %7.1f ms (%zu top-level rows)\n", sortId(s), d, top);
        worst = std::max(worst, d);
        total += d;
    }
    double filterWorst = 0;
    for (const wchar_t* f : {L"is", L"istanbul gece", L"muzk", L"zzzz"}) {
        Query q;
        q.sort = Sort::Alpha;
        q.filter = f;
        t0 = clock::now();
        const size_t n = m.order(q).size();
        const double d = ms(t0);
        std::printf("        filter %-14ls %7.1f ms (%zu hits)\n", f, d, n);
        filterWorst = std::max(filterWorst, d);
        total += d;
    }
    std::printf("        model %.1f ms, worst sort %.1f ms, worst filter %.1f ms, total %.1f ms\n", build, worst, filterWorst, total);
    CHECK(top > 0 && top < 10'000);
#ifdef NDEBUG
    CHECK(worst < 150 && filterWorst < 250 && total < 1'500);
#else
    CHECK(worst < 1'500 && filterWorst < 2'500 && total < 15'000);
#endif
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    // Per process: two checkouts running their tests at the same time must not share (and wipe) one folder.
    g_dir = fs::path(tmp) / (L"shadetube_library_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()

    testIds();
    testRecentsTime();
    testSorts();
    testFolders();
    testFilter();
    testStore();
    testHistory();
    testTiming();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
