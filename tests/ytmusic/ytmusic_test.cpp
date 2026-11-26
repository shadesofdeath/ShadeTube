// ytmusic_test: YouTube Music catalog (src/ytmusic). Offline: the parsers on responses captured from
// music.youtube.com (tests/ytmusic/fixtures, trimmed by row count), the id scheme, durations / counts / picture URLs and
// shapes the parsers must survive. Live: every endpoint for real, plus pinned playback (a YouTube Music song resolves to
// exactly its own video through MatchService, no search).
//   ytmusic_test                 offline
//   ytmusic_test live [query]    offline + live
//   ytmusic_test dump <file>     prints what the parsers make of a raw response (any kind; for new captures)
// Runs in its own profile: SHADETUBE_DATA_DIR = <exe dir>\ytmusic-profile unless already set.
#include "core/Paths.h"
#include "core/Settings.h"
#include "youtube/MatchService.h"
#include "ytmusic/Parse.h"
#include "ytmusic/YtMusic.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace st;
using namespace st::catalog;
namespace fs = std::filesystem;
using ytm::Json;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static Json load(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = Json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) {
        std::printf("  FAIL cannot load %s\n", path.string().c_str());
        ++g_failures;
        return Json::object();
    }
    return j;
}
static Json fixture(const char* name) { return load(fs::path(YTM_FIXTURE_DIR) / name); }

static std::string dur(int ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", ms / 60000, ms / 1000 % 60);
    return buf;
}

static void printTrack(const Track& t) {
    std::printf("    %-44.44s %-28.28s %-24.24s %5s  %s%s\n", t.name.c_str(), t.artistLine().c_str(), t.album.name.c_str(),
                dur(t.durationMs).c_str(), t.id.c_str(), t.playable ? "" : " (unplayable)");
}

static bool squareImages(const std::vector<Image>& images) {
    if (images.empty()) return false;
    for (const auto& i : images)
        if (i.width != i.height) return false;
    return true;
}

