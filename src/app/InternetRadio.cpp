// radio-browser.info client, station model and radio.json store (see InternetRadio.h).
#include "app/InternetRadio.h"

#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")   // GetAddrInfoW / GetNameInfoW (server discovery by DNS)

namespace st::app::radio {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// ---- small string helpers ---------------------------------------------------------------------------------------

std::string trim(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string asciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string asciiUpper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::vector<std::string> splitList(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const size_t end = comma == std::string_view::npos ? s.size() : comma;
        if (std::string item = trim(s.substr(start, end - start)); !item.empty()) out.push_back(std::move(item));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// Collapses runs of whitespace (names sometimes carry tabs / double spaces) and trims.
std::string cleanName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

bool isHttpUrl(std::string_view u) {
    const std::string l = asciiLower(std::string(u.substr(0, 8)));
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int num(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return static_cast<int>(std::clamp<int64_t>(it->get<int64_t>(), INT32_MIN, INT32_MAX));
    if (it->is_number()) return static_cast<int>(std::clamp(it->get<double>(), -2147483648.0, 2147483647.0));
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_string()) return std::atoi(it->get_ref<const std::string&>().c_str());
    return 0;
}

json parseJson(std::string_view s) {
    try {
        return json::parse(s);
    } catch (const json::exception& e) {
        throw RadioError(std::format("radio-browser.info: bad JSON ({})", e.what()));
    }
}

std::vector<Station> stationsFrom(const json& j) {
    if (!j.is_array()) throw RadioError("radio-browser.info: expected a station list");
    std::vector<Station> out;
    out.reserve(j.size());
    std::unordered_set<std::string> seen;
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        Station s;
        s.uuid = trim(str(o, "stationuuid"));
        s.name = cleanName(str(o, "name"));
        s.url = trim(str(o, "url"));
        s.urlResolved = trim(str(o, "url_resolved"));
        if (s.uuid.empty() || s.name.empty() || (s.url.empty() && s.urlResolved.empty())) continue;
        if (!seen.insert(s.uuid).second) continue;
        s.homepage = trim(str(o, "homepage"));
        s.favicon = trim(str(o, "favicon"));
        if (!isHttpUrl(s.favicon)) s.favicon.clear();
        for (auto& t : splitList(str(o, "tags"))) {
            t = asciiLower(std::move(t));
            if (std::find(s.tags.begin(), s.tags.end(), t) == s.tags.end() && s.tags.size() < 32) s.tags.push_back(std::move(t));
        }
        s.country = trim(str(o, "country"));
        s.countryCode = asciiUpper(trim(str(o, "countrycode")));
        s.state = trim(str(o, "state"));
        if (auto langs = splitList(str(o, "language")); !langs.empty()) s.language = asciiLower(langs.front());
        s.codec = asciiUpper(trim(str(o, "codec")));
        s.bitrate = std::max(0, num(o, "bitrate"));
        s.hls = num(o, "hls") != 0;
        s.votes = std::max(0, num(o, "votes"));
        s.clickCount = std::max(0, num(o, "clickcount"));
        out.push_back(std::move(s));
    }
    return out;
}

// ---- genres ---------------------------------------------------------------------------------------------------------

struct Genre {
    std::string_view tag;
    const wchar_t* label;   // translated (tr() pointers live for the whole process)
};

// A function, not a constant table: tr() works only after i18n::init (main), i.e. after static initialization.
const std::vector<Genre>& genres() {
    static const std::vector<Genre> list = {
        {"pop", tr(L"Pop")},
        {"rock", tr(L"Rock")},
        {"news", tr(L"Haber")},
        {"jazz", tr(L"Caz")},
        {"classical", tr(L"Klasik")},
        {"lofi", tr(L"Lo-fi")},
        {"lo-fi", tr(L"Lo-fi")},
        {"dance", tr(L"Dans")},
        {"electronic", tr(L"Elektronik")},
        {"80s", tr(L"80'ler")},
        {"90s", tr(L"90'lar")},
        {"70s", tr(L"70'ler")},
        {"60s", tr(L"60'lar")},
        {"hiphop", tr(L"Hip hop")},
        {"hip hop", tr(L"Hip hop")},
        {"hip-hop", tr(L"Hip hop")},
        {"chillout", tr(L"Chillout")},
        {"ambient", tr(L"Ambient")},
        {"oldies", tr(L"Nostalji")},
        {"talk", tr(L"Sohbet")},
        {"metal", tr(L"Metal")},
        {"blues", tr(L"Blues")},
        {"soul", tr(L"Soul")},
        {"reggae", tr(L"Reggae")},
        {"country", tr(L"Country")},
        {"sports", tr(L"Spor")},
        {"sport", tr(L"Spor")},
        {"house", tr(L"House")},
        {"top 40", tr(L"Top 40")},
        {"hits", tr(L"Hit şarkılar")},
        {"classic rock", tr(L"Klasik rock")},
        {"alternative", tr(L"Alternatif")},
        {"indie", tr(L"Indie")},
        {"folk", tr(L"Folk")},
        {"turkish", tr(L"Türkçe")},
        {"arabesk", tr(L"Arabesk")},
        {"religious", tr(L"Dini")},
        {"christian", tr(L"Hristiyan")},
        {"kids", tr(L"Çocuk")},
        {"latin", tr(L"Latin")},
        {"techno", tr(L"Tekno")},
        {"trance", tr(L"Trance")},
        {"rnb", tr(L"R&B")},
        {"funk", tr(L"Funk")},
        {"disco", tr(L"Disko")},
        {"world music", tr(L"Dünya müziği")},
        {"public radio", tr(L"Kamu yayını")},
        {"community radio", tr(L"Topluluk radyosu")},
        {"instrumental", tr(L"Enstrümantal")},
        {"lounge", tr(L"Lounge")},
        {"soundtrack", tr(L"Film müziği")},
        {"rap", tr(L"Rap")},
        {"punk", tr(L"Punk")},
    };
    return list;
}

const wchar_t* knownGenre(std::string_view tag) {
    for (const auto& g : genres())
        if (g.tag == tag) return g.label;
    return nullptr;
}

} // namespace

// ---- Codec filter ----------------------------------------------------------------------------------------------------

std::string streamUrl(const Station& s) {
    if (isHttpUrl(s.urlResolved)) return s.urlResolved;
    return isHttpUrl(s.url) ? s.url : std::string();
}

bool playable(const Station& s) {
    if (s.uuid.empty() || streamUrl(s).empty()) return false;
    const std::string& c = s.codec;
    const bool known = c == "MP3" || c == "AAC" || c == "AAC+";
    if (s.hls) return kPlayHls && (known || c.empty() || c == "UNKNOWN");
    return known;
}

std::vector<Station> filterPlayable(std::vector<Station> stations) {
    std::erase_if(stations, [](const Station& s) { return !playable(s); });
    return stations;
}

std::string codecBadge(const Station& s) {
    std::string c = s.codec.empty() || s.codec == "UNKNOWN" ? std::string(s.hls ? "HLS" : "") : s.codec;
    if (s.bitrate > 0) c += (c.empty() ? "" : " · ") + std::to_string(s.bitrate);
    return c;
}

std::string mimeType(const Station& s) {
    if (s.hls) return "application/vnd.apple.mpegurl";
    if (s.codec == "MP3") return "audio/mpeg";
    if (s.codec == "AAC" || s.codec == "AAC+") return "audio/aac";
    return {};
}

// ---- JSON ----------------------------------------------------------------------------------------------------------------

std::vector<Station> parseStations(std::string_view body) { return stationsFrom(parseJson(body)); }

std::vector<Tag> parseTags(std::string_view body) {
    const json j = parseJson(body);
    if (!j.is_array()) throw RadioError("radio-browser.info: expected a tag list");
    std::vector<Tag> out;
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        Tag t{asciiLower(trim(str(o, "name"))), std::max(0, num(o, "stationcount"))};
        if (!t.name.empty()) out.push_back(std::move(t));
    }
    return out;
}

