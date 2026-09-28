#include "youtube/AltSource.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <functional>

#pragma comment(lib, "winhttp.lib")

namespace st::youtube::alt {

namespace yte = YoutubeExplode;
using json = nlohmann::json;
using CT = yte::CancellationToken;
using Clock = std::chrono::steady_clock;

namespace {

// Public instances change often (YouTube blocks their IPs, operators shut them down); ~90 known ones were probed on
// 2026-09-28 (tests/altsource `live`). Search worked on all of these but kavin.rocks; streams only for some videos
// (dQw4w9WgXcQ yes, kJQP7kiw5Fk "SignInConfirmNotBot" / companion errors everywhere). Hence the user's own
// instance goes first. Order = most reliable first.
const std::vector<std::string> kPipedInstances{
    "https://api.piped.private.coffee",   // streams: muxed itag 18 only
    "https://pipedapi.ducks.party",       // streams: muxed itag 18 only
    "https://pipedapi.kavin.rocks",       // official; Cloudflare 403 / 502 on 2026-09-28
};
const std::vector<std::string> kInvidiousInstances{
    "https://invidious.ducks.party",      // streams: audio-only via its proxy
    "https://invidious.f5.si",            // search only (companion errors)
    "https://invidious.materialio.us",    // search only
};

constexpr auto kRequestTimeout = std::chrono::seconds(8);   // wall clock per request, body included
constexpr auto kBudget = std::chrono::seconds(25);          // a whole walk, the attempt in flight included
constexpr auto kDemoteFor = std::chrono::minutes(5);        // a failed instance is tried last for a while
constexpr auto kSkipDownFor = std::chrono::seconds(60);     // an unreachable one is not tried at all
constexpr size_t kMaxJson = 4u << 20;                       // decompressed API answers
constexpr size_t kMaxErrorBody = 16u << 10;                 // only the error text is used
constexpr size_t kProbeBytes = 16;

constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36 "
    L"ShadeTube/0.1";

// ---- transport ----------------------------------------------------------------------------------------------------
// AltSource's own synchronous WinHTTP client (YoutubeExplode's IHttpClient buffers whole bodies and only knows
// per-operation timeouts): a wall-clock deadline for the whole request, capped bodies, optional redirects.

// Transport failure (DNS, refused, reset, deadline): the instance counts as down.
struct Unreachable : AltSourceError {
    using AltSourceError::AltSourceError;
};

struct Request {
    const wchar_t* method = L"GET";
    std::string url;
    std::wstring headers;           // "Name: value\r\n"...
    Clock::time_point deadline;
    size_t maxBody = kMaxJson;      // 2xx body cap (error bodies: kMaxErrorBody, truncated)
    bool truncate = false;          // at the cap: stop reading (probe) instead of failing
    bool followRedirects = true;    // stream URLs: off (a redirect could lead anywhere)
    bool decompress = false;
};

struct Response {
    int status = 0;
    std::string body;
    std::wstring contentRange;
    int64_t contentLength = -1;
    bool ok() const { return status >= 200 && status < 300; }
};

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
        HINTERNET h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)   // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
            h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

Response perform(const Request& rq, const CT& ct) {
    ct.throwIfCancellationRequested();
    // Every blocking WinHTTP call gets the time that is left, so the deadline also holds against a slow drip.
    auto left = [&](const char* stage) -> DWORD {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(rq.deadline - Clock::now()).count();
        if (ms <= 0) throw Unreachable(std::format("timed out ({})", stage));
        return static_cast<DWORD>(std::min<long long>(ms, 60000));
    };
    auto failed = [](const char* stage) {
        const DWORD e = GetLastError();
        return Unreachable(e == ERROR_WINHTTP_TIMEOUT ? std::format("timed out ({})", stage) : std::format("{} failed (WinHTTP {})", stage, e));
    };
    if (!session()) throw Unreachable("WinHTTP unavailable");

    const std::wstring wide = toWide(rq.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName)
        throw AltSourceError("invalid URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring{});
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle con(WinHttpConnect(session(), host.c_str(), parts.nPort, 0));
    if (!con) throw failed("connect");
    Handle req(WinHttpOpenRequest(con, rq.method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) throw failed("open");
    DWORD t = left("connect");
    WinHttpSetTimeouts(req, static_cast<int>(t), static_cast<int>(t), static_cast<int>(t), static_cast<int>(t));
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &t, sizeof t);
    DWORD features = WINHTTP_DISABLE_COOKIES | (rq.followRedirects ? 0 : WINHTTP_DISABLE_REDIRECTS);
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    if (rq.decompress) {
        DWORD d = WINHTTP_DECOMPRESSION_FLAG_ALL;   // also sends Accept-Encoding; the cap counts decompressed bytes
        WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &d, sizeof d);
    }
    if (!WinHttpSendRequest(req, rq.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : rq.headers.c_str(),
                            rq.headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        throw failed("send");
    if (!WinHttpReceiveResponse(req, nullptr)) throw failed("receive");

    Response resp;
    DWORD status = 0, statusSize = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &statusSize, WINHTTP_NO_HEADER_INDEX);
    resp.status = static_cast<int>(status);
    resp.contentRange = queryHeader(req, WINHTTP_QUERY_CONTENT_RANGE);
    if (const std::wstring len = queryHeader(req, WINHTTP_QUERY_CONTENT_LENGTH); !len.empty()) resp.contentLength = _wtoi64(len.c_str());

    const size_t cap = resp.ok() ? rq.maxBody : kMaxErrorBody;
    const bool stopAtCap = rq.truncate || !resp.ok();
    if (!stopAtCap && resp.contentLength > static_cast<int64_t>(cap)) throw AltSourceError("response too large");
    char buf[16384];
    while (!stopAtCap || resp.body.size() < cap) {
        ct.throwIfCancellationRequested();
        t = left("body");
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) throw failed("read");
        if (available == 0) break;
        size_t want = std::min<size_t>(available, sizeof buf);
        if (stopAtCap) want = std::min(want, cap - resp.body.size());
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, static_cast<DWORD>(want), &got)) throw failed("read");
        if (got == 0) break;
        if (!stopAtCap && resp.body.size() + got > cap) throw AltSourceError("response too large");
        resp.body.append(buf, got);
    }
    return resp;   // closing the handles aborts whatever the server still wanted to send
}

