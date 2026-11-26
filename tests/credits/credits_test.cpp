// credits_test: song credits (docs/DEVELOPMENT.md, "Song credits").
//
//   model     : role names folded, the groups they belong to (with the source's group title as the hint), one person's
//               roles merged per group, sources deduplicated
//   spotify   : the web player's credits (queryTrackCreditsGroupedModal data) and the legacy track-credits-view JSON
//   mb        : a recording lookup's artist / work / label relationships, song titles cleaned for the search, the
//               recording picked from a search (title, artist, length)
//   cache     : app/Credits: JSON round trip and expiry, the provider chain (first answer naming somebody wins, a failure
//               isn't cached when nobody is known, the cache answers without asking), memory + disk, the summary list
//
// Offline (fixtures in tests/credits/fixtures). Everything runs in a temp folder (SHADETUBE_DATA_DIR points there).
//
//   credits_test live ["Artist" "Title" [seconds]] : MusicBrainz for real (search, then the recording's credits;
//                                                    read-only); with SHADETUBE_SPDC also Spotify's credits for
//                                                    SHADETUBE_CREDITS_TRACK (a spotify:track: URI)
#include "app/Credits.h"
#include "catalog/Credits.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "musicbrainz/Parse.h"
#include "spotify/Auth.h"
#include "spotify/SpotifyApi.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace st;
using catalog::Credit;
using catalog::CreditGroup;
using catalog::Credits;
namespace fs = std::filesystem;
using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

static json fixture(const char* name) {
    std::ifstream in(fs::path(CREDITS_FIXTURE_DIR) / name, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) {
        std::printf("  FAIL cannot load fixture %s\n", name);
        ++g_failures;
    }
    return j;
}

static const Credit* find(const Credits& c, CreditGroup g, const std::string& name) {
    for (const auto& p : c.group(g))
        if (p.name == name) return &p;
    return nullptr;
}

static bool roles(const Credit* p, std::vector<std::string> want) { return p && p->roles == want; }

static void print(const Credits& c) {
    static const char* kNames[] = {"performers", "writers", "producers", "engineers", "other"};
    for (size_t g = 0; g < catalog::kCreditGroups; ++g) {
        if (c.groups[g].empty()) continue;
        std::printf("    %s:\n", kNames[g]);
        for (const auto& p : c.groups[g]) {
            std::string r;
            for (const auto& x : p.roles) r += (r.empty() ? "" : ", ") + x;
            std::printf("      %s  [%s]%s%s\n", p.name.c_str(), r.c_str(), p.artistId.empty() ? "" : "  -> ", p.artistId.c_str());
        }
    }
    std::string s;
    for (const auto& x : c.sources) s += (s.empty() ? "" : " / ") + x;
    if (!s.empty()) std::printf("    sources: %s\n", s.c_str());
}

static void testModel() {
    std::printf("model\n");
    using catalog::canonicalRole;
    using catalog::roleGroup;
    CHECK(canonicalRole("  Lead   Vocals ") == "lead vocals");
    CHECK(canonicalRole("Songwriter") == "writer" && canonicalRole("Mix") == "mixing engineer");
    CHECK(canonicalRole("Primary_Artist") == "main artist" && canonicalRole("Backing Vocals") == "background vocals");
    CHECK(canonicalRole("Co Producer") == "co-producer" && canonicalRole("Piano") == "piano" && canonicalRole("") == "");
    CHECK(roleGroup("Main Artist") == CreditGroup::Performers && roleGroup("Guitar") == CreditGroup::Performers);
    CHECK(roleGroup("Composer") == CreditGroup::Writers && roleGroup("Lyricist") == CreditGroup::Writers);
    CHECK(roleGroup("Executive Producer") == CreditGroup::Producers && roleGroup("Vocal Producer") == CreditGroup::Producers);
    CHECK(roleGroup("Mastering Engineer") == CreditGroup::Engineers && roleGroup("Assistant Recording Engineer") == CreditGroup::Engineers);
    // Unknown roles go by the source's group title.
    CHECK(roleGroup("Theremin", "Performers") == CreditGroup::Performers);
    CHECK(roleGroup("Thanks", "Composition & Lyrics") == CreditGroup::Writers);
    CHECK(roleGroup("Studio Assistant", "Production & Engineering") == CreditGroup::Producers);
    CHECK(roleGroup("Art Direction") == CreditGroup::Other && roleGroup("", "") == CreditGroup::Other);

    Credits c;
    CHECK(c.empty() && c.people() == 0);
    c.add("Freddie Mercury", "", "Composer");
    c.add("freddie mercury ", "mbid-1", "lyricist");   // same person (ASCII case, spaces): merged, id taken over
    c.add("Freddie Mercury", "mbid-2", "Composer");    // the role once, the first id kept
    c.add("Freddie Mercury", "", "Piano", CreditGroup::Performers);   // another group: another entry
    c.add("  ", "x", "Producer");                       // blank: ignored
    c.add("Roy Thomas Baker", "", "");                  // listed without a role (Other)
    CHECK(!c.empty() && c.people() == 3);
    const Credit* fm = find(c, CreditGroup::Writers, "Freddie Mercury");
    CHECK(roles(fm, {"composer", "lyricist"}) && fm && fm->artistId == "mbid-1");
    CHECK(roles(find(c, CreditGroup::Performers, "Freddie Mercury"), {"piano"}));
    CHECK(roles(find(c, CreditGroup::Other, "Roy Thomas Baker"), {}));
    c.addSource("EMI");
    c.addSource(" emi ");
    c.addSource("");
    c.addSource("Hollywood Records");
    CHECK((c.sources == std::vector<std::string>{"EMI", "Hollywood Records"}));
}