std::vector<Country> parseCountries(std::string_view body) {
    const json j = parseJson(body);
    if (!j.is_array()) throw RadioError("radio-browser.info: expected a country list");
    std::vector<Country> out;
    std::unordered_set<std::string> seen;
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        Country c{asciiUpper(trim(str(o, "iso_3166_1"))), trim(str(o, "name")), std::max(0, num(o, "stationcount"))};
        if (c.code.size() != 2 || !seen.insert(c.code).second) continue;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> parseServers(std::string_view body) {
    const json j = parseJson(body);
    if (!j.is_array()) throw RadioError("radio-browser.info: expected a server list");
    std::vector<std::string> out;
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        std::string name = asciiLower(trim(str(o, "name")));
        // Only radio-browser's own API hosts: the name ends up in request URLs.
        if (name.size() <= 23 || !name.ends_with(".api.radio-browser.info")) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    }
    return out;
}

json toJson(const Station& s) {
    json j = {{"uuid", s.uuid}, {"name", s.name}};
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j[k] = v;
    };
    put("url", s.url);
    put("urlResolved", s.urlResolved);
    put("homepage", s.homepage);
    put("favicon", s.favicon);
    if (!s.tags.empty()) j["tags"] = s.tags;
    put("country", s.country);
    put("countryCode", s.countryCode);
    put("state", s.state);
    put("language", s.language);
    put("codec", s.codec);
    if (s.bitrate > 0) j["bitrate"] = s.bitrate;
    if (s.hls) j["hls"] = true;
    if (s.votes > 0) j["votes"] = s.votes;
    if (s.clickCount > 0) j["clicks"] = s.clickCount;
    return j;
}

