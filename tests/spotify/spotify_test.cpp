// spotify_test: offline verification of the TOTP core (base32 + RFC 6238 vectors) plus an optional live run
// against Spotify using an sp_dc cookie supplied via the environment.
//
//   spotify_test                   offline checks (TOTP + base32 vectors, home / buddylist / new-releases parsers and rootlist
//                                  folder fixtures), then the full
//                                  live run when a sp_dc is available (SHADETUBE_SPDC or the app's saved cookie)
//   spotify_test offline           offline checks only (no network besides the nuance gist)
//   spotify_test home [dump.json]  offline checks + ONLY token + the personalized home feed (use this while
//                                  iterating: Spotify rate-limits hard). Optionally dumps the raw `data` object.
//   spotify_test playlist-edit     offline checks + a WRITE round trip on a temporary playlist it creates itself
//                                  ("ShadeTube test - silinecek"): create -> add a track -> rename -> insert a
//                                  second one in front of it -> move it behind -> description + cover picture ->
//                                  clear both -> remove the tracks -> delete, verifying each step and that the
//                                  library's playlist count is back to where it started. Never touches any other
//                                  playlist.
//   spotify_test radio             offline checks + song / artist radio for public seeds (read-only)
//   spotify_test hashes            offline checks + persisted-query hashes (read-only): scans the live web-player
//                                  bundle (+ chunks) and compares it with the built-in table, shows the raw answer to
//                                  an unknown hash, runs the self-heal path (profileAttributes starting from a wrong
//                                  hash must still work) and tries the current hash of every op whose built-in differs.
//                                  Learned hashes go to a temp file, never to %LOCALAPPDATA%\ShadeTube.
//   spotify_test hashes scan       only the web-player scan + comparison (no login, no Pathfinder call)
//
// The cookie / token values are never printed or logged; only lengths and success are reported.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/PlaylistEditing.h"   // cover JPEG for the playlist-edit round trip
#include "core/Http.h"
#include "core/Log.h"
#include "spotify/Auth.h"
#include "spotify/HashRegistry.h"
#include "spotify/Session.h"   // store::loadCookie (DPAPI)
#include "spotify/SpotifyApi.h"
#include "spotify/Totp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::spotify;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static std::string toHex(const std::vector<uint8_t>& v) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) {
        s.push_back(h[b >> 4]);
        s.push_back(h[b & 15]);
    }
    return s;
}