static void testSpotify() {
    std::printf("spotify\n");
    const Credits c = spotify::Api::parseTrackCredits(fixture("spotify_credits.json"));
    print(c);
    CHECK(c.provider == "spotify");
    CHECK(c.group(CreditGroup::Performers).size() == 2 && c.group(CreditGroup::Performers)[0].name == "Queen");
    const Credit* queen = find(c, CreditGroup::Performers, "Queen");
    CHECK(roles(queen, {"main artist"}) && queen && queen->artistId == "spotify:artist:1dfeR4HaWDbWqFHLkxsg1d");
    CHECK(roles(find(c, CreditGroup::Performers, "Brian May"), {"guitar"}));
    const Credit* fm = find(c, CreditGroup::Writers, "Freddie Mercury");
    CHECK(roles(fm, {"composer", "lyricist"}) && fm && fm->artistId == "spotify:artist:2qCdWVPhRbA4G9cvhI7mHn");   // reference.uri
    const Credit* arr = find(c, CreditGroup::Writers, "Some Arranger");
    CHECK(roles(arr, {"arranger"}) && arr && arr->artistId.empty());   // a spotify:creator: is no artist page
    CHECK(roles(find(c, CreditGroup::Writers, "Unknown Helper"), {"thanks"}));   // by its group title
    CHECK(c.group(CreditGroup::Producers).size() == 2 && roles(find(c, CreditGroup::Producers, "Roy Thomas Baker"), {"producer"}));
    CHECK(find(c, CreditGroup::Producers, "Roy Thomas Baker")->artistId.empty());   // "uri": null
    CHECK(roles(find(c, CreditGroup::Engineers, "Mike Stone"), {"mixing engineer"}));
    CHECK(roles(find(c, CreditGroup::Other, "Kosh"), {"art direction"}));
    CHECK(c.people() == 9);   // the nameless entry dropped
    CHECK((c.sources == std::vector<std::string>{"EMI", "Hollywood Records"}));
    // Not a track / no credits trait: nobody, no crash.
    CHECK(spotify::Api::parseTrackCredits(json{{"trackUnion", {{"__typename", "NotFound"}}}}).empty());
    CHECK(spotify::Api::parseTrackCredits(json()).empty() && spotify::Api::parseTrackCredits(json::array()).empty());

    const Credits legacy = spotify::Api::parseLegacyCredits(fixture("spotify_credits_legacy.json"));
    print(legacy);
    CHECK(roles(find(legacy, CreditGroup::Performers, "Queen"), {"main artist"}));
    const Credit* lfm = find(legacy, CreditGroup::Writers, "Freddie Mercury");
    CHECK(roles(lfm, {"composer", "lyricist"}) && lfm && lfm->artistId == "spotify:artist:2qCdWVPhRbA4G9cvhI7mHn");   // creatorUri
    CHECK(roles(find(legacy, CreditGroup::Producers, "Roy Thomas Baker"), {"producer"}));
    CHECK(roles(find(legacy, CreditGroup::Producers, "Queen"), {}));   // no subroles: listed by its group
    CHECK((legacy.sources == std::vector<std::string>{"EMI"}));
}