Station stationFromJson(const json& j) {
    if (!j.is_object()) throw RadioError("station: not an object");
    Station s;
    s.uuid = str(j, "uuid");
    s.name = str(j, "name");
    if (s.uuid.empty() || s.name.empty()) throw RadioError("station: no uuid / name");
    s.url = str(j, "url");
    s.urlResolved = str(j, "urlResolved");
    s.homepage = str(j, "homepage");
    s.favicon = str(j, "favicon");
    if (!isHttpUrl(s.favicon)) s.favicon.clear();
    if (const auto it = j.find("tags"); it != j.end() && it->is_array())
        for (const auto& t : *it)
            if (t.is_string()) s.tags.push_back(t.get<std::string>());
    s.country = str(j, "country");
    s.countryCode = str(j, "countryCode");
    s.state = str(j, "state");
    s.language = str(j, "language");
    s.codec = str(j, "codec");
    s.bitrate = std::max(0, num(j, "bitrate"));
    s.hls = num(j, "hls") != 0;
    s.votes = std::max(0, num(j, "votes"));
    s.clickCount = std::max(0, num(j, "clicks"));
    return s;
}

// ---- Player model ------------------------------------------------------------------------------------------------------

bool isStationId(std::string_view id) { return id.size() > kIdPrefix.size() && id.starts_with(kIdPrefix); }

std::string uuidOf(std::string_view id) { return isStationId(id) ? std::string(id.substr(kIdPrefix.size())) : std::string(); }

std::wstring countryName(const std::string& code, const std::string& fallback) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::wstring> cache;
    if (code.size() == 2) {
        std::lock_guard lock(mu);
        if (auto it = cache.find(code); it != cache.end()) return it->second;
        wchar_t buf[128];
        std::wstring name;
        if (GetGeoInfoEx(const_cast<wchar_t*>(toWide(code).c_str()), GEO_FRIENDLYNAME, buf, 128) > 0) name = buf;
        if (!name.empty()) {
            cache.emplace(code, name);
            return name;
        }
    }
    return toWide(fallback);
}

std::wstring genreLabel(std::string_view tag) {
    if (const wchar_t* label = knownGenre(tag)) return label;
    std::wstring w = toWide(tag);
    // Tags are mostly English / Spanish: capitalized without the Turkish casing rules ("information" -> "Information").
    if (!w.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), 1, w.data(), 1, nullptr, nullptr, 0);
    return w;
}

bool isGenre(std::string_view tag) { return knownGenre(tag) != nullptr; }

const std::vector<std::string>& featuredGenres() {
    static const std::vector<std::string> list = {"pop",   "rock",       "news", "jazz",     "classical", "lofi",
                                                  "dance", "electronic", "80s",  "90s",      "hiphop",    "chillout",
                                                  "ambient", "oldies",   "talk", "metal",    "blues",     "soul"};
    return list;
}

std::wstring subtitle(const Station& s) {
    std::wstring out = countryName(s.countryCode, s.country);
    // Two genres: known (translated) tags first, then whatever the station was tagged with.
    std::vector<std::wstring> labels;
    for (int pass = 0; pass < 2 && labels.size() < 2; ++pass) {
        for (const auto& t : s.tags) {
            if (labels.size() >= 2) break;
            if ((knownGenre(t) != nullptr) != (pass == 0)) continue;
            std::wstring l = genreLabel(t);
            if (!l.empty() && std::find(labels.begin(), labels.end(), l) == labels.end()) labels.push_back(std::move(l));
        }
    }
    std::wstring tags;
    for (const auto& l : labels) tags += (tags.empty() ? L"" : L", ") + l;
    if (!tags.empty()) out += (out.empty() ? L"" : L" · ") + tags;
    return out.empty() ? std::wstring(tr(L"İnternet radyosu")) : out;
}