// RFC 4648 §10 base32 test vectors.
static void testBase32() {
    std::printf("base32Decode:\n");
    CHECK(toHex(base32Decode("")) == "");
    CHECK(toHex(base32Decode("MY======")) == "66");            // "f"
    CHECK(toHex(base32Decode("MZXQ====")) == "666f");          // "fo"
    CHECK(toHex(base32Decode("MZXW6===")) == "666f6f");        // "foo"
    CHECK(toHex(base32Decode("MZXW6YQ=")) == "666f6f62");      // "foob"
    CHECK(toHex(base32Decode("MZXW6YTB")) == "666f6f6261");    // "fooba"
    CHECK(toHex(base32Decode("MZXW6YTBOI======")) == "666f6f626172"); // "foobar"
    // lowercase + stray whitespace must be tolerated (secrets sometimes arrive formatted).
    CHECK(toHex(base32Decode("mz xw 6y tb")) == "666f6f6261");
    // The RFC 6238 seed "12345678901234567890" as base32.
    CHECK(base32Decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ").size() == 20);
}

// RFC 6238 Appendix B reference table (SHA-1, digits=8). We verify the 6-digit tail our signer emits.
static void testTotp() {
    std::printf("totp (RFC 6238 SHA-1 vectors):\n");
    const char* secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"; // ASCII "12345678901234567890"
    struct V {
        int64_t t;
        const char* eight; // the RFC's 8-digit value
        const char* six;   // its 6-digit tail (what totp() returns with digits=6)
    };
    const V vecs[] = {
        {59, "94287082", "287082"},
        {1111111109, "07081804", "081804"},
        {1111111111, "14050471", "050471"},
        {1234567890, "89005924", "005924"},
        {2000000000, "69279037", "279037"},
        {20000000000LL, "65353130", "353130"},
    };
    for (const auto& v : vecs) {
        const std::string six = totp(secret, v.t);
        const std::string eight = totp(secret, v.t, 8);
        std::printf("  t=%-12lld  6=%s (want %s)   8=%s (want %s)\n", static_cast<long long>(v.t), six.c_str(), v.six,
                    eight.c_str(), v.eight);
        CHECK(six == v.six);
        CHECK(eight == v.eight);
    }
    // Codes must be stable within a 30 s window and change across it (base aligned to a period boundary).
    CHECK(totp(secret, 1200) == totp(secret, 1200 + 29));
    CHECK(totp(secret, 1200) != totp(secret, 1200 + 30));
    // The baked-in Spotify nuance decodes to a non-empty key and yields a 6-digit code.
    const Nuance fallback = fetchNuance(/*bustCache=*/false, {}); // no network use path unless it succeeds; fallback is fine
    CHECK(fallback.version > 0);
    CHECK(!fallback.secret.empty());
    CHECK(totp(fallback.secret, 1700000000).size() == 6);
}

static const char* kindName(CardItem::Kind k) {
    switch (k) {
    case CardItem::Kind::Track: return "track";
    case CardItem::Kind::Album: return "album";
    case CardItem::Kind::Artist: return "artist";
    case CardItem::Kind::Playlist: return "playlist";
    }
    return "?";
}

#ifdef SPOTIFY_FIXTURE_DIR
static nlohmann::json loadFixture(const char* name) {
    std::ifstream f(std::string(SPOTIFY_FIXTURE_DIR) + "/" + name, std::ios::binary);
    if (!f) {
        std::printf("  FAIL fixture missing: %s\n", name);
        ++g_failures;
        return {};
    }
    return nlohmann::json::parse(f, nullptr, false);
}
#endif

// Offline: friend activity (buddylist) — the shape spotify-buddylist documents plus edge cases: legacy
// spotify:user:<u>:playlist: contexts, a user without a name, a missing context, an entry without a track.
static void testBuddylistFixture() {
    std::printf("buddylist parser (fixture):\n");
#ifdef SPOTIFY_FIXTURE_DIR
    const auto j = loadFixture("buddylist.json");
    const auto list = Api::parseBuddylist(j);
    for (const auto& a : list)
        std::printf("  %lld %s | %s - %s | %s (%s)\n", static_cast<long long>(a.timestampMs), a.userName.c_str(),
                    a.track.name.c_str(), a.track.artistLine().c_str(), a.contextName.c_str(), a.contextUri.c_str());
    CHECK(list.size() == 3);   // the entry without a track is dropped
    if (list.size() == 3) {
        // Newest first.
        CHECK(list[0].userName == "Elif Yılmaz" && list[1].userName == "shaktirockgym" && list[2].userName == "noname");
        CHECK(list[0].timestampMs == 1600780000000LL);
        CHECK(list[0].imageUrl == "https://i.scdn.co/image/ab6775700000ee8555c25988a6ac314394d3fbf5");
        CHECK(list[0].contextUri == "spotify:album:4yP0hdKOZPNshxUOjY0cZj" && list[0].contextName == "After Hours");
        CHECK(list[0].track.id == "spotify:track:0VjIjW4GlUZAMYd2vXMi3b");
        CHECK(list[0].track.artists.size() == 1 && list[0].track.artists[0].id == "spotify:artist:1Xyo4u8uXC1ZmMpatF05PJ");
        // Legacy playlist URI normalized; http cover upgraded to https.
        CHECK(list[1].contextUri == "spotify:playlist:37i9dQZF1E4riV8HyBkA7r");
        CHECK(list[1].imageUrl.empty());
        CHECK(list[1].track.album.id == "spotify:album:1XORY4rQNhqkZxTze6Px90");
        CHECK(list[1].track.album.images.size() == 1 &&
              list[1].track.album.images[0].url == "https://i.scdn.co/image/ab67616d0000b273bf4b533ee6e9634a6fcd8882");
        // No display name: the username; no context.
        CHECK(list[2].userName == "noname" && list[2].contextUri.empty() && list[2].track.album.images.empty());
    }
    CHECK(Api::parseBuddylist(nlohmann::json::object()).empty());
    CHECK(Api::parseBuddylist(nlohmann::json::parse(R"({"friends":[]})")).empty());
    CHECK(Api::parseBuddylist(nlohmann::json::parse(R"({"friends":{"x":1}})")).empty());
#else
    std::printf("  skipped (SPOTIFY_FIXTURE_DIR not defined)\n");
#endif
}

// Offline: the What's New feed (music) and the artist overview's latest releases.
static void testReleasesFixtures() {
    std::printf("new releases parsers (fixtures):\n");
#ifdef SPOTIFY_FIXTURE_DIR
    const auto feed = loadFixture("whatsnew.json");
    const auto albums = Api::parseWhatsNewFeed(feed.is_object() && feed.contains("data") ? feed["data"] : feed);
    for (const auto& a : albums)
        std::printf("  %s | %s | %s | %s | %zu artist(s) | %zu img\n", a.id.c_str(), a.name.c_str(), a.primaryType.c_str(),
                    a.firstReleaseDate.c_str(), a.artists.size(), a.images.size());
    // The episode, the NotFound entry, the duplicate and the one without a URI are dropped.
    CHECK(albums.size() == 3);
    if (albums.size() == 3) {
        CHECK(albums[0].name == "Gece Yarısı" && albums[0].primaryType == "Single" && albums[0].firstReleaseDate == "2026-10-02");
        CHECK(albums[0].artists.size() == 1 && albums[0].artists[0].name == "Sezen Aksu");
        CHECK(albums[0].images.size() == 2);
        // No type: five tracks make an EP.
        CHECK(albums[1].id == "spotify:album:6dVIqQ8qmQ5GBnJ9shOYGE" && albums[1].primaryType == "EP" &&
              albums[1].totalTracks == 5 && albums[1].artists.size() == 2);
        // {year, month, day} dates.
        CHECK(albums[2].primaryType == "Album" && albums[2].firstReleaseDate == "2026-09-12" && albums[2].images.empty());
    }
    CHECK(Api::parseWhatsNewFeed(nlohmann::json::object()).empty());

    const auto overview = loadFixture("artist_overview.json");
    const auto latest = Api::parseArtistReleases(overview.is_object() && overview.contains("data") ? overview["data"] : overview);
    for (const auto& a : latest)
        std::printf("  latest: %s | %s | %s | %s\n", a.id.c_str(), a.name.c_str(), a.primaryType.c_str(), a.firstReleaseDate.c_str());
    CHECK(latest.size() == 2);   // latest + the newest album (the newest single is the latest again)
    if (latest.size() == 2) {
        CHECK(latest[0].id == "spotify:album:2up3OPMp9Tb4dAKM2erWXQ" && latest[0].primaryType == "Single" &&
              latest[0].firstReleaseDate == "2026-10-02" && latest[0].label == "Plak");
        // Credited to the artist of the page.
        CHECK(latest[0].artists.size() == 1 && latest[0].artists[0].id == "spotify:artist:3BMsK5vWvBMbKu2N4oKnBq");
        CHECK(latest[1].name == "Eski Albüm" && latest[1].firstReleaseDate == "2024-05" && latest[1].primaryType == "Album");
    }

    CHECK(parseRetryAfter("120") == 120);
    CHECK(parseRetryAfter(" 7 ") == 7);
    CHECK(parseRetryAfter("") == 0);
    CHECK(parseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT") == 0);
    CHECK(parseRetryAfter("999999") == 86400);
    CHECK(parseRetryAfter("-5") == 0);
#else
    std::printf("  skipped (SPOTIFY_FIXTURE_DIR not defined)\n");
#endif
}

// Offline: the home parser against a synthetic fixture that mirrors the live response shape
// (data.home.sectionContainer.sections.items[] / sectionItems.items[].content.data).
static void testHomeFixture() {
    std::printf("home parser (fixture):\n");
#ifdef SPOTIFY_FIXTURE_DIR
    std::ifstream f(SPOTIFY_FIXTURE_DIR "/home.json", std::ios::binary);
    if (!f) {
        std::printf("  FAIL fixture missing: %s\n", SPOTIFY_FIXTURE_DIR "/home.json");
        ++g_failures;
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    CHECK(!j.is_discarded() && j.contains("data"));
    if (j.is_discarded() || !j.contains("data")) return;
    const auto shelves = Api::parseHome(j.at("data"));
    for (const auto& sh : shelves) {
        std::printf("  [%zu] %s | %s\n", sh.items.size(), sh.title.c_str(), sh.label.c_str());
        for (const auto& it : sh.items)
            std::printf("      %-8s %s | %s | %s | %zu img\n", kindName(it.kind), it.id.c_str(), it.title.c_str(),
                        it.subtitle.c_str(), it.images.size());
    }
    // The untitled "shorts" grid, a template-only title and a podcast-only shelf are dropped.
    CHECK(shelves.size() == 3);
    if (shelves.size() != 3) return;
    const auto& made = shelves[0];
    CHECK(made.title == "Senin için hazırlandı: Test");
    CHECK(made.label == "Her hafta yenilenir");
    CHECK(made.items.size() == 2);   // duplicate playlist collapsed
    if (made.items.size() == 2) {
        CHECK(made.items[0].kind == CardItem::Kind::Playlist);
        CHECK(made.items[0].id == "spotify:playlist:37i9dQZEVXcDiscoverWkly");
        CHECK(made.items[0].title == "Haftalık Keşif");
        CHECK(made.items[0].subtitle == "Duman, Mor ve Ötesi & daha fazlası. Her pazartesi 'yeni'");
        CHECK(made.items[0].images.size() == 1 && made.items[0].images[0].width == 0);   // null sizes -> 0
        CHECK(made.items[1].subtitle == "Spotify");   // empty description -> owner name
    }
    const auto& recent = shelves[1];
    CHECK(recent.title == "Son çalınanlar");
    CHECK(recent.label.empty());
    CHECK(recent.items.size() == 4);   // album, artist, Liked Songs, track (episode skipped)
    if (recent.items.size() == 4) {
        CHECK(recent.items[0].kind == CardItem::Kind::Album);
        CHECK(recent.items[0].id == "spotify:album:1El3k8dU3sKyoGUeuyrolH");
        CHECK(recent.items[0].subtitle == "Duman");
        CHECK(recent.items[0].images.size() == 3);
        CHECK(recent.items[1].kind == CardItem::Kind::Artist);
        CHECK(recent.items[1].title == "Mor ve Ötesi");
        CHECK(recent.items[1].images.size() == 2);
        CHECK(recent.items[2].kind == CardItem::Kind::Playlist);
        CHECK(recent.items[2].id == Api::kLikedSongsUri);
        CHECK(recent.items[3].kind == CardItem::Kind::Track);
        CHECK(recent.items[3].track.album.id == "spotify:album:1El3k8dU3sKyoGUeuyrolH");
        CHECK(recent.items[3].subtitle == "Duman");
    }
    const auto& mixes = shelves[2];
    CHECK(mixes.title == "En çok dinlediğin mixler");
    CHECK(mixes.label.empty());   // a long subtitle does not fit the mono label
    CHECK(mixes.items.size() == 2);
    if (mixes.items.size() == 2) {
        CHECK(mixes.items[0].title == "Daily Mix 1");
        CHECK(mixes.items[0].subtitle == "Duman, Teoman ve daha fazlası <3");   // unmatched '<' kept, not truncated
        CHECK(mixes.items[1].kind == CardItem::Kind::Artist);   // typed only by its ResponseWrapper
    }
    // Garbage in -> nothing out, no throw.
    CHECK(Api::parseHome(nlohmann::json()).empty());
    CHECK(Api::parseHome(nlohmann::json::parse(R"({"home":{"sectionContainer":{"sections":{"items":[1,"x",{}]}}}})")).empty());
#else
    std::printf("  skipped (SPOTIFY_FIXTURE_DIR not defined)\n");
#endif
}

// Library folders: the rootlist's start-group / end-group brackets and the sidebar order built from them.
static void testPlaylistTree() {
    std::printf("playlist folders (fixture):\n");
    CHECK(decodeGroupName("Chill+%26+Focus") == "Chill & Focus");
    CHECK(decodeGroupName("a%2") == "a%2");            // broken escapes kept as is
    CHECK(decodeGroupName("100%zz+") == "100%zz ");
    CHECK(decodeGroupName("%C3%BC") == "\xC3\xBC");    // UTF-8 bytes pass through
    CHECK(parsePlaylistTree(nlohmann::json()).empty());
    CHECK(parsePlaylistTree(nlohmann::json::parse(R"({"contents":{"items":[1,"x",{"uri":5},{}]}})")).empty());
#ifdef SPOTIFY_FIXTURE_DIR
    std::ifstream f(SPOTIFY_FIXTURE_DIR "/rootlist.json", std::ios::binary);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    CHECK(!j.is_discarded());
    if (j.is_discarded()) return;
    const PlaylistTree tree = parsePlaylistTree(j);
    const std::string road = "a1b2c3d4e5f60718", night = "b2c3d4e5f6071829", empty = "c3d4e5f607182930";
    CHECK(tree.folders.size() == 3);
    if (tree.folders.size() != 3) return;
    CHECK(tree.folders[0].id == road && tree.folders[0].name == "Yol M\xC3\xBCzikleri" && tree.folders[0].parentId.empty());
    CHECK(tree.folders[1].id == night && tree.folders[1].name == "Gece & G\xC3\xBCnd\xC3\xBCz" && tree.folders[1].parentId == road);
    CHECK(tree.folders[2].id == empty && tree.folders[2].name == "Bo\xC5\x9F klas\xC3\xB6r" && tree.folders[2].parentId.empty());
    CHECK(tree.folders[0].playlistCount == 3);   // 2, 3 (nested) and 4
    CHECK(tree.folders[1].playlistCount == 1);
    CHECK(tree.folders[2].playlistCount == 0);
    const auto pl = [](int n) { return "spotify:playlist:0TestPlaylist00000000" + std::to_string(n); };
    CHECK(!tree.folderOf.contains(pl(1)));
    CHECK(tree.folderOf.at(pl(2)) == road);       // listed again at the top level: the first place wins
    CHECK(tree.folderOf.at(pl(3)) == night);
    CHECK(tree.folderOf.at(pl(4)) == road);
    CHECK(!tree.folderOf.contains(pl(5)));        // after a stray end marker: still top level
    CHECK(tree.folder(night) && !tree.folder("nope"));

    // libraryV3 order (recents): Liked Songs, 4, 1, 5, 3, 2.
    std::vector<Playlist> lib;
    for (const std::string& id : {std::string(Api::kLikedSongsUri), pl(4), pl(1), pl(5), pl(3), pl(2)}) {
        Playlist p;
        p.id = id;
        lib.push_back(p);
    }
    auto describe = [&](const std::vector<TreeRow>& rows) {
        std::string s;
        for (const auto& r : rows) {
            if (!s.empty()) s += ' ';
            s += std::string(static_cast<size_t>(r.depth), '>');
            s += r.folder ? "[" + tree.folders[r.index].id.substr(0, 2) + "]" : lib[r.index].id.substr(lib[r.index].id.size() - 1);
        }
        return s;
    };
    const auto none = [](const std::string&) { return false; };
    const auto all = [](const std::string&) { return true; };
    // The road folder takes playlist 4's place (its most recent playlist); the empty folder goes last.
    const std::string collapsed = describe(treeRows(lib, tree, none));
    std::printf("  collapsed: %s\n", collapsed.c_str());
    CHECK(collapsed == "s [a1] 1 5 [c3]");
    const std::string expanded = describe(treeRows(lib, tree, all));
    std::printf("  expanded:  %s\n", expanded.c_str());
    CHECK(expanded == "s [a1] >4 >[b2] >>3 >2 1 5 [c3]");
    const std::string oneLevel = describe(treeRows(lib, tree, [&](const std::string& id) { return id == road; }));
    CHECK(oneLevel == "s [a1] >4 >[b2] >2 1 5 [c3]");
    CHECK(describe(treeRows(lib, tree, all, road)) == "4 [b2] >3 2");   // a folder's own view
    CHECK(treeRows(lib, tree, all, "unknown").empty());
    CHECK(treeRows(lib, PlaylistTree{}, all).size() == lib.size());        // no folders: the flat order
    // Unbalanced input: a group left open ends with the list; a duplicate group id keeps its items in the parent.
    const auto odd = parsePlaylistTree(nlohmann::json::parse(R"({"contents":{"items":[
        {"uri":"spotify:start-group:11:A"},{"uri":"spotify:playlist:p1"},
        {"uri":"spotify:start-group:11:A+again"},{"uri":"spotify:playlist:p2"},{"uri":"spotify:end-group:11"},
        {"uri":"spotify:start-group:22"},{"uri":"spotify:playlist:p3"}]}})"));
    CHECK(odd.folders.size() == 2);
    if (odd.folders.size() == 2) {
        CHECK(odd.folders[1].name.empty() && odd.folders[1].parentId == "11");
        CHECK(odd.folderOf.at("spotify:playlist:p2") == "11");
        CHECK(odd.folderOf.at("spotify:playlist:p3") == "22");
        CHECK(odd.folders[0].playlistCount == 3);
    }
#else
    std::printf("  fixture skipped (SPOTIFY_FIXTURE_DIR not defined)\n");
#endif
}

// Live: the personalized home feed. Prints each shelf with its item count (and dumps the raw `data`).
static void testHomeLive(Api& api, const char* dumpPath) {
    try {
        const nlohmann::json data = api.homeData(10);
        if (dumpPath && *dumpPath) {
            std::ofstream out(dumpPath, std::ios::binary);
            out << data.dump(1);
            std::printf("  home raw data dumped to %s\n", dumpPath);
        }
        const auto shelves = Api::parseHome(data);
        std::printf("  home: %zu shelves\n", shelves.size());
        for (const auto& sh : shelves) {
            std::printf("    [%2zu] %s%s%s\n", sh.items.size(), sh.title.c_str(), sh.label.empty() ? "" : "  -- ",
                        sh.label.c_str());
            if (!sh.items.empty())
                std::printf("         e.g. %s \"%s\" (%s)\n", kindName(sh.items[0].kind), sh.items[0].title.c_str(),
                            sh.items[0].subtitle.c_str());
        }
        CHECK(!shelves.empty());
    } catch (const ApiError& e) {
        std::printf("  FAIL home (status %d): %s  -- the response body is in shadetube.log\n", e.status, e.what());
        ++g_failures;
    } catch (const std::exception& e) {
        std::printf("  FAIL home: %s\n", e.what());
        ++g_failures;
    }
}

// A 320 px square gradient as the JPEG the app uploads (a hand-made BMP through app/PlaylistEditing).
static std::vector<uint8_t> gradientJpeg() {
    const int n = 320, rowBytes = n * 3;
    std::vector<uint8_t> bmp(54 + static_cast<size_t>(rowBytes) * n, 0);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bmp[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, n);
    put32(22, n);
    bmp[26] = 1;    // planes
    bmp[28] = 24;   // bits per pixel
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            uint8_t* p = &bmp[54 + static_cast<size_t>(y) * rowBytes + x * 3];
            p[0] = static_cast<uint8_t>(x * 255 / n);
            p[1] = static_cast<uint8_t>(y * 255 / n);
            p[2] = 200;
        }
    return st::app::pledit::squareJpeg(bmp.data(), bmp.size()).jpeg;
}