static const Shelf* shelfTitled(const std::vector<Shelf>& shelves, const std::string& title) {
    for (const auto& s : shelves)
        if (s.title == title) return &s;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------------------
static void offlinePieces() {
    std::printf("== pieces\n");
    CHECK(ytm::parseDurationMs("3:47") == 227000);
    CHECK(ytm::parseDurationMs("1:02:03") == 3723000);
    CHECK(ytm::parseDurationMs(" 0:05 ") == 5000);
    CHECK(ytm::parseDurationMs("2013") == 0);
    CHECK(ytm::parseDurationMs("1.2B plays") == 0);
    CHECK(ytm::parseDurationMs(":30") == 0);
    CHECK(ytm::parseDurationMs("") == 0);
    CHECK(ytm::parseCount("65.4M monthly audience") == 65400000);
    CHECK(ytm::parseCount("1,234 songs") == 1234);
    CHECK(ytm::parseCount("6.5K views") == 6500);
    CHECK(ytm::parseCount("1.2B plays") == 1200000000);
    CHECK(ytm::parseCount("36 tracks") == 36);
    CHECK(ytm::parseCount("songs") == 0);

    CHECK(ytm::isTrackId("yt:dQw4w9WgXcQ"));
    CHECK(!ytm::isTrackId("yt:short"));
    CHECK(!ytm::isTrackId("spotify:track:x"));
    CHECK(ytm::isYtmId("ytm:album:MPREb_x"));
    CHECK(!ytm::isYtmId("yt:dQw4w9WgXcQ"));
    CHECK(ytm::playlistId("VLRDCLAK5uy_abc") == "ytm:playlist:RDCLAK5uy_abc");
    CHECK(ytm::playlistId("PLabc") == "ytm:playlist:PLabc");
    CHECK(ytm::rawPlaylist("ytm:playlist:PLabc") == "PLabc");
    CHECK(ytm::rawAlbum("ytm:album:MPREb_x") == "MPREb_x");
    CHECK(ytm::rawAlbum("ytm:artist:UCx").empty());
    CHECK(ytm::rawArtist(ytm::artistId("UCabc")) == "UCabc");
    CHECK(ytm::rawVideo("yt:dQw4w9WgXcQ") == "dQw4w9WgXcQ");

    // Pictures: googleusercontent sizes are in the URL; a banner asked for square gets square crops.
    {
        const Json t = Json::parse(R"({"thumbnails":[{"url":"https://lh3.googleusercontent.com/abc=w60-h60-l90-rj","width":60,"height":60},
                                                     {"url":"https://lh3.googleusercontent.com/abc=w120-h120-l90-rj","width":120,"height":120}]})");
        const auto imgs = ytm::images(&t);
        CHECK(imgs.size() == 4);
        CHECK(imgs.front().width == 60);
        CHECK(imgs.back().width == 1080 && imgs.back().height == 1080);
        CHECK(imgs.back().url == "https://lh3.googleusercontent.com/abc=w1080-h1080-l90-rj");
        CHECK(pickImage(imgs, 300)->width == 544);
        const Json banner = Json::parse(R"({"thumbnails":[{"url":"https://lh3.googleusercontent.com/b=w1080-h450-p-l90-rj","width":1080,"height":450}]})");
        const auto sq = ytm::images(&banner, true);
        CHECK(squareImages(sq));
        CHECK(sq.size() == 4 && sq[2].url == "https://lh3.googleusercontent.com/b=w544-h544-p-l90-rj");
        const Json s = Json::parse(R"({"thumbnails":[{"url":"https://yt3.ggpht.com/x=s192","width":192,"height":192}]})");
        const auto ss = ytm::images(&s);
        CHECK(ss.size() == 3 && ss[1].url == "https://yt3.ggpht.com/x=s544");
        const Json yt = Json::parse(R"({"thumbnails":[{"url":"https://i.ytimg.com/vi/x/hqdefault.jpg","width":480,"height":360}]})");
        CHECK(ytm::images(&yt).size() == 1);
        CHECK(ytm::images(nullptr).empty());
    }

    // Shapes nothing should throw on.
    for (const char* bad : {"{}", "[]", "null", "5", R"({"contents":5})", R"({"contents":{"musicShelfRenderer":{"contents":"x"}}})",
                            R"({"a":{"musicResponsiveListItemRenderer":{"flexColumns":[{"x":1},7]}}})",
                            R"({"a":{"musicTwoRowItemRenderer":{"title":{"runs":5},"navigationEndpoint":[]}}})",
                            R"({"a":{"playlistPanelRenderer":{"contents":[{"playlistPanelVideoRenderer":{"videoId":7}}]}}})"}) {
        const Json j = Json::parse(bad);
        const auto s = ytm::parseSearch(j);
        const auto a = ytm::parseAlbum(j, "MPREb_x");
        const auto ar = ytm::parseArtist(j, "UCx");
        const auto p = ytm::parsePlaylist(j, "PLx");
        const auto pc = ytm::parsePlaylistContinuation(j);
        const auto w = ytm::parseWatch(j);
        const auto sh = ytm::parseShelves(j);
        const auto m = ytm::parseMoods(j);
        CHECK(s.tracks.empty() && a.tracks.empty() && ar.page.topTracks.empty() && p.tracks.empty() && pc.tracks.empty() &&
              w.tracks.empty() && sh.shelves.empty() && m.empty());
    }
}

static void offlineSearch() {
    std::printf("== search\n");
    {
        const auto r = ytm::parseSearch(fixture("search_songs.json"));
        CHECK(r.tracks.size() == 16);
        CHECK(r.albums.empty() && r.artists.empty() && r.playlists.empty());
        if (!r.tracks.empty()) {
            const Track& t = r.tracks[0];
            CHECK(t.name == "Instant Crush (feat. Julian Casablancas)");
            CHECK(t.id == "yt:khnokW3Mw24" && t.videoId == "khnokW3Mw24");
            CHECK(t.artists.size() == 2);
            CHECK(t.artists.size() == 2 && t.artists[0].name == "Daft Punk" && t.artists[0].id == "ytm:artist:UCRr1xG_2WIDs18a6cIiCxeA");
            CHECK(t.artists.size() == 2 && t.artists[1].name == "Julian Casablancas");
            CHECK(t.album.id == "ytm:album:MPREb_K8qWMWVqXGi" && t.album.name == "Random Access Memories");
            CHECK(t.durationMs == 338000);
            CHECK(!t.album.images.empty() && pickImage(t.album.images, 300) && pickImage(t.album.images, 300)->width >= 300);
            CHECK(t.playable);
        }
        // Three credited artists: "A, B & C".
        bool three = false;
        for (const auto& t : r.tracks)
            if (t.videoId == "Rgrt_8mXrK8") three = t.artists.size() == 3 && t.artists[2].name == "Nile Rodgers";
        CHECK(three);
        for (const auto& t : r.tracks) printTrack(t);
    }
    {
        const auto r = ytm::parseSearch(fixture("search_albums.json"));
        CHECK(r.albums.size() == 4);
        if (!r.albums.empty()) {
            const Album& a = r.albums[0];
            CHECK(a.name == "Random Access Memories" && a.id == "ytm:album:MPREb_K8qWMWVqXGi");
            CHECK(a.year() == "2013" && a.primaryType == "Album");
            CHECK(a.artists.size() == 1 && a.artists[0].name == "Daft Punk");
            CHECK(!a.images.empty());
        }
    }
    {
        const auto r = ytm::parseSearch(fixture("search_artists.json"));
        CHECK(r.artists.size() == 4);
        if (!r.artists.empty()) {
            CHECK(r.artists[0].name == "Daft Punk" && r.artists[0].id == "ytm:artist:UCRr1xG_2WIDs18a6cIiCxeA");
            CHECK(r.artists[0].listenCount == 65400000);
            CHECK(!r.artists[0].images.empty());
        }
    }
    {
        const auto r = ytm::parseSearch(fixture("search_playlists.json"));
        CHECK(r.playlists.size() == 4);
        if (!r.playlists.empty()) {
            const Playlist& p = r.playlists[0];
            CHECK(p.name == "Presenting Daft Punk");
            CHECK(p.id == "ytm:playlist:RDCLAK5uy_n20FRYQXNt1p1wS55Nj2r14IouO5weaYU");
            CHECK(p.countKnown && p.totalTracks == 35);
            CHECK(p.ownerName == "YouTube Music");
        }
    }
    {
        // An unfiltered search in the layout where every row sits in its own itemSectionRenderer, a top-result card
        // first; videos, episodes, podcasts and profiles are not catalog rows.
        const auto r = ytm::parseSearch(fixture("search_all.json"));
        CHECK(!r.tracks.empty() && r.tracks[0].videoId == "khnokW3Mw24");
        bool ram = false, weeknd = false, presenting = false;
        for (const auto& a : r.albums) ram |= a.id == "ytm:album:MPREb_K8qWMWVqXGi";
        for (const auto& a : r.artists) weeknd |= a.name == "The Weeknd";
        for (const auto& p : r.playlists) presenting |= p.name == "Presenting Daft Punk";
        CHECK(ram && weeknd && presenting);
        for (const auto& t : r.tracks) {
            CHECK(t.videoId != "GMKkPLUwWO4");   // a podcast episode
            CHECK(t.videoId != "yydNF8tuVmU");   // a music video
        }
        for (const auto& a : r.artists) CHECK(a.id != "ytm:artist:UCGrU_BTtdEEmK4Kl6WD1R3g");   // a profile
        std::printf("    unfiltered: %zu songs, %zu albums, %zu artists, %zu playlists\n", r.tracks.size(), r.albums.size(),
                    r.artists.size(), r.playlists.size());
    }
}

static void offlinePages() {
    std::printf("== album\n");
    {
        const Album a = ytm::parseAlbum(fixture("album.json"), "MPREb_K8qWMWVqXGi");
        CHECK(a.id == "ytm:album:MPREb_K8qWMWVqXGi");
        CHECK(a.name == "Random Access Memories");
        CHECK(a.primaryType == "Album" && a.year() == "2013");
        CHECK(a.artists.size() == 1 && a.artists[0].id == "ytm:artist:UCRr1xG_2WIDs18a6cIiCxeA");
        CHECK(a.releaseId == "OLAK5uy_kNhM2yaBTOVwrcZJepB1C9P3-n5_Sfy5c");
        CHECK(a.tracks.size() == 13 && a.totalTracks == 13);
        CHECK(!a.images.empty() && a.images.back().width >= 544);
        if (a.tracks.size() == 13) {
            const Track& t = a.tracks[4];
            CHECK(t.name == "Instant Crush (feat. Julian Casablancas)");
            CHECK(t.videoId == "a5uQMwRMHcs" && t.id == "yt:a5uQMwRMHcs");
            CHECK(t.trackNumber == 5 && t.durationMs == 338000);
            CHECK(t.artists.size() == 2);
            CHECK(t.album.id == a.id && t.album.name == a.name && !t.album.images.empty());
            CHECK(a.tracks[0].trackNumber == 1 && a.tracks[12].trackNumber == 13);
            int64_t total = 0;
            for (const auto& x : a.tracks) total += x.durationMs;
            CHECK(total / 60000 == 74);   // "1 hour, 14 minutes"
        }
        for (const auto& t : a.tracks) printTrack(t);
    }
    std::printf("== artist\n");
    {
        const auto r = ytm::parseArtist(fixture("artist.json"), "UCRr1xG_2WIDs18a6cIiCxeA");
        const auto& p = r.page;
        CHECK(p.artist.name == "Daft Punk" && p.artist.id == "ytm:artist:UCRr1xG_2WIDs18a6cIiCxeA");
        CHECK(squareImages(p.artist.images));
        CHECK(p.artist.listenCount == 65400000);
        CHECK(p.topTracks.size() == 3);   // the fixture keeps three of the five
        if (!p.topTracks.empty()) {
            CHECK(p.topTracks[0].videoId == "khnokW3Mw24");
            CHECK(p.topTracks[0].album.id == "ytm:album:MPREb_K8qWMWVqXGi");
        }
        CHECK(!p.albums.empty() && p.albums[0].name == "Random Access Memories (Drumless Edition)" && p.albums[0].year() == "2023");
        CHECK(!p.albums.empty() && p.albums[0].artists.size() == 1 && p.albums[0].artists[0].name == "Daft Punk");
        CHECK(!p.singles.empty() && p.singles[0].name == "GLBTM (Studio Outtakes)" && p.singles[0].primaryType == "Single");
        CHECK(!p.similar.empty() && p.similar[0].name == "Mr. Oizo");
        CHECK(p.other.empty());
        CHECK(r.radioPlaylistId == "RDEMni3jl65KF37F9JYsJM8DGg" && r.radioVideoId == "F6dMKqiDhGk");
        CHECK(r.songsPlaylistId == "OLAK5uy_lNVBcjNtiCwq-n95uoxJ-Gd4vsfMkyZNs");
        std::printf("    %zu top songs, %zu albums, %zu singles, %zu related\n", p.topTracks.size(), p.albums.size(), p.singles.size(),
                    p.similar.size());
    }
    std::printf("== playlist\n");
    {
        const auto r = ytm::parsePlaylist(fixture("playlist.json"), "PLt5IiqEadInpLjM3jcq_B0kjC4Mm0bL6N");
        CHECK(r.playlist.id == "ytm:playlist:PLt5IiqEadInpLjM3jcq_B0kjC4Mm0bL6N");
        CHECK(r.playlist.name == "Gorillaz, daft punk, capital cities etc");
        CHECK(r.playlist.countKnown && r.playlist.totalTracks == 36);
        CHECK(!r.playlist.images.empty());
        CHECK(r.tracks.size() == 5);
        if (!r.tracks.empty()) {
            CHECK(r.tracks[0].name == "Feel Good Inc." && r.tracks[0].videoId == "HyHNuVaZJ-k");
            CHECK(!r.tracks[0].artists.empty() && r.tracks[0].artists[0].name == "Gorillaz");
            CHECK(r.tracks[0].durationMs == 255000);
        }
        std::printf("    owner \"%s\"\n", r.playlist.ownerName.c_str());
    }
    {
        const auto r = ytm::parsePlaylist(fixture("playlist_long.json"), "RDCLAK5uy_mkXiaMATud8GJQ3aX7f0nCeCqvehA9wTQ");
        CHECK(r.tracks.size() == 5);
        CHECK(!r.continuation.empty() && !r.continuationInUrl);
        CHECK(r.playlist.countKnown && r.playlist.totalTracks == 118);
        const auto c = ytm::parsePlaylistContinuation(fixture("playlist_cont.json"));
        CHECK(!c.tracks.empty() && c.tracks[0].videoId == "kOkQ4T5WO9E");
        CHECK(!c.tracks.empty() && c.tracks[0].artists.size() == 2 && c.tracks[0].artists[1].name == "Rihanna");
    }
    std::printf("== radio (next)\n");
    {
        const auto w = ytm::parseWatch(fixture("next.json"));
        CHECK(w.tracks.size() == 8);
        CHECK(!w.continuation.empty());
        if (!w.tracks.empty()) {
            const Track& t = w.tracks[0];
            CHECK(t.videoId == "khnokW3Mw24");
            CHECK(t.artists.size() == 2 && t.album.id == "ytm:album:MPREb_K8qWMWVqXGi" && t.durationMs == 338000);
        }
        if (w.tracks.size() > 1) CHECK(w.tracks[1].artists.size() == 3);
        const auto c = ytm::parseWatchContinuation(fixture("next_cont.json"));
        CHECK(c.tracks.size() == 6);
        CHECK(!c.continuation.empty());
        for (const auto& t : w.tracks) printTrack(t);
    }
}

static void printShelves(const std::vector<Shelf>& shelves) {
    for (const auto& s : shelves) {
        std::printf("    [%s] %zu items:", s.title.c_str(), s.items.size());
        for (size_t i = 0; i < s.items.size() && i < 3; ++i) {
            const auto& it = s.items[i];
            const char* k = it.kind == CardItem::Kind::Track ? "song" : it.kind == CardItem::Kind::Album ? "album"
                            : it.kind == CardItem::Kind::Artist ? "artist" : "playlist";
            std::printf(" %s \"%s\";", k, it.title.c_str());
        }
        std::printf("\n");
    }
}

static void offlineShelves() {
    std::printf("== home / explore / charts / moods\n");
    {
        const auto h = ytm::parseShelves(fixture("home.json"));
        CHECK(!h.shelves.empty());
        CHECK(!h.continuation.empty());
        for (const auto& s : h.shelves) {
            CHECK(!s.title.empty() && !s.items.empty());
            for (const auto& it : s.items) {
                CHECK(!it.id.empty() && !it.title.empty());
                if (it.kind == CardItem::Kind::Track) CHECK(it.track.videoId.size() == 11 && it.id == "yt:" + it.track.videoId);
            }
        }
        printShelves(h.shelves);
        const auto c = ytm::parseShelvesContinuation(fixture("home_cont.json"));
        CHECK(!c.shelves.empty());
        printShelves(c.shelves);
    }
    {
        const auto e = ytm::parseShelves(fixture("explore.json"));
        const Shelf* fresh = shelfTitled(e.shelves, "New albums & singles");
        CHECK(fresh && !fresh->items.empty() && fresh->items[0].kind == CardItem::Kind::Album);
        CHECK(fresh && !fresh->items.empty() && fresh->items[0].id.rfind("ytm:album:MPREb_", 0) == 0);
        const Shelf* trending = shelfTitled(e.shelves, "Trending");
        CHECK(trending && !trending->items.empty() && trending->items[0].kind == CardItem::Kind::Track);
        CHECK(!shelfTitled(e.shelves, "Moods & genres"));   // buttons, not cards
        printShelves(e.shelves);
    }
    {
        const auto c = ytm::parseShelves(fixture("charts.json"));
        const Shelf* video = shelfTitled(c.shelves, "Video charts");
        CHECK(video && !video->items.empty() && video->items[0].kind == CardItem::Kind::Playlist);
        const Shelf* artists = shelfTitled(c.shelves, "Top artists");
        CHECK(artists && !artists->items.empty() && artists->items[0].kind == CardItem::Kind::Artist);
        printShelves(c.shelves);
    }
    {
        const auto m = ytm::parseMoods(fixture("moods.json"));
        CHECK(m.size() >= 6);
        bool chill = false, genres = false;
        for (const auto& x : m) {
            if (x.title == "Chill") chill = x.params == "ggMPOg1uX1JOQWZFeDByc2Jm" && x.color != 0 && x.section == "Moods & moments";
            genres |= x.section == "Genres";
        }
        CHECK(chill && genres);
        const auto shelves = ytm::parseShelves(fixture("mood.json")).shelves;
        CHECK(shelves.size() >= 2);
        CHECK(!shelves.empty() && shelves[0].title == "Coffee shop blends" && shelves[0].items[0].title == "Coffee Shop Blend" &&
              shelves[0].items[0].kind == CardItem::Kind::Playlist);
    }
}

static void dump(const fs::path& file) {
    const Json j = load(file);
    const auto s = ytm::parseSearch(j);
    std::printf("search: %zu songs, %zu albums, %zu artists, %zu playlists\n", s.tracks.size(), s.albums.size(), s.artists.size(),
                s.playlists.size());
    for (const auto& t : s.tracks) printTrack(t);
    for (const auto& a : s.albums) std::printf("  album %s | %s | %s %s\n", a.name.c_str(), a.id.c_str(), a.primaryType.c_str(), a.year().c_str());
    for (const auto& a : s.artists) std::printf("  artist %s | %s | %lld\n", a.name.c_str(), a.id.c_str(), static_cast<long long>(a.listenCount));
    for (const auto& p : s.playlists) std::printf("  playlist %s | %s | %s | %d\n", p.name.c_str(), p.id.c_str(), p.ownerName.c_str(), p.totalTracks);
    const auto w = ytm::parseWatch(j);
    std::printf("watch: %zu tracks, continuation %s\n", w.tracks.size(), w.continuation.empty() ? "no" : "yes");
    const auto sh = ytm::parseShelves(j);
    std::printf("shelves: %zu, continuation %s\n", sh.shelves.size(), sh.continuation.empty() ? "no" : "yes");
    printShelves(sh.shelves);
}

// ------------------------------------------------------------------------------------------------------------
template <class F>
static auto timed(const char* what, F&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = fn();
    std::printf("  %-34s %6.0f ms\n", what,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return r;
}

static void live(const std::string& query) {
    std::printf("== live (music.youtube.com)\n");
    Settings::get().load();
    YoutubeExplode::YoutubeClientOptions o;
    o.language = "tr";
    o.region = "TR";
    YoutubeExplode::YoutubeClient client(o);
    ytm::setClient(client);
    try {
        const auto s = timed("search", [&] { return ytm::search(query, 20); });
        std::printf("    %zu songs, %zu albums, %zu artists, %zu playlists\n", s.tracks.size(), s.albums.size(), s.artists.size(),
                    s.playlists.size());
        CHECK(!s.tracks.empty() && !s.albums.empty() && !s.artists.empty() && !s.playlists.empty());
        for (size_t i = 0; i < s.tracks.size() && i < 5; ++i) printTrack(s.tracks[i]);
        const auto again = timed("search (memory cache)", [&] { return ytm::search(query, 20); });
        CHECK(again.tracks.size() == s.tracks.size());

        if (!s.albums.empty()) {
            const auto a = timed("album", [&] { return ytm::album(s.albums[0].id); });
            std::printf("    \"%s\" %s %s, %zu tracks, playlist %s\n", a.name.c_str(), a.primaryType.c_str(), a.year().c_str(),
                        a.tracks.size(), a.releaseId.c_str());
            CHECK(!a.tracks.empty() && !a.releaseId.empty());
            for (const auto& t : a.tracks) CHECK(t.videoId.size() == 11 && t.album.id == a.id);
            const auto radio = timed("album radio", [&] { return ytm::radio(a.id, 60); });
            std::printf("    %zu radio tracks\n", radio.size());
            CHECK(radio.size() >= 20);
        }
        if (!s.artists.empty()) {
            const auto p = timed("artist", [&] { return ytm::artistPage(s.artists[0].id); });
            std::printf("    \"%s\": %zu top songs, %zu albums, %zu singles, %zu related, %lld monthly\n", p.artist.name.c_str(),
                        p.topTracks.size(), p.albums.size(), p.singles.size(), p.similar.size(), static_cast<long long>(p.artist.listenCount));
            CHECK(!p.topTracks.empty() && !p.albums.empty() && !p.similar.empty());
            CHECK(squareImages(p.artist.images));
            const auto radio = timed("artist radio", [&] { return ytm::radio(p.artist.id, 60); });
            CHECK(radio.size() >= 20);
        }
        if (!s.playlists.empty()) {
            const auto p = timed("playlist", [&] { return ytm::playlist(s.playlists[0].id); });
            std::printf("    \"%s\" by %s: %zu rows of %d%s\n", p.playlist.name.c_str(), p.playlist.ownerName.c_str(), p.tracks.size(),
                        p.playlist.totalTracks, p.continuation.empty() ? "" : " (more)");
            CHECK(!p.tracks.empty());
        }
        {
            const auto big = timed("long playlist, every page", [&] {
                return ytm::playlistTracks("ytm:playlist:RDCLAK5uy_mkXiaMATud8GJQ3aX7f0nCeCqvehA9wTQ", 400);
            });
            std::printf("    %zu rows\n", big.size());
            CHECK(big.size() > 100);
        }
        if (!s.tracks.empty()) {
            const auto radio = timed("song radio (2 pages)", [&] { return ytm::radio(s.tracks[0].id, 100); });
            std::printf("    %zu tracks, first %s\n", radio.size(), radio.empty() ? "-" : radio[0].name.c_str());
            CHECK(radio.size() > 50);
            CHECK(!radio.empty() && radio[0].videoId == s.tracks[0].videoId);
        }
        const auto home = timed("home (feed + explore + charts)", [&] { return ytm::home(); });
        printShelves(home);
        CHECK(home.size() >= 5);
        const auto moods = timed("moods & genres", [&] { return ytm::moods(); });
        std::printf("    %zu tiles\n", moods.size());
        CHECK(moods.size() > 10);
        if (!moods.empty()) {
            const auto shelves = timed("one mood", [&] { return ytm::moodShelves(moods[0].params); });
            std::printf("    \"%s\": %zu shelves\n", moods[0].title.c_str(), shelves.size());
            CHECK(!shelves.empty());
        }

        // Pinned playback: a song from YouTube Music resolves to its own video, without a search.
        if (!s.tracks.empty()) {
            youtube::MatchService matcher(client);
            const Track& t = s.tracks[0];
            const auto r = timed("resolve (pinned)", [&] { return matcher.resolve(t, false, false); });
            std::printf("    \"%s\" -> video %s, %s %d kbps (%s)\n", t.name.c_str(), r.match.videoId.c_str(), r.stream.mimeType.c_str(),
                        r.stream.bitrateKbps, r.stream.source.c_str());
            CHECK(r.match.videoId == t.videoId);
            CHECK(!r.stream.url.empty());
        }
        const auto st = ytm::stats();
        std::printf("  %d requests, %d memory-cache hits\n", st.requests, st.cacheHits);
    } catch (const std::exception& e) {
        std::printf("  FAIL live: %s\n", e.what());
        ++g_failures;
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr, 0))
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (fs::path(exe).parent_path() / L"ytmusic-profile").c_str());
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "dump" && argc > 2) {
        dump(argv[2]);
        return 0;
    }
    offlinePieces();
    offlineSearch();
    offlinePages();
    offlineShelves();
    if (mode == "live") live(argc > 2 ? argv[2] : "daft punk");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