static void testMusicBrainz() {
    std::printf("mb\n");
    namespace d = mb::detail;
    const Credits c = d::parseRecordingCredits(fixture("mb_recording_credits.json"));
    print(c);
    CHECK(c.provider == "musicbrainz");
    const auto& perf = c.group(CreditGroup::Performers);
    CHECK(perf.size() == 4 && perf[0].name == "Queen" && perf[0].artistId == "0383dadf-2a4e-4d10-a46a-e9e041da8eb3");
    CHECK(roles(find(c, CreditGroup::Performers, "Queen"), {"main artist"}));
    CHECK(roles(find(c, CreditGroup::Performers, "Freddie Mercury"), {"lead vocals", "piano"}));
    CHECK(roles(find(c, CreditGroup::Performers, "Brian Harold May"), {"electric guitar"}));   // credited name, "guest" dropped
    CHECK(roles(find(c, CreditGroup::Performers, "Roger Taylor"), {"vocals"}));
    CHECK(roles(find(c, CreditGroup::Writers, "Freddie Mercury"), {"composer", "lyricist"}));   // from the work
    CHECK(roles(find(c, CreditGroup::Producers, "Roy Thomas Baker"), {"producer"}));
    CHECK(roles(find(c, CreditGroup::Producers, "Queen"), {"producer"}));
    CHECK(roles(find(c, CreditGroup::Producers, "John Reid"), {"executive producer"}));
    CHECK(roles(find(c, CreditGroup::Engineers, "Gary Lyons"), {"assistant engineer"}));
    CHECK(roles(find(c, CreditGroup::Engineers, "Mike Stone"), {"mixing engineer", "recording engineer"}));
    CHECK(roles(find(c, CreditGroup::Other, "Mick Rock"), {"photography"}));
    CHECK(!find(c, CreditGroup::Other, "Tea Maker"));   // "misc" is left out
    CHECK((c.sources == std::vector<std::string>{"EMI Records Ltd"}));   // the publisher is no source
    CHECK(d::parseRecordingCredits(json()).empty());

    CHECK(d::searchableTitle("Bohemian Rhapsody - Remastered 2011") == "Bohemian Rhapsody");
    CHECK(d::searchableTitle("Señorita (feat. Camila Cabello)") == "Señorita");
    CHECK(d::searchableTitle("Song [with Someone] - Radio Edit") == "Song");
    CHECK(d::searchableTitle("Stay (with Justin Bieber)") == "Stay");
    CHECK(d::searchableTitle("Live - Forever") == "Live - Forever");
    CHECK(d::searchableTitle("Love (Interlude)") == "Love (Interlude)");
    CHECK(d::searchableTitle("A (Ft. B) C") == "A C");

    const json search = fixture("mb_recording_search.json");
    auto id = [](int n) { return "aaaaaaaa-0000-4000-8000-00000000000" + std::to_string(n); };
    // 355 s: within 2 s #7 (on three releases) before #5 (1 s off, Queen among two credited artists, upper case), then
    // within 5 s #3 (3 s) and #6 (no length: 5 s); #1 is 55 s off, #2 another title, #4 another artist.
    CHECK((d::rankRecordings(search, "Bohemian Rhapsody - Remastered 2011", "Queen", 355000) ==
           std::vector<std::string>{id(7), id(5), id(3), id(6)}));
    CHECK(d::pickRecording(search, "Bohemian Rhapsody - Remastered 2011", "Queen", 355000) == id(7));
    CHECK(d::pickRecording(search, "Bohemian Rhapsody", "Queen", 359500) == id(3));   // 1.5 s beats #7's 4.7 s
    // Length unknown: every hit counts as 5 s off; the releases, then the score decide.
    CHECK(d::pickRecording(search, "Bohemian Rhapsody", "Queen", 0) == id(7));
    // Nothing within 10 s: only the hit without a length is left.
    CHECK(d::pickRecording(search, "Bohemian Rhapsody", "Queen", 200000) == id(6));
    CHECK(d::pickRecording(search, "Another Song", "Queen", 355000).empty());
    CHECK(d::pickRecording(search, "Bohemian Rhapsody", "Nobody", 355000).empty());
    CHECK(d::pickRecording(json(), "Bohemian Rhapsody", "Queen", 355000).empty());
}

static Credits sample(const std::string& name) {
    Credits c;
    c.provider = "spotify";
    c.add(name, "spotify:artist:x", "Main Artist");
    c.add("Writer One", "", "Composer");
    return c;
}

