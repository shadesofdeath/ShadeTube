#pragma once
// Playlists, albums and songs of Deezer, Apple Music and TIDAL, read without an account (docs/DEVELOPMENT.md, "Import
// from Deezer, Apple Music and TIDAL"). Only the reading: matching the songs to the active catalog is
// app/CatalogMatch, the dialogs and the local playlist app/PlaylistTransfer.
//
//   Deezer       api.deezer.com, the public API (no key): /playlist/<id> (name, description, cover, count) + its
//                /tracks pages (index + limit, `next`; ISRC per song), /album/<id> + /album/<id>/tracks, /track/<id>.
//                Short links (link.deezer.com/s/..., deezer.page.link/...) redirect to a deezer.com page whose canonical
//                link names the list.
//   Apple Music  amp-api.music.apple.com, the web player's catalog API, with the developer token its web bundle
//                carries (a JWT, kid "WebPlayKid", ~10 weeks valid): read from the bundle the music.apple.com page
//                names, kept until it expires. /v1/catalog/<storefront>/playlists/<id> + /tracks pages (300 a page,
//                `next`), /albums/<id> (+ /tracks when longer), /songs/<id>; ISRC per song. When no token can be had (or
//                amp-api refuses it), the page's own data (`serialized-server-data`: the first 300 songs, no ISRC).
//   TIDAL        openapi.tidal.com/v2, TIDAL's JSON:API, with a client-credentials token of the public client the
//                signed-out tidal.com web app uses (its id and key are in its bundle; read again from there when the
//                known pair stops working). /playlists/<uuid> + /relationships/items pages (20 a page, cursor),
//                /albums/<id> + its items, /tracks?filter[id]=; ISRC per song.
//
// Network through a Fetch function (the app's shared transport; tests replay recorded answers). Blocking: worker
// threads. Errors are thrown as svc::Error with a kind the dialogs explain.
//
// Standalone (core + catalog + nlohmann/json): tests/services compiles it.
#include "app/Links.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::svc {

using CT = YoutubeExplode::CancellationToken;
using YoutubeExplode::Http::HttpRequest;
using YoutubeExplode::Http::HttpResponse;

enum class Kind { Playlist, Album, Track };

// What a link names (links::Kind::Service* -> ref()).
struct Ref {
    links::Service service = links::Service::None;
    Kind kind = Kind::Playlist;
    std::string id;          // Deezer digits, Apple "<storefront>/<id>", TIDAL uuid / digits
    explicit operator bool() const { return !id.empty(); }
};
std::optional<Ref> ref(const links::Link& link);   // nullopt for other links (and short links: resolveShortLink)
std::wstring serviceName(links::Service s);         // L"Deezer", L"Apple Music", L"TIDAL"

struct Song {
    std::string title;                 // with its version ("Song (Remastered 2011)") as the service shows it
    std::vector<std::string> artists;  // main artist first
    std::string album;
    int durationMs = 0;
    std::string isrc;                  // upper case, "" = unknown
    std::string serviceId;             // the song's id on the service
    std::string coverUrl;              // the album's cover (~600 px), "" = none
    int trackNumber = 0;
    int64_t addedAt = 0;               // unix seconds, 0 = unknown

    std::string artistLine() const;    // "A, B"
};

struct List {
    links::Service service = links::Service::None;
    Kind kind = Kind::Playlist;
    std::string id;
    std::string title;
    std::string description;           // plain text (HTML removed)
    std::string owner;                 // the playlist's curator / creator, an album's artist
    std::string coverUrl;              // ~600 px, "" = none
    std::string upc;                   // an album's barcode, "" = unknown
    std::string releaseDate;           // an album's ("2001-03-12"), "" = unknown
    int expected = 0;                  // songs the service says the list has (0 = unknown)
    std::vector<Song> songs;
    int skipped = 0;                   // rows that are no song (videos) or that the service no longer has
    bool partial = false;              // the service gave only the first part (Apple's page without the API)
};