// Live WRITE round trip on a temporary playlist this test creates itself. Safety rules:
//  * only the playlist created here (named kTempName) is ever modified or deleted;
//  * the rootlist playlist count is recorded first and must be back to it at the end;
//  * if anything fails after the playlist exists, cleanup (delete) is retried and reported loudly.
static void testPlaylistEditLive(Api& api) {
    static const char* kTempName = "ShadeTube test - silinecek";
    static const char* kTempRenamed = "ShadeTube test - silinecek (yeniden adlandırıldı)";
    auto pause = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    std::printf("playlist-edit (live, temporary playlist only):\n");

    const UserProfile me = api.me({});
    std::printf("  profile ok: username %s\n", me.username.empty() ? "MISSING" : "present");
    CHECK(!me.username.empty());
    if (me.username.empty()) return;

    // Read-only first: the library (owner / editable / counts) and the rootlist baseline.
    std::vector<std::string> before;
    try {
        const auto playlists = api.libraryPlaylists({});
        int known = 0, editable = 0, owned = 0;
        for (const auto& p : playlists) {
            known += p.countKnown ? 1 : 0;
            editable += p.editable ? 1 : 0;
            owned += p.owned ? 1 : 0;
        }
        std::printf("  library: %zu playlists (%d with a count, %d editable, %d owned)\n", playlists.size(), known, editable,
                    owned);
        for (size_t i = 0; i < playlists.size() && i < 6; ++i)
            std::printf("    %-40.40s  count=%s%d  owner=%s  editable=%d owned=%d\n", playlists[i].name.c_str(),
                        playlists[i].countKnown ? "" : "?", playlists[i].totalTracks, playlists[i].ownerName.c_str(),
                        playlists[i].editable ? 1 : 0, playlists[i].owned ? 1 : 0);
        CHECK(playlists.size() <= 1 || known > 1);   // counts come from the rootlist
        for (const auto& p : playlists)
            if (p.name.rfind("ShadeTube test", 0) == 0)
                std::printf("  NOTE: a leftover temp playlist exists already: %s (%s)\n", p.name.c_str(), p.id.c_str());
        // "Your Episodes" (a real playlist, format listen-later, not in the rootlist -> no count) must never be
        // offered for edits — neither from the library list nor from its own page (fetchPlaylist).
        for (const auto& p : playlists)
            if (!p.countKnown && p.id.rfind("spotify:playlist:", 0) == 0) {
                Playlist m;
                api.playlistTracks(p.id, 0, 1, {}, &m, nullptr);
                std::printf("  no-count playlist \"%s\": library editable=%d, page editable=%d owned=%d\n", p.name.c_str(),
                            p.editable ? 1 : 0, m.editable ? 1 : 0, m.owned ? 1 : 0);
                CHECK(!p.editable && !m.editable && !m.owned);
                break;
            }
        before = api.rootlistPlaylists({});
        std::printf("  rootlist baseline: %zu playlists\n", before.size());
    } catch (const std::exception& e) {
        std::printf("  FAIL read-only phase: %s -- aborting BEFORE any write\n", e.what());
        ++g_failures;
        return;
    }
    if (before.empty()) {
        std::printf("  FAIL rootlist baseline is empty -- aborting BEFORE any write\n");
        ++g_failures;
        return;
    }

    // A track to add: the newest Liked Song (read only).
    std::string trackUri;
    try {
        const auto liked = api.playlistTracks(Api::kLikedSongsUri, 0, 1);
        if (!liked.items.empty()) trackUri = liked.items[0].id;
    } catch (const std::exception& e) {
        std::printf("  liked lookup failed: %s\n", e.what());
    }
    if (trackUri.empty()) trackUri = "spotify:track:4uLU6hMCjMI75M1A2tKUQC";   // fallback: a well-known track
    std::printf("  track to add: %s\n", trackUri.c_str());

    // 1) create
    const std::string uri = api.createPlaylist(kTempName);
    std::printf("  create: %s\n", uri.empty() ? "FAILED" : uri.c_str());
    CHECK(!uri.empty());
    if (uri.empty()) return;

    bool ok = true;
    try {
        pause(1500);
        auto after = api.rootlistPlaylists({});
        const bool listed = std::find(after.begin(), after.end(), uri) != after.end();
        std::printf("  rootlist after create: %zu playlists, new one listed=%d\n", after.size(), listed ? 1 : 0);
        CHECK(listed);
        CHECK(after.size() == before.size() + 1);

        // 2) add a track
        const bool added = api.addToPlaylist(uri, {trackUri});
        std::printf("  addToPlaylist: %s\n", added ? "ok" : "FAILED");
        CHECK(added);
        pause(1500);
        Playlist meta;
        std::string owner;
        auto pg = api.playlistTracks(uri, 0, 25, {}, &meta, &owner);
        std::printf("  verify add: total=%d items=%zu  owner=\"%s\" owned=%d editable=%d  uid=%s\n", pg.total,
                    pg.items.size(), meta.ownerName.c_str(), meta.owned ? 1 : 0, meta.editable ? 1 : 0,
                    pg.items.empty() || pg.items[0].uid.empty() ? "MISSING" : "present");
        CHECK(pg.total == 1 && pg.items.size() == 1);
        CHECK(meta.owned && meta.editable);
        const std::string uid = pg.items.empty() ? std::string() : pg.items[0].uid;
        if (!pg.items.empty()) CHECK(pg.items[0].id == trackUri);
        CHECK(!uid.empty());

        // 3) rename
        const bool renamed = api.renamePlaylist(uri, kTempRenamed);
        std::printf("  renamePlaylist: %s\n", renamed ? "ok" : "FAILED");
        CHECK(renamed);
        pause(1500);
        Playlist meta2;
        api.playlistTracks(uri, 0, 1, {}, &meta2, nullptr);
        std::printf("  verify rename: name=\"%s\"\n", meta2.name.c_str());
        CHECK(meta2.name == kTempRenamed);

        // 4) a second track in front of the first (addToPlaylist BEFORE_UID: undo of a removal), then moved behind it
        //    (moveItemsInPlaylist AFTER_UID: drag and drop)
        const std::string second = trackUri == "spotify:track:6habFhsOp2NvshLv26DqMb" ? "spotify:track:4uLU6hMCjMI75M1A2tKUQC"
                                                                                       : "spotify:track:6habFhsOp2NvshLv26DqMb";
        std::vector<std::string> uids{uid};
        PlaylistPosition front{PlaylistPosition::Kind::BeforeUid, uid};
        const bool inserted = api.insertIntoPlaylist(uri, {second}, front);
        std::printf("  insertIntoPlaylist (before the first row): %s\n", inserted ? "ok" : "FAILED");
        CHECK(inserted);
        pause(1500);
        pg = api.playlistTracks(uri, 0, 25);
        CHECK(pg.items.size() == 2 && pg.items[0].id == second && pg.items[1].id == trackUri);
        const std::string uid2 = pg.items.size() == 2 ? pg.items[0].uid : std::string();
        if (!uid2.empty()) uids.push_back(uid2);
        const bool moved = api.moveInPlaylist(uri, {uid2}, PlaylistPosition{PlaylistPosition::Kind::AfterUid, uid});
        std::printf("  moveInPlaylist (behind the first row): %s\n", moved ? "ok" : "FAILED");
        CHECK(moved);
        pause(1500);
        pg = api.playlistTracks(uri, 0, 25);
        std::printf("  verify move: %s, %s\n", pg.items.size() > 0 ? pg.items[0].id.c_str() : "-",
                    pg.items.size() > 1 ? pg.items[1].id.c_str() : "-");
        CHECK(pg.items.size() == 2 && pg.items[0].uid == uid && pg.items[1].uid == uid2);   // rows keep their uids

        // 5) details: description + a cover picture (a generated 320 px gradient), then both cleared
        PlaylistDetailsChange details;
        details.description = "ShadeTube test açıklaması";
        details.cover = PlaylistDetailsChange::Cover::Set;
        details.jpeg = gradientJpeg();
        CHECK(!details.jpeg.empty());
        const bool detailed = api.updatePlaylistDetails(uri, details);
        std::printf("  updatePlaylistDetails (description + %zu byte cover): %s\n", details.jpeg.size(),
                    detailed ? "ok" : "FAILED");
        CHECK(detailed);
        pause(2500);
        Playlist meta3;
        api.playlistTracks(uri, 0, 1, {}, &meta3, nullptr);
        std::printf("  verify details: description=\"%s\" image=%s\n", meta3.description.c_str(),
                    meta3.images.empty() ? "(none)" : meta3.images[0].url.c_str());
        CHECK(meta3.description == *details.description);
        CHECK(!meta3.images.empty() && meta3.images[0].url.find("mosaic") == std::string::npos);
        PlaylistDetailsChange clear;
        clear.description = "";
        clear.cover = PlaylistDetailsChange::Cover::Remove;
        const bool cleared = api.updatePlaylistDetails(uri, clear);
        std::printf("  updatePlaylistDetails (clear): %s\n", cleared ? "ok" : "FAILED");
        CHECK(cleared);
        pause(2500);
        Playlist meta4;
        api.playlistTracks(uri, 0, 1, {}, &meta4, nullptr);
        std::printf("  verify clear: description=\"%s\" image=%s\n", meta4.description.c_str(),
                    meta4.images.empty() ? "(none)" : meta4.images[0].url.c_str());
        CHECK(meta4.description.empty());
        CHECK(meta4.images.empty() || meta4.images[0].url.find("mosaic") != std::string::npos);

        // 6) remove the tracks (by row uid)
        const bool removed = api.removeFromPlaylist(uri, uids);
        std::printf("  removeFromPlaylist: %s\n", removed ? "ok" : "FAILED");
        CHECK(removed);
        pause(1500);
        pg = api.playlistTracks(uri, 0, 25);
        std::printf("  verify remove: total=%d items=%zu\n", pg.total, pg.items.size());
        CHECK(pg.total == 0 && pg.items.empty());
    } catch (const std::exception& e) {
        std::printf("  FAIL during the round trip: %s\n", e.what());
        ++g_failures;
        ok = false;
    }

    // 5) delete (always, even after a failure) + verify the count is back to the baseline.
    bool cleaned = false;
    for (int attempt = 1; attempt <= 4 && !cleaned; ++attempt) {
        if (attempt > 1) pause(5000 * attempt);
        const bool del = api.deletePlaylist(uri);
        std::printf("  deletePlaylist (attempt %d): %s\n", attempt, del ? "ok" : "FAILED");
        if (!del) continue;
        pause(1500);
        try {
            const auto after = api.rootlistPlaylists({});
            const bool stillListed = std::find(after.begin(), after.end(), uri) != after.end();
            std::printf("  rootlist after delete: %zu playlists (baseline %zu), temp still listed=%d\n", after.size(),
                        before.size(), stillListed ? 1 : 0);
            cleaned = !stillListed;
            CHECK(after.size() == before.size());
        } catch (const std::exception& e) {
            std::printf("  verify delete failed: %s\n", e.what());
        }
    }
    if (!cleaned) {
        std::printf("\n  !!!!! CLEANUP FAILED: the temporary playlist %s (\"%s\") is still in the library. Delete it by hand.\n\n",
                    uri.c_str(), kTempName);
        ++g_failures;
    }
    std::printf("  playlist-edit round trip: %s\n", ok && cleaned ? "PASSED" : "had failures (see above)");
}