Clock::time_point requestDeadline(Clock::time_point walkEnd) { return std::min(Clock::now() + kRequestTimeout, walkEnd); }

Response getJson(const std::string& url, Clock::time_point walkEnd, const CT& ct) {
    Request rq;
    rq.url = url;
    rq.headers = L"Accept: application/json\r\n";
    rq.deadline = requestDeadline(walkEnd);
    rq.decompress = true;
    return perform(rq, ct);
}

// ---- helpers ------------------------------------------------------------------------------------------------------

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// ---- JSON helpers: Invidious sends numbers as strings ("bitrate": "130516"), Piped uses null for "unknown".
std::string jstr(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int64_t jint(const json& o, const char* key, int64_t fallback = 0) {
    const auto it = o.find(key);
    if (it == o.end()) return fallback;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    if (it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        char* end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (end != s.c_str()) return v;
    }
    return fallback;
}

bool jbool(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_boolean() && it->get<bool>();
}

json parseBody(std::string_view body) {
    json j = json::parse(body.begin(), body.end(), nullptr, false);
    if (j.is_discarded()) {
        std::string head = trim(body.substr(0, 60));
        std::replace(head.begin(), head.end(), '\n', ' ');
        throw AltSourceError("not a JSON response (" + (head.empty() ? std::string("empty") : head) + ")");
    }
    return j;
}

// {"error": "...", "message": "..."} (Piped puts a Java stack trace there: keep its first line).
std::string errorText(const json& j) {
    if (!j.is_object()) return {};
    std::string e = jstr(j, "error");
    if (e.empty()) e = jstr(j, "message");
    e = e.substr(0, e.find('\n'));
    if (e.size() > 160) e = e.substr(0, 157) + "...";
    return e;
}

std::string errorOfBody(const std::string& body) {   // body <= kMaxErrorBody
    const json j = json::parse(body, nullptr, false);
    if (!j.is_discarded()) return errorText(j);
    std::string head = trim(std::string_view(body).substr(0, 80));
    std::replace(head.begin(), head.end(), '\n', ' ');
    std::replace(head.begin(), head.end(), '\r', ' ');
    return head;
}

AltSourceError httpError(const Response& r) {
    const std::string e = errorOfBody(r.body);
    return AltSourceError(std::format("HTTP {}{}", r.status, e.empty() ? std::string{} : " (" + e + ")"));
}

// The instance says the video itself is gone: no other instance will have it, and it says nothing about the
// instance's health (NewPipe / Invidious wording).
bool videoGone(const std::string& message) {
    const std::string m = lower(message);
    for (const char* p : {"video is unavailable", "video unavailable", "video is private", "private video", "has been removed",
                          "account terminated", "accountterminated", "video does not exist"})
        if (m.find(p) != std::string::npos) return true;
    return false;
}

std::string absolute(const std::string& url, const std::string& base) {
    if (url.starts_with("//")) return "https:" + url;
    if (url.starts_with("/")) return base + url;
    return url;
}

// "audio/webm; codecs=\"opus\"" -> "audio/webm"
std::string mimeOf(const std::string& type) { return lower(trim(std::string_view(type).substr(0, type.find(';')))); }

// "audio/webm; codecs=\"opus\"" -> "opus"
std::string codecsOf(const std::string& type) {
    const auto p = type.find("codecs=\"");
    if (p == std::string::npos) return {};
    const auto b = p + 8;
    return type.substr(b, type.find('"', b) - b);
}

// googlevideo `xtags` names the audio content of multi-language videos: acont=original | dubbed(-auto) | descriptive.
bool dubbedByUrl(const std::string& url) {
    auto p = url.find("acont");
    if (p == std::string::npos) return false;
    p += 5;
    if (url.compare(p, 1, "=") == 0) p += 1;
    else if (url.compare(p, 3, "%3D") == 0 || url.compare(p, 3, "%3d") == 0) p += 3;
    else return false;
    return url.compare(p, 8, "original") != 0;
}

bool validVideoId(const std::string& id) {
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
           });
}

