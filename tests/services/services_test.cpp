// services_test: playlists / albums / songs of Deezer, Apple Music and TIDAL (app/ServiceImport) and their matching to
// a catalog (app/CatalogMatch).
//
//   links    : every link form of the three services (playlists, albums, songs, short links, storefronts, browse /
//              listen / embed hosts, slugs, query strings) and the ones that stay Unsupported (artists, mixes, library)
//   parsers  : recorded answers (tests/services/fixtures, cut down by record_fixtures.py): Deezer header / tracks pages /
//              track / errors, Apple Music's page data, web bundle token, amp-api resources and pages, TIDAL's JSON:API
//              header / items pages / track list / bundle client, ISO 8601 durations, HTML stripping, canonical links
//   reading  : svc::read() on a fake transport replaying the fixtures: Deezer paging (next links, totals), album, song,
//              "no data"; Apple Music through amp-api (token from the bundle the page names, paging, the Authorization /
//              Origin headers), a refused token falling back to the page, a missing list; TIDAL (client-credentials
//              token, header, cursor pages, a refused client replaced by the bundle's, album, track, 404); short links
//   matching : catmatch::score / best / albumScore on made-up candidates (versions, featuring, live, wrong artist,
//              lengths, Turkish letters, other scripts), nameOnly ids
//   live     : `services_test live <link> [ytm|mb|spotify] [--quiet]`: reads the list for real and matches it against
//              that catalog (Spotify: an anonymous web-player token, search only), printing timings and the match rate;
//              `services_test live-set [ytm|mb]` runs the list of public playlists in kLiveSet
#include "app/CatalogMatch.h"
#include "app/Links.h"
#include "app/PlaylistIO.h"
#include "app/ServiceImport.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "spotify/Auth.h"
#include "spotify/SpotifyApi.h"
#include "spotify/Totp.h"
#include "ytmusic/YtMusic.h"