// Optional: runs when SHADETUBE_SPDC is set, or when a saved sp_dc exists (DPAPI, same Windows user).
// Never prints the cookie/token value — only lengths.
// Read-only: song radio + artist radio for public seeds (the user's library is not touched).
static void testRadioLive(Api& api) {
    for (const char* seed : {"spotify:track:6habFhsOp2NvshLv26DqMb", "spotify:artist:0TnOYISbd1XYRBk9myaseg"}) {
        try {
            const std::string uri = api.radioPlaylist(seed, {});
            std::printf("  radio(%s) -> %s\n", seed, uri.empty() ? "(none)" : uri.c_str());
            CHECK(uri.rfind("spotify:playlist:", 0) == 0);
            if (uri.empty()) continue;
            Playlist meta;
            const auto page = api.playlistTracks(uri, 0, 50, {}, &meta);
            std::printf("    \"%s\": %zu tracks (total %d)\n", meta.name.c_str(), page.items.size(), page.total);
            for (size_t i = 0; i < page.items.size() && i < 3; ++i)
                std::printf("      %s - %s\n", page.items[i].name.c_str(), page.items[i].artistLine().c_str());
            CHECK(page.items.size() >= 10);
        } catch (const ApiError& e) {
            std::printf("  radio(%s) FAILED: HTTP %d %s\n", seed, e.status, e.what());
            CHECK(false);
        }
    }
}

