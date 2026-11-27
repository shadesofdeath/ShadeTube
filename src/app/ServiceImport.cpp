// Reading Deezer / Apple Music / TIDAL lists (see ServiceImport.h).
#include "app/ServiceImport.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace st::app::svc {

namespace {

using json = nlohmann::json;
using links::Service;
using OperationCanceled = YoutubeExplode::Exceptions::OperationCanceledException;

constexpr const char* kTag = "services";
constexpr const char* kDeezer = "https://api.deezer.com/";
constexpr const char* kApplePage = "https://music.apple.com/";
constexpr const char* kAppleApi = "https://amp-api.music.apple.com";
constexpr const char* kTidalApi = "https://openapi.tidal.com/v2/";
constexpr const char* kTidalAuth = "https://auth.tidal.com/v1/oauth2/token";
// The client the signed-out tidal.com web app uses (its bundle, 2026-11-27; read again from the bundle when refused).
constexpr const char* kTidalClientId = "txNoH4kkV41MfH25";
constexpr const char* kTidalClientKey = "dQjy0MinCEvxi1O4UmxvxWnDjt4cgHBPw8ll6nYBk98=";
constexpr const char* kTidalCountry = "US";
constexpr int kDeezerPage = 500;   // rows per /tracks request (the API answers up to 1000 at once)
constexpr int kApplePageSize = 300;    // amp-api's maximum for a tracks page

json parseJson(std::string_view text) {
    return json::parse(text.begin(), text.end(), nullptr, false);
}

const json& at(const json& j, const char* key) {
    static const json kNull;
    if (!j.is_object()) return kNull;
    const auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}
std::string str(const json& j, const char* key) {
    const json& v = at(j, key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    return {};
}
int64_t num(const json& j, const char* key) {
    const json& v = at(j, key);
    if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int64_t>();
    if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}
const json& arr(const json& j, const char* key) {
    static const json kEmpty = json::array();
    const json& v = at(j, key);
    return v.is_array() ? v : kEmpty;
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

// ---- Network ----------------------------------------------------------------------------------------------------------

// One request through `fetch` with retries: a transport failure (TIDAL's hosts reset connections now and then), 429 and
// 5xx are tried twice more after a short pause (Retry-After when given, at most 10 s). Returns any other status.
struct Net {
    const Fetch& fetch;
    const CT& ct;

    void pause(std::chrono::milliseconds d) const {
        const auto until = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < until) {
            ct.throwIfCancellationRequested();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    HttpResponse send(const HttpRequest& r, int attempts = 3) const {
        for (int a = 1;; ++a) {
            ct.throwIfCancellationRequested();
            HttpResponse resp;
            try {
                resp = fetch(r, ct);
            } catch (const OperationCanceled&) {
                throw;
            } catch (const std::exception& e) {
                if (ct.isCancellationRequested()) ct.throwIfCancellationRequested();
                if (a >= attempts) throw Error(Error::Network, e.what());
                ST_LOG_INFO(kTag, "{}: {} (attempt {})", r.url.substr(0, r.url.find('?')), e.what(), a);
                pause(std::chrono::milliseconds(700 * a));
                continue;
            }
            if ((resp.statusCode == 429 || resp.statusCode >= 500) && a < attempts) {
                int wait = 1500 * a;
                if (const auto ra = resp.header("Retry-After")) {
                    try {
                        wait = std::clamp(std::stoi(*ra), 1, 10) * 1000;
                    } catch (...) {
                    }
                }
                ST_LOG_INFO(kTag, "{}: HTTP {} (attempt {})", r.url.substr(0, r.url.find('?')), resp.statusCode, a);
                pause(std::chrono::milliseconds(wait));
                continue;
            }
            return resp;
        }
    }

    HttpResponse get(std::string url, YoutubeExplode::Http::Headers headers = {}) const {
        HttpRequest r;
        r.url = std::move(url);
        r.headers = std::move(headers);
        return send(r);
    }
};

[[noreturn]] void failStatus(const HttpResponse& r, const std::string& what) {
    if (r.statusCode == 404) throw Error(Error::NotFound, what + ": not found");
    if (r.statusCode == 401 || r.statusCode == 403) throw Error(Error::Private, what + ": HTTP " + std::to_string(r.statusCode));
    if (r.statusCode == 429) throw Error(Error::Blocked, what + ": rate limited");
    throw Error(Error::Network, what + ": HTTP " + std::to_string(r.statusCode));
}

// Base64url without padding -> bytes ("" on bad input).
std::string base64UrlDecode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return {};
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// The artwork URL template of amp-api / the page ("…/{w}x{h}bb.{f}") at 600 px.
std::string appleArtwork(const json& artwork) {
    std::string url = str(artwork, "url");
    if (url.empty()) {
        const json& dict = at(artwork, "dictionary");
        url = str(dict, "url");
    }
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"{w}", "600"}, {"{h}", "600"}, {"{f}", "jpg"}, {"{c}", "bb"}}) {
        for (size_t p = url.find(from); p != std::string::npos; p = url.find(from, p)) url.replace(p, std::strlen(from), to);
    }
    return url;
}

} // namespace

// ---- Small pieces -----------------------------------------------------------------------------------------------------

std::string Song::artistLine() const {
    std::string out;
    for (const auto& a : artists) {
        if (a.empty()) continue;
        if (!out.empty()) out += ", ";
        out += a;
    }
    return out;
}