// Muxed mp4 (itag 18) plays as audio: the YouTube path's last resort too. Its audio track is AAC-LC 96 kbps;
// Piped reports bitrate 0 for it.
AudioStream muxed(const std::string& url, int bitrate, int64_t length) {
    AudioStream a;
    a.url = url;
    a.mimeType = "audio/mp4";
    a.codec = "mp4a.40.2";
    a.bitrate = bitrate > 0 ? bitrate : 96000;
    a.contentLength = std::max<int64_t>(0, length);
    a.itag = 18;
    a.muxed = true;
    return a;
}

// Loopback / private / link-local address literals ("host" or "host:port", IPv6 in brackets). DNS names that
// resolve to such addresses are out of scope.
bool privateHost(const std::string& hostPort) {
    std::string h = lower(hostPort);
    if (h.starts_with("[")) {
        h = h.substr(0, h.find(']') + 1);
        return h == "[::1]" || h == "[::]" || h.starts_with("[fc") || h.starts_with("[fd") || h.starts_with("[fe8") ||
               h.starts_with("[fe9") || h.starts_with("[fea") || h.starts_with("[feb") || h.starts_with("[::ffff:");
    }
    h = h.substr(0, h.find(':'));
    if (h == "localhost" || h.ends_with(".localhost")) return true;
    int o[4] = {};
    int n = 0;
    size_t pos = 0;
    while (n < 4 && pos <= h.size()) {
        const size_t dot = h.find('.', pos);
        const std::string part = h.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        o[n++] = std::atoi(part.c_str());
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (n != 4) return false;
    return o[0] == 0 || o[0] == 10 || o[0] == 127 || (o[0] == 169 && o[1] == 254) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
           (o[0] == 192 && o[1] == 168) || (o[0] == 100 && o[1] >= 64 && o[1] <= 127);
}

// Proves the URL answers ranged requests with the promised container (what ProgressiveBuffer needs to play and
// seek) and makes sure the total length is known: ProgressiveBuffer would otherwise take it from its first
// Content-Range, and Piped's proxy reports the chunk size there ("bytes 0-15/16", "bytes 1000000-15/16").
// A server that ignores Range costs at most 16 bytes: the transfer is closed after them.
void probeRange(AudioStream& s, Clock::time_point walkEnd, const CT& ct) {
    Request rq;
    rq.url = s.url;
    rq.headers = std::format(L"Range: bytes=0-{}\r\n", kProbeBytes - 1);
    rq.deadline = requestDeadline(walkEnd);
    rq.maxBody = kProbeBytes;
    rq.truncate = true;
    rq.followRedirects = false;
    const Response r = perform(rq, ct);
    if (r.status != 206)
        throw AltSourceError(r.status == 200 ? std::string("stream ignores range requests") : std::format("stream answered HTTP {}", r.status));
    const Container want = containerOf(s.mimeType);
    if (sniffContainer(r.body) != want)
        throw AltSourceError(std::format("stream is not {} data", want == Container::Webm ? "WebM" : "MP4"));
    if (s.contentLength > 0) return;   // from the API (clen / contentLength)
    long long total = 0;
    if (const auto slash = r.contentRange.rfind(L'/'); slash != std::wstring::npos)   // "bytes 0-15/3433514"
        total = _wtoi64(r.contentRange.c_str() + slash + 1);
    if (total <= static_cast<long long>(kProbeBytes)) {   // missing, "*" or the chunk size: HEAD has the real length
        Request head;
        head.method = L"HEAD";
        head.url = s.url;
        head.deadline = requestDeadline(walkEnd);
        head.maxBody = 0;
        head.truncate = true;
        head.followRedirects = false;
        const Response h = perform(head, ct);
        total = h.ok() ? h.contentLength : 0;
    }
    if (total <= static_cast<long long>(kProbeBytes)) throw AltSourceError("stream length unknown");
    s.contentLength = total;
}

// One instance: API answer -> acceptable stream URLs -> pick -> Range probe. `checkpoint` runs before the probe (the
// walk's configuration / cancellation check).
Served fetchStream(Kind kind, const std::string& instance, const std::string& videoId, bool allowWebm, bool lowQuality,
                   const CT& ct, Clock::time_point walkEnd, const std::function<void()>& checkpoint) {
    if (kind == Kind::Off) throw AltSourceError("backup source is off");
    const std::string url = kind == Kind::Piped ? instance + "/streams/" + videoId
                                                : instance + "/api/v1/videos/" + videoId + "?local=true";
    const Response r = getJson(url, walkEnd, ct);
    if (!r.ok()) throw httpError(r);
    VideoInfo v = kind == Kind::Piped ? parsePipedStreams(r.body, instance) : parseInvidiousVideo(r.body, instance);
    const size_t offered = v.streams.size();
    std::erase_if(v.streams, [&](const AudioStream& a) { return !acceptableStreamUrl(a.url, instance, v.proxyUrl); });
    const AudioStream* pick = pickStream(v.streams, allowWebm, lowQuality);
    if (!pick)
        throw AltSourceError(offered == 0 ? "no audio streams" : v.streams.empty() ? "stream URLs outside the instance" : "no usable audio stream");
    Served s;
    s.stream = *pick;
    s.kind = kind;
    s.instance = instance;
    v.streams.clear();
    s.video = std::move(v);
    if (checkpoint) checkpoint();
    probeRange(s.stream, walkEnd, ct);
    return s;
}

std::vector<SearchItem> fetchSearch(Kind kind, const std::string& instance, const std::string& query, bool music,
                                    const CT& ct, Clock::time_point walkEnd) {
    if (kind == Kind::Off) throw AltSourceError("backup source is off");
    const std::string q = http::urlEncode(query);
    const std::string url = kind == Kind::Piped ? instance + "/search?q=" + q + "&filter=" + (music ? "music_songs" : "videos")
                                                : instance + "/api/v1/search?q=" + q + "&type=video";
    const Response r = getJson(url, walkEnd, ct);
    if (!r.ok()) throw httpError(r);
    return kind == Kind::Piped ? parsePipedSearch(r.body) : parseInvidiousSearch(r.body);
}

} // namespace

