// mb_test: offline parsing tests against captured fixtures + a live run against MusicBrainz / ListenBrainz /
// Wikidata (cold vs warm cache timings).
//   mb_test              offline + live (clears %LOCALAPPDATA%\ShadeTube\cache\mb first to measure a cold run)
//   mb_test --offline    fixtures only
//   mb_test --keep-cache live run without clearing the disk cache
#include "core/Paths.h"
#include "musicbrainz/MusicBrainz.h"
#include "musicbrainz/Net.h"
#include "musicbrainz/Parse.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::catalog;
namespace d = st::mb::detail;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static d::json fixture(const char* name) {
    std::ifstream in(std::filesystem::path(MB_FIXTURE_DIR) / name, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = d::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) {
        std::printf("  FAIL cannot load fixture %s\n", name);
        ++g_failures;
        return {};
    }
    return j;
}

static std::string fmtDur(int ms) {
    const int s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

static double timeIt(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static const char* firstUrl(const std::vector<Image>& images, int w) {
    const Image* img = pickImage(images, w);
    return img ? img->url.c_str() : "(none)";
}

// ------------------------------------------------------------------------------------------------------------
static void offlineTests() {
    std::printf("== offline (fixtures)\n");

    // Models
    {
        auto imgs = coverArt("abc");
        CHECK(imgs.size() == 3);
        CHECK(pickImage(imgs, 300)->width == 500);
        CHECK(pickImage(imgs, 5000)->width == 1200);
        CHECK(pickImage({}, 100) == nullptr);
        CHECK(coverArt("").empty());
        Track t;
        t.artists = {{"1", "A"}, {"2", "B"}};
        CHECK(t.artistLine() == "A, B");
    }
    // Queries
    {
        CHECK(d::hasFieldPrefix("tag:\"anadolu rock\""));
        CHECK(d::hasFieldPrefix("tag:rock"));
        CHECK(d::hasFieldPrefix("foo AND country:TR"));
        CHECK(!d::hasFieldPrefix("Tarkan"));
        CHECK(!d::hasFieldPrefix("Re:Cognition"));   // not a known field
        CHECK(d::releaseGroupQuery("tag:\"anadolu rock\"") == "tag:\"anadolu rock\"");
        CHECK(d::luceneEscape("AC/DC (live)") == "AC\\/DC \\(live\\)");
        CHECK(d::recordingQuery("Tarkan Şımarık") == "(recording:(tarkan Şımarık) OR artist:(tarkan Şımarık)) AND video:false");
        CHECK(d::artistQuery("  ").empty());
    }
    // Artist search + lookup + wikidata
    {
        auto artists = d::parseArtistSearch(fixture("artist_search.json"), 10);
        CHECK(!artists.empty());
        CHECK(artists[0].name == "Tarkan");
        CHECK(artists[0].country == "TR");
        CHECK(artists[0].beginYear == "1972");
        CHECK(!artists[0].tags.empty() && artists[0].tags[0] == "pop");

        auto lookup = fixture("artist_lookup.json");
        auto a = d::parseArtist(lookup);
        CHECK(a.id == "4ec2451d-ed0c-4273-b683-4c1312df25fd");
        CHECK(a.type == "Person");
        CHECK(d::wikidataId(lookup) == "Q485771");
        auto file = d::wikidataImageFile(fixture("wikidata_entity.json"));
        CHECK(file == "Tarkan (9).jpg");
        auto photos = d::commonsImages(file);
        CHECK(photos.size() == 3);
        CHECK(photos[1].url == "https://commons.wikimedia.org/wiki/Special:FilePath/Tarkan_%289%29.jpg?width=500");
    }
    // Release groups
    {
        auto albums = d::parseReleaseGroupSearch(fixture("release_group_search.json"), true, 10);
        CHECK(!albums.empty());
        for (auto& al : albums) CHECK(al.primaryType == "Album" || al.primaryType == "EP" || al.primaryType == "Single");
        CHECK(!albums.empty() && albums[0].images.size() == 3);

        auto browse = d::parseReleaseGroupBrowse(fixture("release_group_browse.json"));
        CHECK(browse.size() == 10);
        d::sortNewestFirst(browse);
        for (size_t i = 1; i < browse.size(); ++i) CHECK(browse[i - 1].firstReleaseDate >= browse[i].firstReleaseDate);
        int other = 0;
        for (auto& al : browse) other += d::isOtherRelease(al) ? 1 : 0;
        CHECK(other >= 1);   // "Metamorfoz Remixes" (Remix)
    }
    // Album: representative release + tracklist
    {
        auto rg = fixture("release_group_lookup.json");
        auto al = d::parseReleaseGroup(rg);
        CHECK(al.name == "Karma");
        CHECK(d::pickRepresentativeRelease(rg) == "39a72717-1477-43ab-90e7-83d429bf85e1");   // TR CD 2001
        d::applyRelease(al, fixture("release_lookup.json"));
        CHECK(al.tracks.size() == 3);
        CHECK(al.releaseId == "39a72717-1477-43ab-90e7-83d429bf85e1");
        CHECK(!al.label.empty());
        CHECK(!al.tracks.empty() && al.tracks[0].id == "9c7b0a23-236a-4fb3-880e-62896df12db4");   // recording id
        CHECK(!al.tracks.empty() && al.tracks[0].durationMs == 262160 && al.tracks[0].trackNumber == 1);
        CHECK(!al.tracks.empty() && al.tracks[0].album.id == al.id && al.tracks[0].artistLine() == "Tarkan");
    }
    // Recordings
    {
        auto tracks = d::rankRecordingSearch(d::parseRecordingHits(fixture("recording_search.json")), "Tarkan", true, 10);
        CHECK(!tracks.empty());
        for (auto& t : tracks) CHECK(!t.id.empty() && !t.name.empty());

        auto hits = d::parseRecordingHits(fixture("artist_recordings.json"));
        CHECK(hits.size() == 12);
        auto pop = d::parsePopularity(fixture("lb_popularity_recording.json"));
        CHECK(pop.size() == 2);
        CHECK(pop["9c7b0a23-236a-4fb3-880e-62896df12db4"] > 0);
        auto top = d::rankTopTracks(hits, {{hits.back().track.id, 999999}}, 5);
        CHECK(top.size() == 5);
        CHECK(!top.empty() && top[0].listenCount >= 999999);
        CHECK(!top.empty() && !top[0].album.id.empty());
    }
    // ListenBrainz
    {
        auto artists = d::parseLbArtists(fixture("lb_sitewide_artists.json"), 10);
        CHECK(artists.size() == 3 && artists[0].listenCount > 0);
        auto rgs = d::parseLbReleaseGroups(fixture("lb_sitewide_release_groups.json"), 10);
        CHECK(!rgs.empty() && !rgs[0].images.empty());
        auto recs = d::parseLbRecordings(fixture("lb_sitewide_recordings.json"), 10);
        CHECK(recs.size() == 2 && /* same recording listed twice */ recs[0].listenCount > 0 && !recs[0].album.images.empty());
        auto fresh = d::parseFreshReleases(fixture("lb_fresh_releases.json"), 10);
        CHECK(!fresh.empty() && !fresh[0].id.empty());
        auto similar = d::parseSimilarArtists(fixture("lb_similar_artists.json"), "4ec2451d-ed0c-4273-b683-4c1312df25fd", 10);
        CHECK(!similar.empty() && similar[0].name == "Mustafa Sandal");
    }
    // Robustness: garbage never crashes
    {
        const d::json junk[] = {d::json(), d::json::array(), d::json(42), d::json::parse(R"({"artists":[null,1,{"id":5}],"releases":"x","media":[{"tracks":[{}]}]})")};
        for (const auto& j : junk) {
            (void)d::parseArtistSearch(j, 5);
            (void)d::parseReleaseGroupSearch(j, true, 5);
            (void)d::parseRecordingHits(j);
            (void)d::pickRepresentativeRelease(j);
            Album a;
            d::applyRelease(a, j);
            (void)d::parseLbRecordings(j, 5);
            (void)d::parseFreshReleases(j, 5);
            (void)d::parseSimilarArtists(j, "", 5);
            (void)d::wikidataImageFile(j);
        }
    }
    std::printf("   offline done, failures so far: %d\n", g_failures);
}

// ------------------------------------------------------------------------------------------------------------
struct LiveResult {
    mb::SearchResults search;
    ArtistPage page;
    Album album;
    std::vector<Artist> trending;
    std::vector<Album> trendingAlbums;
    std::vector<Track> trendingTracks;
    std::vector<Album> fresh;
};

static LiveResult liveScenario(bool print) {
    LiveResult r;
    double t;
    t = timeIt([&] { r.search = mb::search("Tarkan"); });
    if (print) std::printf("   search(\"Tarkan\"): %.2fs  artists=%zu albums=%zu tracks=%zu\n", t, r.search.artists.size(), r.search.albums.size(), r.search.tracks.size());
    std::string artistId;
    for (auto& a : r.search.artists)
        if (a.name == "Tarkan") { artistId = a.id; break; }
    CHECK(!artistId.empty());
    if (artistId.empty()) return r;

    t = timeIt([&] { r.page = mb::artistPage(artistId); });
    if (print) std::printf("   artistPage: %.2fs\n", t);
    if (!r.page.albums.empty()) {
        t = timeIt([&] { r.album = mb::album(r.page.albums.front().id); });
        if (print) std::printf("   album(%s): %.2fs\n", r.page.albums.front().name.c_str(), t);
    }
    t = timeIt([&] { r.trending = mb::trendingArtists("week"); });
    if (print) std::printf("   trendingArtists(week): %.2fs\n", t);
    t = timeIt([&] { r.trendingAlbums = mb::trendingAlbums("week", 10); r.trendingTracks = mb::trendingTracks("week", 10); });
    if (print) std::printf("   trendingAlbums+Tracks(week): %.2fs\n", t);
    t = timeIt([&] { r.fresh = mb::freshReleases(14); });
    if (print) std::printf("   freshReleases(14): %.2fs\n", t);
    return r;
}

static void printDetails(const LiveResult& r) {
    std::printf("\n-- search results\n");
    for (auto& a : r.search.artists) std::printf("   artist  %-28s %s %s  [%s]\n", a.name.c_str(), a.country.c_str(), a.disambiguation.c_str(), a.id.c_str());
    for (auto& a : r.search.albums) std::printf("   album   %-36s %-6s %s  (%s)\n", a.name.c_str(), a.primaryType.c_str(), a.year().c_str(), a.artists.empty() ? "" : a.artists[0].name.c_str());
    for (auto& t : r.search.tracks) std::printf("   track   %-36s %-20s %s  album=%s\n", t.name.c_str(), t.artistLine().c_str(), fmtDur(t.durationMs).c_str(), t.album.name.c_str());

    const auto& p = r.page;
    std::printf("\n-- artist: %s (%s, %s, since %s) tags=", p.artist.name.c_str(), p.artist.type.c_str(), p.artist.country.c_str(), p.artist.beginYear.c_str());
    for (auto& tg : p.artist.tags) std::printf("%s ", tg.c_str());
    std::printf("\n   photo: %s\n", firstUrl(p.artist.images, 500));
    std::printf("   albums=%zu singles/EPs=%zu other=%zu\n", p.albums.size(), p.singles.size(), p.other.size());
    for (auto& a : p.albums) std::printf("     %s  %s\n", a.firstReleaseDate.c_str(), a.name.c_str());
    std::printf("   top tracks:\n");
    for (auto& t : p.topTracks) std::printf("     %8lld listens  %-34s %s  [%s] cover=%s\n", static_cast<long long>(t.listenCount), t.name.c_str(), fmtDur(t.durationMs).c_str(), t.album.name.c_str(), firstUrl(t.album.images, 250));
    std::printf("   similar: ");
    for (auto& a : p.similar) std::printf("%s%s, ", a.name.c_str(), a.images.empty() ? "" : "*");
    std::printf("\n");

    const auto& al = r.album;
    std::printf("\n-- album: %s (%s, %s) label=%s release=%s tracks=%d\n   cover: %s\n", al.name.c_str(), al.primaryType.c_str(), al.firstReleaseDate.c_str(), al.label.c_str(), al.releaseId.c_str(), al.totalTracks, firstUrl(al.images, 500));
    for (auto& t : al.tracks) std::printf("   %2d. %-40s %s  %s\n", t.trackNumber, t.name.c_str(), fmtDur(t.durationMs).c_str(), t.artistLine().c_str());

    std::printf("\n-- trending artists (week):\n");
    for (size_t i = 0; i < r.trending.size() && i < 10; ++i) std::printf("   %8lld  %s\n", static_cast<long long>(r.trending[i].listenCount), r.trending[i].name.c_str());
    std::printf("-- trending albums (week):\n");
    for (size_t i = 0; i < r.trendingAlbums.size() && i < 5; ++i) std::printf("   %s - %s  %s\n", r.trendingAlbums[i].artists.empty() ? "" : r.trendingAlbums[i].artists[0].name.c_str(), r.trendingAlbums[i].name.c_str(), firstUrl(r.trendingAlbums[i].images, 250));
    std::printf("-- trending tracks (week):\n");
    for (size_t i = 0; i < r.trendingTracks.size() && i < 5; ++i) std::printf("   %8lld  %s - %s  %s\n", static_cast<long long>(r.trendingTracks[i].listenCount), r.trendingTracks[i].artistLine().c_str(), r.trendingTracks[i].name.c_str(), firstUrl(r.trendingTracks[i].album.images, 250));
    std::printf("-- fresh releases (14 days): %zu\n", r.fresh.size());
    for (size_t i = 0; i < r.fresh.size() && i < 8; ++i) std::printf("   %s  %-6s %s - %s\n", r.fresh[i].firstReleaseDate.c_str(), r.fresh[i].primaryType.c_str(), r.fresh[i].artists.empty() ? "" : r.fresh[i].artists[0].name.c_str(), r.fresh[i].name.c_str());
}

static void checkLive(const LiveResult& r) {
    CHECK(r.page.artist.name == "Tarkan");
    CHECK(!r.page.artist.images.empty());
    CHECK(r.page.albums.size() >= 5);
    CHECK(!r.page.singles.empty());
    CHECK(r.page.topTracks.size() >= 5);
    CHECK(!r.page.topTracks.empty() && r.page.topTracks[0].listenCount > 0);
    CHECK(!r.page.similar.empty());
    CHECK(!r.album.tracks.empty());
    for (auto& t : r.album.tracks) CHECK(t.durationMs > 0);
    CHECK(!r.search.albums.empty() && !r.search.tracks.empty());
    CHECK(!r.trending.empty() && !r.trendingAlbums.empty() && !r.trendingTracks.empty());
    CHECK(!r.fresh.empty());
}

static void liveTests(bool clearCache) {
    std::printf("\n== live\n");
    if (clearCache) {
        std::error_code ec;
        std::filesystem::remove_all(paths::cacheDir() / L"mb", ec);
        std::printf("   cleared %s\n", (paths::cacheDir() / L"mb").string().c_str());
    }
    const auto s0 = mb::net::stats();
    LiveResult cold;
    const double tCold = timeIt([&] { cold = liveScenario(true); });
    const auto s1 = mb::net::stats();
    printDetails(cold);
    checkLive(cold);

    mb::net::clearMemoryCache();
    const double tDisk = timeIt([&] { (void)liveScenario(false); });
    const auto s2 = mb::net::stats();
    const double tMem = timeIt([&] { (void)liveScenario(false); });
    const auto s3 = mb::net::stats();
    std::printf("\n-- timings: cold %.2fs (%d requests) | warm disk %.3fs (%d requests, %d disk hits) | warm memory %.3fs (%d mem hits)\n",
                tCold, s1.networkRequests - s0.networkRequests, tDisk, s2.networkRequests - s1.networkRequests,
                s2.diskHits - s1.diskHits, tMem, s3.memoryHits - s2.memoryHits);
    CHECK(s2.networkRequests == s1.networkRequests);
    CHECK(tMem < 1.0);

    // Genre tile passthrough
    auto genre = mb::searchAlbums("tag:\"anadolu rock\"", 5);
    std::printf("\n-- searchAlbums(tag:\"anadolu rock\"): ");
    for (auto& a : genre) std::printf("%s - %s; ", a.artists.empty() ? "" : a.artists[0].name.c_str(), a.name.c_str());
    std::printf("\n");
    CHECK(!genre.empty());

    // Rate limiter: 3 uncached MB requests from 3 threads must span >= 2 s.
    const auto stamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const double tRl = timeIt([&] {
        std::vector<std::thread> ts;
        for (int i = 0; i < 3; ++i)
            ts.emplace_back([&, i] {
                try { (void)mb::searchArtists("zzq" + stamp + "x" + std::to_string(i), 1); } catch (const std::exception& e) { std::printf("   rl err %s\n", e.what()); }
            });
        for (auto& th : ts) th.join();
    });
    std::printf("-- rate limiter: 3 parallel uncached requests took %.2fs\n", tRl);
    CHECK(tRl >= 1.9);

    // Cancellation propagates.
    YoutubeExplode::CancellationTokenSource cts;
    cts.cancel();
    bool canceled = false;
    try {
        (void)mb::searchTracks("never cached " + stamp, 5, cts.token());
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        canceled = true;
    }
    CHECK(canceled);

    // 404 -> ApiError(404)
    int status = 0;
    try {
        (void)mb::album("00000000-0000-0000-0000-000000000000");
    } catch (const mb::ApiError& e) {
        status = e.status;
    }
    std::printf("-- bogus release group -> ApiError %d\n", status);
    CHECK(status == 404 || status == 400);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool offlineOnly = false, keep = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--offline") offlineOnly = true;
        if (a == "--keep-cache") keep = true;
    }
    offlineTests();
    if (!offlineOnly) {
        try {
            liveTests(!keep);
        } catch (const std::exception& e) {
            std::printf("  FAIL live exception: %s\n", e.what());
            ++g_failures;
        }
    }
    std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