struct Error : std::runtime_error {
    enum Code { NotFound, Private, Unavailable, Blocked, Network, Format };
    Code code;
    Error(Code c, const std::string& what) : std::runtime_error(what), code(c) {}
};

// One HTTP request (the shared transport follows redirects and decompresses). Throws on transport failures.
using Fetch = std::function<HttpResponse(const HttpRequest&, const CT&)>;
Fetch liveFetch();   // http::send

// Called on the reading worker as songs arrive: (songs read, songs expected or 0).
using Progress = std::function<void(int read, int expected)>;

// The whole list (playlists: every page, at most kMaxSongs songs).
inline constexpr int kMaxSongs = 5000;
List read(const Ref& ref, const Fetch& fetch, const Progress& progress = {}, const CT& ct = {});

// A Deezer short link -> the link it stands for (nullopt: expired / not a list, album or song).
std::optional<Ref> resolveShortLink(const std::string& url, const Fetch& fetch, const CT& ct = {});

// Tests: drops the kept Apple token and TIDAL token (and goes back to the built-in TIDAL client).
void forgetTokens();

// ---- Parsers (exposed for tests; tolerant: missing fields keep defaults, nothing throws on odd shapes) ----------------
namespace deezer {
// /playlist/<id> or /album/<id> (header fields; an album's embedded tracks are not used: they have no ISRC).
List parseHeader(std::string_view json, Kind kind);
// A /tracks page: appends its songs; returns the next page's URL ("" = last). `album`: the album's tracks (no album
// object per row: the header's name and cover are used). `total` gets the page's total when it has one.
std::string parseTracks(std::string_view json, List& into, bool album, int* total = nullptr);
Song parseTrack(std::string_view json);   // /track/<id>
// {"error":{...}} -> its Error (code 800 "no data" = not found or private, 4 = quota); nullopt when not an error.
std::optional<Error> parseError(std::string_view json);
} // namespace deezer

namespace apple {
// The music.apple.com page: the web bundle's path ("/assets/index~….js"), "" = none.
std::string bundlePath(std::string_view html);
// The developer token in a web bundle (the JWT with kid "WebPlayKid"), "" = none; `expires` gets its exp (unix s).
std::string tokenFromBundle(std::string_view js, int64_t* expires = nullptr);
// amp-api: a playlist / album resource (header + its first tracks; returns the next tracks page's path, "").
std::string parseResource(std::string_view json, List& into);
std::string parseTracks(std::string_view json, List& into);   // a /tracks page; returns `next` ("" = last)
Song parseSong(std::string_view json);                        // /songs/<id>
// The page's own data when there is no API: header, the songs it lists (up to 300, no ISRC) and its song count
// (schema.org numTracks; partial when it says more than were listed). False when the page has none.
bool parsePage(std::string_view html, List& into);
} // namespace apple

namespace tidal {
// The client the signed-out web app uses (id + key) in its index bundle; nullopt when not found.
struct Client {
    std::string id, secret;
};
std::optional<Client> clientFromBundle(std::string_view js);
std::string bundlePath(std::string_view html);   // "/assets/index-….js", "" = none
// A /playlists/<uuid> or /albums/<id> resource (header: name, description, count, cover, owner / artists, barcode).
void parseHeader(std::string_view json, List& into);
// A /relationships/items page (with the included tracks, artists and albums): appends its songs in the page's order;
// returns the next page's path ("" = last). `album`: the list is an album (album name / cover from its header).
std::string parseItems(std::string_view json, List& into, bool album);
// /tracks?filter[id]=...&include=artists,albums: the first song, nullopt when none.
std::optional<Song> parseTrackList(std::string_view json);
int durationMs(std::string_view iso8601);   // "PT3M28S" -> 208000, "PT1H2M" -> 3720000; 0 = none
} // namespace tidal

// Text helpers (tests).
std::string stripHtml(std::string_view html);   // tags removed, entities decoded, whitespace collapsed
// A canonical deezer.com link in a page (<link rel="canonical"> / og:url), "" = none.
std::string canonicalLink(std::string_view html);

} // namespace st::app::svc