std::string hostOf(const std::string& url) {
    auto p = url.find("://");
    p = p == std::string::npos ? 0 : p + 3;
    return url.substr(p, url.find_first_of("/?#", p) - p);
}

Kind parseKind(std::string_view v) {
    if (v == "piped") return Kind::Piped;
    if (v == "invidious") return Kind::Invidious;
    return Kind::Off;
}

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Piped: return "piped";
    case Kind::Invidious: return "invidious";
    case Kind::Off: break;
    }
    return "off";
}

std::optional<std::string> normalizeInstance(std::string_view input) {
    std::string s = trim(input);
    if (s.empty()) return std::nullopt;
    const std::string l = lower(s);
    size_t hostStart;
    if (l.starts_with("https://")) hostStart = 8;
    else if (l.starts_with("http://")) hostStart = 7;
    else if (l.find("://") != std::string::npos) return std::nullopt;
    else {
        s = "https://" + s;
        hostStart = 8;
    }
    s.replace(0, hostStart, lower(s.substr(0, hostStart)));
    while (s.size() > hostStart && s.back() == '/') s.pop_back();
    const std::string host = s.substr(hostStart, s.find('/', hostStart) - hostStart);
    if (host.empty() || host.front() == '.' || host.front() == ':') return std::nullopt;
    for (char c : host)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_' && c != ':' && c != '[' && c != ']')
            return std::nullopt;
    for (char c : s)
        if (static_cast<unsigned char>(c) <= ' ' || c == '?' || c == '#') return std::nullopt;
    return s;
}