// ---- Persisted-query hashes (HashRegistry) -----------------------------------------------------------------------

static const char* kBogusHash = "00000000000000000000000000000000000000000000000000000000deadbeef";

// A scratch store for the registry, so tests never read or write the user's %LOCALAPPDATA%\ShadeTube file.
static std::filesystem::path scratchHashStore() {
    const auto file = std::filesystem::temp_directory_path() / "shadetube_spotify_test_hashes.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return file;
}

// Offline: the bundle / chunk-map / failure parsers on a synthetic snippet shaped like web-player.<h>.js.
static void testHashExtractor() {
    std::printf("hash extractor (synthetic bundle):\n");
    const std::string html =
        R"(<link rel="preload" href="https://open.spotifycdn.com/cdn/build/web-player/web-player.abc12345.js">)"
        R"(<script src="https://open.spotifycdn.com/cdn/build/web-player/web-player.abc12345.js"></script>)"
        R"(<script src="https://open.spotifycdn.com/cdn/build/web-player/vendor~web-player.4ad2b3e0.js"></script>)"
        R"(<script src="/cdn/build/web-player/web-player.lazy0001.js"></script>)"
        // Foreign hosts are never fetched, whatever the page says.
        R"(<script src="https://evil.example/cdn/build/web-player/web-player.bad00001.js"></script>)"
        R"(<script src="//evil.example/cdn/build/web-player/web-player.bad00002.js"></script>)"
        R"(<script src="https://open.spotifycdn.com.evil.example/cdn/build/web-player/web-player.bad00003.js"></script>)";
    const auto bundles = findBundleUrls(html);
    for (const auto& b : bundles) std::printf("  bundle %s\n", b.c_str());
    CHECK(bundles.size() == 2);
    if (bundles.size() == 2) {
        CHECK(bundles[0] == "https://open.spotifycdn.com/cdn/build/web-player/web-player.abc12345.js");
        CHECK(bundles[1] == "https://open.spotifycdn.com/cdn/build/web-player/web-player.lazy0001.js");
    }

    const std::string h1 = "243c0ba2736f16da721e3a227004bbcdb8df6c846f198bd478172e00aa1faf42";
    const std::string h2 = "1ad0d40b3c09660d818b9e770eb1e84745dfbe941df159a64f8772b6fa2bfc3a";
    const std::string js =
        R"(var fs=r(84989);let fo=new aJ.l("fetchPlaylist","query",")" + h1 + R"(",null),fl=new aJ.l()"
        R"("fetchPlaylistMetadata","query",")" + h1 + R"(",null);let n=new i.l("addToLibrary","mutation",")" + h2 +
        R"(",null);let d=new i.l("fetchPlaylist","query",")" + std::string(64, 'f') + R"(",null);)"   // repeat: first wins
        R"(x("shortHash","query","abc123");y("notHex","query",")" + std::string(64, 'z') + R"(");)"
        R"(z(a,"query",")" + h1 + R"(");w("two words","query",")" + h1 + R"(");)"
        R"(u.miniCssF=e=>""+(({4406:"xpui-routes-search"})[e]||e)+"."+({4406:"11111111"})[e]+".css",)"
        R"(u.u=e=>""+(({4406:"xpui-routes-search",2706:"xpui-routes-recent-searches"})[e]||e)+"."+)"
        R"(({4406:"96d81afe",2706:"b94d0057",77:"deadbeef"})[e]+".js",r.p="",)"
        R"(u.p="https://open.spotifycdn.com/cdn/build/web-player/";)";
    const auto ops = extractPersistedOps(js);
    for (const auto& op : ops) std::printf("  op %-24s %-8s %.12s\n", op.name.c_str(), op.kind.c_str(), op.hash.c_str());
    CHECK(ops.size() == 3);
    if (ops.size() == 3) {
        CHECK(ops[0].name == "fetchPlaylist" && ops[0].kind == "query" && ops[0].hash == h1);
        CHECK(ops[1].name == "fetchPlaylistMetadata" && ops[1].hash == h1);
        CHECK(ops[2].name == "addToLibrary" && ops[2].kind == "mutation" && ops[2].hash == h2);
    }

    const auto chunks = findChunkUrls(js, "https://fallback.example/");
    for (const auto& c : chunks) std::printf("  chunk %-28s %s\n", c.name.c_str(), c.url.c_str());
    CHECK(chunks.size() == 3);   // the .css map is not a chunk list
    if (chunks.size() == 3) {
        CHECK(chunks[0].name == "xpui-routes-search");
        CHECK(chunks[0].url == "https://open.spotifycdn.com/cdn/build/web-player/xpui-routes-search.96d81afe.js");
        CHECK(chunks[1].url == "https://open.spotifycdn.com/cdn/build/web-player/xpui-routes-recent-searches.b94d0057.js");
        CHECK(chunks[2].name == "77" && chunks[2].url == "https://open.spotifycdn.com/cdn/build/web-player/77.deadbeef.js");
    }
    // Unnamed-only runtime, no public path: the base URL is used, and a preceding .css map is not taken as names.
    const auto bare = findChunkUrls(R"(})[e]+".css",u.u=e=>""+e+"."+({12:"aaaa1111",7:"bbbb2222"})[e]+".js")",
                                    "https://cdn.example/x/");
    CHECK(bare.size() == 2 && bare[0].url == "https://cdn.example/x/12.aaaa1111.js" && bare[1].name == "7");
    CHECK(findChunkUrls("no runtime here", "https://cdn.example/").empty());
    // A public path on a foreign host is ignored (the bundle's own directory is used instead).
    const auto foreign = findChunkUrls(R"(u.u=e=>""+e+"."+({5:"cccc3333"})[e]+".js",u.p="https://evil.example/js/";)",
                                       "https://open.spotifycdn.com/cdn/build/web-player/");
    CHECK(foreign.size() == 1 && foreign[0].url == "https://open.spotifycdn.com/cdn/build/web-player/5.cccc3333.js");
    CHECK(extractPersistedOps("").empty() && findBundleUrls("<html></html>").empty());

    std::printf("failure classifier:\n");
    std::string var;
    CHECK(classifyFailure(412, "\"Invalid query hash\"") == QueryFailure::HashRejected);   // live response 2026-09-28
    CHECK(classifyFailure(200, R"([{"message":"PersistedQueryNotFound"}])") == QueryFailure::HashRejected);
    CHECK(classifyFailure(400, R"({"errors":[{"message":"Variable '$libraryItemUris' of required type '[String!]!' was not provided."}]})",
                          &var) == QueryFailure::VariableError);
    CHECK(var == "libraryItemUris");
    CHECK(classifyFailure(400, R"({"errors":[{"extensions":{"code":"VALIDATION_INVALID_TYPE_VARIABLE"}}]})") ==
          QueryFailure::VariableError);
    CHECK(classifyFailure(400, "Bad Request") == QueryFailure::Other);
    CHECK(classifyFailure(429, "") == QueryFailure::Other);
    CHECK(classifyFailure(500, "variable") == QueryFailure::Other);
}