catalog::Track toTrack(const Station& s) {
    catalog::Track t;
    t.id = std::string(kIdPrefix) + s.uuid;
    t.name = s.name;
    t.artists.push_back({"", toUtf8(subtitle(s))});
    if (!s.favicon.empty()) t.album.images.push_back({s.favicon, 0, 0});
    t.durationMs = 0;
    return t;
}

std::vector<catalog::Track> toTracks(const std::vector<Station>& stations) {
    std::vector<catalog::Track> out;
    out.reserve(stations.size());
    for (const auto& s : stations) out.push_back(toTrack(s));
    return out;
}

// ---- Transport ------------------------------------------------------------------------------------------------------------
// A small synchronous WinHTTP client: a wall-clock deadline per request (every blocking call gets the time that is
// left), cancellation between reads, a capped body. The shared st::http client only knows a fixed 20 s timeout.

namespace {

constexpr auto kRequestTimeout = std::chrono::seconds(8);
constexpr auto kDiscoveryTimeout = std::chrono::seconds(5);
constexpr auto kBudget = std::chrono::seconds(20);        // one request, failover included
constexpr auto kCacheTtl = std::chrono::minutes(10);
constexpr size_t kCacheMax = 64;
constexpr size_t kMaxBody = 4u << 20;                      // 100 stations ~ 120 KB, countries ~ 15 KB
constexpr auto kServerDown = std::chrono::minutes(5);
constexpr auto kServersFresh = std::chrono::hours(6);
constexpr const char* kDiscoveryHost = "all.api.radio-browser.info";
constexpr const char* kFallbackServers[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info",
                                            "fi1.api.radio-browser.info"};

// Transport failure (DNS, refused, reset, deadline), a server error (5xx / 429) or a 2xx body that is not the JSON the
// endpoint returns: try another server.
struct Unreachable : RadioError {
    using RadioError::RadioError;
};

// An empty body, an HTML maintenance page or an error object in a 2xx answer is the server failing, not data: it is
// never cached. `list`: the endpoint returns a JSON array.
bool validBody(std::string_view body, bool list) {
    if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || (list && body[first] != '[')) return false;
    return json::accept(body);
}

class Handle {
public:
    explicit Handle(HINTERNET h) : h_(h) {}
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h_; }

private:
    HINTERNET h_;
};

HINTERNET session() {
    static const HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"ShadeTube", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)   // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
            h = WinHttpOpen(L"ShadeTube", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        return h;
    }();
    return s;
}

std::wstring queryHeader(HINTERNET req, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::string perform(const std::string& url, const std::string& userAgent, Clock::time_point deadline, const CancellationToken& ct) {
    ct.throwIfCancellationRequested();
    auto left = [&](const char* stage) -> DWORD {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0) throw Unreachable(std::format("timed out ({})", stage));
        return static_cast<DWORD>(std::min<long long>(ms, 60000));
    };
    auto failed = [](const char* stage) {
        const DWORD e = GetLastError();
        return Unreachable(e == ERROR_WINHTTP_TIMEOUT ? std::format("timed out ({})", stage)
                                                      : std::format("{} failed (WinHTTP {})", stage, e));
    };
    if (!session()) throw Unreachable("WinHTTP unavailable");

    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName) throw RadioError("invalid URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring{});
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle con(WinHttpConnect(session(), host.c_str(), parts.nPort, 0));
    if (!con) throw failed("connect");
    Handle req(WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) throw failed("open");
    DWORD t = left("connect");
    WinHttpSetTimeouts(req, static_cast<int>(t), static_cast<int>(t), static_cast<int>(t), static_cast<int>(t));
    DWORD features = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;   // sends Accept-Encoding; the cap counts decompressed bytes
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    // The session's agent string is replaced per request: the version is known only after startup.
    const std::wstring headers = L"User-Agent: " + toWide(userAgent) + L"\r\nAccept: application/json\r\n";
    WinHttpAddRequestHeaders(req, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) throw failed("send");
    if (!WinHttpReceiveResponse(req, nullptr)) throw failed("receive");

    DWORD status = 0, statusSize = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status >= 500 || status == 429) throw Unreachable(std::format("HTTP {}", status));
    if (status < 200 || status >= 300) throw RadioError(std::format("radio-browser.info: HTTP {}", status));
    if (const std::wstring len = queryHeader(req, WINHTTP_QUERY_CONTENT_LENGTH); !len.empty() && _wtoi64(len.c_str()) > static_cast<int64_t>(kMaxBody))
        throw RadioError("radio-browser.info: response too large");

    std::string body;
    char buf[16384];
    for (;;) {
        ct.throwIfCancellationRequested();
        t = left("body");
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) throw failed("read");
        if (available == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, std::min(available, static_cast<DWORD>(sizeof buf)), &got)) throw failed("read");
        if (got == 0) break;
        if (body.size() + got > kMaxBody) throw RadioError("radio-browser.info: response too large");
        body.append(buf, got);
    }
    return body;
}