bool acceptableStreamUrl(const std::string& url, const std::string& instance, const std::string& declaredProxy) {
    const std::string scheme = lower(url.substr(0, url.find("://") + 3));
    const bool httpInstance = lower(instance).starts_with("http://");
    if (scheme != "https://" && !(scheme == "http://" && httpInstance)) return false;
    const std::string host = lower(hostOf(url));
    if (host.empty() || host.find('@') != std::string::npos) return false;
    const std::string instanceHost = lower(hostOf(instance));
    if (host == instanceHost) return true;
    if (declaredProxy.empty() || host != lower(hostOf(declaredProxy))) return false;
    return !privateHost(host) || privateHost(instanceHost);
}

// ---- parsing ------------------------------------------------------------------------------------------------------

VideoInfo parsePipedStreams(std::string_view body, const std::string& base) {
    const json j = parseBody(body);
    if (!j.is_object()) throw AltSourceError("unexpected streams response");
    if (!j.contains("audioStreams")) {
        const std::string e = errorText(j);
        throw AltSourceError(e.empty() ? std::string("no audioStreams in the response") : e);
    }
    VideoInfo v;
    v.title = jstr(j, "title");
    v.uploader = jstr(j, "uploader");
    v.durationSec = static_cast<int>(jint(j, "duration"));
    v.proxyUrl = jstr(j, "proxyUrl");
    if (const auto it = j.find("audioStreams"); it != j.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (!s.is_object() || jbool(s, "videoOnly")) continue;
            AudioStream a;
            a.url = absolute(jstr(s, "url"), base);
            a.mimeType = mimeOf(jstr(s, "mimeType"));
            a.codec = jstr(s, "codec");
            a.bitrate = static_cast<int>(jint(s, "bitrate"));
            a.contentLength = std::max<int64_t>(0, jint(s, "contentLength"));
            a.itag = static_cast<int>(jint(s, "itag"));
            // NewPipe's AudioTrackType: ORIGINAL | DUBBED | DESCRIPTIVE | SECONDARY; null = the only track.
            const std::string track = jstr(s, "audioTrackType");
            a.dubbed = !track.empty() && track != "ORIGINAL";
            if (a.url.empty() || containerOf(a.mimeType) == Container::Unknown) continue;
            v.streams.push_back(std::move(a));
        }
    }
    if (const auto it = j.find("videoStreams"); it != j.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (!s.is_object() || jbool(s, "videoOnly") || jint(s, "itag") != 18) continue;
            const std::string url = absolute(jstr(s, "url"), base);
            if (!url.empty()) v.streams.push_back(muxed(url, static_cast<int>(jint(s, "bitrate")), jint(s, "contentLength")));
        }
    }
    return v;
}