// Offline: built-in table, persisted overrides, adoption from the last scan and the refresh throttle (no network:
// every heal() below is answered from the scratch file or refused by the throttle).
static void testHashRegistryOffline() {
    std::printf("hash registry (offline):\n");
    auto& reg = HashRegistry::instance();
    const auto file = scratchHashStore();
    reg.setStorePath(file);
    for (const auto& [op, h] : HashRegistry::builtins()) {
        CHECK(std::strlen(h) == 64);
        CHECK(reg.hash(op) == h);
    }
    CHECK(reg.hash("noSuchOperation").empty());

    const std::string X(64, '1'), Y(64, '2'), Z(64, '3'), W(64, '4'), V(64, '5');
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    {
        const nlohmann::json j = {
            {"bundle", "web-player.test0001.js"},
            {"fetchedAt", now},
            {"hashes", {{"libraryV3", X}, {"searchDesktop", Y}, {"someNewOp", Z}}},
            {"replaced", {{"libraryV3", HashRegistry::builtin("libraryV3")}, {"searchDesktop", std::string(64, '0')}}},
            {"discovered", {{"getAlbum", W}, {"fetchLibraryTracks", V}, {"profileAttributes", HashRegistry::builtin("profileAttributes")}}},
            {"searched", {"getAlbum", "fetchLibraryTracks", "profileAttributes"}},
        };
        std::ofstream(file, std::ios::binary) << j.dump();
    }
    reg.setStorePath(file);   // reload
    CHECK(reg.bundle() == "web-player.test0001.js");
    CHECK(reg.hash("libraryV3") == X);                                        // learned against this build's built-in
    CHECK(reg.hash("searchDesktop") == HashRegistry::builtin("searchDesktop"));   // learned against another: dropped
    CHECK(reg.hash("someNewOp") == Z);
    CHECK(reg.hash("getAlbum") == HashRegistry::builtin("getAlbum"));             // a scan alone never overrides
    // getAlbum rejected: the (fresh) last scan knows another hash -> adopted, no network.
    CHECK(reg.heal("getAlbum", HashRegistry::builtin("getAlbum"), {}) == W);
    CHECK(reg.hash("getAlbum") == W);
    // ...rejected again: a scan < 10 min old (on disk) already searched for it -> no new scan, nothing to retry with.
    CHECK(reg.heal("getAlbum", W, {}).empty());
    // The web player sends the very hash that was rejected -> not a rotation, no retry.
    CHECK(reg.heal("profileAttributes", HashRegistry::builtin("profileAttributes"), {}).empty());
    // (An op the recent scan did NOT search for is scanned anyway: exercised live, `spotify_test hashes` step e.)
    // A second query that failed with the old hash while the first one healed reuses the result.
    CHECK(reg.heal("getAlbum", HashRegistry::builtin("getAlbum"), {}) == W);
    // setOverride is never persisted, even when another adoption saves the file.
    reg.setOverride("home", kBogusHash);
    CHECK(reg.hash("home") == kBogusHash);
    CHECK(reg.heal("fetchLibraryTracks", HashRegistry::builtin("fetchLibraryTracks"), {}) == V);
    {
        std::ifstream f(file, std::ios::binary);
        const nlohmann::json saved = nlohmann::json::parse(f, nullptr, false);
        CHECK(saved.is_object());
        if (saved.is_object()) {
            CHECK(saved["hashes"].value("getAlbum", "") == W);
            CHECK(saved["replaced"].value("getAlbum", "") == HashRegistry::builtin("getAlbum"));
            CHECK(saved["hashes"].value("fetchLibraryTracks", "") == V);
            CHECK(!saved["hashes"].contains("home"));
            CHECK(saved.dump().find(kBogusHash) == std::string::npos);
            CHECK(saved["hashes"].value("libraryV3", "") == X);
            CHECK(saved["searched"].is_array() && saved["searched"].size() == 3);
            CHECK(saved.value("fetchedAt", int64_t{0}) == now);   // no scan ran: the scan time is untouched
        }
    }
    reg.setStorePath(file);   // reload: adoptions survive a restart
    CHECK(reg.hash("getAlbum") == W && reg.hash("home") == HashRegistry::builtin("home"));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    reg.setStorePath({});   // back to the default (%LOCALAPPDATA%\ShadeTube\spotify-hashes.json)
}