// Reverse DNS of all.api.radio-browser.info: the documented way to list the servers when json/servers fails.
std::vector<std::string> dnsServers() {
    static const bool wsa = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!wsa) return {};
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(L"all.api.radio-browser.info", L"443", &hints, &res) != 0 || !res) return {};
    std::vector<std::string> out;
    for (const ADDRINFOW* p = res; p; p = p->ai_next) {
        wchar_t host[NI_MAXHOST] = {};
        if (GetNameInfoW(p->ai_addr, static_cast<socklen_t>(p->ai_addrlen), host, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) != 0)
            continue;
        std::string name = asciiLower(toUtf8(host));
        if (name.ends_with(".api.radio-browser.info") && std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(std::move(name));
    }
    FreeAddrInfoW(res);
    return out;
}

std::string encodeUuids(const std::vector<std::string>& uuids, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i < to; ++i) s += (s.empty() ? "" : ",") + http::urlEncode(uuids[i]);
    return s;
}

} // namespace

struct Client::Impl {
    mutable std::mutex mu;
    std::mutex discoveryMu;           // serializes server discovery (held without `mu`)
    std::string userAgent = "ShadeTube (+https://github.com/shadesofdeath/ShadeTube)";
    std::vector<std::string> hosts;   // shuffled once per discovery; hosts[current] is tried first
    size_t current = 0;
    bool pinned = false;              // setServers() (tests)
    Clock::time_point hostsAt{};
    std::unordered_map<std::string, Clock::time_point> downUntil;
    struct Entry {
        std::string body;
        Clock::time_point at;
    };
    std::unordered_map<std::string, Entry> cache;
};