VideoInfo parseInvidiousVideo(std::string_view body, const std::string& base) {
    const json j = parseBody(body);
    if (!j.is_object()) throw AltSourceError("unexpected video response");
    if (!j.contains("adaptiveFormats")) {
        const std::string e = errorText(j);
        throw AltSourceError(e.empty() ? std::string("no adaptiveFormats in the response") : e);
    }
    VideoInfo v;
    v.title = jstr(j, "title");
    v.uploader = jstr(j, "author");
    v.durationSec = static_cast<int>(jint(j, "lengthSeconds"));
    if (const auto it = j.find("adaptiveFormats"); it != j.end() && it->is_array()) {
        for (const auto& f : *it) {
            if (!f.is_object()) continue;
            const std::string type = jstr(f, "type");
            const std::string mime = mimeOf(type);
            if (!mime.starts_with("audio/") || containerOf(mime) == Container::Unknown) continue;
            AudioStream a;
            a.url = absolute(jstr(f, "url"), base);
            a.mimeType = mime;
            a.codec = codecsOf(type);
            if (a.codec.empty()) a.codec = jstr(f, "encoding");
            a.bitrate = static_cast<int>(jint(f, "bitrate"));
            a.contentLength = std::max<int64_t>(0, jint(f, "clen"));
            a.itag = static_cast<int>(jint(f, "itag"));
            if (const auto t = f.find("audioTrack"); t != f.end() && t->is_object() && t->contains("isDefault"))
                a.dubbed = !jbool(*t, "isDefault");
            else
                a.dubbed = dubbedByUrl(a.url);
            if (a.url.empty()) continue;
            v.streams.push_back(std::move(a));
        }
    }
    if (const auto it = j.find("formatStreams"); it != j.end() && it->is_array()) {
        for (const auto& f : *it) {
            if (!f.is_object() || jint(f, "itag") != 18) continue;
            const std::string url = absolute(jstr(f, "url"), base);
            if (!url.empty()) v.streams.push_back(muxed(url, static_cast<int>(jint(f, "bitrate")), jint(f, "clen")));
        }
    }
    return v;
}