static void testCache() {
    std::printf("cache\n");
    namespace cr = app::credits;
    cr::setCacheDir(g_dir / L"credits");
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    // JSON round trip; expiry (30 days found, 3 days "nobody").
    Credits c = spotify::Api::parseTrackCredits(fixture("spotify_credits.json"));
    auto back = cr::fromJson(cr::toJson(c, now), now);
    CHECK(back && back->provider == "spotify" && back->people() == c.people() && back->sources == c.sources);
    CHECK(back && back->group(CreditGroup::Writers)[0].roles == c.group(CreditGroup::Writers)[0].roles);
    CHECK(back && back->group(CreditGroup::Performers)[0].artistId == "spotify:artist:1dfeR4HaWDbWqFHLkxsg1d");
    CHECK(cr::fromJson(cr::toJson(c, now - cr::kFoundTtlSec + 60), now).has_value());
    CHECK(!cr::fromJson(cr::toJson(c, now - cr::kFoundTtlSec - 60), now));
    CHECK(!cr::fromJson(cr::toJson(Credits{"musicbrainz", {}, {}}, now - cr::kNoneTtlSec - 60), now));
    CHECK(!cr::fromJson("{\"v\":2}", now) && !cr::fromJson("garbage", now));
    CHECK(cr::fromJson(R"({"v":1,"at":1,"groups":[[["A","",["x"]],[1,2],"bad"]],"sources":["S",3]})", 1).value().people() == 1);

    // The chain: the first provider that names somebody wins, its answer is cached (memory and disk).
    int a = 0, b = 0;
    cr::Request r;
    r.cacheKey = "spotify:track:one";
    r.providers = {[&](const cr::CT&) -> std::optional<Credits> { ++a; return Credits{"spotify", {}, {"EMI"}}; },   // nobody
                   [&](const cr::CT&) -> std::optional<Credits> { ++b; return sample("Queen"); }};
    auto got = cr::fetch(r);
    CHECK(got && a == 1 && b == 1 && got->group(CreditGroup::Performers)[0].name == "Queen");
    CHECK(fs::exists(g_dir / L"credits" / L"spotify_track_one.json"));
    got = cr::fetch(r);
    CHECK(got && a == 1 && b == 1);   // the memory cache
    cr::clearMemory();
    got = cr::fetch(r);
    CHECK(got && a == 1 && b == 1 && got->people() == 2);   // the disk cache
    CHECK(cr::cached("spotify:track:one").has_value() && !cr::cached("spotify:track:none").has_value());

    // Nobody known, and a provider failed: returned, but not cached (asked again next time).
    a = b = 0;
    cr::Request f;
    f.cacheKey = "local:failed";
    f.providers = {[&](const cr::CT&) -> std::optional<Credits> { ++a; return std::nullopt; },
                   [&](const cr::CT&) -> std::optional<Credits> { ++b; return Credits{"musicbrainz", {}, {}}; }};
    got = cr::fetch(f);
    CHECK(got && got->empty() && a == 1 && b == 1);
    got = cr::fetch(f);
    CHECK(a == 2 && b == 2 && !fs::exists(g_dir / L"credits" / L"local_failed.json"));
    // Every provider answered "nobody": that is cached.
    cr::Request n;
    n.cacheKey = "local:none";
    n.providers = {[&](const cr::CT&) -> std::optional<Credits> { ++a; return Credits{"musicbrainz", {}, {}}; }};
    a = 0;
    cr::fetch(n);
    got = cr::fetch(n);
    CHECK(got && got->empty() && a == 1);
    // Every provider failed: nothing (the UI says so), nothing cached.
    cr::Request x;
    x.cacheKey = "local:offline";
    x.providers = {[&](const cr::CT&) -> std::optional<Credits> { throw std::runtime_error("offline"); },
                   [&](const cr::CT&) -> std::optional<Credits> { return std::nullopt; }};
    CHECK(!cr::fetch(x) && !cr::cached("local:offline"));
    // Nobody to ask: nobody known (not "failed"), nothing cached.
    cr::Request empty;
    empty.cacheKey = "local:noartist";
    auto none = cr::fetch(empty);
    CHECK(none && none->empty() && !cr::cached("local:noartist"));

    // The memory cache stays bounded.
    for (int i = 0; i < static_cast<int>(cr::kMemoryEntries) + 10; ++i) {
        cr::Request m;
        m.cacheKey = "mem:" + std::to_string(i);
        m.providers = {[i](const cr::CT&) -> std::optional<Credits> { return sample("P" + std::to_string(i)); }};
        cr::fetch(m);
    }
    CHECK(cr::cached("mem:0").has_value());   // evicted from memory, still on disk

    // requestFor: Spotify first only for a spotify:track: with an Api; MusicBrainz always (by name or MBID).
    catalog::Track t;
    t.id = "spotify:track:4u7EnebtmKWzUH433cf5Qv";
    t.name = "Bohemian Rhapsody";
    t.artists = {{"spotify:artist:1dfeR4HaWDbWqFHLkxsg1d", "Queen"}};
    spotify::Api api;
    CHECK(cr::requestFor(&api, t).providers.size() == 2 && cr::requestFor(nullptr, t).providers.size() == 1);
    t.id = "local:abc";
    CHECK(cr::requestFor(&api, t).providers.size() == 1 && cr::requestFor(&api, t).cacheKey == "local:abc");
    t.artists.clear();
    CHECK(cr::requestFor(&api, t).providers.empty());   // a title alone: somebody else's song of that name
    t.artists = {{"", "Queen"}};
    t.name.clear();
    CHECK(cr::requestFor(&api, t).providers.empty());   // nothing to search by
    t.id = "b1a9c0e9-d987-4042-ae91-78d6a3267d69";
    CHECK(cr::requestFor(nullptr, t).providers.size() == 1);   // an MBID is looked up directly

    // The summary: two per group in group order, a person once with the roles of every group.
    const auto s = cr::summary(mb::detail::parseRecordingCredits(fixture("mb_recording_credits.json")));
    CHECK(s.size() == 5);
    if (s.size() == 5) {
        CHECK(s[0].name == "Queen" && (s[0].roles == std::vector<std::string>{"main artist", "producer"}));
        CHECK(s[1].name == "Freddie Mercury" &&
              (s[1].roles == std::vector<std::string>{"lead vocals", "piano", "composer", "lyricist"}));
        CHECK(s[2].name == "Roy Thomas Baker" && s[3].name == "Gary Lyons" && s[4].name == "Mike Stone");
        CHECK(s[4].roles.size() == 2);
    }
    CHECK(cr::summary(Credits{}).empty() && cr::summary(c, 2).size() == 2);
}