// Live, read-only. (a) needs no login: scan the current web player and compare with the built-in table. With a
// session: (b) the raw response to an unknown hash, (c) the self-heal path end to end, (e) a chunk-only op healing
// right after a scan that did not look for it, (d) whether the current documents of the ops whose built-in hash is
// older still accept our variables (what a heal would switch them to).
static Discovery g_discovery;
static std::vector<std::string> g_mismatched;

static bool testHashDiscoveryLive() {
    std::printf("hashes (live, read-only):\n");
    std::vector<std::string> wanted;
    for (const auto& [op, h] : HashRegistry::builtins()) wanted.push_back(op);
    Discovery& d = g_discovery;
    try {
        const auto t0 = std::chrono::steady_clock::now();
        d = discoverHashes(wanted, {});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  (a) %s: %zu persisted ops, %d chunk(s) downloaded, %lld ms\n", d.bundle.c_str(), d.ops.size(),
                    d.chunksScanned, static_cast<long long>(ms));
    } catch (const std::exception& e) {
        std::printf("  FAIL discovery: %s\n", e.what());
        ++g_failures;
        return false;
    }
    std::vector<std::string>& mismatched = g_mismatched;
    int found = 0;
    for (const auto& [op, h] : HashRegistry::builtins()) {
        const auto it = d.ops.find(op);
        const bool chunk = std::find(d.fromChunks.begin(), d.fromChunks.end(), op) != d.fromChunks.end();
        const bool registry = std::find(d.fromRegistry.begin(), d.fromRegistry.end(), op) != d.fromRegistry.end();
        const char* where = registry ? "registry" : chunk ? "chunk" : "bundle";
        if (it == d.ops.end()) {
            std::printf("      %-22s MISSING (built-in %.8s…)\n", op, h);
            continue;
        }
        ++found;
        if (it->second.hash == h) {
            std::printf("      %-22s ok        %-8s %.8s… (%s)\n", op, it->second.kind.c_str(), h, where);
        } else {
            std::printf("      %-22s MISMATCH  %-8s built-in %.8s… current %.8s… (%s)\n", op, it->second.kind.c_str(), h,
                        it->second.hash.c_str(), where);
            mismatched.push_back(op);
        }
    }
    std::printf("      found %d/%zu, mismatched %zu\n", found, HashRegistry::builtins().size(), mismatched.size());
    CHECK(found == static_cast<int>(HashRegistry::builtins().size()));
    CHECK(d.ops.size() > 50);
    // Chunk ranking: an op's own route chunk ("searchDesktop" -> xpui-routes-search) is among the first tried.
    CHECK(d.chunksScanned <= 2 * static_cast<int>(d.fromChunks.size()));
    return true;
}