std::vector<SearchItem> parsePipedSearch(std::string_view body) {
    const json j = parseBody(body);
    const auto items = j.is_object() ? j.find("items") : j.end();
    if (!j.is_object() || items == j.end() || !items->is_array()) {
        const std::string e = errorText(j);
        throw AltSourceError(e.empty() ? std::string("no items in the search response") : e);
    }
    std::vector<SearchItem> out;
    for (const auto& it : *items) {
        if (!it.is_object() || jstr(it, "type") != "stream" || jbool(it, "isShort")) continue;
        SearchItem s;
        const std::string url = jstr(it, "url");   // "/watch?v=kJQP7kiw5Fk"
        if (const auto p = url.find("v="); p != std::string::npos) s.videoId = url.substr(p + 2, 11);
        if (!validVideoId(s.videoId)) continue;
        s.title = jstr(it, "title");
        s.channel = jstr(it, "uploaderName");
        const std::string ch = jstr(it, "uploaderUrl");   // "/channel/UC..."
        if (const auto p = ch.rfind('/'); p != std::string::npos) s.channelId = ch.substr(p + 1);
        const int64_t d = jint(it, "duration", -1);
        s.durationSec = d > 0 ? static_cast<int>(d) : -1;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<SearchItem> parseInvidiousSearch(std::string_view body) {
    const json j = parseBody(body);
    if (!j.is_array()) {
        const std::string e = errorText(j);
        throw AltSourceError(e.empty() ? std::string("unexpected search response") : e);
    }
    std::vector<SearchItem> out;
    for (const auto& it : j) {
        if (!it.is_object() || jstr(it, "type") != "video") continue;
        SearchItem s;
        s.videoId = jstr(it, "videoId");
        if (!validVideoId(s.videoId)) continue;
        s.title = jstr(it, "title");
        s.channel = jstr(it, "author");
        s.channelId = jstr(it, "authorId");
        const int64_t d = jint(it, "lengthSeconds", -1);
        s.durationSec = d > 0 && !jbool(it, "liveNow") ? static_cast<int>(d) : -1;
        out.push_back(std::move(s));
    }
    return out;
}

Container containerOf(const std::string& mimeType) {
    const std::string m = mimeOf(mimeType);
    if (m == "audio/mp4" || m == "video/mp4") return Container::Mp4;
    if (m == "audio/webm" || m == "video/webm") return Container::Webm;
    return Container::Unknown;
}

Container sniffContainer(std::string_view b) {
    if (b.size() >= 8 && b.substr(4, 4) == "ftyp") return Container::Mp4;
    if (b.size() >= 4 && static_cast<unsigned char>(b[0]) == 0x1A && static_cast<unsigned char>(b[1]) == 0x45 &&
        static_cast<unsigned char>(b[2]) == 0xDF && static_cast<unsigned char>(b[3]) == 0xA3)
        return Container::Webm;
    return Container::Unknown;
}

const AudioStream* pickStream(const std::vector<AudioStream>& streams, bool allowWebm, bool lowQuality) {
    const AudioStream* best = nullptr;
    auto audioOnly = [](const AudioStream& s) { return !s.muxed && !s.dubbed; };
    for (const auto& s : streams) {
        if (!audioOnly(s)) continue;
        if (containerOf(s.mimeType) == Container::Webm && !allowWebm) continue;
        if (lowQuality && s.bitrate / 1000.0 > 140) continue;
        if (!best || s.bitrate > best->bitrate) best = &s;
    }
    if (!best && lowQuality)   // nothing under the cap: take the smallest mp4
        for (const auto& s : streams)
            if (audioOnly(s) && containerOf(s.mimeType) == Container::Mp4 && (!best || s.bitrate < best->bitrate)) best = &s;
    if (!best)   // no audio-only stream at all: muxed mp4 (itag 18)
        for (const auto& s : streams)
            if (s.muxed && containerOf(s.mimeType) == Container::Mp4 && (!best || s.bitrate < best->bitrate)) best = &s;
    return best;
}

// ---- AltSource ----------------------------------------------------------------------------------------------------

const std::vector<std::string>& AltSource::builtinInstances(Kind kind) {
    static const std::vector<std::string> none;
    return kind == Kind::Piped ? kPipedInstances : kind == Kind::Invidious ? kInvidiousInstances : none;
}

void AltSource::configure(Kind kind, std::string customInstance) {
    const auto normalized = normalizeInstance(customInstance);
    if (!normalized && !trim(customInstance).empty())
        ST_LOG_WARN("altsource", "ignoring invalid instance address \"{}\"", customInstance);
    const std::string next = normalized.value_or(std::string{});
    std::lock_guard lock(mutex_);
    if (kind == kind_ && next == custom_) return;
    ST_LOG_INFO("altsource", "backup source: {}{}", kindName(kind), next.empty() ? std::string{} : " via " + next);
    // A replaced own instance must never be contacted again: forget what was learned about it.
    if (!custom_.empty() && next != custom_) {
        std::erase_if(lastGood_, [&](const auto& kv) { return kv.second == custom_; });
        std::erase_if(failed_, [&](const auto& kv) { return kv.first.substr(2) == custom_; });
    }
    kind_ = kind;
    custom_ = next;
    ++generation_;   // walks in flight stop at their next step
}

Kind AltSource::kind() const {
    std::lock_guard lock(mutex_);
    return kind_;
}

uint64_t AltSource::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::string AltSource::label() const {
    std::lock_guard lock(mutex_);
    return kindName(kind_);
}

std::vector<std::string> AltSource::instanceOrder() const {
    std::lock_guard lock(mutex_);
    return orderLocked(Op::Stream);
}

AltSource::Snapshot AltSource::snapshot(Op op) const {
    std::lock_guard lock(mutex_);
    return {kind_, generation_, orderLocked(op)};
}

// Health is kept per operation: an instance can answer searches while YouTube blocks its stream extraction.
std::string AltSource::healthKey(Op op, const std::string& instance) {
    return (op == Op::Stream ? "s|" : "q|") + instance;
}

std::vector<std::string> AltSource::orderLocked(Op op) const {
    std::vector<std::string> order, demoted;
    auto push = [&order](const std::string& s) {
        if (!s.empty() && std::find(order.begin(), order.end(), s) == order.end()) order.push_back(s);
    };
    static const std::vector<std::string> none;
    const auto& builtin = useBuiltins_ ? builtinInstances(kind_) : none;
    push(custom_);
    // The last good instance only while it is still a current choice (never a replaced own instance).
    if (const auto it = lastGood_.find(healthKey(op, kindName(kind_)));
        it != lastGood_.end() && (it->second == custom_ || std::find(builtin.begin(), builtin.end(), it->second) != builtin.end()))
        push(it->second);
    const auto now = Clock::now();
    for (const auto& b : builtin) {
        const auto f = failed_.find(healthKey(op, b));
        if (f == failed_.end() || now - f->second.at >= kDemoteFor) push(b);
        else if (!f->second.down || now - f->second.at >= kSkipDownFor) demoted.push_back(b);
    }
    for (const auto& d : demoted) push(d);
    // The user's own / last good instance is skipped too while it is unreachable (its timeout would be paid again).
    std::erase_if(order, [&](const std::string& s) {
        const auto f = failed_.find(healthKey(op, s));
        return f != failed_.end() && f->second.down && now - f->second.at < kSkipDownFor;
    });
    return order;
}

void AltSource::noteResult(Op op, const Snapshot& snap, const std::string& instance, Outcome outcome) {
    std::lock_guard lock(mutex_);
    if (snap.generation != generation_) return;   // reconfigured meanwhile: this result is about an old choice
    auto& last = lastGood_[healthKey(op, kindName(snap.kind))];
    if (outcome == Outcome::Ok) {
        last = instance;
        failed_.erase(healthKey(op, instance));
    } else {
        failed_[healthKey(op, instance)] = {Clock::now(), outcome == Outcome::Down};
        if (last == instance) last.clear();
    }
}

// Transport failures (DNS, refused, timeout) mean the instance is down: HTTP error answers come back as responses.
AltSource::Outcome AltSource::failureOf(const std::exception& e) {
    return dynamic_cast<const Unreachable*>(&e) ? Outcome::Down : Outcome::Failed;
}

Served AltSource::streamFrom(Kind kind, const std::string& instance, const std::string& videoId, bool allowWebm,
                             bool lowQuality, const CT& ct) {
    return fetchStream(kind, instance, videoId, allowWebm, lowQuality, ct, Clock::now() + kBudget, {});
}

std::vector<SearchItem> AltSource::searchOn(Kind kind, const std::string& instance, const std::string& query, bool music,
                                            const CT& ct) {
    return fetchSearch(kind, instance, query, music, ct, Clock::now() + kBudget);
}

Served AltSource::stream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct) {
    const Snapshot snap = snapshot(Op::Stream);
    if (snap.kind == Kind::Off) throw AltSourceError("backup source is off");
    const auto walkEnd = Clock::now() + kBudget;
    // Between any two requests: the user may have cancelled, or turned the source off / changed it.
    const auto checkpoint = [&] {
        ct.throwIfCancellationRequested();
        if (generation() != snap.generation) throw AltSourceError("backup source changed");
    };
    std::string errors;
    for (const auto& instance : snap.order) {
        checkpoint();
        if (!errors.empty() && Clock::now() >= walkEnd) {
            errors += "; out of time";
            break;
        }
        try {
            Served s = fetchStream(snap.kind, instance, videoId, allowWebm, lowQuality, ct, walkEnd, checkpoint);
            s.generation = snap.generation;
            noteResult(Op::Stream, snap, instance, Outcome::Ok);
            return s;
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            if (generation() != snap.generation) throw AltSourceError("backup source changed");
            ST_LOG_WARN("altsource", "{} stream {} via {}: {}", kindName(snap.kind), videoId, hostOf(instance), e.what());
            if (videoGone(e.what()))   // about the video, not the instance: no demotion, no other instance
                throw AltSourceError(std::format("{}: video unavailable ({})", kindName(snap.kind), e.what()));
            noteResult(Op::Stream, snap, instance, failureOf(e));
            errors += std::format("{}{}: {}", errors.empty() ? "" : "; ", hostOf(instance), e.what());
        }
    }
    throw AltSourceError(std::format("{}: no instance delivered {} ({})", kindName(snap.kind), videoId, errors.empty() ? "no instances" : errors));
}

std::vector<SearchItem> AltSource::search(const std::string& query, bool music, const CT& ct) {
    const Snapshot snap = snapshot(Op::Search);
    if (snap.kind == Kind::Off) throw AltSourceError("backup source is off");
    const auto walkEnd = Clock::now() + kBudget;
    std::string errors;
    for (const auto& instance : snap.order) {
        ct.throwIfCancellationRequested();
        if (generation() != snap.generation) throw AltSourceError("backup source changed");
        if (!errors.empty() && Clock::now() >= walkEnd) {
            errors += "; out of time";
            break;
        }
        try {
            auto items = fetchSearch(snap.kind, instance, query, music, ct, walkEnd);
            noteResult(Op::Search, snap, instance, Outcome::Ok);
            ST_LOG_INFO("altsource", "{} search \"{}\" via {}: {} result(s)", kindName(snap.kind), query, hostOf(instance), items.size());
            return items;
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            if (generation() != snap.generation) throw AltSourceError("backup source changed");
            noteResult(Op::Search, snap, instance, failureOf(e));
            ST_LOG_WARN("altsource", "{} search via {}: {}", kindName(snap.kind), hostOf(instance), e.what());
            errors += std::format("{}{}: {}", errors.empty() ? "" : "; ", hostOf(instance), e.what());
        }
    }
    throw AltSourceError(std::format("{} search failed ({})", kindName(snap.kind), errors.empty() ? "no instances" : errors));
}

} // namespace st::youtube::alt
