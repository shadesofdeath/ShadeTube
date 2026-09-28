// radio_test: checks for app/InternetRadio (Radyo: radio-browser.info client, station model, radio.json store).
//
//   parsing : saved radio-browser.info responses (tests/radio/fixtures): stations, tags, countries, servers;
//             malformed JSON, wrong shapes, entries without a uuid / stream
//   filter  : the codec filter (MP3 / AAC / AAC+ in; OGG, FLAC, video, unknown, non-HTTP and HLS out), the codec
//             badge, the stream URL and media type
//   mapping : station -> catalog::Track (id, title, artist line, favicon, no duration), station ids, genre labels
//   store   : favorites / recent / queue round trip, order and bounds (the queue window of a long list), registry
//             lookups (also into favorites / recent once the registry dropped a station), refresh from the server,
//             listeners, a damaged file kept as radio.json.bad
//   live    : (`radio_test live`) server discovery, topclick, topvote, a tag, a country, a name search, paging, tags,
//             countries, byuuid, the cache and failover to another server. Never counts a click.
//
// Temporary files go to a folder next to the exe (removed at the end); SHADETUBE_DATA_DIR points there too.
#include "app/InternetRadio.h"
#include "core/I18n.h"
#include "core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

static fs::path g_dir;

static std::string readFixture(const char* name) {
    std::ifstream f(fs::path(ST_RADIO_FIXTURES) / name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static radio::Station station(std::string uuid, std::string name, std::string codec = "MP3", int bitrate = 128) {
    radio::Station s;
    s.uuid = std::move(uuid);
    s.name = std::move(name);
    s.urlResolved = "https://stream.example/" + s.uuid;
    s.codec = std::move(codec);
    s.bitrate = bitrate;
    s.countryCode = "TR";
    s.country = "Turkey";
    s.tags = {"pop", "news"};
    s.favicon = "https://img.example/" + s.uuid + ".png";
    return s;
}

template <class F>
static bool throwsRadioError(F&& f) {
    try {
        f();
    } catch (const radio::RadioError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------

static void testParsing() {
    std::printf("parsing\n");
    const auto top = radio::parseStations(readFixture("topclick.json"));
    CHECK(top.size() == 12);
    if (!top.empty()) {
        const auto& s = top.front();
        CHECK(s.uuid == "a3c93ca9-9891-4249-a805-d622dadeed21");
        CHECK(s.name == "Cool FM: Lagos");
        CHECK(s.codec == "MP3" && s.bitrate == 128 && !s.hls);
        CHECK(s.countryCode == "NG" && s.country == "Nigeria");
        CHECK(s.urlResolved.rfind("https://", 0) == 0);
        CHECK(s.favicon == "https://cdn.onlineradiobox.com/img/l/3/6733.v14.png");
        CHECK(s.votes == 30 && s.clickCount == 4273);
    }
    for (const auto& s : top) CHECK(!s.uuid.empty() && !s.name.empty() && !radio::streamUrl(s).empty());

    const auto jazz = radio::parseStations(readFixture("search_tag_jazz.json"));
    CHECK(!jazz.empty());
    for (const auto& s : jazz) CHECK(std::find(s.tags.begin(), s.tags.end(), "jazz") != s.tags.end());
    const auto turkey = radio::parseStations(readFixture("search_country_tr.json"));
    CHECK(!turkey.empty());
    for (const auto& s : turkey) CHECK(s.countryCode == "TR");
    for (const auto& s : turkey)
        std::printf("    TR  %-32s %-12s %s\n", s.name.c_str(), radio::codecBadge(s).c_str(), toUtf8(radio::subtitle(s)).c_str());

    const auto tags = radio::parseTags(readFixture("tags.json"));
    CHECK(tags.size() == 40);
    if (!tags.empty()) CHECK(tags.front().name == "pop" && tags.front().stationCount > 1000);
    for (size_t i = 1; i < tags.size(); ++i) CHECK(tags[i - 1].stationCount >= tags[i].stationCount);

    const auto countries = radio::parseCountries(readFixture("countries.json"));
    CHECK(countries.size() > 100);
    auto hasCountry = [&](const char* code) {
        return std::any_of(countries.begin(), countries.end(), [&](const radio::Country& c) { return c.code == code && c.stationCount > 0; });
    };
    CHECK(hasCountry("TR") && hasCountry("DE") && hasCountry("US"));
    for (const auto& c : countries) CHECK(c.code.size() == 2);

    const auto servers = radio::parseServers(readFixture("servers.json"));   // an IPv4 + IPv6 entry of one host
    CHECK(servers.size() == 1 && servers.front() == "de1.api.radio-browser.info");
    const auto hostile = radio::parseServers(
        R"([{"name":"evil.example.com"},{"name":"x.api.radio-browser.info"},{"name":"api.radio-browser.info.evil.com"}])");
    CHECK(hostile.size() == 1 && hostile.front() == "x.api.radio-browser.info");

    // Malformed input.
    CHECK(throwsRadioError([] { radio::parseStations("{not json"); }));
    CHECK(throwsRadioError([] { radio::parseStations(R"({"stationuuid":"x"})"); }));
    CHECK(throwsRadioError([] { radio::parseTags("42"); }));
    const auto mixed = radio::parseStations(
        R"J([1, "x", null, {"name":"no uuid","url":"http://a"}, {"stationuuid":"u1","name":"  No   stream "},
            {"stationuuid":"u2","name":"  Good \t Station ","url":"http://s.example/a","codec":"mp3","bitrate":"96",
             "tags":"Pop, ,POP,news","favicon":"javascript:alert(1)","hls":0,"serveruuid":null,"geo_lat":null},
            {"stationuuid":"u2","name":"Duplicate","url":"http://s.example/b"}])J");
    CHECK(mixed.size() == 1);
    if (!mixed.empty()) {
        const auto& s = mixed.front();
        CHECK(s.name == "Good Station");
        CHECK(s.codec == "MP3" && s.bitrate == 96);
        CHECK(s.tags == std::vector<std::string>({"pop", "news"}));
        CHECK(s.favicon.empty());   // only http(s) images reach the image cache
        CHECK(radio::streamUrl(s) == "http://s.example/a");
    }
}

static void testFilter() {
    std::printf("filter\n");
    auto with = [](std::string codec, bool hls = false, std::string url = "https://s.example/x") {
        radio::Station s = station("f", "F", std::move(codec));
        s.urlResolved = std::move(url);
        s.hls = hls;
        return s;
    };
    CHECK(radio::playable(with("MP3")));
    CHECK(radio::playable(with("AAC")));
    CHECK(radio::playable(with("AAC+")));
    CHECK(!radio::playable(with("OGG")));
    CHECK(!radio::playable(with("FLAC")));
    CHECK(!radio::playable(with("AAC,H.264")));
    CHECK(!radio::playable(with("UNKNOWN")));
    CHECK(!radio::playable(with("")));
    CHECK(radio::playable(with("AAC", true)) == radio::kPlayHls);
    CHECK(!radio::playable(with("MP3", false, "rtsp://s.example/x")));
    CHECK(!radio::playable(with("MP3", false, "")));
    radio::Station fallback = with("MP3", false, "");
    fallback.url = "http://s.example/raw";
    CHECK(radio::playable(fallback) && radio::streamUrl(fallback) == "http://s.example/raw");

    const auto kept = radio::filterPlayable({with("MP3"), with("OGG"), with("AAC+"), with("AAC", true)});
    CHECK(kept.size() == (radio::kPlayHls ? 3u : 2u));
    // Every station of a saved response passes or fails for its codec only.
    for (const auto& s : radio::parseStations(readFixture("topclick.json")))
        CHECK(radio::playable(s) == ((s.codec == "MP3" || s.codec == "AAC" || s.codec == "AAC+") && (!s.hls || radio::kPlayHls)));

    CHECK(radio::codecBadge(with("MP3")) == "MP3 · 128");
    radio::Station he = with("AAC+");
    he.bitrate = 0;
    CHECK(radio::codecBadge(he) == "AAC+");
    radio::Station hls = with("UNKNOWN", true);
    hls.bitrate = 0;
    CHECK(radio::codecBadge(hls) == "HLS");
    CHECK(radio::mimeType(with("MP3")) == "audio/mpeg");
    CHECK(radio::mimeType(with("AAC+")) == "audio/aac");
    CHECK(radio::mimeType(with("AAC", true)) == "application/vnd.apple.mpegurl");
    CHECK(radio::mimeType(with("OGG")).empty());

    radio::Query q;
    q.tag = "hip hop";
    q.countryCode = "TR";
    q.order = radio::Query::Order::Votes;
    q.limit = 20;
    q.offset = 40;
    const std::string path = radio::searchPath(q);
    CHECK(path.find("tag=hip%20hop&tagExact=true") != std::string::npos);
    CHECK(path.find("countrycode=TR") != std::string::npos);
    CHECK(path.find("order=votes&reverse=true&hidebroken=true&limit=20&offset=40") != std::string::npos);
}

static void testMapping() {
    std::printf("mapping\n");
    const radio::Station s = station("0d6f7c1a-0000-4000-8000-000000000001", "Radyo Test");
    const catalog::Track t = radio::toTrack(s);
    CHECK(t.id == "radio:0d6f7c1a-0000-4000-8000-000000000001");
    CHECK(t.name == "Radyo Test");
    CHECK(t.durationMs == 0);
    CHECK(t.album.images.size() == 1 && t.album.images[0].url == s.favicon);
    CHECK(t.album.id.empty());   // never shows up as a "recent album"
    const std::wstring line = toWide(t.artistLine());
    std::printf("    artist line: %s\n", t.artistLine().c_str());
    CHECK(line.find(radio::countryName("TR", "Turkey")) != std::wstring::npos);
    CHECK(line.find(tr(L"Haber")) != std::wstring::npos && line.find(tr(L"Pop")) != std::wstring::npos);
    CHECK(radio::isStationId(t.id));
    CHECK(radio::uuidOf(t.id) == s.uuid);
    CHECK(!radio::isStationId("radio:"));
    CHECK(!radio::isStationId("spotify:track:1"));
    CHECK(radio::uuidOf("local:abc").empty());
    CHECK(!radio::countryName("TR", "Turkey").empty() && radio::countryName("TR", "Turkey") != L"TR");
    CHECK(radio::countryName("", "Atlantis") == L"Atlantis");
    radio::Station bare = station("b", "Bare");
    bare.countryCode.clear();
    bare.country.clear();
    bare.tags.clear();
    CHECK(radio::subtitle(bare) == tr(L"İnternet radyosu"));
    bare.tags = {"moi merino", "80s"};   // a known genre comes first
    CHECK(radio::subtitle(bare) == std::wstring(tr(L"80'ler")) + L", Moi merino");
    CHECK(radio::genreLabel("news") == tr(L"Haber"));
    CHECK(radio::genreLabel("méxico") == L"México");
    CHECK(radio::genreLabel("information") == L"Information");   // no Turkish dotted İ for tags
    CHECK(radio::isGenre("jazz") && !radio::isGenre("moi merino"));
    CHECK(radio::featuredGenres().size() >= 12);
    for (const auto& g : radio::featuredGenres()) CHECK(radio::genreLabel(g) != toWide(g));   // all translated
    CHECK(radio::toTracks({s, bare}).size() == 2);
}

static void testStore() {
    std::printf("store\n");
    const fs::path file = g_dir / L"radio.json";
    std::error_code ec;
    fs::remove(file, ec);

    radio::Store st;
    st.load(file);
    CHECK(st.favorites().empty() && st.recent().empty() && st.queue().empty() && st.country().empty());

    int notified = 0;
    auto owner = std::make_shared<char>();
    st.subscribe(owner, [&] { ++notified; });

    const auto a = station("a", "Alpha"), b = station("b", "Bravo", "AAC+", 64), c = station("c", "Charlie");
    st.setFavorite(a, true);
    st.setFavorite(b, true);
    st.setFavorite(b, true);   // no-op
    CHECK(st.favorites().size() == 2 && st.favorites()[0].uuid == "b" && st.favorites()[1].uuid == "a");
    CHECK(st.isFavorite("a") && !st.isFavorite("c"));
    st.toggleFavorite(a);
    CHECK(!st.isFavorite("a") && st.favorites().size() == 1);
    st.toggleFavorite(a);
    CHECK(st.favorites().front().uuid == "a");
    CHECK(notified == 4);

    st.recordPlayed(a);
    st.recordPlayed(b);
    st.recordPlayed(a);
    st.recordPlayed(a);   // already the newest: no change
    CHECK(st.recent().size() == 2 && st.recent()[0].uuid == "a" && st.recent()[1].uuid == "b");
    for (int i = 0; i < 40; ++i) st.recordPlayed(station("r" + std::to_string(i), "R"));
    CHECK(st.recent().size() == radio::Store::kMaxRecent && st.recent().front().uuid == "r39");
    st.removeRecent("r39");
    CHECK(st.recent().front().uuid == "r38");

    // A long list keeps the window the player's session keeps (100 back, kMaxQueue in all).
    std::vector<radio::Station> longList;
    for (int i = 0; i < 700; ++i) longList.push_back(station("L" + std::to_string(i), "L"));
    st.rememberQueue(longList, 650);
    CHECK(st.queue().size() == radio::Store::kMaxQueue && st.queue().front().uuid == "L200" && st.queue().back().uuid == "L699");
    st.rememberQueue(longList, 30);
    CHECK(st.queue().size() == radio::Store::kMaxQueue && st.queue().front().uuid == "L0");
    st.rememberQueue(longList, 400);
    CHECK(st.queue().front().uuid == "L200" && st.queue().back().uuid == "L699");

    std::vector<radio::Station> list;
    for (int i = 0; i < 250; ++i) list.push_back(station("q" + std::to_string(i), "Q"));
    const int before = notified;
    st.rememberQueue(list, 210);
    CHECK(st.queue().size() == 250);   // fits whole: station #210 and the ones after it resolve after a restart
    CHECK(notified == before);   // the queue is not shown: no notification
    CHECK(st.find("q249") && st.find("q249")->name == "Q");   // the registry knows the whole list
    CHECK(!st.find("zzz"));
    st.setCountry("de");
    CHECK(st.country() == "DE");
    st.remember(c);
    CHECK(st.find("c") && st.find("c")->name == "Charlie");

    CHECK(st.dirty());
    CHECK(st.save());
    CHECK(!st.dirty());
    CHECK(fs::exists(file) && !fs::exists(fs::path(file).concat(L".tmp")));

    radio::Store back;
    back.load(file);
    CHECK(back.country() == "DE");
    CHECK(back.favorites().size() == st.favorites().size());
    for (size_t i = 0; i < std::min(back.favorites().size(), st.favorites().size()); ++i)
        CHECK(radio::toJson(back.favorites()[i]) == radio::toJson(st.favorites()[i]));
    CHECK(back.recent().size() == st.recent().size() && back.recent().front().uuid == "r38");
    CHECK(back.queue().size() == 250 && back.find("q249"));
    CHECK(back.find("q10") && radio::streamUrl(*back.find("q10")) == "https://stream.example/q10");   // a restored queue plays
    CHECK(back.find("b") && back.find("b")->codec == "AAC+" && back.find("b")->bitrate == 64);
    CHECK(!back.find("c"));   // remembered only in memory

    // Fresher data from the server replaces the stored copies.
    radio::Station renamed = b;
    renamed.name = "Bravo FM";
    const uint64_t rev = back.revision();
    back.refresh({renamed});
    CHECK(back.find("b") && back.find("b")->name == "Bravo FM" && back.revision() > rev);
    CHECK(std::any_of(back.favorites().begin(), back.favorites().end(), [](const radio::Station& s) { return s.name == "Bravo FM"; }));
    const uint64_t rev2 = back.revision();
    back.refresh({renamed});   // same data: no change
    CHECK(back.revision() == rev2);

    // Listeners die with their owner.
    owner.reset();
    const int n = notified;
    st.setFavorite(c, true);
    CHECK(notified == n);

    // find() of a station the bounded registry dropped points into favorites / recent: the mutators change that very
    // list and must not read the element they removed.
    {
        radio::Store alias;
        alias.load(g_dir / L"alias.json");
        alias.setFavorite(station("fx", "Fox"), true);
        alias.setFavorite(station("fy", "Yak"), true);
        alias.recordPlayed(station("r1", "Old"));
        alias.recordPlayed(station("r2", "New"));
        std::vector<radio::Station> many;
        for (size_t i = 0; i < radio::Store::kMaxKnown; ++i) many.push_back(station("k" + std::to_string(i), "K"));
        alias.remember(many);
        const radio::Station* fav = alias.find("fx");
        CHECK(fav && fav == &alias.favorites().back());   // served from favorites itself
        if (fav) alias.toggleFavorite(*fav);
        CHECK(!alias.isFavorite("fx") && alias.favorites().size() == 1 && alias.find("fx") && alias.find("fx")->name == "Fox");
        const radio::Station* old = alias.find("r1");
        CHECK(old && old == &alias.recent().back());
        if (old) alias.recordPlayed(*old);
        CHECK(alias.recent().size() == 2 && alias.recent()[0].uuid == "r1" && alias.recent()[0].name == "Old" &&
              alias.recent()[1].uuid == "r2");
    }

    // A damaged file: kept as .bad, the app starts empty, the next save writes a good file.
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "{ \"favorites\": [ {\"uuid\": \"x\", ";
    }
    radio::Store broken;
    broken.load(file);
    CHECK(broken.favorites().empty() && broken.recent().empty());
    CHECK(fs::exists(fs::path(file).concat(L".bad")));
    broken.setFavorite(a, true);
    CHECK(broken.save());
    radio::Store healed;
    healed.load(file);
    CHECK(healed.favorites().size() == 1 && healed.favorites()[0].uuid == "a");

    // One bad entry never loses the others.
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << R"({"version":1,"favorites":[{"uuid":"ok","name":"Fine","urlResolved":"http://x"},42,{"name":"no uuid"},)"
             R"({"uuid":"ok","name":"dup"}],"recent":"oops","country":"TRX"})";
    }
    radio::Store partial;
    partial.load(file);
    CHECK(partial.favorites().size() == 1 && partial.favorites()[0].name == "Fine");
    CHECK(partial.recent().empty() && partial.country().empty());
}

// ---------------------------------------------------------------------------------------------------------------------

static double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void testLive() {
    std::printf("live (radio-browser.info)\n");
    const std::string ua = "ShadeTube/radio_test (+https://github.com/shadesofdeath/ShadeTube)";
    radio::Client client;
    client.setUserAgent(ua);
    auto t0 = std::chrono::steady_clock::now();
    try {
        const auto top = client.topClick(20);
        std::printf("  topclick    %6.0f ms  %zu playable of 20 via %s\n", msSince(t0), top.stations.size(),
                    client.currentServer().c_str());
        CHECK(!top.stations.empty() && top.more && top.nextOffset == 20);
        CHECK(client.currentServer().ends_with(".api.radio-browser.info"));
        for (const auto& s : top.stations) CHECK(radio::playable(s));
        for (size_t i = 0; i < std::min<size_t>(top.stations.size(), 5); ++i)
            std::printf("    %-36s %s\n", top.stations[i].name.c_str(), radio::codecBadge(top.stations[i]).c_str());

        t0 = std::chrono::steady_clock::now();
        const auto again = client.topClick(20);
        const double cached = msSince(t0);
        std::printf("  cached      %6.1f ms\n", cached);
        CHECK(cached < 50 && again.stations.size() == top.stations.size() && client.cacheSize() >= 1);

        t0 = std::chrono::steady_clock::now();
        const auto votes = client.topVote(10);
        const auto votes2 = client.topVote(10, 10);
        std::printf("  topvote     %6.0f ms  %zu + %zu\n", msSince(t0), votes.stations.size(), votes2.stations.size());
        CHECK(!votes.stations.empty() && !votes2.stations.empty());
        for (const auto& s : votes2.stations)
            CHECK(std::none_of(votes.stations.begin(), votes.stations.end(), [&](const radio::Station& o) { return o.uuid == s.uuid; }));

        radio::Query q;
        q.tag = "jazz";
        q.limit = 20;
        t0 = std::chrono::steady_clock::now();
        const auto jazz = client.search(q);
        std::printf("  tag jazz    %6.0f ms  %zu\n", msSince(t0), jazz.stations.size());
        CHECK(!jazz.stations.empty());
        for (const auto& s : jazz.stations) CHECK(std::find(s.tags.begin(), s.tags.end(), "jazz") != s.tags.end());

        q = {};
        q.countryCode = "TR";
        q.limit = 20;
        t0 = std::chrono::steady_clock::now();
        const auto turkey = client.search(q);
        std::printf("  country TR  %6.0f ms  %zu\n", msSince(t0), turkey.stations.size());
        CHECK(!turkey.stations.empty());
        for (const auto& s : turkey.stations) CHECK(s.countryCode == "TR");
        for (size_t i = 0; i < std::min<size_t>(turkey.stations.size(), 5); ++i)
            std::printf("    %-36s %s\n", turkey.stations[i].name.c_str(), toUtf8(radio::subtitle(turkey.stations[i])).c_str());

        q = {};
        q.name = "jazz";
        q.order = radio::Query::Order::Votes;
        q.limit = 10;
        CHECK(!client.search(q).stations.empty());

        t0 = std::chrono::steady_clock::now();
        const auto tags = client.tags(30);
        const auto countries = client.countries();
        std::printf("  tags        %6.0f ms  %zu tags, %zu countries\n", msSince(t0), tags.size(), countries.size());
        CHECK(tags.size() >= 20);
        CHECK(std::any_of(countries.begin(), countries.end(), [](const radio::Country& c) { return c.code == "TR"; }));

        std::vector<std::string> uuids;
        for (size_t i = 0; i < std::min<size_t>(top.stations.size(), 3); ++i) uuids.push_back(top.stations[i].uuid);
        const auto fresh = client.byUuids(uuids);
        CHECK(fresh.size() == uuids.size());
        for (const auto& s : fresh) CHECK(std::find(uuids.begin(), uuids.end(), s.uuid) != uuids.end());

        // Failover: an unreachable server first, then the one that worked.
        const std::string good = client.currentServer();
        radio::Client failover;
        failover.setUserAgent(ua);
        failover.setServers({"nonexistent-shadetube-test.api.radio-browser.info", good});
        t0 = std::chrono::steady_clock::now();
        const auto viaSecond = failover.topVote(5);
        std::printf("  failover    %6.0f ms  -> %s\n", msSince(t0), failover.currentServer().c_str());
        CHECK(!viaSecond.stations.empty() && failover.currentServer() == good);

        // Cancellation stops a request before it starts.
        YoutubeExplode::CancellationTokenSource cts;
        cts.cancel();
        bool cancelled = false;
        try {
            client.topClick(7, 0, cts.token());
        } catch (const radio::RadioError&) {
        } catch (const std::exception&) {
            cancelled = true;   // OperationCanceledException
        }
        CHECK(cancelled);
    } catch (const std::exception& e) {
        std::printf("  FAIL live: %s\n", e.what());
        ++g_failures;
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool live = argc > 1 && std::wstring(argv[1]) == L"live";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = fs::path(exe).parent_path() / L"radio_test_tmp";   // inside the build folder, never the user's profile
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()
    i18n::init("tr");

    testParsing();
    testFilter();
    testMapping();
    testStore();
    if (live) testLive();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall radio checks passed%s\n", live ? " (with live)" : "");
    return 0;
}