Client& Client::shared() {
    static Client client;
    return client;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

void Client::setUserAgent(std::string userAgent) {
    std::lock_guard lock(impl_->mu);
    impl_->userAgent = std::move(userAgent);
}

void Client::setServers(std::vector<std::string> hosts) {
    std::lock_guard lock(impl_->mu);
    impl_->pinned = !hosts.empty();
    impl_->hosts = std::move(hosts);
    impl_->current = 0;
    impl_->hostsAt = impl_->pinned ? Clock::now() : Clock::time_point{};
    impl_->downUntil.clear();
}

std::string Client::currentServer() const {
    std::lock_guard lock(impl_->mu);
    return impl_->hosts.empty() ? std::string() : impl_->hosts[impl_->current % impl_->hosts.size()];
}

void Client::clearCache() {
    std::lock_guard lock(impl_->mu);
    impl_->cache.clear();
}

size_t Client::cacheSize() const {
    std::lock_guard lock(impl_->mu);
    return impl_->cache.size();
}

std::vector<std::string> Client::servers(const CancellationToken& ct) {
    auto fresh = [this] {
        return !impl_->hosts.empty() && (impl_->pinned || Clock::now() - impl_->hostsAt < kServersFresh);
    };
    {
        std::lock_guard lock(impl_->mu);
        if (fresh()) return impl_->hosts;
    }
    // One discovery at a time: the requests of a page opening together wait for it instead of each asking.
    std::lock_guard discovery(impl_->discoveryMu);
    std::string ua;
    {
        std::lock_guard lock(impl_->mu);
        if (fresh()) return impl_->hosts;
        ua = impl_->userAgent;
    }
    std::vector<std::string> list;
    try {
        list = parseServers(perform(std::string("https://") + kDiscoveryHost + "/json/servers", ua, Clock::now() + kDiscoveryTimeout, ct));
    } catch (const RadioError& e) {
        ST_LOG_WARN("iradio", "server list unavailable ({}); asking DNS", e.what());
    }
    if (list.empty()) list = dnsServers();
    if (list.empty()) {
        ST_LOG_WARN("iradio", "no servers discovered; using the built-in list");
        for (const char* h : kFallbackServers) list.emplace_back(h);
    }
    std::shuffle(list.begin(), list.end(), std::mt19937(std::random_device{}()));   // spread the load (API docs)
    std::lock_guard lock(impl_->mu);
    if (!impl_->pinned) {
        impl_->hosts = list;
        impl_->current = 0;
        impl_->hostsAt = Clock::now();
        ST_LOG_INFO("iradio", "{} server(s), using {}", list.size(), list.front());
    }
    return impl_->hosts;
}

std::string Client::get(const std::string& pathAndQuery, const CancellationToken& ct, bool useCache) {
    if (useCache) {
        std::lock_guard lock(impl_->mu);
        if (auto it = impl_->cache.find(pathAndQuery); it != impl_->cache.end() && Clock::now() - it->second.at < kCacheTtl)
            return it->second.body;
    }
    const auto hosts = servers(ct);
    const auto end = Clock::now() + kBudget;
    std::string ua;
    size_t start = 0;
    {
        std::lock_guard lock(impl_->mu);
        ua = impl_->userAgent;
        start = impl_->current;
    }
    std::string lastError = "no servers";
    // Servers marked down are skipped unless every server is (then they are all tried again).
    for (int round = 0; round < 2; ++round) {
        for (size_t k = 0; k < hosts.size(); ++k) {
            const size_t i = (start + k) % hosts.size();
            const std::string& host = hosts[i];
            {
                std::lock_guard lock(impl_->mu);
                const auto down = impl_->downUntil.find(host);
                if (round == 0 && down != impl_->downUntil.end() && Clock::now() < down->second) continue;
            }
            ct.throwIfCancellationRequested();
            if (Clock::now() >= end) throw RadioError("radio-browser.info: " + lastError);
            try {
                std::string body = perform("https://" + host + pathAndQuery, ua, std::min(Clock::now() + kRequestTimeout, end), ct);
                // The cached requests are the list endpoints (stations, tags, countries).
                if (!validBody(body, useCache)) throw Unreachable(body.empty() ? "empty response" : "not the expected JSON");
                std::lock_guard lock(impl_->mu);
                impl_->downUntil.erase(host);
                impl_->current = i;
                if (useCache) {
                    if (impl_->cache.size() >= kCacheMax) {
                        auto oldest = std::min_element(impl_->cache.begin(), impl_->cache.end(),
                                                       [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
                        impl_->cache.erase(oldest);
                    }
                    impl_->cache[pathAndQuery] = {body, Clock::now()};
                }
                return body;
            } catch (const Unreachable& e) {
                lastError = std::format("{}: {}", host, e.what());
                ST_LOG_WARN("iradio", "{} failed: {}", host, e.what());
                std::lock_guard lock(impl_->mu);
                impl_->downUntil[host] = Clock::now() + kServerDown;
            }
        }
    }
    throw RadioError("radio-browser.info: " + lastError);
}

StationPage Client::stationPage(const std::string& pathAndQuery, int limit, int offset, const CancellationToken& ct) {
    const json j = parseJson(get(pathAndQuery, ct, true));
    StationPage page;
    page.stations = filterPlayable(stationsFrom(j));
    page.more = static_cast<int>(j.size()) >= limit;
    page.nextOffset = offset + limit;
    return page;
}

StationPage Client::topClick(int limit, int offset, const CancellationToken& ct) {
    return stationPage(std::format("/json/stations/topclick?limit={}&offset={}&hidebroken=true", limit, offset), limit, offset, ct);
}

StationPage Client::topVote(int limit, int offset, const CancellationToken& ct) {
    return stationPage(std::format("/json/stations/topvote?limit={}&offset={}&hidebroken=true", limit, offset), limit, offset, ct);
}

std::string searchPath(const Query& q) {
    std::string p = "/json/stations/search?";
    if (!q.name.empty()) p += "name=" + http::urlEncode(q.name) + "&";
    if (!q.tag.empty()) p += "tag=" + http::urlEncode(q.tag) + "&tagExact=true&";
    if (!q.countryCode.empty()) p += "countrycode=" + http::urlEncode(q.countryCode) + "&";
    if (!q.language.empty()) p += "language=" + http::urlEncode(q.language) + "&languageExact=true&";
    p += std::format("order={}&reverse={}&hidebroken=true&limit={}&offset={}",
                     q.order == Query::Order::Votes ? "votes" : "clickcount", q.reverse ? "true" : "false", q.limit, q.offset);
    return p;
}

StationPage Client::search(const Query& q, const CancellationToken& ct) { return stationPage(searchPath(q), q.limit, q.offset, ct); }

std::vector<Tag> Client::tags(int limit, const CancellationToken& ct) {
    return parseTags(get(std::format("/json/tags?order=stationcount&reverse=true&hidebroken=true&limit={}", limit), ct, true));
}

std::vector<Country> Client::countries(const CancellationToken& ct) {
    return parseCountries(get("/json/countries?order=stationcount&reverse=true&hidebroken=true", ct, true));
}

std::vector<Station> Client::byUuids(const std::vector<std::string>& uuids, const CancellationToken& ct) {
    std::vector<Station> out;
    constexpr size_t kChunk = 40;   // keeps the URL short
    for (size_t i = 0; i < uuids.size(); i += kChunk) {
        const size_t to = std::min(uuids.size(), i + kChunk);
        auto part = parseStations(get("/json/stations/byuuid?uuids=" + encodeUuids(uuids, i, to), ct, true));
        for (auto& s : part) out.push_back(std::move(s));
    }
    return out;
}

bool Client::countClick(const std::string& uuid) {
    if (uuid.empty()) return false;
    try {
        get("/json/url/" + http::urlEncode(uuid), CancellationToken{}, false);
        ST_LOG_DEBUG("iradio", "click counted for {}", uuid);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("iradio", "click count for {} failed: {}", uuid, e.what());
        return false;
    }
}

// ---- Store -------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<Station> stationList(const json& j, const char* key) {
    std::vector<Station> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    std::unordered_set<std::string> seen;
    for (const auto& o : *it) {
        try {
            Station s = stationFromJson(o);
            if (seen.insert(s.uuid).second) out.push_back(std::move(s));
        } catch (const std::exception&) {
            // one bad entry never loses the others
        }
    }
    return out;
}

json listJson(const std::vector<Station>& list) {
    json a = json::array();
    for (const auto& s : list) a.push_back(toJson(s));
    return a;
}

void eraseUuid(std::vector<Station>& list, const std::string& uuid) {
    std::erase_if(list, [&](const Station& s) { return s.uuid == uuid; });
}

} // namespace

void Store::load(const std::filesystem::path& file) {
    file_ = file;
    favorites_.clear();
    recent_.clear();
    queue_.clear();
    known_.clear();
    knownOrder_.clear();
    country_.clear();
    dirty_ = false;
    ++revision_;
    std::ifstream f(file, std::ios::binary);
    if (!f) return;   // first run
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const json j = json::parse(ss.str());
        if (!j.is_object()) throw RadioError("not an object");
        favorites_ = stationList(j, "favorites");
        recent_ = stationList(j, "recent");
        queue_ = stationList(j, "queue");
        if (recent_.size() > kMaxRecent) recent_.resize(kMaxRecent);
        if (queue_.size() > kMaxQueue) queue_.resize(kMaxQueue);
        country_ = asciiUpper(str(j, "country"));
        if (country_.size() != 2) country_.clear();
    } catch (const std::exception& e) {
        // Kept as radio.json.bad, so the next save (which writes only what could be read) loses nothing.
        f.close();
        auto bad = file;
        bad += L".bad";
        std::error_code ec;
        std::filesystem::copy_file(file, bad, std::filesystem::copy_options::overwrite_existing, ec);
        ST_LOG_WARN("iradio", "radio.json is damaged ({}): kept as radio.json.bad{}", e.what(), ec ? " (copy failed)" : "");
    }
    for (const auto* list : {&queue_, &recent_, &favorites_})
        for (const auto& s : *list) rememberOne(s);
    ST_LOG_INFO("iradio", "{} favorite(s), {} recent station(s)", favorites_.size(), recent_.size());
}