static void testLive() {
    std::printf("live\n");
    // The arguments as UTF-8 (char** argv is in the ANSI code page: "Şımarık" would not survive it).
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto arg = [&](int i, const char* fallback) { return i < argc ? toUtf8(wargv[i]) : std::string(fallback); };
    const std::string artist = arg(2, "Queen");
    const std::string title = arg(3, "Bohemian Rhapsody");
    const int seconds = std::atoi(arg(4, "355").c_str());
    LocalFree(wargv);
    try {
        const auto recs = mb::findRecordings(title, artist, seconds * 1000);
        std::printf("  musicbrainz recordings for %s - %s:", artist.c_str(), title.c_str());
        for (const auto& r : recs) std::printf(" %s", r.c_str());
        std::printf("%s\n", recs.empty() ? " (none)" : "");
        CHECK(!recs.empty());
        const Credits c = mb::songCredits(title, artist, seconds * 1000);
        print(c);
        CHECK(!c.empty());
        // The app's chain end to end (MusicBrainz by name: no Api), then the same answer from its cache.
        catalog::Track t;
        t.id = "live:" + artist + ":" + title;
        t.name = title;
        t.artists = {{"", artist}};
        t.durationMs = seconds * 1000;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = app::credits::fetch(app::credits::requestFor(nullptr, t));
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(r && !r->empty());
        app::credits::clearMemory();
        auto again = app::credits::fetch(app::credits::requestFor(nullptr, t));
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(again && r && again->people() == r->people());
        std::printf("  chain: %lld ms, from the cache: %lld ms\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()));
    } catch (const std::exception& e) {
        std::printf("  FAIL live musicbrainz: %s\n", e.what());
        ++g_failures;
    }
    // Spotify only with SHADETUBE_SPDC: the app profile's cookie is not read here.
    const char* sp = std::getenv("SHADETUBE_SPDC");
    if (!sp || !*sp) {
        std::printf("  spotify: skipped (no SHADETUBE_SPDC)\n");
        return;
    }
    const char* uri = std::getenv("SHADETUBE_CREDITS_TRACK");
    try {
        const auto tok = spotify::fetchAccessToken(sp, {});
        CHECK(tok.valid());
        spotify::Api api;
        api.setCredentials(tok.accessToken, sp);
        const Credits c = api.trackCredits(uri && *uri ? uri : "spotify:track:4u7EnebtmKWzUH433cf5Qv");
        print(c);
        CHECK(!c.empty());
    } catch (const std::exception& e) {
        std::printf("  FAIL live spotify: %s\n", e.what());
        ++g_failures;
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_credits_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()

    testModel();
    testSpotify();
    testMusicBrainz();
    testCache();
    if (argc > 1 && std::string(argv[1]) == "live") testLive();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall credits checks passed\n");
    return 0;
}