static void testHashesLive(Api& api, const std::string& token, const std::string& sp) {
    Discovery& d = g_discovery;
    // (b) The raw Pathfinder answer to an unknown hash.
    {
        const nlohmann::json body = {
            {"variables", nlohmann::json::object()},
            {"operationName", "profileAttributes"},
            {"extensions", {{"persistedQuery", {{"version", 1}, {"sha256Hash", kBogusHash}}}}},
        };
        http::HttpRequest r;
        r.method = "POST";
        r.url = "https://api-partner.spotify.com/pathfinder/v2/query";
        r.headers = {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"},
                     {"Accept", "application/json"}, {"User-Agent", kUserAgent}, {"App-Platform", "WebPlayer"},
                     {"Cookie", "sp_dc=" + sp + ";"}};
        r.body = body.dump();
        const auto resp = http::send(r, {});
        std::printf("  (b) unknown hash -> HTTP %d %s\n", resp.statusCode, resp.body.substr(0, 200).c_str());
        CHECK(classifyFailure(resp.statusCode, resp.body) == QueryFailure::HashRejected);
    }

    // (c) Self-heal: profileAttributes starts from a wrong hash; me() must still succeed (412 -> scan -> retry).
    auto& reg = HashRegistry::instance();
    const auto store = scratchHashStore();
    reg.setStorePath(store);
    reg.setOverride("profileAttributes", kBogusHash);
    try {
        const UserProfile me = api.me({});
        const std::string healed = reg.hash("profileAttributes");
        std::printf("  (c) me() with a wrong hash: %s; profileAttributes now %.8s… (bundle %s)\n",
                    me.valid() ? "ok" : "EMPTY", healed.c_str(), reg.bundle().c_str());
        CHECK(me.valid());
        CHECK(healed != kBogusHash && healed == d.ops["profileAttributes"].hash);
        std::ifstream f(store, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(!saved.empty() && saved.find(kBogusHash) == std::string::npos);
    } catch (const ApiError& e) {
        std::printf("  FAIL (c) self-heal: HTTP %d %s (see shadetube.log)\n", e.status, e.what());
        ++g_failures;
    }

    // (e) A chunk-only op right after (c)'s scan, which only looked for profileAttributes (main bundle): the fresh scan
    // did not search for searchDesktop, so its heal must scan again (and open xpui-routes-search) instead of giving up.
    // Also proves the current searchDesktop document takes our variables.
    reg.setOverride("searchDesktop", kBogusHash);
    try {
        const auto sr = api.search("duman", 8);
        const std::string healed = reg.hash("searchDesktop");
        std::printf("  (e) search() with a wrong hash right after (c): %zu artists, %zu albums, %zu tracks; "
                    "searchDesktop now %.8s…\n",
                    sr.artists.size(), sr.albums.size(), sr.tracks.size(), healed.c_str());
        CHECK(!sr.tracks.empty());
        CHECK(healed == d.ops["searchDesktop"].hash);
    } catch (const ApiError& e) {
        std::printf("  FAIL (e) chunk-op heal: HTTP %d %s (see shadetube.log)\n", e.status, e.what());
        ++g_failures;
    }

    // (d) Ops whose built-in document is older than the web player's: does the current one take our variables?
    for (const auto& op : g_mismatched) {
        if (op == "searchDesktop") continue;   // (e) ran it with the current hash
        const std::string current = d.ops[op].hash;
        reg.setOverride(op, current);
        try {
            if (op == "queryArtistOverview") {
                const auto ap = api.artistPage("spotify:artist:0TnOYISbd1XYRBk9myaseg");
                std::printf("  (d) queryArtistOverview @%.8s…: \"%s\", %zu top tracks, %zu albums\n", current.c_str(),
                            ap.artist.name.c_str(), ap.topTracks.size(), ap.albums.size());
                CHECK(!ap.artist.name.empty() && !ap.topTracks.empty());
            } else {
                std::printf("  (d) %s @%.8s…: no read-only probe for this op (skipped)\n", op.c_str(), current.c_str());
            }
        } catch (const ApiError& e) {
            std::printf("  FAIL (d) %s with the current hash: HTTP %d %s (body in shadetube.log)\n", op.c_str(), e.status,
                        e.what());
            ++g_failures;
        }
    }
    std::error_code ec;
    std::filesystem::remove(store, ec);
    reg.setStorePath({});
}

static void testLive(bool homeOnly, const char* dumpPath, bool playlistEdit, bool radio, bool hashes) {
    std::string sp;
    if (const char* e = std::getenv("SHADETUBE_SPDC"); e && *e) sp = e;
    else sp = store::loadCookie();   // decrypts %LOCALAPPDATA%\ShadeTube\spotify.dat for this user
    if (sp.empty()) {
        std::printf("live: skipped (no SHADETUBE_SPDC and no saved sp_dc — log in via the app first)\n");
        return;
    }
    std::printf("live: sp_dc present (len=%zu). Fetching web-player token...\n", sp.size());
    try {
        const int64_t serverT = fetchServerTimeSeconds({});
        std::printf("  server-time=%lld\n", static_cast<long long>(serverT));
        const TokenBundle tok = fetchAccessToken(sp, {});
        std::printf("  token ok: len=%zu  anonymous=%d  expiresAtMs=%lld\n", tok.accessToken.size(), tok.anonymous ? 1 : 0,
                    static_cast<long long>(tok.expiresAtMs));
        CHECK(tok.valid());
        CHECK(!tok.anonymous);

        Api api;
        api.setCredentials(tok.accessToken, sp);
        if (hashes) {
            testHashesLive(api, tok.accessToken, sp);
            return;
        }
        if (homeOnly) {
            testHomeLive(api, dumpPath);
            return;
        }
        if (playlistEdit) {
            testPlaylistEditLive(api);
            return;
        }
        if (radio) {
            testRadioLive(api);
            return;
        }
        const UserProfile me = api.me({});
        std::printf("  profile: name=\"%s\"  username=\"%s\"  hasImage=%d\n", me.name.c_str(), me.username.c_str(),
                    me.imageUrl.empty() ? 0 : 1);
        CHECK(me.valid());

        const auto playlists = api.libraryPlaylists({});
        const auto albums = api.libraryAlbums({});
        const auto artists = api.libraryArtists({});
        std::printf("  library: %zu playlists, %zu albums, %zu artists\n", playlists.size(), albums.size(),
                    artists.size());

        // The failing case: opening a real playlist. Try Liked Songs and the first non-Liked playlist,
        // reporting each independently so we can see exactly which call (and variables) 400s.
        try {
            Playlist meta;
            std::string owner;
            const auto liked = api.playlistTracks(Api::kLikedSongsUri, 0, 25, {}, &meta, &owner);
            std::printf("  liked songs: total=%d, first page=%zu\n", liked.total, liked.items.size());
            if (!liked.items.empty())
                std::printf("    e.g. \"%s\" — %s\n", liked.items[0].name.c_str(), liked.items[0].artistLine().c_str());
        } catch (const std::exception& e) {
            std::printf("  FAIL liked (fetchPlaylist collection:tracks): %s\n", e.what());
            ++g_failures;
        }
        std::string firstUri;
        for (const auto& p : playlists)
            if (p.id != Api::kLikedSongsUri) {
                firstUri = p.id;
                break;
            }
        if (!firstUri.empty()) {
            try {
                Playlist meta;
                std::string owner;
                const auto pg = api.playlistTracks(firstUri, 0, 25, {}, &meta, &owner);
                std::printf("  playlist \"%s\" (owner %s): total=%d, first page=%zu\n", meta.name.c_str(), owner.c_str(),
                            pg.total, pg.items.size());
                if (!pg.items.empty())
                    std::printf("    e.g. \"%s\" — %s\n", pg.items[0].name.c_str(), pg.items[0].artistLine().c_str());
            } catch (const std::exception& e) {
                std::printf("  FAIL playlist (fetchPlaylist %s): %s\n", firstUri.c_str(), e.what());
                ++g_failures;
            }
        }
        // A real music playlist (skip pseudo "Your Episodes" and empty ones).
        std::string musicUri;
        for (const auto& p : playlists)
            if (p.id != Api::kLikedSongsUri && p.name != "Your Episodes") {
                musicUri = p.id;
                break;
            }
        if (!musicUri.empty()) {
            try {
                Playlist m2;
                const auto pg = api.playlistTracks(musicUri, 0, 25, {}, &m2, nullptr);
                std::printf("  music playlist \"%s\": total=%d, first page=%zu\n", m2.name.c_str(), pg.total, pg.items.size());
                if (!pg.items.empty())
                    std::printf("    e.g. \"%s\" — %s\n", pg.items[0].name.c_str(), pg.items[0].artistLine().c_str());
            } catch (const std::exception& e) {
                std::printf("  FAIL music playlist: %s\n", e.what());
                ++g_failures;
            }
        }
        // Album (getAlbum) — use the first liked track's album.
        try {
            const auto liked = api.playlistTracks(Api::kLikedSongsUri, 0, 1);
            if (!liked.items.empty() && !liked.items[0].album.id.empty()) {
                const auto al = api.album(liked.items[0].album.id);
                std::printf("  album \"%s\": %zu tracks\n", al.name.c_str(), al.tracks.size());
            }
        } catch (const std::exception& e) {
            std::printf("  FAIL album (getAlbum): %s\n", e.what());
            ++g_failures;
        }
        // Search (searchDesktop — hash not in the registry, so this is the risky one).
        try {
            const auto sr = api.search("duman", 8);
            std::printf("  search 'duman': %zu artists, %zu albums, %zu tracks\n", sr.artists.size(), sr.albums.size(),
                        sr.tracks.size());
        } catch (const std::exception& e) {
            std::printf("  FAIL search (searchDesktop): %s\n", e.what());
            ++g_failures;
        }
        // Artist overview (queryArtistOverview — also not in the registry).
        try {
            if (!artists.empty()) {
                const auto ap = api.artistPage(artists[0].id);
                std::printf("  artist \"%s\": %zu top tracks, %zu albums\n", ap.artist.name.c_str(), ap.topTracks.size(),
                            ap.albums.size());
            }
        } catch (const std::exception& e) {
            std::printf("  FAIL artist (queryArtistOverview): %s\n", e.what());
            ++g_failures;
        }
        // Mutation (Web API v1) — verified WITHOUT side effects: an already-liked track (idempotent PUT).
        try {
            const auto lk = api.playlistTracks(Api::kLikedSongsUri, 0, 1);
            if (!lk.items.empty()) {
                const std::string tid = lk.items[0].id;
                const auto contains = api.tracksSaved({tid});
                std::printf("  mutation contains-check: %s\n",
                            (!contains.empty() && contains[0]) ? "true (already liked)" : "false");
                const bool ok = api.setTracksSaved({tid}, true);   // idempotent: already saved, no net change
                std::printf("  mutation PUT /me/tracks (idempotent): %s\n", ok ? "200 OK" : "FAILED");
                if (!ok) ++g_failures;
            }
        } catch (const std::exception& e) {
            std::printf("  FAIL mutation: %s\n", e.what());
            ++g_failures;
        }
        // Personalized home shelves (home).
        testHomeLive(api, nullptr);
    } catch (const std::exception& e) {
        std::printf("  FAIL live: %s\n", e.what());
        ++g_failures;
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);   // shelf titles are UTF-8 (Turkish)
    st::log::init();   // so query()'s HTTP-error bodies land in shadetube.log
    const bool homeOnly = argc > 1 && std::strcmp(argv[1], "home") == 0;
    const bool playlistEdit = argc > 1 && std::strcmp(argv[1], "playlist-edit") == 0;
    const bool radio = argc > 1 && std::strcmp(argv[1], "radio") == 0;
    const bool hashes = argc > 1 && std::strcmp(argv[1], "hashes") == 0;
    const char* dumpPath = homeOnly && argc > 2 ? argv[2] : nullptr;
    testBase32();
    testTotp();
    testHomeFixture();
    testBuddylistFixture();
    testReleasesFixtures();
    testPlaylistTree();
    testHashExtractor();
    testHashRegistryOffline();
    if (argc > 1 && std::strcmp(argv[1], "offline") == 0) std::printf("live: skipped (offline)\n");
    else if (hashes && !testHashDiscoveryLive()) std::printf("live: skipped (hash discovery failed)\n");
    else if (hashes && argc > 2 && std::strcmp(argv[2], "scan") == 0) std::printf("live: skipped (hashes scan only)\n");
    else testLive(homeOnly, dumpPath, playlistEdit, radio, hashes);
    st::log::shutdown();
    if (g_failures == 0) std::printf("\nAll offline checks passed.\n");
    else std::printf("\n%d check(s) FAILED.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