std::optional<Ref> ref(const links::Link& link) {
    if (link.service != Service::Deezer && link.service != Service::AppleMusic && link.service != Service::Tidal) return std::nullopt;
    Ref r;
    r.service = link.service;
    r.id = link.id;
    switch (link.kind) {
    case links::Kind::ServicePlaylist: r.kind = Kind::Playlist; break;
    case links::Kind::ServiceAlbum: r.kind = Kind::Album; break;
    case links::Kind::ServiceTrack: r.kind = Kind::Track; break;
    default: return std::nullopt;
    }
    if (r.id.empty()) return std::nullopt;
    return r;
}

std::wstring serviceName(Service s) {
    switch (s) {
    case Service::Deezer: return L"Deezer";
    case Service::AppleMusic: return L"Apple Music";
    case Service::Tidal: return L"TIDAL";
    default: return {};
    }
}

Fetch liveFetch() {
    return [](const HttpRequest& r, const CT& ct) { return http::send(r, ct); };
}

std::string stripHtml(std::string_view html) {
    std::string text;
    text.reserve(html.size());
    for (size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (c == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string_view::npos) break;
            std::string tag(html.substr(i + 1, end - i - 1));
            for (char& t : tag) t = static_cast<char>(std::tolower(static_cast<unsigned char>(t)));
            if (tag.rfind("br", 0) == 0 || tag == "/p" || tag == "p") text.push_back('\n');
            i = end;
            continue;
        }
        if (c == '&') {
            static const std::pair<std::string_view, std::string_view> kEntities[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
            bool done = false;
            for (const auto& [e, v] : kEntities) {
                if (html.substr(i, e.size()) == e) {
                    text += v;
                    i += e.size() - 1;
                    done = true;
                    break;
                }
            }
            if (!done && i + 2 < html.size() && html[i + 1] == '#') {
                const size_t semi = html.find(';', i);
                if (semi != std::string_view::npos && semi - i <= 10) {
                    const bool hex = html[i + 2] == 'x' || html[i + 2] == 'X';
                    unsigned long cp = 0;
                    try {
                        cp = std::stoul(std::string(html.substr(i + (hex ? 3 : 2), semi - i - (hex ? 3 : 2))), nullptr, hex ? 16 : 10);
                    } catch (...) {
                        cp = 0;
                    }
                    if (cp > 0 && cp < 0x110000) {   // UTF-8
                        if (cp < 0x80) text.push_back(static_cast<char>(cp));
                        else if (cp < 0x800) {
                            text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        i = semi;
                        done = true;
                    }
                }
            }
            if (done) continue;
        }
        text.push_back(c);
    }
    // Collapse spaces (keep single line breaks), trim.
    std::string out;
    for (char c : text) {
        if (c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')) continue;
        if (c == '\n') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (out.empty() || (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')) continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

std::string canonicalLink(std::string_view html) {
    for (const std::string_view marker : {std::string_view("rel=\"canonical\" href=\""), std::string_view("property=\"og:url\" content=\"")}) {
        const size_t p = html.find(marker);
        if (p == std::string_view::npos) continue;
        const size_t b = p + marker.size(), e = html.find('"', b);
        if (e != std::string_view::npos && e - b < 512) return std::string(html.substr(b, e - b));
    }
    return {};
}

// ---- Deezer -----------------------------------------------------------------------------------------------------------

namespace deezer {

std::optional<Error> parseError(std::string_view text) {
    const json j = parseJson(text);
    const json& e = at(j, "error");
    if (!e.is_object()) return std::nullopt;
    const int64_t code = num(e, "code");
    const std::string type = str(e, "type"), message = str(e, "message");
    const std::string what = "Deezer: " + type + " " + message + " (" + std::to_string(code) + ")";
    if (code == 4 || code == 700) return Error(Error::Blocked, what);          // quota / service busy
    if (code == 800) return Error(Error::NotFound, what);                      // "no data": missing or private
    if (type == "OAuthException" || code == 200 || code == 300) return Error(Error::Private, what);
    if (code == 801 || code == 802) return Error(Error::Unavailable, what);
    return Error(Error::Format, what);
}

List parseHeader(std::string_view text, Kind kind) {
    List l;
    l.service = Service::Deezer;
    l.kind = kind;
    const json j = parseJson(text);
    if (!j.is_object()) return l;
    l.id = str(j, "id");
    l.title = str(j, "title");
    l.description = stripHtml(str(j, "description"));
    l.expected = static_cast<int>(num(j, "nb_tracks"));
    if (kind == Kind::Album) {
        l.owner = str(at(j, "artist"), "name");
        l.coverUrl = str(j, "cover_xl");
        if (l.coverUrl.empty()) l.coverUrl = str(j, "cover_big");
        l.upc = str(j, "upc");
        l.releaseDate = str(j, "release_date");
    } else {
        l.owner = str(at(j, "creator"), "name");
        l.coverUrl = str(j, "picture_xl");
        if (l.coverUrl.empty()) l.coverUrl = str(j, "picture_big");
    }
    return l;
}

namespace {
Song songFrom(const json& t) {
    Song s;
    s.serviceId = str(t, "id");
    s.title = str(t, "title");
    s.isrc = upper(str(t, "isrc"));
    s.durationMs = static_cast<int>(num(t, "duration") * 1000);
    for (const auto& c : arr(t, "contributors"))
        if (std::string n = str(c, "name"); !n.empty() && std::find(s.artists.begin(), s.artists.end(), n) == s.artists.end())
            s.artists.push_back(std::move(n));
    if (s.artists.empty())
        if (std::string n = str(at(t, "artist"), "name"); !n.empty()) s.artists.push_back(std::move(n));
    const json& album = at(t, "album");
    s.album = str(album, "title");
    s.coverUrl = str(album, "cover_big");
    if (s.coverUrl.empty()) s.coverUrl = str(album, "cover_xl");
    s.trackNumber = static_cast<int>(num(t, "track_position"));
    s.addedAt = num(t, "time_add");
    return s;
}
} // namespace

std::string parseTracks(std::string_view text, List& into, bool album, int* total) {
    const json j = parseJson(text);
    if (!j.is_object()) return {};
    for (const auto& t : arr(j, "data")) {
        if (str(t, "type") != "track" && !str(t, "type").empty()) {
            ++into.skipped;
            continue;
        }
        Song s = songFrom(t);
        if (s.title.empty()) {
            ++into.skipped;
            continue;
        }
        if (album) {
            if (s.album.empty()) s.album = into.title;
            if (s.coverUrl.empty()) s.coverUrl = into.coverUrl;
        }
        into.songs.push_back(std::move(s));
    }
    if (total && at(j, "total").is_number()) *total = static_cast<int>(num(j, "total"));
    return str(j, "next");
}

Song parseTrack(std::string_view text) {
    const json j = parseJson(text);
    return j.is_object() ? songFrom(j) : Song{};
}

} // namespace deezer

namespace {

HttpResponse deezerGet(const Net& net, const std::string& url) {
    for (int attempt = 1;; ++attempt) {
        HttpResponse r = net.get(url);
        if (!r.isSuccessStatusCode()) failStatus(r, "Deezer");
        if (auto e = deezer::parseError(r.body)) {
            if (e->code == Error::Blocked && attempt < 4) {   // 50 requests per 5 s: wait for the window
                net.pause(std::chrono::seconds(2 * attempt));
                continue;
            }
            throw *e;
        }
        return r;
    }
}

List readDeezer(const Ref& ref, const Net& net, const Progress& progress) {
    const std::string id = ref.id;
    if (ref.kind == Kind::Track) {
        List l;
        l.service = Service::Deezer;
        l.kind = Kind::Track;
        l.id = id;
        Song s = deezer::parseTrack(deezerGet(net, std::string(kDeezer) + "track/" + id).body);
        if (s.title.empty()) throw Error(Error::Format, "Deezer: track " + id + " without a title");
        l.title = s.title;
        l.owner = s.artistLine();
        l.coverUrl = s.coverUrl;
        l.expected = 1;
        l.songs.push_back(std::move(s));
        return l;
    }
    const bool album = ref.kind == Kind::Album;
    const std::string base = std::string(kDeezer) + (album ? "album/" : "playlist/") + id;
    List l = deezer::parseHeader(deezerGet(net, base).body, ref.kind);
    if (l.id.empty()) l.id = id;
    std::string url = base + "/tracks?index=0&limit=" + std::to_string(kDeezerPage);
    for (int page = 0; !url.empty() && page < 100 && static_cast<int>(l.songs.size()) < kMaxSongs; ++page) {
        int total = 0;
        const size_t before = l.songs.size();
        const int skippedBefore = l.skipped;
        const std::string current = std::exchange(url, deezer::parseTracks(deezerGet(net, url).body, l, album, &total));
        if (total > 0) l.expected = total;
        if (progress) progress(static_cast<int>(l.songs.size()), l.expected);
        // An empty page or a link back to the same page: stop rather than loop.
        if ((l.songs.size() == before && l.skipped == skippedBefore) || url == current) break;
    }
    if (static_cast<int>(l.songs.size()) > kMaxSongs) l.songs.resize(kMaxSongs);
    return l;
}

} // namespace

std::optional<Ref> resolveShortLink(const std::string& url, const Fetch& fetch, const CT& ct) {
    const Net net{fetch, ct};
    const HttpResponse r = net.get(url, {{"Accept", "text/html"}});
    if (contains(r.body, "deezer-links-404")) return std::nullopt;
    std::string target = canonicalLink(r.body);
    if (target.empty()) {   // an interstitial page: the first deezer.com list / album / song link in it
        for (const char* kind : {"/playlist/", "/album/", "/track/"}) {
            for (size_t p = r.body.find(kind); p != std::string::npos; p = r.body.find(kind, p + 1)) {
                const size_t host = r.body.rfind("deezer.com", p);
                if (host == std::string::npos || p - host > 16) continue;
                size_t e = p + std::strlen(kind);
                while (e < r.body.size() && r.body[e] >= '0' && r.body[e] <= '9') ++e;
                if (e == p + std::strlen(kind)) continue;
                target = "https://www." + r.body.substr(host, e - host);
                break;
            }
            if (!target.empty()) break;
        }
    }
    if (target.empty()) return std::nullopt;
    const auto link = links::parse(std::string_view(target));
    if (link.service != Service::Deezer) return std::nullopt;
    return ref(link);
}

// ---- Apple Music ------------------------------------------------------------------------------------------------------

namespace apple {

std::string bundlePath(std::string_view html) {
    for (const std::string_view prefix : {std::string_view("/assets/index~"), std::string_view("/assets/index-")}) {
        for (size_t p = html.find(prefix); p != std::string_view::npos; p = html.find(prefix, p + 1)) {
            size_t e = p + prefix.size();
            while (e < html.size() && (std::isalnum(static_cast<unsigned char>(html[e])) || html[e] == '_' || html[e] == '-' || html[e] == '~')) ++e;
            if (html.substr(e, 3) == ".js") return std::string(html.substr(p, e + 3 - p));
        }
    }
    return {};
}

std::string tokenFromBundle(std::string_view js, int64_t* expires) {
    for (size_t p = js.find("eyJ"); p != std::string_view::npos; p = js.find("eyJ", p + 3)) {
        size_t e = p;
        int dots = 0;
        while (e < js.size() && (std::isalnum(static_cast<unsigned char>(js[e])) || js[e] == '-' || js[e] == '_' || js[e] == '.')) {
            if (js[e] == '.') ++dots;
            ++e;
        }
        if (dots != 2 || e - p < 100) continue;
        const std::string_view token = js.substr(p, e - p);
        const size_t d1 = token.find('.'), d2 = token.find('.', d1 + 1);
        const json header = parseJson(base64UrlDecode(token.substr(0, d1)));
        if (str(header, "kid") != "WebPlayKid") continue;
        if (expires) *expires = num(parseJson(base64UrlDecode(token.substr(d1 + 1, d2 - d1 - 1))), "exp");
        return std::string(token);
    }
    return {};
}

namespace {
// An amp-api song (type "songs"); nullopt for music videos and rows without a name.
std::optional<Song> songFrom(const json& t) {
    if (str(t, "type") != "songs" && !str(t, "type").empty()) return std::nullopt;
    const json& a = at(t, "attributes");
    Song s;
    s.serviceId = str(t, "id");
    s.title = str(a, "name");
    if (s.title.empty()) return std::nullopt;
    if (std::string artist = str(a, "artistName"); !artist.empty()) s.artists.push_back(std::move(artist));
    s.album = str(a, "albumName");
    s.durationMs = static_cast<int>(num(a, "durationInMillis"));
    s.isrc = upper(str(a, "isrc"));
    s.trackNumber = static_cast<int>(num(a, "trackNumber"));
    s.coverUrl = appleArtwork(at(a, "artwork"));
    return s;
}

std::string appendTracks(const json& tracks, List& into) {
    for (const auto& t : arr(tracks, "data")) {
        if (auto s = songFrom(t)) into.songs.push_back(std::move(*s));
        else ++into.skipped;
    }
    return str(tracks, "next");
}
} // namespace

std::string parseResource(std::string_view text, List& into) {
    const json j = parseJson(text);
    const json& data = arr(j, "data");
    if (data.empty()) return {};
    const json& res = data[0];
    const json& a = at(res, "attributes");
    into.service = Service::AppleMusic;
    if (into.id.empty()) into.id = str(res, "id");
    into.title = str(a, "name");
    const json& desc = at(a, "description");
    std::string d = str(desc, "standard");
    if (d.empty()) d = str(at(a, "editorialNotes"), "standard");
    if (d.empty()) d = str(desc, "short");
    into.description = stripHtml(d);
    into.coverUrl = appleArtwork(at(a, "artwork"));
    if (str(res, "type") == "albums") {
        into.kind = Kind::Album;
        into.owner = str(a, "artistName");
        into.upc = str(a, "upc");
        into.releaseDate = str(a, "releaseDate");
        into.expected = static_cast<int>(num(a, "trackCount"));
    } else {
        into.owner = str(a, "curatorName");
    }
    const json& tracks = at(at(res, "relationships"), "tracks");
    const std::string next = appendTracks(tracks, into);
    if (into.kind == Kind::Album)
        for (auto& s : into.songs)
            if (s.album.empty()) s.album = into.title;
    return next;
}

std::string parseTracks(std::string_view text, List& into) { return appendTracks(parseJson(text), into); }

Song parseSong(std::string_view text) {
    const json j = parseJson(text);
    const json& data = arr(j, "data");
    if (data.empty()) return {};
    return songFrom(data[0]).value_or(Song{});
}

bool parsePage(std::string_view html, List& into) {
    constexpr std::string_view open = "id=\"serialized-server-data\">";
    const size_t p = html.find(open);
    if (p == std::string_view::npos) return false;
    const size_t b = p + open.size(), e = html.find("</script>", b);
    if (e == std::string_view::npos) return false;
    const json j = parseJson(html.substr(b, e - b));
    const json& pages = arr(j, "data");
    if (pages.empty()) return false;
    const json& sections = arr(at(pages[0], "data"), "sections");
    if (sections.empty()) return false;
    into.service = Service::AppleMusic;
    bool any = false;
    for (const auto& sec : sections) {
        const std::string kind = str(sec, "itemKind");
        const json& items = arr(sec, "items");
        if (kind == "containerDetailHeaderLockup" && !items.empty()) {
            const json& h = items[0];
            into.title = str(h, "title");
            const json& subs = arr(h, "subtitleLinks");
            if (!subs.empty()) into.owner = str(subs[0], "title");
            into.description = stripHtml(str(at(h, "modalPresentationDescriptor"), "paragraphText"));
            into.coverUrl = appleArtwork(at(h, "artwork"));
            any = true;
        } else if (kind == "trackLockup") {
            for (const auto& t : items) {
                Song s;
                s.title = str(t, "title");
                if (s.title.empty()) {
                    ++into.skipped;
                    continue;
                }
                s.serviceId = str(at(at(t, "contentDescriptor"), "identifiers"), "storeAdamID");
                std::string artist = str(t, "artistName");
                if (artist.empty()) {
                    for (const auto& l : arr(t, "subtitleLinks")) {
                        if (!artist.empty()) artist += " & ";
                        artist += str(l, "title");
                    }
                }
                if (!artist.empty()) s.artists.push_back(std::move(artist));
                const json& albums = arr(t, "tertiaryLinks");
                s.album = albums.empty() ? into.title : str(albums[0], "title");
                s.durationMs = static_cast<int>(num(t, "duration"));
                s.trackNumber = static_cast<int>(num(t, "trackNumber"));
                s.coverUrl = appleArtwork(at(t, "artwork"));
                if (s.coverUrl.empty()) s.coverUrl = into.coverUrl;
                into.songs.push_back(std::move(s));
            }
            any = true;
        }
    }
    // The page lists at most 300 songs and counts only those: a full page may be the first part of a longer list.
    if (into.kind == Kind::Playlist && into.songs.size() >= 300) into.partial = true;
    return any;
}

} // namespace apple

namespace {

struct AppleToken {
    std::mutex m;
    std::string token;
    int64_t expires = 0;
};
AppleToken& appleTokenStore() {
    static AppleToken t;
    return t;
}

// The web player's developer token: the one kept, else read from the bundle `pageHtml` names (the page is fetched when
// not given). "" when none could be read.
std::string appleToken(const Net& net, const std::string& pageUrl, std::string* pageHtml, bool refresh) {
    auto& store = appleTokenStore();
    {
        std::lock_guard lock(store.m);
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!refresh && !store.token.empty() && store.expires > now + 3600) return store.token;
    }
    std::string html;
    if (pageHtml && !pageHtml->empty()) html = *pageHtml;
    else {
        const HttpResponse page = net.get(pageUrl, {{"Accept", "text/html"}});
        html = page.body;
        if (pageHtml) *pageHtml = page.isSuccessStatusCode() ? page.body : std::string{};
    }
    std::string path = apple::bundlePath(html);
    if (path.empty()) {   // a page without it (404): the home page names the same bundle
        const HttpResponse home = net.get(std::string(kApplePage) + "us/new", {{"Accept", "text/html"}});
        path = apple::bundlePath(home.body);
    }
    if (path.empty()) return {};
    const HttpResponse js = net.get("https://music.apple.com" + path);
    if (!js.isSuccessStatusCode()) return {};
    int64_t expires = 0;
    std::string token = apple::tokenFromBundle(js.body, &expires);
    if (token.empty()) return {};
    ST_LOG_INFO(kTag, "Apple Music web token read (valid until {})", expires);
    std::lock_guard lock(store.m);
    store.token = token;
    store.expires = expires > 0 ? expires : static_cast<int64_t>(std::time(nullptr)) + 86400;
    return token;
}

List readApple(const Ref& ref, const Net& net, const Progress& progress) {
    const size_t slash = ref.id.find('/');
    const std::string store = slash == std::string::npos ? "us" : ref.id.substr(0, slash);
    const std::string id = slash == std::string::npos ? ref.id : ref.id.substr(slash + 1);
    const char* type = ref.kind == Kind::Playlist ? "playlist" : ref.kind == Kind::Album ? "album" : "song";
    const std::string pageUrl = std::string(kApplePage) + store + "/" + type + "/" + id;
    std::string page;
    List l;
    l.service = Service::AppleMusic;
    l.kind = ref.kind;
    l.id = id;

    std::string token = appleToken(net, pageUrl, &page, false);
    bool apiFailed = token.empty();
    if (!token.empty()) {
        const std::string resource = std::string(kAppleApi) + "/v1/catalog/" + store + "/" + type + "s/" + id + "?l=en-US" +
                                     (ref.kind == Kind::Track ? "" : "&include=tracks&limit%5Btracks%5D=" + std::to_string(kApplePageSize));
        auto api = [&](const std::string& url) {
            HttpResponse r = net.get(url, {{"Authorization", "Bearer " + token}, {"Origin", "https://music.apple.com"}});
            if (r.statusCode == 401 || r.statusCode == 403) {   // a token Apple replaced: read the bundle again, once
                token = appleToken(net, pageUrl, &page, true);
                if (!token.empty()) r = net.get(url, {{"Authorization", "Bearer " + token}, {"Origin", "https://music.apple.com"}});
            }
            return r;
        };
        const HttpResponse r = api(resource);
        if (r.isSuccessStatusCode()) {
            if (ref.kind == Kind::Track) {
                Song s = apple::parseSong(r.body);
                if (s.title.empty()) throw Error(Error::Format, "Apple Music: song " + id + " without a name");
                l.title = s.title;
                l.owner = s.artistLine();
                l.coverUrl = s.coverUrl;
                l.expected = 1;
                l.songs.push_back(std::move(s));
                return l;
            }
            std::string next = apple::parseResource(r.body, l);
            l.kind = ref.kind;
            l.id = id;
            if (progress) progress(static_cast<int>(l.songs.size()), l.expected);
            for (int n = 0; !next.empty() && n < 40 && static_cast<int>(l.songs.size()) < kMaxSongs; ++n) {
                const HttpResponse pr = api(std::string(kAppleApi) + next + (contains(next, "limit=") ? "" : "&limit=" + std::to_string(kApplePageSize)));
                if (!pr.isSuccessStatusCode()) failStatus(pr, "Apple Music");
                const std::string current = std::exchange(next, apple::parseTracks(pr.body, l));
                if (next == current) break;
                if (progress) progress(static_cast<int>(l.songs.size()), l.expected);
            }
            if (static_cast<int>(l.songs.size()) > kMaxSongs) l.songs.resize(kMaxSongs);
            if (l.expected <= 0) l.expected = static_cast<int>(l.songs.size()) + l.skipped;
            return l;
        }
        if (r.statusCode == 404) {
            // Not in this storefront's catalog (or a private list): the page says the same; nothing to fall back to.
            throw Error(Error::NotFound, "Apple Music: " + std::string(type) + " " + id + " not found in " + store);
        }
        ST_LOG_WARN(kTag, "amp-api refused ({}): reading the page instead", r.statusCode);
        apiFailed = true;
    }
    if (apiFailed) {
        if (page.empty()) {
            const HttpResponse pr = net.get(pageUrl, {{"Accept", "text/html"}});
            if (pr.statusCode == 404) throw Error(Error::NotFound, "Apple Music: " + std::string(type) + " " + id + " not found");
            if (!pr.isSuccessStatusCode()) failStatus(pr, "Apple Music page");
            page = pr.body;
        }
        if (!apple::parsePage(page, l) || l.songs.empty()) throw Error(Error::Blocked, "Apple Music: no token and no data in the page");
        if (ref.kind == Kind::Track) {   // a song page lists the song (first) among others
            l.songs.resize(1);
            l.title = l.songs[0].title;
        }
        l.expected = static_cast<int>(l.songs.size());
        if (progress) progress(static_cast<int>(l.songs.size()), l.expected);
    }
    return l;
}

} // namespace

// ---- TIDAL ------------------------------------------------------------------------------------------------------------

namespace tidal {

int durationMs(std::string_view s) {
    if (s.size() < 3 || s[0] != 'P') return 0;
    double total = 0, value = 0, frac = 0, scale = 0;
    bool inTime = false, any = false;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == 'T') {
            inTime = true;
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (scale > 0) {
                frac += (c - '0') * scale;
                scale /= 10;
            } else {
                value = value * 10 + (c - '0');
            }
            any = true;
            continue;
        }
        if (c == '.') {
            scale = 0.1;
            continue;
        }
        const double v = value + frac;
        if (c == 'H' && inTime) total += v * 3600;
        else if (c == 'M' && inTime) total += v * 60;
        else if (c == 'S' && inTime) total += v;
        else if (c == 'D') total += v * 86400;
        value = frac = scale = 0;
    }
    return any ? static_cast<int>(total * 1000 + 0.5) : 0;
}

std::string bundlePath(std::string_view html) {
    constexpr std::string_view prefix = "/assets/index-";
    for (size_t p = html.find(prefix); p != std::string_view::npos; p = html.find(prefix, p + 1)) {
        size_t e = p + prefix.size();
        while (e < html.size() && (std::isalnum(static_cast<unsigned char>(html[e])) || html[e] == '_' || html[e] == '-')) ++e;
        if (html.substr(e, 3) == ".js") return std::string(html.substr(p, e + 3 - p));
    }
    return {};
}

std::optional<Client> clientFromBundle(std::string_view js) {
    // [<id or variable>,{authType:`clientCredentials`,env:`PROD`,publicKey:`<key>`}]
    constexpr std::string_view marker = ",{authType:`clientCredentials`,env:`PROD`,publicKey:`";
    const size_t m = js.find(marker);
    if (m == std::string_view::npos) return std::nullopt;
    const size_t kb = m + marker.size(), ke = js.find('`', kb);
    const size_t open = js.rfind('[', m);
    if (ke == std::string_view::npos || open == std::string_view::npos || m - open > 64) return std::nullopt;
    Client c;
    c.secret = std::string(js.substr(kb, ke - kb));
    std::string_view name = js.substr(open + 1, m - open - 1);
    if (name.size() >= 2 && name.front() == '`' && name.back() == '`') {
        c.id = std::string(name.substr(1, name.size() - 2));
    } else {
        for (const char* sep : {",", " ", ";", "("}) {   // `,sbt=\`id\``, `var sbt=\`id\``
            const std::string decl = std::string(sep) + std::string(name) + "=`";
            if (const size_t d = js.find(decl); d != std::string_view::npos) {
                const size_t b = d + decl.size(), e = js.find('`', b);
                if (e != std::string_view::npos && e - b <= 64) c.id = std::string(js.substr(b, e - b));
                break;
            }
        }
    }
    if (c.id.empty() || c.secret.empty()) return std::nullopt;
    return c;
}

namespace {

using Included = std::unordered_map<std::string, const json*>;   // "<type>:<id>" -> resource

Included index(const json& doc) {
    Included out;
    for (const auto& r : arr(doc, "included")) out[str(r, "type") + ":" + str(r, "id")] = &r;
    return out;
}
const json* find(const Included& inc, const std::string& type, const std::string& id) {
    const auto it = inc.find(type + ":" + id);
    return it == inc.end() ? nullptr : it->second;
}
const json& relData(const json& res, const char* rel) { return arr(at(at(res, "relationships"), rel), "data"); }

std::string artworkUrl(const json* art) {
    if (!art) return {};
    std::string best, largest;
    int bestW = 1 << 30, largestW = 0;
    for (const auto& f : arr(at(*art, "attributes"), "files")) {
        const int w = static_cast<int>(num(at(f, "meta"), "width"));
        const std::string href = str(f, "href");
        if (href.empty()) continue;
        if (w >= 600 && w < bestW) {
            bestW = w;
            best = href;
        }
        if (w > largestW) {
            largestW = w;
            largest = href;
        }
    }
    return best.empty() ? largest : best;
}

std::optional<Song> songFrom(const json& t, const Included& inc) {
    const json& a = at(t, "attributes");
    Song s;
    s.serviceId = str(t, "id");
    s.title = str(a, "title");
    if (s.title.empty()) return std::nullopt;
    if (std::string v = str(a, "version"); !v.empty() && !contains(s.title, v)) s.title += " (" + v + ")";
    s.isrc = upper(str(a, "isrc"));
    s.durationMs = durationMs(str(a, "duration"));
    for (const auto& r : relData(t, "artists"))
        if (const json* artist = find(inc, "artists", str(r, "id")))
            if (std::string n = str(at(*artist, "attributes"), "name"); !n.empty()) s.artists.push_back(std::move(n));
    const json& albums = relData(t, "albums");
    if (!albums.empty())
        if (const json* album = find(inc, "albums", str(albums[0], "id"))) {
            s.album = str(at(*album, "attributes"), "title");
            const json& cover = relData(*album, "coverArt");
            if (!cover.empty()) s.coverUrl = artworkUrl(find(inc, "artworks", str(cover[0], "id")));
        }
    return s;
}

} // namespace

void parseHeader(std::string_view text, List& into) {
    const json doc = parseJson(text);
    const json& res = at(doc, "data");
    if (!res.is_object()) return;
    const Included inc = index(doc);
    const json& a = at(res, "attributes");
    into.service = Service::Tidal;
    if (into.id.empty()) into.id = str(res, "id");
    const bool album = str(res, "type") == "albums";
    if (album) into.kind = Kind::Album;
    into.title = str(a, album ? "title" : "name");
    into.description = stripHtml(str(a, "description"));
    into.expected = static_cast<int>(num(a, "numberOfItems"));
    const json& cover = relData(res, "coverArt");
    if (!cover.empty()) into.coverUrl = artworkUrl(find(inc, "artworks", str(cover[0], "id")));
    if (album) {
        into.upc = str(a, "barcodeId");
        into.releaseDate = str(a, "releaseDate");
        for (const auto& r : relData(res, "artists"))
            if (const json* artist = find(inc, "artists", str(r, "id"))) {
                if (!into.owner.empty()) into.owner += ", ";
                into.owner += str(at(*artist, "attributes"), "name");
            }
    } else {
        for (const auto& r : relData(res, "owners"))
            if (const json* user = find(inc, str(r, "type"), str(r, "id"))) {
                const json& ua = at(*user, "attributes");
                into.owner = str(ua, "name");
                if (into.owner.empty()) into.owner = str(ua, "username");
                break;
            }
    }
}

std::string parseItems(std::string_view text, List& into, bool album) {
    const json doc = parseJson(text);
    const Included inc = index(doc);
    for (const auto& row : arr(doc, "data")) {
        if (str(row, "type") != "tracks") {   // videos
            ++into.skipped;
            continue;
        }
        const json* t = find(inc, "tracks", str(row, "id"));
        auto s = t ? songFrom(*t, inc) : std::nullopt;
        if (!s) {   // no longer available on TIDAL
            ++into.skipped;
            continue;
        }
        const json& meta = at(row, "meta");
        s->trackNumber = static_cast<int>(num(meta, "trackNumber"));
        if (album) {
            if (s->album.empty()) s->album = into.title;
            if (s->coverUrl.empty()) s->coverUrl = into.coverUrl;
            if (s->artists.empty() && !into.owner.empty()) s->artists.push_back(into.owner);
        }
        into.songs.push_back(std::move(*s));
    }
    return str(at(doc, "links"), "next");
}

std::optional<Song> parseTrackList(std::string_view text) {
    const json doc = parseJson(text);
    const Included inc = index(doc);
    for (const auto& t : arr(doc, "data"))
        if (str(t, "type") == "tracks")
            if (auto s = songFrom(t, inc)) return s;
    return std::nullopt;
}

} // namespace tidal

namespace {

struct TidalAuth {
    std::mutex m;
    tidal::Client client{kTidalClientId, kTidalClientKey};
    std::string token;
    std::chrono::steady_clock::time_point expires{};
};
TidalAuth& tidalAuth() {
    static TidalAuth a;
    return a;
}

std::string requestTidalToken(const Net& net, const tidal::Client& c) {
    HttpRequest r;
    r.method = "POST";
    r.url = kTidalAuth;
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    r.body = http::formEncode({{"client_id", c.id}, {"client_secret", c.secret}, {"grant_type", "client_credentials"}});
    const HttpResponse resp = net.send(r);
    if (!resp.isSuccessStatusCode()) {
        ST_LOG_WARN(kTag, "TIDAL token refused: HTTP {}", resp.statusCode);
        return {};
    }
    return str(parseJson(resp.body), "access_token");
}

// A client-credentials token of the signed-out web app; when its client is refused, the one in tidal.com's bundle (read
// through `pageUrl`, any tidal.com page). Throws Error::Blocked when neither works.
std::string tidalToken(const Net& net, const std::string& pageUrl, bool refresh) {
    auto& a = tidalAuth();
    tidal::Client client;
    {
        std::lock_guard lock(a.m);
        if (!refresh && !a.token.empty() && std::chrono::steady_clock::now() < a.expires) return a.token;
        client = a.client;
    }
    std::string token = requestTidalToken(net, client);
    if (token.empty()) {
        const HttpResponse page = net.get(pageUrl, {{"Accept", "text/html"}});
        const std::string path = tidal::bundlePath(page.body);
        if (!path.empty()) {
            const HttpResponse js = net.get("https://tidal.com" + path);
            if (auto fresh = tidal::clientFromBundle(js.body); fresh && (fresh->id != client.id || fresh->secret != client.secret)) {
                ST_LOG_INFO(kTag, "TIDAL web client read from the bundle");
                client = *fresh;
                token = requestTidalToken(net, client);
            }
        }
    }
    if (token.empty()) throw Error(Error::Blocked, "TIDAL: no client-credentials token");
    std::lock_guard lock(a.m);
    a.client = client;
    a.token = token;
    a.expires = std::chrono::steady_clock::now() + std::chrono::hours(3);   // TIDAL gives 4 h
    return token;
}

List readTidal(const Ref& ref, const Net& net, const Progress& progress) {
    const std::string pageUrl = std::string("https://tidal.com/browse/") +
                                (ref.kind == Kind::Playlist ? "playlist/" : ref.kind == Kind::Album ? "album/" : "track/") + ref.id;
    std::string token = tidalToken(net, pageUrl, false);
    auto api = [&](const std::string& pathAndQuery) {
        const std::string url = pathAndQuery.rfind("http", 0) == 0 ? pathAndQuery
                                : std::string(kTidalApi) + (pathAndQuery.front() == '/' ? pathAndQuery.substr(1) : pathAndQuery);
        auto headers = [&] {
            return YoutubeExplode::Http::Headers{{"Authorization", "Bearer " + token}, {"Accept", "application/vnd.api+json"}};
        };
        HttpResponse r = net.get(url, headers());
        if (r.statusCode == 401) {
            token = tidalToken(net, pageUrl, true);
            r = net.get(url, headers());
        }
        return r;
    };
    const std::string cc = std::string("countryCode=") + kTidalCountry;
    List l;
    l.service = Service::Tidal;
    l.kind = ref.kind;
    l.id = ref.id;
    if (ref.kind == Kind::Track) {
        const HttpResponse r = api("tracks?" + cc + "&filter%5Bid%5D=" + ref.id + "&include=artists,albums,albums.coverArt");
        if (!r.isSuccessStatusCode()) failStatus(r, "TIDAL");
        auto s = tidal::parseTrackList(r.body);
        if (!s) throw Error(Error::NotFound, "TIDAL: track " + ref.id + " not found");
        l.title = s->title;
        l.owner = s->artistLine();
        l.coverUrl = s->coverUrl;
        l.expected = 1;
        l.songs.push_back(std::move(*s));
        return l;
    }
    const bool album = ref.kind == Kind::Album;
    const std::string base = (album ? "albums/" : "playlists/") + ref.id;
    const HttpResponse h = api(base + "?" + cc + (album ? "&include=artists,coverArt" : "&include=coverArt,owners"));
    if (!h.isSuccessStatusCode()) failStatus(h, "TIDAL");
    tidal::parseHeader(h.body, l);
    l.kind = ref.kind;
    l.id = ref.id;
    std::string next = base + "/relationships/items?" + cc + "&include=items,items.artists" + (album ? "" : ",items.albums");
    for (int page = 0; !next.empty() && page < 300 && static_cast<int>(l.songs.size()) < kMaxSongs; ++page) {
        const HttpResponse r = api(next);
        if (!r.isSuccessStatusCode()) failStatus(r, "TIDAL");
        const std::string current = std::exchange(next, tidal::parseItems(r.body, l, album));
        if (next == current) break;   // a cursor that doesn't move on
        if (progress) progress(static_cast<int>(l.songs.size()), l.expected);
    }
    if (static_cast<int>(l.songs.size()) > kMaxSongs) l.songs.resize(kMaxSongs);
    return l;
}

} // namespace

// ---- Entry ------------------------------------------------------------------------------------------------------------

void forgetTokens() {
    {
        auto& a = appleTokenStore();
        std::lock_guard lock(a.m);
        a.token.clear();
        a.expires = 0;
    }
    auto& t = tidalAuth();
    std::lock_guard lock(t.m);
    t.client = {kTidalClientId, kTidalClientKey};
    t.token.clear();
    t.expires = {};
}

List read(const Ref& ref, const Fetch& fetch, const Progress& progress, const CT& ct) {
    const Net net{fetch, ct};
    const auto started = std::chrono::steady_clock::now();
    List l;
    switch (ref.service) {
    case Service::Deezer: l = readDeezer(ref, net, progress); break;
    case Service::AppleMusic: l = readApple(ref, net, progress); break;
    case Service::Tidal: l = readTidal(ref, net, progress); break;
    default: throw Error(Error::Format, "not a Deezer / Apple Music / TIDAL link");
    }
    l.service = ref.service;
    ST_LOG_INFO(kTag, "{} {} {}: {} song(s), {} skipped, expected {}{} in {} ms", toUtf8(serviceName(ref.service)),
                ref.kind == Kind::Playlist ? "playlist" : ref.kind == Kind::Album ? "album" : "track", ref.id, l.songs.size(),
                l.skipped, l.expected, l.partial ? " (partial)" : "",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return l;
}

} // namespace st::app::svc