#include <YoutubeExplode/YoutubeClient.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace st;
using namespace st::app;
using links::Kind;
using links::Service;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static std::string fixture(const char* name) {
    std::ifstream in(std::filesystem::path(SERVICES_FIXTURE_DIR) / name, std::ios::binary);
    if (!in) {
        std::printf("  FAIL missing fixture %s\n", name);
        ++g_failures;
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- links ----------------------------------------------------------------------------------------------------------

static void expect(const char* text, Kind kind, Service service, const char* id = nullptr) {
    const auto l = links::parse(std::string_view(text));
    const bool ok = l.kind == kind && l.service == service && (!id || l.id == id);
    if (!ok) {
        std::printf("  FAIL parse(\"%s\"): kind %d service %d id \"%s\" (want kind %d service %d id \"%s\")\n", text,
                    static_cast<int>(l.kind), static_cast<int>(l.service), l.id.c_str(), static_cast<int>(kind),
                    static_cast<int>(service), id ? id : "");
        ++g_failures;
    }
}

static void testLinks() {
    std::printf("links\n");
    const auto D = Service::Deezer, A = Service::AppleMusic, T = Service::Tidal;
    // Deezer
    expect("https://www.deezer.com/playlist/3155776842", Kind::ServicePlaylist, D, "3155776842");
    expect("https://www.deezer.com/tr/playlist/3155776842?utm_source=deezer&utm_content=playlist-3155776842", Kind::ServicePlaylist, D, "3155776842");
    expect("deezer.com/en/album/302127", Kind::ServiceAlbum, D, "302127");
    expect("https://www.deezer.com/pt-br/track/3135556#x", Kind::ServiceTrack, D, "3135556");
    expect("http://deezer.com/album/302127/", Kind::ServiceAlbum, D, "302127");
    expect("https://www.deezer.com/us/artist/27", Kind::Unsupported, D);
    expect("https://www.deezer.com/show/1234", Kind::Unsupported, D);
    expect("https://www.deezer.com/playlist/abc", Kind::Unsupported, D);
    expect("https://www.deezer.com/song/3135556", Kind::Unsupported, D);
    expect("https://link.deezer.com/s/30Bx3dQd5pP9AbCd", Kind::ServiceShortLink, D, "https://link.deezer.com/s/30Bx3dQd5pP9AbCd");
    expect("https://link.deezer.com/s/30Bx3dQd5pP9AbCd?x=1", Kind::ServiceShortLink, D, "https://link.deezer.com/s/30Bx3dQd5pP9AbCd");
    expect("https://deezer.page.link/AbCdEfGh123", Kind::ServiceShortLink, D, "https://deezer.page.link/AbCdEfGh123");
    expect("dzr.page.link/AbCdEfGh123", Kind::ServiceShortLink, D, "https://dzr.page.link/AbCdEfGh123");
    expect("https://link.deezer.com/", Kind::Unsupported, D);
    expect("https://link.deezer.com/x/y/z", Kind::Unsupported, D);
    // Apple Music
    expect("https://music.apple.com/us/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb", Kind::ServicePlaylist, A,
           "us/pl.f4d106fed2bd41149aaacabb233eb5eb");
    expect("https://music.apple.com/TR/playlist/pl.f4d106fed2bd41149aaacabb233eb5eb", Kind::ServicePlaylist, A,
           "tr/pl.f4d106fed2bd41149aaacabb233eb5eb");
    expect("https://music.apple.com/gb/playlist/my-list/pl.u-AkAmPlyUxpxxXk", Kind::ServicePlaylist, A, "gb/pl.u-AkAmPlyUxpxxXk");
    expect("https://music.apple.com/us/album/stupid-song/1889992111", Kind::ServiceAlbum, A, "us/1889992111");
    expect("https://music.apple.com/us/album/stupid-song/1889992111?i=1889992115", Kind::ServiceTrack, A, "us/1889992115");
    expect("https://music.apple.com/de/album/x/1889992111?l=en&i=1889992115", Kind::ServiceTrack, A, "de/1889992115");
    expect("https://music.apple.com/us/song/stupid-song/1889992115", Kind::ServiceTrack, A, "us/1889992115");
    expect("https://music.apple.com/us/song/1889992115", Kind::ServiceTrack, A, "us/1889992115");
    expect("https://geo.music.apple.com/us/album/_/1889992111?app=music", Kind::ServiceAlbum, A, "us/1889992111");
    expect("https://embed.music.apple.com/us/album/x/1889992111", Kind::ServiceAlbum, A, "us/1889992111");
    expect("https://itunes.apple.com/us/album/x/1889992111", Kind::ServiceAlbum, A, "us/1889992111");
    expect("https://music.apple.com/playlist/x/pl.f4d106fed2bd41149aaacabb233eb5eb", Kind::ServicePlaylist, A,
           "us/pl.f4d106fed2bd41149aaacabb233eb5eb");
    expect("https://music.apple.com/us/artist/olivia-rodrigo/979458609", Kind::Unsupported, A);
    expect("https://music.apple.com/library/playlist/p.AbCdEf", Kind::Unsupported, A);
    expect("https://music.apple.com/us/album/x/notdigits", Kind::Unsupported, A);
    expect("https://music.apple.com/us/playlist/x/12345", Kind::Unsupported, A);
    // TIDAL
    expect("https://tidal.com/browse/playlist/4fd2cace-5255-409b-8748-b3661380bc7a", Kind::ServicePlaylist, T,
           "4fd2cace-5255-409b-8748-b3661380bc7a");
    expect("https://tidal.com/playlist/4FD2CACE-5255-409B-8748-B3661380BC7A?u", Kind::ServicePlaylist, T,
           "4fd2cace-5255-409b-8748-b3661380bc7a");
    expect("https://listen.tidal.com/playlist/4fd2cace-5255-409b-8748-b3661380bc7a", Kind::ServicePlaylist, T,
           "4fd2cace-5255-409b-8748-b3661380bc7a");
    expect("https://embed.tidal.com/playlists/4fd2cace-5255-409b-8748-b3661380bc7a", Kind::ServicePlaylist, T,
           "4fd2cace-5255-409b-8748-b3661380bc7a");
    expect("https://tidal.com/browse/album/165842024", Kind::ServiceAlbum, T, "165842024");
    expect("https://listen.tidal.com/album/165842024/track/165842038", Kind::ServiceTrack, T, "165842038");
    expect("https://tidal.com/browse/track/165842038?u", Kind::ServiceTrack, T, "165842038");
    expect("https://embed.tidal.com/tracks/165842038", Kind::ServiceTrack, T, "165842038");
    expect("https://www.tidal.com/album/165842024", Kind::ServiceAlbum, T, "165842024");
    expect("https://tidal.com/browse/artist/8847", Kind::Unsupported, T);
    expect("https://tidal.com/browse/mix/0123abc", Kind::Unsupported, T);
    expect("https://tidal.com/browse/playlist/not-a-uuid", Kind::Unsupported, T);
    expect("https://tidal.com/", Kind::Unsupported, T);
    // Others unchanged, and not links.
    expect("https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, Service::Spotify);
    expect("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG", Kind::YouTubePlaylist, Service::YouTube);
    expect("deezer playlist", Kind::None, Service::None);
    expect("https://example.com/playlist/123", Kind::None, Service::None);
    // ref() of the kinds.
    const auto r = svc::ref(links::parse(std::string_view("https://music.apple.com/us/album/x/1889992111?i=1889992115")));
    CHECK(r && r->service == A && r->kind == svc::Kind::Track && r->id == "us/1889992115");
    CHECK(!svc::ref(links::parse(std::string_view("https://link.deezer.com/s/30Bx3dQd5pP9AbCd"))));
    CHECK(!svc::ref(links::parse(std::string_view("https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH"))));
}

// ---- parsers --------------------------------------------------------------------------------------------------------

static void testParsers() {
    std::printf("parsers\n");
    // Deezer
    {
        auto l = svc::deezer::parseHeader(fixture("dz_playlist.json"), svc::Kind::Playlist);
        std::printf("  deezer playlist '%s' by %s, %d songs, cover %s\n", l.title.c_str(), l.owner.c_str(), l.expected,
                    l.coverUrl.substr(0, 40).c_str());
        CHECK(l.id == "526783865" && !l.title.empty() && l.expected == 500 && l.coverUrl.rfind("https://", 0) == 0);
        int total = 0;
        const std::string next = svc::deezer::parseTracks(fixture("dz_playlist_tracks_0.json"), l, false, &total);
        CHECK(l.songs.size() == 3 && total == 500);
        CHECK(next.find("index=3") != std::string::npos);
        const auto& s = l.songs[0];
        std::printf("  first: %s - %s [%s] %d ms isrc %s\n", s.artistLine().c_str(), s.title.c_str(), s.album.c_str(), s.durationMs,
                    s.isrc.c_str());
        CHECK(!s.title.empty() && !s.artists.empty() && !s.album.empty() && s.durationMs > 60000 && s.isrc.size() == 12);
        CHECK(svc::deezer::parseTracks(fixture("dz_playlist_tracks_3.json"), l, false).empty());
        CHECK(l.songs.size() == 6);
        auto a = svc::deezer::parseHeader(fixture("dz_album.json"), svc::Kind::Album);
        CHECK(a.title == "Discovery" && a.owner == "Daft Punk" && a.upc == "724384960650" && a.releaseDate.size() == 10);
        svc::deezer::parseTracks(fixture("dz_album_tracks.json"), a, true);
        CHECK(a.songs.size() == 3 && a.songs[0].album == "Discovery" && a.songs[0].trackNumber == 1 && a.songs[0].isrc == "GBDUW0000053");
        CHECK(!a.songs[0].coverUrl.empty());   // the album's cover
        const auto t = svc::deezer::parseTrack(fixture("dz_track.json"));
        CHECK(t.title == "Harder, Better, Faster, Stronger" && t.isrc == "GBDUW0000059" && t.album == "Discovery" && t.durationMs > 200000);
        const auto e = svc::deezer::parseError(fixture("dz_error.json"));
        CHECK(e && e->code == svc::Error::NotFound);
        CHECK(!svc::deezer::parseError(fixture("dz_track.json")));
        CHECK(svc::deezer::parseError(R"({"error":{"type":"Exception","message":"Quota limit exceeded","code":4}})")->code == svc::Error::Blocked);
        CHECK(svc::deezer::parseError(R"({"error":{"type":"OAuthException","message":"x","code":200}})")->code == svc::Error::Private);
        CHECK(svc::deezer::parseHeader("not json", svc::Kind::Playlist).title.empty());
        svc::List none;
        CHECK(svc::deezer::parseTracks(R"({"data":"x"})", none, false).empty() && none.songs.empty());
    }
    // Apple Music
    {
        const std::string page = fixture("am_page_playlist.html");
        CHECK(svc::apple::bundlePath(page).rfind("/assets/index", 0) == 0);
        CHECK(svc::apple::bundlePath("<html></html>").empty());
        int64_t exp = 0;
        const std::string tok = svc::apple::tokenFromBundle(fixture("am_bundle.js"), &exp);
        CHECK(!tok.empty() && exp == 1799948708);
        CHECK(tok.find("LT2ZDZSXNQ") == std::string::npos);   // the other JWT is skipped
        CHECK(svc::apple::tokenFromBundle("const a='eyJabc.def.ghi';").empty());
        svc::List pl;
        pl.kind = svc::Kind::Playlist;
        const std::string next = svc::apple::parseResource(fixture("am_playlist.json"), pl);
        std::printf("  apple playlist '%s' by %s, %zu songs, next %s\n", pl.title.c_str(), pl.owner.c_str(), pl.songs.size(), next.c_str());
        CHECK(pl.title == "Berlioz: The Complete Works" && pl.owner == "Warner Classics" && pl.songs.size() == 3);
        CHECK(next.find("/tracks?") != std::string::npos && next.find("offset=3") != std::string::npos);
        CHECK(!pl.description.empty() && pl.description.find('<') == std::string::npos);
        CHECK(pl.coverUrl.find("600x600") != std::string::npos && pl.coverUrl.find('{') == std::string::npos);
        const auto& s = pl.songs[0];
        CHECK(!s.title.empty() && !s.artists.empty() && !s.album.empty() && s.durationMs > 0 && s.isrc.size() == 12);
        CHECK(svc::apple::parseTracks(fixture("am_playlist_tracks_3.json"), pl).empty() && pl.songs.size() == 6);
        svc::List al;
        al.kind = svc::Kind::Album;
        CHECK(svc::apple::parseResource(fixture("am_album.json"), al).empty());
        CHECK(al.kind == svc::Kind::Album && al.owner == "Olivia Rodrigo" && !al.upc.empty() && al.songs.size() == 3 && al.expected > 3);
        const auto song = svc::apple::parseSong(fixture("am_song.json"));
        CHECK(song.title == "stupid song" && song.isrc == "USUG12601723" && song.artists.size() == 1 && song.durationMs == 209680);
        svc::List fromPage;
        CHECK(svc::apple::parsePage(page, fromPage));
        std::printf("  apple page: '%s', %zu songs (%s)\n", fromPage.title.c_str(), fromPage.songs.size(),
                    fromPage.songs.empty() ? "" : fromPage.songs[0].title.c_str());
        CHECK(fromPage.title == "Berlioz: The Complete Works" && fromPage.songs.size() == 3 && !fromPage.partial);
        CHECK(!fromPage.songs[0].artists.empty() && !fromPage.songs[0].album.empty() && fromPage.songs[0].durationMs > 0 &&
              fromPage.songs[0].isrc.empty());
        svc::List bad;
        CHECK(!svc::apple::parsePage("<html><body>nothing</body></html>", bad));
        CHECK(svc::apple::parseResource(fixture("am_404.json"), bad).empty() && bad.title.empty());
    }
    // TIDAL
    {
        CHECK(svc::tidal::durationMs("PT3M28S") == 208000);
        CHECK(svc::tidal::durationMs("PT1H2M") == 3720000);
        CHECK(svc::tidal::durationMs("PT11H53M38S") == 42818000);
        CHECK(svc::tidal::durationMs("PT4.5S") == 4500);
        CHECK(svc::tidal::durationMs("") == 0 && svc::tidal::durationMs("3:28") == 0);
        const auto c = svc::tidal::clientFromBundle(fixture("td_bundle.js"));
        CHECK(c && c->id == "testClientId0001" && c->secret == "dGVzdC1rZXk=");
        CHECK(!svc::tidal::clientFromBundle("var x=1;"));
        CHECK(svc::tidal::bundlePath(fixture("td_page.html")).rfind("/assets/index-", 0) == 0);
        svc::List pl;
        svc::tidal::parseHeader(fixture("td_playlist.json"), pl);
        std::printf("  tidal playlist '%s', %d items, cover %s\n", pl.title.c_str(), pl.expected, pl.coverUrl.c_str());
        CHECK(pl.title.find("Test Trax") == 0 && pl.expected == 132 && pl.coverUrl.find("resources.tidal.com") != std::string::npos);
        CHECK(pl.description == "Tidal Masters Only Test Tracks");
        const std::string next = svc::tidal::parseItems(fixture("td_items_1.json"), pl, false);
        CHECK(pl.songs.size() == 3 && next.find("page%5Bcursor%5D=") != std::string::npos);
        const auto& s = pl.songs[0];
        std::printf("  first: %s - %s [%s] %d ms isrc %s\n", s.artistLine().c_str(), s.title.c_str(), s.album.c_str(), s.durationMs,
                    s.isrc.c_str());
        CHECK(s.title == "Fall" && s.artistLine() == "Daft Punk" && s.album.find("TRON") == 0 && s.durationMs == 83000 &&
              s.isrc == "USWD11054889");
        CHECK(svc::tidal::parseItems(fixture("td_items_2.json"), pl, false).empty() && pl.songs.size() == 6);
        svc::List al;
        svc::tidal::parseHeader(fixture("td_album.json"), al);
        CHECK(al.kind == svc::Kind::Album && al.title.find("TRON") == 0 && al.owner == "Daft Punk" && !al.upc.empty() && al.expected == 31);
        CHECK(svc::tidal::parseItems(fixture("td_album_items.json"), al, true).empty());
        CHECK(al.songs.size() == 3 && al.songs[0].album == al.title && al.songs[0].trackNumber == 1 && !al.songs[0].coverUrl.empty());
        const auto t = svc::tidal::parseTrackList(fixture("td_tracks.json"));
        CHECK(t && t->title == "Fall" && t->isrc == "USWD11054889" && !t->album.empty() && !t->coverUrl.empty());
        CHECK(!svc::tidal::parseTrackList(fixture("td_404.json")));
        // A row whose track is not included (no longer on TIDAL) and a video count as skipped.
        svc::List sk;
        svc::tidal::parseItems(R"({"data":[{"id":"1","type":"tracks"},{"id":"2","type":"videos"}],"included":[]})", sk, false);
        CHECK(sk.songs.empty() && sk.skipped == 2);
    }
    // Text helpers
    CHECK(svc::stripHtml("<b>Morgan</b> &amp; &quot;You&quot;&#39;s <i>x</i><br>next&nbsp;line &#x2019;") ==
          "Morgan & \"You\"'s x\nnext line \xE2\x80\x99");
    CHECK(svc::canonicalLink(fixture("dz_short_landing.html")) == "https://www.deezer.com/tr/playlist/3155776842");
    CHECK(svc::canonicalLink("<html></html>").empty());
}

// ---- reading on a fake transport --------------------------------------------------------------------------------------

struct Rule {
    std::string method;      // "" = any
    std::string contains;    // URL substring
    std::string bodyHas;     // request body substring ("" = any)
    int status = 200;
    std::string file;        // fixture ("" = empty body)
};

struct FakeNet {
    std::vector<Rule> rules;
    std::vector<svc::HttpRequest> seen;
    svc::Fetch fetch() {
        return [this](const svc::HttpRequest& r, const svc::CT&) {
            seen.push_back(r);
            for (const auto& rule : rules) {
                if (!rule.method.empty() && rule.method != r.method) continue;
                if (r.url.find(rule.contains) == std::string::npos) continue;
                if (!rule.bodyHas.empty() && r.body.find(rule.bodyHas) == std::string::npos) continue;
                svc::HttpResponse resp;
                resp.statusCode = rule.status;
                if (!rule.file.empty()) resp.body = fixture(rule.file.c_str());
                return resp;
            }
            std::printf("  (fake net: no rule for %s %s)\n", r.method.c_str(), r.url.c_str());
            svc::HttpResponse resp;
            resp.statusCode = 404;
            return resp;
        };
    }
    bool sent(const std::string& urlPart) const {
        for (const auto& r : seen)
            if (r.url.find(urlPart) != std::string::npos) return true;
        return false;
    }
    const svc::HttpRequest* request(const std::string& urlPart) const {
        for (const auto& r : seen)
            if (r.url.find(urlPart) != std::string::npos) return &r;
        return nullptr;
    }
};

static svc::Ref refOf(const char* link) {
    const auto r = svc::ref(links::parse(std::string_view(link)));
    return r ? *r : svc::Ref{};
}

template <class F>
static svc::Error::Code errorOf(F&& f) {
    try {
        f();
    } catch (const svc::Error& e) {
        return e.code;
    }
    return static_cast<svc::Error::Code>(-1);
}

static void testReading() {
    std::printf("reading\n");
    // Deezer: header, then the tracks pages following `next`.
    {
        FakeNet n;
        n.rules = {{"", "/playlist/526783865/tracks?index=0", "", 200, "dz_playlist_tracks_0.json"},
                   {"", "/playlist/526783865/tracks?limit=3&index=3", "", 200, "dz_playlist_tracks_3.json"},
                   {"", "/playlist/526783865", "", 200, "dz_playlist.json"}};
        int lastRead = 0, lastExpected = 0;
        const auto l = svc::read(refOf("https://www.deezer.com/playlist/526783865"), n.fetch(), [&](int r, int e) {
            lastRead = r;
            lastExpected = e;
        });
        CHECK(l.songs.size() == 6 && l.expected == 500 && lastRead == 6 && lastExpected == 500);
        CHECK(n.sent("tracks?index=0&limit=500"));
        CHECK(l.service == Service::Deezer && l.kind == svc::Kind::Playlist && !l.title.empty());
        FakeNet a;
        a.rules = {{"", "/album/302127/tracks", "", 200, "dz_album_tracks.json"}, {"", "/album/302127", "", 200, "dz_album.json"}};
        const auto al = svc::read(refOf("https://www.deezer.com/album/302127"), a.fetch());
        CHECK(al.kind == svc::Kind::Album && al.songs.size() == 3 && al.upc == "724384960650");
        FakeNet t;
        t.rules = {{"", "/track/3135556", "", 200, "dz_track.json"}};
        const auto tr = svc::read(refOf("https://www.deezer.com/track/3135556"), t.fetch());
        CHECK(tr.songs.size() == 1 && tr.songs[0].isrc == "GBDUW0000059");
        FakeNet e;
        e.rules = {{"", "/playlist/1", "", 200, "dz_error.json"}};
        CHECK(errorOf([&] { svc::read(refOf("https://www.deezer.com/playlist/1"), e.fetch()); }) == svc::Error::NotFound);
    }
    // Apple Music: the page names the bundle, the bundle has the token, amp-api pages.
    {
        svc::forgetTokens();
        FakeNet n;
        n.rules = {{"", "music.apple.com/us/playlist/pl.2b7ed7ebbc6e494caabebc04f2ac71dd", "", 200, "am_page_playlist.html"},
                   {"", "music.apple.com/assets/index", "", 200, "am_bundle.js"},
                   {"", "/tracks?l=en-US&offset=3", "", 200, "am_playlist_tracks_3.json"},
                   {"", "amp-api.music.apple.com/v1/catalog/us/playlists/pl.2b7ed7ebbc6e494caabebc04f2ac71dd?", "", 200, "am_playlist.json"}};
        const auto l = svc::read(refOf("https://music.apple.com/us/playlist/x/pl.2b7ed7ebbc6e494caabebc04f2ac71dd"), n.fetch());
        CHECK(l.songs.size() == 6 && !l.partial && l.songs[0].isrc.size() == 12);
        const auto* api = n.request("amp-api.music.apple.com/v1/catalog/us/playlists/");
        CHECK(api && api->header("Authorization").value_or("").rfind("Bearer eyJ", 0) == 0 &&
              api->header("Origin").value_or("") == "https://music.apple.com");
        CHECK(n.sent("limit%5Btracks%5D=300") && n.sent("offset=3&limit=300"));
        // The kept token: the next read goes straight to the API.
        FakeNet again;
        again.rules = {{"", "amp-api.music.apple.com/v1/catalog/us/songs/1889992115", "", 200, "am_song.json"}};
        const auto song = svc::read(refOf("https://music.apple.com/us/song/x/1889992115"), again.fetch());
        CHECK(song.songs.size() == 1 && song.songs[0].isrc == "USUG12601723" && again.seen.size() == 1);
        // A token amp-api refuses (twice: the bundle read again gives the same): the page's own songs.
        FakeNet refused;
        refused.rules = {{"", "music.apple.com/us/playlist/pl.2b7ed7ebbc6e494caabebc04f2ac71dd", "", 200, "am_page_playlist.html"},
                         {"", "music.apple.com/assets/index", "", 200, "am_bundle.js"},
                         {"", "amp-api.music.apple.com", "", 401, ""}};
        const auto fb = svc::read(refOf("https://music.apple.com/us/playlist/x/pl.2b7ed7ebbc6e494caabebc04f2ac71dd"), refused.fetch());
        CHECK(fb.songs.size() == 3 && fb.title == "Berlioz: The Complete Works" && fb.songs[0].isrc.empty());
        // Not in this storefront / private: NotFound.
        FakeNet missing;
        missing.rules = {{"", "amp-api.music.apple.com", "", 404, "am_404.json"}};
        CHECK(errorOf([&] { svc::read(refOf("https://music.apple.com/us/playlist/pl.u-doesnotexist000"), missing.fetch()); }) ==
              svc::Error::NotFound);
        // No token at all (no bundle in the page) and no data in it: Blocked.
        svc::forgetTokens();
        FakeNet blank;
        blank.rules = {{"", "music.apple.com/", "", 200, "dz_short_404.html"}};
        CHECK(errorOf([&] { svc::read(refOf("https://music.apple.com/us/album/1889992111"), blank.fetch()); }) == svc::Error::Blocked);
        svc::forgetTokens();
    }
    // TIDAL: client-credentials token, header, cursor pages.
    {
        svc::forgetTokens();
        FakeNet n;
        n.rules = {{"POST", "auth.tidal.com/v1/oauth2/token", "client_id=txNoH4kkV41MfH25", 200, "td_token.json"},
                   {"", "relationships/items?countryCode=US&include=items%2Citems.artists%2Citems.albums&page%5Bcursor%5D=", "", 200, "td_items_2.json"},
                   {"", "relationships/items", "", 200, "td_items_1.json"},
                   {"", "openapi.tidal.com/v2/playlists/4fd2cace-5255-409b-8748-b3661380bc7a?", "", 200, "td_playlist.json"}};
        const auto l = svc::read(refOf("https://tidal.com/browse/playlist/4fd2cace-5255-409b-8748-b3661380bc7a"), n.fetch());
        CHECK(l.songs.size() == 6 && l.expected == 132 && l.title.find("Test Trax") == 0);
        const auto* tok = n.request("auth.tidal.com");
        CHECK(tok && tok->method == "POST" && tok->body.find("grant_type=client_credentials") != std::string::npos);
        const auto* items = n.request("relationships/items");
        CHECK(items && items->header("Authorization").value_or("") == "Bearer test-token");
        // A refused client: the one in tidal.com's bundle.
        svc::forgetTokens();
        FakeNet r;
        r.rules = {{"POST", "auth.tidal.com", "client_id=testClientId0001", 200, "td_token.json"},
                   {"POST", "auth.tidal.com", "", 401, ""},
                   {"", "tidal.com/browse/album/165842024", "", 200, "td_page.html"},
                   {"", "tidal.com/assets/index-", "", 200, "td_bundle.js"},
                   {"", "albums/165842024/relationships/items", "", 200, "td_album_items.json"},
                   {"", "openapi.tidal.com/v2/albums/165842024?", "", 200, "td_album.json"}};
        const auto al = svc::read(refOf("https://tidal.com/browse/album/165842024"), r.fetch());
        CHECK(al.kind == svc::Kind::Album && al.songs.size() == 3 && al.owner == "Daft Punk");
        CHECK(r.sent("tidal.com/assets/index-"));
        // The track list endpoint; a playlist that doesn't exist.
        FakeNet t;
        t.rules = {{"", "tracks?countryCode=US&filter%5Bid%5D=165842038", "", 200, "td_tracks.json"}};
        const auto tr = svc::read(refOf("https://tidal.com/browse/track/165842038"), t.fetch());
        CHECK(tr.songs.size() == 1 && tr.songs[0].title == "Fall");
        FakeNet m;
        m.rules = {{"", "openapi.tidal.com", "", 404, "td_404.json"}};
        CHECK(errorOf([&] { svc::read(refOf("https://tidal.com/playlist/00000000-0000-0000-0000-000000000000"), m.fetch()); }) ==
              svc::Error::NotFound);
        // No token from either client: Blocked.
        svc::forgetTokens();
        FakeNet b;
        b.rules = {{"POST", "auth.tidal.com", "", 401, ""}, {"", "tidal.com/browse/", "", 200, "td_page.html"},
                   {"", "tidal.com/assets/index-", "", 200, "td_bundle.js"}};
        CHECK(errorOf([&] { svc::read(refOf("https://tidal.com/browse/track/1"), b.fetch()); }) == svc::Error::Blocked);
        svc::forgetTokens();
    }
    // Deezer short links.
    {
        FakeNet n;
        n.rules = {{"", "link.deezer.com/s/30Bx", "", 200, "dz_short_landing.html"}, {"", "link.deezer.com/s/dead", "", 200, "dz_short_404.html"}};
        const auto r = svc::resolveShortLink("https://link.deezer.com/s/30Bx3dQd5pP9AbCd", n.fetch());
        CHECK(r && r->service == Service::Deezer && r->kind == svc::Kind::Playlist && r->id == "3155776842");
        CHECK(!svc::resolveShortLink("https://link.deezer.com/s/deadlink0", n.fetch()));
    }
    // A transport failure is retried, then reported as Network.
    {
        int calls = 0;
        svc::Fetch flaky = [&](const svc::HttpRequest&, const svc::CT&) -> svc::HttpResponse {
            ++calls;
            throw std::runtime_error("connection reset");
        };
        CHECK(errorOf([&] { svc::read(refOf("https://www.deezer.com/track/3135556"), flaky); }) == svc::Error::Network);
        CHECK(calls == 3);
        int n = 0;
        svc::Fetch once = [&](const svc::HttpRequest&, const svc::CT&) -> svc::HttpResponse {
            if (n++ == 0) throw std::runtime_error("connection reset");
            svc::HttpResponse resp;
            resp.statusCode = 200;
            resp.body = fixture("dz_track.json");
            return resp;
        };
        CHECK(svc::read(refOf("https://www.deezer.com/track/3135556"), once).songs.size() == 1);
    }
}

// ---- matching -------------------------------------------------------------------------------------------------------

static catalog::Track cand(const char* id, const char* name, std::initializer_list<const char*> artists, int ms) {
    catalog::Track t;
    t.id = id;
    t.name = name;
    for (const char* a : artists) t.artists.push_back({"", a});
    t.durationMs = ms;
    return t;
}

static svc::Song song(const char* title, std::initializer_list<const char*> artists, int ms, const char* isrc = "") {
    svc::Song s;
    s.title = title;
    for (const char* a : artists) s.artists.push_back(a);
    s.durationMs = ms;
    s.isrc = isrc;
    return s;
}

static void testMatching() {
    std::printf("matching\n");
    using catmatch::score;
    const auto hb = song("Harder, Better, Faster, Stronger", {"Daft Punk"}, 224000);
    CHECK(score(hb, cand("a", "Harder, Better, Faster, Stronger", {"Daft Punk"}, 224693)) >= 99);
    CHECK(score(hb, cand("a", "Harder Better Faster Stronger", {"Daft Punk"}, 0)) >= 85);
    CHECK(score(hb, cand("a", "Harder, Better, Faster, Stronger", {"Kanye West"}, 224000)) < catmatch::kAccept);   // wrong artist
    CHECK(score(hb, cand("a", "Harder, Better, Faster, Stronger (Live)", {"Daft Punk"}, 230000)) < catmatch::kAccept);
    CHECK(score(hb, cand("a", "Around the World", {"Daft Punk"}, 224000)) < 50);
    // Versions and credits that name the same recording.
    CHECK(score(song("Here Comes the Sun - Remastered 2009", {"The Beatles"}, 185000),
                cand("a", "Here Comes The Sun (2019 Mix)", {"The Beatles"}, 185500)) >= catmatch::kAccept);
    CHECK(score(song("Blinding Lights", {"The Weeknd"}, 200040), cand("a", "Blinding Lights (Radio Edit)", {"The Weeknd"}, 201000)) >=
          catmatch::kAccept);
    CHECK(score(song("Stay (with Justin Bieber)", {"The Kid LAROI", "Justin Bieber"}, 141000),
                cand("a", "STAY", {"The Kid LAROI", "Justin Bieber"}, 141800)) >= catmatch::kAccept);
    CHECK(score(song("Mood feat. iann dior", {"24kGoldn"}, 140000), cand("a", "Mood (feat. iann dior)", {"24kGoldn", "iann dior"}, 140500)) >=
          catmatch::kAccept);
    // Apple's single artist string vs separate artists.
    CHECK(score(song("Les Francs-Juges, Op. 3, H. 23d: Overture", {"Sir Roger Norrington & London Classical Players"}, 694653),
                cand("a", "Les Francs-Juges, Op. 3, H. 23d: Overture", {"Sir Roger Norrington", "London Classical Players"}, 694000)) >=
          catmatch::kAccept);
    // Remix / live on one side only.
    CHECK(score(song("One More Time - Radio Edit", {"Daft Punk"}, 320000), cand("a", "One More Time (Romanthony Remix)", {"Daft Punk"}, 320000)) <
          catmatch::kAccept);
    CHECK(score(song("Creep (Acoustic)", {"Radiohead"}, 250000), cand("a", "Creep (Acoustic)", {"Radiohead"}, 251000)) >= 95);
    // Lengths: far apart costs, unknown is neutral.
    CHECK(score(hb, cand("a", "Harder, Better, Faster, Stronger", {"Daft Punk"}, 400000)) <
          score(hb, cand("b", "Harder, Better, Faster, Stronger", {"Daft Punk"}, 0)));
    // Turkish letters / case, accents, other scripts.
    CHECK(score(song("İstanbul'u Dinliyorum", {"Barış Manço"}, 200000), cand("a", "ISTANBUL'U DINLIYORUM", {"Baris Manco"}, 201000)) >=
          catmatch::kAccept);
    CHECK(score(song("Déjà Vu", {"Beyoncé"}, 240000), cand("a", "Deja Vu", {"Beyonce", "JAY-Z"}, 240500)) >= catmatch::kAccept);
    CHECK(score(song("夜に駆ける", {"YOASOBI"}, 261000), cand("a", "夜に駆ける", {"YOASOBI"}, 261500)) >= 95);
    CHECK(score(song("夜に駆ける", {"YOASOBI"}, 261000), cand("a", "群青", {"YOASOBI"}, 261500)) < catmatch::kAccept);
    // best(): the right one among several, none below the bar.
    std::vector<catalog::Track> cs = {cand("x", "Harder, Better, Faster, Stronger (Live)", {"Daft Punk"}, 230000),
                                      cand("y", "Harder, Better, Faster, Stronger", {"Daft Punk"}, 224500),
                                      cand("z", "Stronger", {"Kanye West"}, 311000)};
    double top = 0;
    CHECK(catmatch::best(hb, cs, catmatch::kAccept, &top) == 1 && top >= 95);
    CHECK(catmatch::best(song("Nothing like it", {"Nobody"}, 100000), cs, catmatch::kAccept) == -1);
    CHECK(catmatch::best(hb, {}, catmatch::kAccept) == -1);
    // Albums.
    catalog::Album a;
    a.id = "x";
    a.name = "Discovery";
    a.artists = {{"", "Daft Punk"}};
    CHECK(catmatch::albumScore("Discovery", "Daft Punk", a) >= 95);
    a.name = "Discovery (Deluxe Edition)";
    CHECK(catmatch::albumScore("Discovery", "Daft Punk", a) >= catmatch::kAccept);
    a.name = "Homework";
    CHECK(catmatch::albumScore("Discovery", "Daft Punk", a) < catmatch::kAccept);
    // Words.
    CHECK(catmatch::coreWords("Song (Remastered 2011) - Single Version") == std::vector<std::wstring>{L"song"});
    std::vector<std::wstring> flags;
    CHECK((catmatch::coreWords("Song - Live at Wembley", &flags) == std::vector<std::wstring>{L"song"}) && flags == std::vector<std::wstring>{L"live"});
    CHECK((catmatch::coreWords("(I Can't Get No) Satisfaction") == std::vector<std::wstring>{L"i", L"can", L"t", L"get", L"no", L"satisfaction"}));
    // A song known by name only: the playlist files' import id, its artists / album / cover.
    svc::Song s = song("Fall", {"Daft Punk"}, 83000);
    s.album = "TRON: Legacy";
    s.coverUrl = "https://example.com/c.jpg";
    const auto t = catmatch::nameOnly(s);
    CHECK(t.id == playlistio::importId("Fall", "Daft Punk") && t.name == "Fall" && t.artists.size() == 1 && t.album.name == "TRON: Legacy" &&
          t.album.images.size() == 1 && t.durationMs == 83000);
}

// ---- live -----------------------------------------------------------------------------------------------------------

static std::string anonymousSpotifyToken() {
    using namespace st::spotify;
    const Nuance n = fetchNuance(false, {});
    const std::string code = totp(n.secret, fetchServerTimeSeconds({}));
    const std::string url = "https://open.spotify.com/api/token?reason=init&productType=web-player&totp=" + code +
                            "&totpServer=" + code + "&totpVer=" + std::to_string(n.version);
    const auto resp = http::get(url, {{"User-Agent", kUserAgent}, {"Accept", "application/json"},
                                      {"Referer", "https://open.spotify.com/"}, {"App-Platform", "WebPlayer"}});
    if (!resp.isSuccessStatusCode()) return {};
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    return j.is_object() ? j.value("accessToken", std::string()) : std::string();
}

static double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

struct LiveResult {
    size_t songs = 0;
    int found = 0, byIsrc = 0;
    double readS = 0, matchS = 0;
};

static LiveResult liveOne(const std::string& linkText, const std::string& catalogName, spotify::Api* api, bool quiet) {
    LiveResult out;
    auto link = links::parse(std::string_view(linkText));
    std::optional<svc::Ref> ref;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (link.kind == Kind::ServiceShortLink) ref = svc::resolveShortLink(link.id, svc::liveFetch());
        else ref = svc::ref(link);
        if (!ref) {
            std::printf("  %s: not a Deezer / Apple Music / TIDAL list (kind %d)\n", linkText.c_str(), static_cast<int>(link.kind));
            ++g_failures;
            return out;
        }
        const svc::List l = svc::read(*ref, svc::liveFetch());
        out.readS = secondsSince(t0);
        out.songs = l.songs.size();
        int withIsrc = 0;
        for (const auto& s : l.songs) withIsrc += !s.isrc.empty();
        std::printf("  %s '%s' by %s: %zu songs (service says %d, %d skipped%s), %d with ISRC, read in %.1f s\n",
                    toUtf8(svc::serviceName(l.service)).c_str(), l.title.c_str(), l.owner.c_str(), l.songs.size(), l.expected,
                    l.skipped, l.partial ? ", PARTIAL" : "", withIsrc, out.readS);
        if (catalogName.empty() || l.songs.empty()) return out;
        const auto cat = catalogName == "mb" ? catmatch::Catalog::MusicBrainz
                         : catalogName == "spotify" ? catmatch::Catalog::Spotify
                                                    : catmatch::Catalog::YouTubeMusic;
        catmatch::Counters c;
        const auto t1 = std::chrono::steady_clock::now();
        bool interrupted = false;
        const auto results = catmatch::matchAll(l.songs, cat, api, c, {}, &interrupted);
        out.matchS = secondsSince(t1);
        out.found = c.found;
        out.byIsrc = c.byIsrc;
        std::printf("    %ls: %d / %zu matched (%.0f %%; %d by ISRC) in %.1f s%s\n", catmatch::catalogName(cat), out.found,
                    l.songs.size(), 100.0 * out.found / static_cast<double>(l.songs.size()), out.byIsrc, out.matchS,
                    interrupted ? " (interrupted)" : "");
        int shown = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            if (!quiet && i < 5 && r.found)
                std::printf("      %zu. %s - %s  ->  %s (%s - %s)\n", i + 1, l.songs[i].artistLine().c_str(), l.songs[i].title.c_str(),
                            r.track.id.c_str(), r.track.artistLine().c_str(), r.track.name.c_str());
            if (!r.found && shown < (quiet ? 0 : 8)) {
                ++shown;
                std::printf("      not found: %s - %s (%d s)\n", l.songs[i].artistLine().c_str(), l.songs[i].title.c_str(),
                            l.songs[i].durationMs / 1000);
            }
        }
    } catch (const svc::Error& e) {
        std::printf("  %s: error %d: %s\n", linkText.c_str(), static_cast<int>(e.code), e.what());
        ++g_failures;
    } catch (const std::exception& e) {
        std::printf("  %s: %s\n", linkText.c_str(), e.what());
        ++g_failures;
    }
    return out;
}

// Public lists used for the live report: two per service, one of them long.
static const char* kLiveSet[] = {
    "https://www.deezer.com/playlist/3155776842",                                         // Top Worldwide (100)
    "https://www.deezer.com/playlist/526783865",                                          // 500 Greatest Songs (500)
    "https://music.apple.com/us/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb",  // Today's Hits (50)
    "https://music.apple.com/us/playlist/x/pl.2b7ed7ebbc6e494caabebc04f2ac71dd",          // Berlioz: The Complete Works (424)
    "https://tidal.com/browse/playlist/4fd2cace-5255-409b-8748-b3661380bc7a",             // Test Trax (132)
};

static void live(int argc, char** argv) {
    Settings::get().load();
    YoutubeExplode::YoutubeClientOptions o;
    o.language = "en";
    o.region = "US";
    YoutubeExplode::YoutubeClient client(o);
    ytm::setClient(client);
    const bool set = std::strcmp(argv[1], "live-set") == 0;
    std::vector<std::string> linksToRead;
    std::string cat = "ytm";
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "ytm" || a == "mb" || a == "spotify" || a == "none") cat = a == "none" ? "" : a;
        else if (a == "--quiet") quiet = true;
        else linksToRead.push_back(a);
    }
    if (set)
        for (const char* l : kLiveSet) linksToRead.push_back(l);
    spotify::Api api;
    if (cat == "spotify") {
        const std::string token = anonymousSpotifyToken();
        std::printf("anonymous Spotify web-player token: %s\n", token.empty() ? "FAILED" : "ok");
        if (token.empty()) {
            ++g_failures;
            return;
        }
        api.setCredentials(token, "");
    }
    std::printf("live (%s)\n", cat.empty() ? "read only" : cat.c_str());
    for (const auto& l : linksToRead) liveOne(l, cat, &api, quiet);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    st::log::init();
    if (argc > 1 && (std::strcmp(argv[1], "live") == 0 || std::strcmp(argv[1], "live-set") == 0)) {
        live(argc, argv);
    } else {
        testLinks();
        testParsers();
        testReading();
        testMatching();
    }
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