bool Store::save() {
    if (file_.empty()) return false;
    const json j = {{"version", 1},
                    {"country", country_},
                    {"favorites", listJson(favorites_)},
                    {"recent", listJson(recent_)},
                    {"queue", listJson(queue_)}};
    const std::string data = j.dump(-1, ' ', false, json::error_handler_t::replace);   // never throws on bad UTF-8
    auto tmp = file_;
    tmp += L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    {
        // On the disk before the rename (WRITE_THROUGH only makes the rename durable): a power loss right after a save
        // must not leave a radio.json of zeros. A full disk shows up here too, before the good file is replaced.
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size() && FlushFileBuffers(h);
        const DWORD error = ok ? 0 : GetLastError();
        if (h != INVALID_HANDLE_VALUE) ok = CloseHandle(h) && ok;
        if (!ok) {
            ST_LOG_ERROR("iradio", "failed to write radio.json (error {})", error);
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ST_LOG_ERROR("iradio", "failed to replace radio.json (error {})", GetLastError());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    dirty_ = false;
    return true;
}

void Store::saveIfDirty() {
    if (dirty_) save();
}

bool Store::isFavorite(const std::string& uuid) const {
    return std::any_of(favorites_.begin(), favorites_.end(), [&](const Station& s) { return s.uuid == uuid; });
}

// Both take a copy first: `station` may be an element of the very list they change (find() returns one).
void Store::setFavorite(const Station& station, bool favorite) {
    if (station.uuid.empty() || favorite == isFavorite(station.uuid)) return;
    const Station s = station;
    if (favorite) favorites_.insert(favorites_.begin(), s);
    else eraseUuid(favorites_, s.uuid);
    rememberOne(s);
    changed();
}

void Store::recordPlayed(const Station& station) {
    if (station.uuid.empty()) return;
    if (!recent_.empty() && recent_.front().uuid == station.uuid) return;   // already the newest
    const Station s = station;
    eraseUuid(recent_, s.uuid);
    recent_.insert(recent_.begin(), s);
    if (recent_.size() > kMaxRecent) recent_.resize(kMaxRecent);
    rememberOne(s);
    changed();
}

void Store::removeRecent(const std::string& uuid) {
    const size_t before = recent_.size();
    eraseUuid(recent_, uuid);
    if (recent_.size() != before) changed();
}

void Store::rememberQueue(const std::vector<Station>& stations, size_t playing) {
    // The window Player::saveSession keeps of a long queue: 100 back, kMaxQueue in all.
    const size_t total = stations.size();
    const size_t first = total <= kMaxQueue ? 0 : std::min(playing > 100 ? playing - 100 : 0, total - kMaxQueue);
    const size_t last = std::min(total, first + kMaxQueue);
    queue_.assign(stations.begin() + static_cast<std::ptrdiff_t>(first), stations.begin() + static_cast<std::ptrdiff_t>(last));
    remember(stations);
    dirty_ = true;   // not shown anywhere: no notification
}

void Store::rememberOne(const Station& s) {
    if (s.uuid.empty()) return;
    auto [it, inserted] = known_.insert_or_assign(s.uuid, s);
    (void)it;
    if (!inserted) return;
    knownOrder_.push_back(s.uuid);
    // Bounded: the oldest entries go (favorites / recent / queue keep their own copies, see find()).
    while (knownOrder_.size() > kMaxKnown) {
        known_.erase(knownOrder_.front());
        knownOrder_.erase(knownOrder_.begin());
    }
}

void Store::remember(const std::vector<Station>& stations) {
    for (const auto& s : stations) rememberOne(s);
}

const Station* Store::find(const std::string& uuid) const {
    if (auto it = known_.find(uuid); it != known_.end()) return &it->second;
    for (const auto* list : {&favorites_, &recent_, &queue_})
        for (const auto& s : *list)
            if (s.uuid == uuid) return &s;
    return nullptr;
}

void Store::refresh(const std::vector<Station>& fresh) {
    bool touched = false;
    for (const auto& f : fresh) {
        for (auto* list : {&favorites_, &recent_, &queue_}) {
            for (auto& s : *list) {
                if (s.uuid != f.uuid) continue;
                const bool same = toJson(s) == toJson(f);
                if (!same) {
                    s = f;
                    touched = true;
                }
            }
        }
        rememberOne(f);
    }
    if (touched) changed();
}

void Store::setCountry(std::string code) {
    code = asciiUpper(std::move(code));
    if (code == country_) return;
    country_ = std::move(code);
    changed();
}

void Store::subscribe(std::weak_ptr<const void> owner, std::function<void()> fn) {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    listeners_.emplace_back(std::move(owner), std::move(fn));
}

void Store::changed() {
    ++revision_;
    dirty_ = true;
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    const auto snapshot = listeners_;   // a listener may rebuild a page that subscribes again
    for (const auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

Store& store() {
    static Store instance;
    return instance;
}

} // namespace st::app::radio
