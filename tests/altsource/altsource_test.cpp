// Developer test for youtube/AltSource (Piped / Invidious backup audio source) and its MatchService integration.
//   altsource_test                     offline: saved responses (fixtures/), stream choice, container sniffing, URLs
//   altsource_test mock                + a local mock instance speaking the Piped and Invidious APIs (backed by this
//                                        process's YoutubeExplode; media proxied with Range like the instances'
//                                        proxies): AltSource end to end, then MatchService with YoutubeExplode
//                                        bypassed (test switch) and with YouTube made unreachable (no switch)
//   altsource_test live [kind:url...]  + the public built-in instances (or the given ones, e.g.
//                                        piped:https://pipedapi.example.org): stream + 256 KB Range download +
//                                        container check, search, and MatchService through them; prints which worked
//   altsource_test serve [port]        only run the mock instance (app check: altSourceInstance=http://127.0.0.1:port)
// Runs in its own profile: SHADETUBE_DATA_DIR = <exe dir>\altsource-profile unless already set.
//
// Fixtures: piped_search_*.json, piped_streams_muxed_only.json, piped_streams_error.json, invidious_search.json and
// invidious_video_error.json are real responses (api.piped.private.coffee / invidious.f5.si, 2026-09-28, trimmed).
// piped_streams.json and invidious_video.json follow the backends' serializers field by field (Piped-Backend
// PipedStream.java, Invidious jsonify/api_v1/video_json.cr): no public instance served streams that day.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/Http.h"
#include "core/Log.h"
#include "youtube/AltSource.h"
#include "youtube/MatchService.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace yte = YoutubeExplode;
namespace alt = st::youtube::alt;
using json = nlohmann::json;

// Friend of AltSource (see AltSource.h): records results and narrows the instance list for deterministic tests.
namespace st::youtube::alt {
struct AltSourceTestAccess {
    static AltSource::Snapshot snapshot(AltSource& s) { return s.snapshot(AltSource::Op::Stream); }
    static void succeeded(AltSource& s, const AltSource::Snapshot& snap, const std::string& instance) {
        s.noteResult(AltSource::Op::Stream, snap, instance, AltSource::Outcome::Ok);
    }
    static void useBuiltins(AltSource& s, bool on) {
        std::lock_guard lock(s.mutex_);
        s.useBuiltins_ = on;
    }
};
} // namespace st::youtube::alt
using alt::AltSourceTestAccess;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

constexpr const char* kVideo = "kJQP7kiw5Fk";   // Luis Fonsi - Despacito ft. Daddy Yankee
constexpr const char* kRick = "dQw4w9WgXcQ";    // served (muxed only) by public Piped instances on 2026-09-28

std::string fixture(const char* name) {
    std::ifstream f(fs::path(ST_ALTSOURCE_FIXTURES) / name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().empty()) std::printf("  !! fixture %s missing\n", name);
    return ss.str();
}

template <class F>
bool throwsAltError(F&& f, const char* mustContain = nullptr) {
    try {
        f();
    } catch (const alt::AltSourceError& e) {
        std::printf("        threw: %s\n", e.what());
        return !mustContain || std::string(e.what()).find(mustContain) != std::string::npos;
    }
    return false;
}

yte::YoutubeClient youtubeClient(std::shared_ptr<yte::Http::IHttpClient> http = st::http::client()) {
    yte::YoutubeClientOptions o;
    o.httpClient = std::move(http);
    return yte::YoutubeClient(o);
}

st::catalog::Track despacito() {
    st::catalog::Track t;
    t.name = "Despacito";
    t.artists = {{"", "Luis Fonsi"}, {"", "Daddy Yankee"}};
    t.album.name = "VIDA";
    t.durationMs = 229360;
    return t;
}

st::catalog::Track rick() {
    st::catalog::Track t;
    t.name = "Never Gonna Give You Up";
    t.artists = {{"", "Rick Astley"}};
    t.album.name = "Whenever You Need Somebody";
    t.durationMs = 213573;
    return t;
}

// ---- offline ----------------------------------------------------------------------------------------------------

void testBasics() {
    std::printf("[kinds / instance addresses / sniffing]\n");
    CHECK(alt::parseKind("piped") == alt::Kind::Piped);
    CHECK(alt::parseKind("invidious") == alt::Kind::Invidious);
    CHECK(alt::parseKind("off") == alt::Kind::Off && alt::parseKind("") == alt::Kind::Off && alt::parseKind("x") == alt::Kind::Off);
    CHECK(std::string(alt::kindName(alt::Kind::Invidious)) == "invidious");
    CHECK(alt::normalizeInstance("pipedapi.example.org/") == "https://pipedapi.example.org");
    CHECK(alt::normalizeInstance("  HTTPS://Inv.Example.org//  ") == "https://Inv.Example.org");
    CHECK(alt::normalizeInstance("http://127.0.0.1:8765") == "http://127.0.0.1:8765");
    CHECK(alt::normalizeInstance("https://example.org/piped-api") == "https://example.org/piped-api");
    CHECK(!alt::normalizeInstance(""));
    CHECK(!alt::normalizeInstance("ftp://example.org"));
    CHECK(!alt::normalizeInstance("https://exa mple.org"));
    CHECK(!alt::normalizeInstance("https://example.org/?x=1"));
    CHECK(!alt::normalizeInstance("https:///path"));
    CHECK(alt::hostOf("https://pipedapi.example.org/streams/x") == "pipedapi.example.org");
    CHECK(alt::sniffContainer(std::string("\0\0\0\x18" "ftypdash", 12)) == alt::Container::Mp4);
    CHECK(alt::sniffContainer(std::string("\x1A\x45\xDF\xA3\x9F\x42\x86\x81", 8)) == alt::Container::Webm);
    CHECK(alt::sniffContainer("<!DOCTYPE html>") == alt::Container::Unknown);
    CHECK(alt::sniffContainer("") == alt::Container::Unknown);
    CHECK(alt::containerOf("audio/webm; codecs=\"opus\"") == alt::Container::Webm);
    CHECK(alt::containerOf("audio/mp4") == alt::Container::Mp4);
    CHECK(alt::containerOf("audio/3gpp") == alt::Container::Unknown);

    alt::AltSource src;
    CHECK(!src.enabled() && src.instanceOrder().empty());
    src.configure(alt::Kind::Piped, "my.piped.example/");
    const auto order = src.instanceOrder();
    CHECK(src.enabled() && !order.empty() && order.front() == "https://my.piped.example");
    CHECK(order.size() == 1 + alt::AltSource::builtinInstances(alt::Kind::Piped).size());
    src.configure(alt::Kind::Invidious, "");
    CHECK(src.instanceOrder() == alt::AltSource::builtinInstances(alt::Kind::Invidious));
    src.configure(alt::Kind::Piped, "not a url");   // ignored (logged): built-in list
    CHECK(src.instanceOrder() == alt::AltSource::builtinInstances(alt::Kind::Piped));

    std::printf("[a replaced own instance is never contacted again]\n");
    const auto& builtins = alt::AltSource::builtinInstances(alt::Kind::Piped);
    alt::AltSource own;
    own.configure(alt::Kind::Piped, "https://a.example");
    AltSourceTestAccess::succeeded(own, AltSourceTestAccess::snapshot(own), "https://a.example");
    CHECK(own.instanceOrder().front() == "https://a.example");
    own.configure(alt::Kind::Piped, "");
    CHECK(own.instanceOrder() == builtins);
    own.configure(alt::Kind::Piped, "https://a.example");
    const auto before = AltSourceTestAccess::snapshot(own);   // a walk in flight...
    own.configure(alt::Kind::Piped, "https://b.example");
    AltSourceTestAccess::succeeded(own, before, "https://a.example");   // ...reporting after the change: ignored
    const auto ownOrder = own.instanceOrder();
    CHECK(ownOrder.front() == "https://b.example" && std::find(ownOrder.begin(), ownOrder.end(), "https://a.example") == ownOrder.end());
    AltSourceTestAccess::succeeded(own, AltSourceTestAccess::snapshot(own), builtins.back());   // a built-in stays valid
    CHECK(own.instanceOrder().size() > 1 && own.instanceOrder()[1] == builtins.back());

    std::printf("[configuration generation]\n");
    const uint64_t g0 = own.generation();
    own.configure(alt::Kind::Piped, "https://b.example");
    CHECK(own.generation() == g0);   // unchanged settings
    own.configure(alt::Kind::Invidious, "https://b.example");
    CHECK(own.generation() == g0 + 1);
    own.configure(alt::Kind::Off, "");
    CHECK(own.generation() == g0 + 2 && !own.enabled());

    std::printf("[stream URLs the engine may fetch]\n");
    CHECK(alt::acceptableStreamUrl("https://pipedapi.example.org/videoplayback?x", "https://pipedapi.example.org"));
    CHECK(alt::acceptableStreamUrl("https://proxy.example.org/videoplayback?x", "https://api.example.org", "https://proxy.example.org"));
    CHECK(!alt::acceptableStreamUrl("https://proxy.example.org/videoplayback", "https://api.example.org"));   // not declared
    CHECK(!alt::acceptableStreamUrl("http://pipedapi.example.org/videoplayback", "https://pipedapi.example.org"));   // downgrade
    CHECK(!alt::acceptableStreamUrl("http://192.168.1.1/apply.cgi", "https://api.example.org", "http://192.168.1.1"));
    CHECK(!alt::acceptableStreamUrl("https://192.168.1.1/x", "https://api.example.org", "https://192.168.1.1"));   // LAN proxy
    CHECK(!alt::acceptableStreamUrl("https://127.0.0.1/x", "https://api.example.org"));
    CHECK(!alt::acceptableStreamUrl("https://rr3---sn-x.googlevideo.com/videoplayback", "https://inv.example.org"));
    CHECK(!alt::acceptableStreamUrl("https://api.example.org@evil.example/x", "https://api.example.org"));
    CHECK(!alt::acceptableStreamUrl("ftp://api.example.org/x", "https://api.example.org"));
    CHECK(alt::acceptableStreamUrl("http://127.0.0.1:8765/pmedia/1", "http://127.0.0.1:8765"));   // the user's LAN instance
    CHECK(alt::acceptableStreamUrl("http://127.0.0.1:8766/p", "http://127.0.0.1:8765", "http://127.0.0.1:8766"));
    CHECK(!alt::acceptableStreamUrl("http://127.0.0.1:8766/p", "http://127.0.0.1:8765"));   // another port, not declared
}

void printStreams(const alt::VideoInfo& v) {
    for (const auto& s : v.streams)
        std::printf("        itag %3d %-10s %-10s %4d kbps %9lld B%s%s  %s\n", s.itag, s.mimeType.c_str(), s.codec.c_str(),
                    s.bitrate / 1000, static_cast<long long>(s.contentLength), s.muxed ? " muxed" : "",
                    s.dubbed ? " dubbed" : "", s.url.substr(0, 48).c_str());
}

void testPipedParsing() {
    std::printf("[piped: /streams]\n");
    const auto v = alt::parsePipedStreams(fixture("piped_streams.json"), "https://pipedapi.example.org");
    printStreams(v);
    CHECK(v.title == "Luis Fonsi - Despacito ft. Daddy Yankee" && v.uploader == "Luis Fonsi" && v.durationSec == 282);
    CHECK(v.streams.size() == 8);   // 7 audio (2 dubbed) + muxed itag 18; video-only dropped
    CHECK(v.proxyUrl == "https://pipedproxy.example.org");
    CHECK(std::all_of(v.streams.begin(), v.streams.end(), [&](const auto& s) {
        return alt::acceptableStreamUrl(s.url, "https://pipedapi.example.org", v.proxyUrl);
    }));
    const auto* opus = alt::pickStream(v.streams, true, false);
    CHECK(opus && opus->itag == 251 && !opus->dubbed && opus->mimeType == "audio/webm" && opus->codec == "opus");
    CHECK(opus && opus->contentLength == 4661289 && opus->url.find("pipedproxy.example.org") != std::string::npos);
    const auto* aac = alt::pickStream(v.streams, false, false);
    CHECK(aac && aac->itag == 140 && !aac->dubbed && aac->mimeType == "audio/mp4" && aac->codec == "mp4a.40.2");
    const auto* low = alt::pickStream(v.streams, false, true);
    CHECK(low && low->itag == 140 && low->bitrate == 130516);
    CHECK(std::count_if(v.streams.begin(), v.streams.end(), [](const auto& s) { return s.dubbed; }) == 2);

    std::vector<alt::AudioStream> onlyMuxed;
    for (const auto& s : v.streams)
        if (s.muxed) onlyMuxed.push_back(s);
    const auto* mux = alt::pickStream(onlyMuxed, true, false);
    CHECK(mux && mux->itag == 18 && mux->mimeType == "audio/mp4");

    std::printf("[piped: real responses]\n");
    // No audioStreams (YouTube blocked the instance's audio formats); videoStreams = LBRY mp4, LBRY HLS and a proxied
    // itag 18 with contentLength -1: only the itag 18 is taken, and the length must come from the probe.
    const auto rick = alt::parsePipedStreams(fixture("piped_streams_muxed_only.json"), "https://api.piped.private.coffee");
    printStreams(rick);
    CHECK(rick.title.find("Never Gonna Give You Up") != std::string::npos && rick.durationSec == 213);
    CHECK(rick.streams.size() == 1 && rick.streams[0].muxed && rick.streams[0].itag == 18 && rick.streams[0].contentLength == 0);
    CHECK(!rick.streams.empty() && rick.streams[0].url.starts_with("https://proxy.piped.private.coffee/videoplayback?"));
    CHECK(!rick.streams.empty() && alt::acceptableStreamUrl(rick.streams[0].url, "https://api.piped.private.coffee", rick.proxyUrl));
    const auto* muxedPick = alt::pickStream(rick.streams, true, false);
    CHECK(muxedPick && muxedPick->itag == 18 && muxedPick->mimeType == "audio/mp4");
    CHECK(throwsAltError([] { alt::parsePipedStreams(fixture("piped_streams_error.json"), "x"); }, "SignInConfirmNotBot"));
    CHECK(throwsAltError([] { alt::parsePipedStreams("<!DOCTYPE html><html>", "x"); }, "not a JSON"));

    std::printf("[piped: /search]\n");
    const auto songs = alt::parsePipedSearch(fixture("piped_search_music_songs.json"));
    for (const auto& s : songs)
        std::printf("        %s  %-40s %-14s %d s\n", s.videoId.c_str(), s.title.c_str(), s.channel.c_str(), s.durationSec);
    CHECK(songs.size() == 6);
    CHECK(!songs.empty() && songs[0].videoId == "FXovf5dsRTw" && songs[0].title == "Despacito" && songs[0].channel == "Luis Fonsi" &&
          songs[0].durationSec == 229 && songs[0].channelId == "UCk91oFc2hY2CdHirU93baLg");
    const auto videos = alt::parsePipedSearch(fixture("piped_search_videos.json"));
    CHECK(!videos.empty() && videos[0].videoId == kVideo && videos[0].durationSec == 282);
    CHECK(throwsAltError([] { alt::parsePipedSearch(R"({"error":"boom"})"); }, "boom"));
}

void testInvidiousParsing() {
    std::printf("[invidious: /api/v1/videos]\n");
    const auto v = alt::parseInvidiousVideo(fixture("invidious_video.json"), "https://inv.example.org");
    printStreams(v);
    CHECK(v.title == "Luis Fonsi - Despacito ft. Daddy Yankee" && v.uploader == "Luis Fonsi" && v.durationSec == 282);
    CHECK(v.streams.size() == 8);   // 7 audio (2 dubbed by xtags) + formatStreams itag 18
    CHECK(!v.streams.empty() && v.streams[0].url.starts_with("https://inv.example.org/companion/videoplayback?"));
    CHECK(std::all_of(v.streams.begin(), v.streams.end(), [](const auto& s) {
        return alt::acceptableStreamUrl(s.url, "https://inv.example.org");
    }));
    const auto* opus = alt::pickStream(v.streams, true, false);
    CHECK(opus && opus->itag == 251 && opus->bitrate == 141592 && !opus->dubbed && opus->codec == "opus" && opus->contentLength == 4661289);
    const auto* aac = alt::pickStream(v.streams, false, false);
    CHECK(aac && aac->itag == 140 && aac->bitrate == 130516 && aac->codec == "mp4a.40.2");
    CHECK(std::count_if(v.streams.begin(), v.streams.end(), [](const auto& s) { return s.dubbed; }) == 2);
    CHECK(std::count_if(v.streams.begin(), v.streams.end(), [](const auto& s) { return s.muxed && s.itag == 18; }) == 1);
    CHECK(throwsAltError([] { alt::parseInvidiousVideo(fixture("invidious_video_error.json"), "x"); }, "companion"));

    std::printf("[invidious: /api/v1/search]\n");
    const auto items = alt::parseInvidiousSearch(fixture("invidious_search.json"));
    for (const auto& s : items)
        std::printf("        %s  %-50s %-14s %d s\n", s.videoId.c_str(), s.title.c_str(), s.channel.c_str(), s.durationSec);
    CHECK(items.size() == 5);
    CHECK(!items.empty() && items[0].videoId == kVideo && items[0].channel == "Luis Fonsi" && items[0].durationSec == 282 &&
          items[0].channelId == "UCxoq-PAQeAdk_zyg8YS0JqA");
    CHECK(throwsAltError([] { alt::parseInvidiousSearch(R"({"error":"Endpoint disabled"})"); }, "Endpoint disabled"));
}

// The backup search feeds TrackMatcher's own scorer: the real Piped music results must rank the album track first.
void testRanking() {
    std::printf("[ranking backup search results with TrackMatcher]\n");
    std::vector<yte::Search::VideoSearchResult> results;
    for (const auto& s : alt::parsePipedSearch(fixture("piped_search_music_songs.json")))
        results.emplace_back(yte::Videos::VideoId(s.videoId), s.title, yte::Common::Author(s.channelId, s.channel),
                             s.durationSec > 0 ? std::optional<yte::TimeSpan>(std::chrono::seconds(s.durationSec)) : std::nullopt,
                             yte::Common::Thumbnail::getDefaultSet(s.videoId));
    yte::Music::TrackQuery q{"Despacito", {"Luis Fonsi", "Daddy Yankee"}, std::chrono::milliseconds(229360), "VIDA"};
    const auto ranked = yte::Music::TrackMatcher(youtubeClient()).rank(q, results);
    for (const auto& c : ranked) std::printf("        %5.1f  %s  %s\n", c.score, c.video.id().value().c_str(), c.video.title().c_str());
    CHECK(!ranked.empty() && ranked[0].score >= 60);
    CHECK(!ranked.empty() && ranked[0].video.title() == "Despacito");
    CHECK(ranked.size() >= 2 && ranked.back().video.title() != "Despacito");   // the remix / other song rank lower
}

// ---- mock instance ------------------------------------------------------------------------------------------------

// Speaks just enough of the Piped (/streams, /search) and Invidious (/api/v1/videos, /api/v1/search) APIs, backed by
// YoutubeExplode. Stream URLs point back here and are proxied to googlevideo with the Range the client asked for,
// like the instances' own proxies. Piped URLs (/pmedia/<n>) copy piped-proxy's quirks seen on 2026-09-28: the
// Content-Range total is the chunk size and itag 18 has contentLength -1 (the real length only comes from HEAD).
// Invidious URLs (/media/<n>) are relative, like local=true behind a companion.
// Misbehaving variants (path prefixes of the instance URL, e.g. http://127.0.0.1:port/drip):
//   /drip      answers one byte per second            /huge, /huge2  6 MB search answer (with / without length)
//   /norange   stream URLs ignore Range (64 MB, 200)  /evil          stream URLs on other hosts
//   /slow      /streams answers after 3 s             /refuse=<id>   that video fails like "SignInConfirmNotBot"
class MockInstance {
public:
    explicit MockInstance(yte::YoutubeClient yt) : yt_(std::move(yt)) {}
    ~MockInstance() { stop(); }

    bool start(uint16_t port) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (listen_ == INVALID_SOCKET || bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(listen_, 64) != 0)
            return false;
        int len = sizeof a;
        getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        acceptThread_ = std::thread([this, s = listen_] { acceptLoop(s); });
        return true;
    }

    void stop() {
        stopping_ = true;
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (acceptThread_.joinable()) acceptThread_.join();
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            workers.swap(workers_);
        }
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    int mediaRequests() const { return mediaHits_.load(); }
    int64_t floodedBytes() const { return flooded_.load(); }

private:
    struct Response {
        int status = 200;
        std::string type = "application/json";
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
        int64_t length = -1;   // Content-Length of a HEAD answer (-1 = body size)
    };
    struct Media {
        std::string url;
        int64_t length = 0;
        std::string mime;
    };

    void acceptLoop(SOCKET listener) {
        for (;;) {
            const SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;   // closed by stop()
            std::lock_guard lock(mutex_);
            workers_.emplace_back([this, c] {
                serve(c);
                closesocket(c);
            });
        }
    }

    static std::string reason(int status) {
        switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 404: return "Not Found";
        case 416: return "Range Not Satisfiable";
        case 502: return "Bad Gateway";
        default: return "Error";
        }
    }

    void serve(SOCKET c) {
        const DWORD timeout = 20000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0 || req.size() > 65536) return;
            req.append(buf, static_cast<size_t>(n));
        }
        std::istringstream lines(req);
        std::string method, target, line, range;
        lines >> method >> target;
        std::getline(lines, line);
        while (std::getline(lines, line) && line != "\r") {
            std::string lower = line;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower.starts_with("range:")) range = line.substr(line.find_first_not_of(' ', 6), std::string::npos);
        }
        if (!range.empty() && range.back() == '\r') range.pop_back();
        if (target.starts_with("/drip/")) return drip(c);
        if (target.starts_with("/huge2/")) return unbounded(c);
        if (target.starts_with("/nrmedia/")) return flood(c);
        Response r;
        try {
            r = route(target, range, method == "HEAD");
        } catch (const std::exception& e) {
            r = {500, "application/json", json{{"error", std::string("mock: ") + e.what()}}.dump(), {}};
        }
        std::string head = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n",
                                       r.status, reason(r.status), r.type,
                                       r.length >= 0 ? r.length : static_cast<int64_t>(r.body.size()));
        for (const auto& [k, v] : r.headers) head += k + ": " + v + "\r\n";
        head += "\r\n";
        if (method != "HEAD") head += r.body;
        for (size_t off = 0; off < head.size();) {
            const int n = send(c, head.data() + off, static_cast<int>(std::min<size_t>(head.size() - off, 1 << 20)), 0);
            if (n <= 0) return;
            off += static_cast<size_t>(n);
        }
    }

    static bool sendAll(SOCKET c, std::string_view data) {
        for (size_t off = 0; off < data.size();) {
            const int n = send(c, data.data() + off, static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20)), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool pause(int ms) {   // false once stopping
        for (int waited = 0; waited < ms && !stopping_; waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return !stopping_;
    }

    // A tarpit: headers at once, then one byte per second.
    void drip(SOCKET c) {
        if (!sendAll(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n{")) return;
        for (int i = 0; i < 40 && pause(1000); ++i)
            if (!sendAll(c, " ")) return;
    }

    // A JSON answer without Content-Length that never ends before 8 MB.
    void unbounded(SOCKET c) {
        if (!sendAll(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"items\":[")) return;
        const std::string pad(1 << 20, ' ');
        for (int i = 0; i < 8 && !stopping_; ++i)
            if (!sendAll(c, pad)) return;
    }

    // A stream server that ignores Range: 200 and 64 MB of "MP4".
    void flood(SOCKET c) {
        constexpr int64_t total = 64ll << 20;
        if (!sendAll(c, std::format("HTTP/1.1 200 OK\r\nContent-Type: audio/mp4\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", total)))
            return;
        std::string chunk(256 << 10, '\0');
        chunk.replace(0, 12, std::string("\0\0\0\x18" "ftypdash", 12));
        for (int64_t sent = 0; sent < total && !stopping_; sent += static_cast<int64_t>(chunk.size())) {
            if (!sendAll(c, chunk)) return;
            flooded_ += static_cast<int64_t>(chunk.size());
        }
    }

    static std::string decode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') out += ' ';
            else if (s[i] == '%' && i + 2 < s.size()) {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else out += s[i];
        }
        return out;
    }

    static std::string param(const std::string& query, const std::string& name) {
        std::istringstream ss(query);
        std::string kv;
        while (std::getline(ss, kv, '&'))
            if (kv.starts_with(name + "=")) return decode(kv.substr(name.size() + 1));
        return {};
    }

    Response route(const std::string& target, const std::string& range, bool head) {
        const auto q = target.find('?');
        std::string path = target.substr(0, q);
        const std::string query = q == std::string::npos ? "" : target.substr(q + 1);
        // Variant prefixes. /muxedonly: no audio-only formats, like the public Piped instances on 2026-09-28.
        bool muxedOnly = false, huge = false, norange = false, evil = false;
        std::string refuse;
        for (;;) {
            if (path.starts_with("/muxedonly/")) muxedOnly = true, path = path.substr(10);
            else if (path.starts_with("/huge/")) huge = true, path = path.substr(5);
            else if (path.starts_with("/norange/")) norange = true, path = path.substr(8);
            else if (path.starts_with("/evil/")) evil = true, path = path.substr(5);
            else if (path.starts_with("/slow/")) path = path.substr(5), pause(3000);
            else if (path.starts_with("/refuse=")) {
                const auto end = path.find('/', 1);
                refuse = path.substr(8, end - 8);
                path = path.substr(end);
            } else break;
        }
        if (huge) return {200, "application/json", "{\"items\":[" + std::string(6 << 20, ' ') + "]}", {}};
        const bool streams = path.starts_with("/streams/"), videos = path.starts_with("/api/v1/videos/");
        const std::string id = streams ? path.substr(9) : videos ? path.substr(15) : std::string{};
        if (!refuse.empty() && id == refuse)
            return {500, "application/json",
                    R"({"error":"org.schabi.newpipe.extractor.exceptions.SignInConfirmNotBotException: mock refuses this video"})", {}};
        if (evil && streams)
            return {200, "application/json",
                    json{{"title", "x"}, {"proxyUrl", "https://pipedproxy.evil.example"}, {"videoStreams", json::array()},
                         {"audioStreams", json::array({
                              {{"url", "http://192.168.1.1/videoplayback?itag=140"}, {"mimeType", "audio/mp4"}, {"codec", "mp4a.40.2"},
                               {"bitrate", 130000}, {"contentLength", 1000}, {"itag", 140}},
                              {{"url", "https://elsewhere.example/videoplayback?itag=251"}, {"mimeType", "audio/webm"}, {"codec", "opus"},
                               {"bitrate", 140000}, {"contentLength", 1000}, {"itag", 251}}})}}
                        .dump(),
                    {}};
        if (streams) return {200, "application/json", pipedStreams(id, muxedOnly, norange).dump(), {}};
        if (videos) return {200, "application/json", invidiousVideo(id, muxedOnly).dump(), {}};
        if (path == "/search") return {200, "application/json", search(param(query, "q"), true).dump(), {}};
        if (path == "/api/v1/search") return {200, "application/json", search(param(query, "q"), false).dump(), {}};
        if (path.starts_with("/media/")) return media(std::stoi(path.substr(7)), range, head, false);
        if (path.starts_with("/pmedia/")) return media(std::stoi(path.substr(8)), range, head, true);
        return {404, "application/json", R"({"error":"not found"})", {}};
    }

    int addMedia(const std::string& url, int64_t length, const std::string& mime) {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < media_.size(); ++i)
            if (media_[i].url == url) return static_cast<int>(i);
        media_.push_back({url, length, mime});
        return static_cast<int>(media_.size() - 1);
    }

    struct Entry {
        std::shared_ptr<const yte::Videos::Streams::IAudioStreamInfo> s;
        bool muxed;
    };
    std::vector<Entry> streamsOf(const std::string& id, bool muxedOnly, json& meta) {
        const auto manifest = yt_.videos().streams().getManifest(id);
        std::vector<Entry> out;
        if (muxedOnly) {
            // YouTube does not offer its muxed itag 18 to every client: an audio-only mp4 stands in for it (same
            // container), listed where the instances list itag 18.
            for (const auto& s : manifest.getAudioOnlyStreams())
                if (s->container().name() == "mp4" && out.empty()) out.push_back({s, true});
        } else {
            for (const auto& s : manifest.getAudioOnlyStreams()) out.push_back({s, false});
            for (const auto& s : manifest.getMuxedStreams())
                if (s->itag() == 18) out.push_back({s, true});
        }
        try {
            const auto v = yt_.videos().get(id);
            meta = {{"title", v.title()}, {"author", v.author().channelTitle()},
                    {"length", v.duration() ? std::chrono::duration_cast<std::chrono::seconds>(*v.duration()).count() : 0}};
        } catch (const std::exception&) {
            meta = {{"title", ""}, {"author", ""}, {"length", 0}};
        }
        return out;
    }

    json pipedStreams(const std::string& id, bool muxedOnly, bool norange) {
        json meta;
        json audio = json::array(), video = json::array();
        for (const auto& [s, muxed] : streamsOf(id, muxedOnly, meta)) {
            const bool webm = s->container().name() == "webm";
            const std::string mime = muxed ? "video/mp4" : webm ? "audio/webm" : "audio/mp4";
            const int n = addMedia(s->url(), s->size().bytes(), muxed ? "video/mp4" : mime);
            const auto def = s->isAudioLanguageDefault();
            const int64_t bps = s->bitrate().bitsPerSecond();
            json e = {{"url", base() + (norange ? "/nrmedia/" : "/pmedia/") + std::to_string(n)},
                      {"format", muxed ? "MPEG_4" : webm ? "WEBMA_OPUS" : "M4A"},
                      {"quality", std::to_string(bps / 1000) + " kbps"}, {"mimeType", mime}, {"codec", s->audioCodec()},
                      {"audioTrackId", nullptr}, {"audioTrackName", nullptr},
                      {"audioTrackType", def ? json(*def ? "ORIGINAL" : "DUBBED") : json(nullptr)}, {"audioTrackLocale", nullptr},
                      {"videoOnly", false}, {"itag", muxed ? 18 : s->itag()}, {"bitrate", bps}, {"initStart", 0}, {"initEnd", 0},
                      {"indexStart", 0}, {"indexEnd", 0}, {"width", 0}, {"height", 0}, {"fps", 0},
                      {"contentLength", muxed ? -1 : s->size().bytes()}};
            (muxed ? video : audio).push_back(std::move(e));
        }
        return {{"title", meta["title"]}, {"uploader", meta["author"]}, {"duration", meta["length"]}, {"livestream", false},
                {"audioStreams", audio}, {"videoStreams", video}, {"proxyUrl", base()}};
    }

    json invidiousVideo(const std::string& id, bool muxedOnly) {
        json meta;
        json adaptive = json::array(), format = json::array();
        for (const auto& [s, muxed] : streamsOf(id, muxedOnly, meta)) {
            const bool webm = s->container().name() == "webm";
            const std::string mime = muxed ? "video/mp4" : webm ? "audio/webm" : "audio/mp4";
            const int n = addMedia(s->url(), s->size().bytes(), mime);
            json e = {{"url", "/media/" + std::to_string(n)},   // relative, like local=true behind a companion
                      {"itag", std::to_string(muxed ? 18 : s->itag())},
                      {"type", mime + "; codecs=\"" + s->audioCodec() + "\""},
                      {"bitrate", std::to_string(s->bitrate().bitsPerSecond())},
                      {"container", muxed ? "mp4" : webm ? "webm" : "m4a"},
                      {"encoding", webm ? "opus" : "aac"}};
            if (!muxed) e["clen"] = std::to_string(s->size().bytes());   // formatStreams have no clen
            (muxed ? format : adaptive).push_back(std::move(e));
        }
        return {{"title", meta["title"]}, {"videoId", id}, {"author", meta["author"]}, {"lengthSeconds", meta["length"]},
                {"liveNow", false}, {"adaptiveFormats", adaptive}, {"formatStreams", format}};
    }

    json search(const std::string& q, bool piped) {
        const auto results = yt_.search().getVideos(q, 15);
        json items = json::array();
        for (const auto& v : results) {
            const long long secs = v.duration() ? std::chrono::duration_cast<std::chrono::seconds>(*v.duration()).count() : -1;
            if (piped)
                items.push_back({{"url", "/watch?v=" + v.id().value()}, {"type", "stream"}, {"title", v.title()},
                                 {"uploaderName", v.author().channelTitle()}, {"uploaderUrl", "/channel/" + v.author().channelId()},
                                 {"duration", secs}, {"isShort", false}});
            else
                items.push_back({{"type", "video"}, {"title", v.title()}, {"videoId", v.id().value()},
                                 {"author", v.author().channelTitle()}, {"authorId", v.author().channelId()},
                                 {"lengthSeconds", secs < 0 ? 0 : secs}, {"liveNow", secs < 0}});
        }
        return piped ? json{{"items", items}, {"nextpage", nullptr}, {"suggestion", nullptr}, {"corrected", false}} : items;
    }

    Response media(int n, const std::string& range, bool head, bool pipedProxy) {
        Media m;
        {
            std::lock_guard lock(mutex_);
            if (n < 0 || n >= static_cast<int>(media_.size())) return {404, "text/plain", "no such stream", {}};
            m = media_[static_cast<size_t>(n)];
        }
        if (head) return {200, m.mime, "", {{"Accept-Ranges", "bytes"}}, m.length};
        ++mediaHits_;
        int64_t first = 0, last = m.length - 1;
        const bool ranged = range.starts_with("bytes=");
        if (ranged) {
            const auto dash = range.find('-');
            first = std::stoll(range.substr(6, dash - 6));
            if (dash + 1 < range.size()) last = std::min(last, std::stoll(range.substr(dash + 1)));
        }
        if (first >= m.length || first > last)
            return {416, "text/plain", "", {{"Content-Range", "bytes */" + std::to_string(m.length)}}};
        // googlevideo serves `&range=a-b` at full speed (what YouTube's players use).
        const auto r = st::http::get(m.url + "&range=" + std::to_string(first) + "-" + std::to_string(last));
        if (!r.isSuccessStatusCode()) return {502, "text/plain", std::format("upstream HTTP {}", r.statusCode), {}};
        Response out{ranged ? 206 : 200, m.mime, r.body, {{"Accept-Ranges", "bytes"}}};
        const int64_t end = first + static_cast<int64_t>(r.body.size()) - 1;
        if (ranged)
            out.headers.emplace_back("Content-Range", pipedProxy ? std::format("bytes {}-{}/{}", first, end, r.body.size())
                                                                 : std::format("bytes {}-{}/{}", first, end, m.length));
        return out;
    }

    yte::YoutubeClient yt_;
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread acceptThread_;
    std::mutex mutex_;
    std::vector<std::thread> workers_;
    std::vector<Media> media_;
    std::atomic<int> mediaHits_{0};
    std::atomic<int64_t> flooded_{0};
    std::atomic<bool> stopping_{false};
};

// YouTube made unreachable for YoutubeExplode (search, player, googlevideo); everything else goes through.
class YoutubeDown : public yte::Http::IHttpClient {
public:
    yte::Http::HttpResponse send(const yte::Http::HttpRequest& r, const yte::CancellationToken& ct) override {
        if (r.url.find("youtube.com") != std::string::npos || r.url.find("googlevideo.com") != std::string::npos ||
            r.url.find("youtu.be") != std::string::npos)
            throw yte::Exceptions::HttpRequestException("simulated outage: " + r.url.substr(0, 40), 0);
        return st::http::client()->send(r, ct);
    }
};

// ---- live checks (mock or public instances) -------------------------------------------------------------------

// GET the first 256 KB with a Range request, like audio::ProgressiveBuffer does.
bool rangeCheck(const std::string& url, const std::string& mime, int64_t contentLength, std::string& detail) {
    const auto r = st::http::get(url, {{"Range", "bytes=0-262143"}});
    const size_t want = contentLength > 0 ? static_cast<size_t>(std::min<int64_t>(contentLength, 262144)) : 262144;
    const auto got = alt::sniffContainer(r.body);
    detail = std::format("HTTP {}, {} bytes, {}", r.statusCode, r.body.size(),
                         got == alt::Container::Mp4 ? "ftyp (MP4)" : got == alt::Container::Webm ? "EBML (WebM)" : "unknown bytes");
    return r.statusCode == 206 && r.body.size() == want && got == alt::containerOf(mime) && got != alt::Container::Unknown;
}

struct InstanceReport {
    std::string kind, instance, search;
    std::vector<std::string> streams;   // per checked (video, container): "ok" / "FAIL: ..."
};
std::vector<InstanceReport> g_reports;
std::vector<std::string> g_streamColumns;

// strict: failures count as test failures (mock); otherwise they only mark the instance as not working (public).
void checkInstance(alt::Kind kind, const std::string& instance, bool strict, const std::vector<const char*>& videos) {
    std::printf("[%s %s]\n", alt::kindName(kind), instance.c_str());
    InstanceReport rep{alt::kindName(kind), instance, "-", {}};
    auto soft = [&](bool ok, const std::string& what, const std::string& detail) {
        std::printf("  %s %s  %s\n", ok ? "ok   " : (strict ? "FAIL " : "down "), what.c_str(), detail.c_str());
        if (!ok && strict) ++g_failures;
        return ok ? std::string("ok") : "FAIL: " + detail;
    };
    try {
        const auto items = alt::AltSource::searchOn(kind, instance, "Luis Fonsi - Despacito", true);
        const bool found = std::any_of(items.begin(), items.end(), [](const auto& s) { return s.title.find("Despacito") != std::string::npos; });
        rep.search = soft(found, "search", std::format("{} result(s){}", items.size(), items.empty() ? "" : ", first: " + items[0].title));
    } catch (const std::exception& e) {
        rep.search = soft(false, "search", e.what());
    }
    g_streamColumns.clear();
    for (const char* video : videos) {
        for (const bool webm : {false, true}) {
            const std::string what = std::format("{} {}", video, webm ? "opus/webm" : "aac/m4a");
            g_streamColumns.push_back(what);
            try {
                const auto t0 = std::chrono::steady_clock::now();
                const auto s = alt::AltSource::streamFrom(kind, instance, video, webm, false);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                std::string detail;
                const bool ok = rangeCheck(s.stream.url, s.stream.mimeType, s.stream.contentLength, detail);
                rep.streams.push_back(soft(ok && (webm || s.stream.mimeType == "audio/mp4"), what,
                                           std::format("itag {}{} {} {} kbps {} B in {} ms; 256 KB range: {}", s.stream.itag,
                                                       s.stream.muxed ? " (muxed)" : "", s.stream.codec, s.stream.bitrate / 1000,
                                                       s.stream.contentLength, ms, detail)));
            } catch (const std::exception& e) {
                rep.streams.push_back(soft(false, what, e.what()));
            }
        }
    }
    g_reports.push_back(std::move(rep));
}

// MatchService end to end through the backup source (`instance` empty = the built-in list).
void checkMatchService(const char* kind, const std::string& instance, bool strict, const st::catalog::Track& track) {
    std::printf("[MatchService via %s %s: \"%s\"]\n", kind, instance.empty() ? "(built-in list)" : instance.c_str(),
                track.name.c_str());
    auto expect = [&](bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok   " : (strict ? "FAIL " : "down "), what.c_str());
        if (!ok && strict) ++g_failures;
    };
    const std::string prefix = std::string(kind) + " ";
    // Strict (mock) runs never reach the public instances.
    auto setup = [&](st::youtube::MatchService& ms, const std::string& inst) {
        if (strict) AltSourceTestAccess::useBuiltins(ms.altSource(), false);
        ms.setAltSource(kind, inst);
    };

    // 1. YoutubeExplode bypassed by the test switch (what SHADETUBE_FORCE_ALT=1 does in the app).
    try {
        st::youtube::MatchService ms(youtubeClient());
        setup(ms, instance);
        ms.forceAltSource(true);
        const auto r = ms.resolve(track, true, false);
        std::string detail;
        const bool bytes = rangeCheck(r.stream.url, r.stream.mimeType, r.stream.contentLength, detail);
        expect(r.stream.source.starts_with(prefix) && bytes,
               std::format("forced: \"{}\" -> {} ({:.0f}) {} {} kbps via {}; {}", r.match.title, r.match.videoId, r.match.score,
                           r.stream.mimeType, r.stream.bitrateKbps, r.stream.source, detail));
        const auto again = ms.stream(r.match.videoId, true, false, false);
        expect(again.url == r.stream.url, "stream cache holds the backup stream");
        const auto cands = ms.candidates(track);
        expect(cands.size() >= 3, std::format("candidates via backup search: {}", cands.size()));
    } catch (const std::exception& e) {
        expect(false, std::string("forced: ") + e.what());
    }

    // 2. No switch: YouTube unreachable for YoutubeExplode -> search and stream fall back on their own.
    try {
        st::youtube::MatchService ms(youtubeClient(std::make_shared<YoutubeDown>()));
        setup(ms, instance);
        const auto r = ms.resolve(track, false, false);
        std::string detail;
        const bool bytes = rangeCheck(r.stream.url, r.stream.mimeType, r.stream.contentLength, detail);
        expect(r.stream.source.starts_with(prefix) && r.stream.mimeType == "audio/mp4" && bytes,
               std::format("YouTube down: -> {} {} via {}; {}", r.match.videoId, r.stream.mimeType, r.stream.source, detail));
    } catch (const std::exception& e) {
        expect(false, std::string("YouTube down: ") + e.what());
    }

    // 3. YouTube works, but the stream it just handed out failed in the engine (the Player re-resolves with
    //    bypassCache minutes later): the backup source is asked first.
    try {
        st::youtube::MatchService ms(youtubeClient());
        setup(ms, instance);
        const auto first = ms.stream(kVideo, false, false, false);
        const auto retry = ms.stream(kVideo, false, false, true);
        expect(first.source == "youtube" && retry.source.starts_with(prefix),
               std::format("re-resolve after a failed YouTube stream: {} -> {}", first.source, retry.source));
    } catch (const std::exception& e) {
        expect(false, std::string("re-resolve: ") + e.what());
    }
    if (!strict) return;   // the rest needs a controllable instance

    // 4. Turning the backup off drops what it served: a replay within the stream-cache lifetime goes to YouTube.
    try {
        st::youtube::MatchService ms(youtubeClient());
        setup(ms, instance);
        ms.forceAltSource(true);
        const auto viaAlt = ms.stream(kVideo, false, false, false);
        ms.setAltSource("off", "");
        const auto replay = ms.stream(kVideo, false, false, false);
        expect(viaAlt.source.starts_with(prefix) && replay.source == "youtube" && replay.url != viaAlt.url,
               std::format("backup off: cached {} stream dropped, replay via {}", viaAlt.source, replay.source));
    } catch (const std::exception& e) {
        expect(false, std::string("backup off: ") + e.what());
    }

    // 5. The persisted match fails "this time" (YouTube down, the instance refuses that video): a stand-in plays, the
    //    failed video is not tried again as a candidate, and the persisted match is kept.
    try {
        st::youtube::MatchService ms(youtubeClient(std::make_shared<YoutubeDown>()));
        setup(ms, instance);
        auto t = track;
        t.id = std::format("test:persist:{}:{}", kind, std::chrono::system_clock::now().time_since_epoch().count());
        const auto first = ms.resolve(t, false, false);
        const auto saved = ms.cachedMatch(t.id);
        setup(ms, instance + "/refuse=" + first.match.videoId);
        const auto second = ms.resolve(t, false, false);
        const auto kept = ms.cachedMatch(t.id);
        expect(saved && saved->videoId == first.match.videoId && second.match.videoId != first.match.videoId && kept &&
                   kept->videoId == first.match.videoId,
               std::format("transient failure: saved {}, played {} this time, still saved {}", saved ? saved->videoId : "-",
                           second.match.videoId, kept ? kept->videoId : "-"));
    } catch (const std::exception& e) {
        expect(false, std::string("transient failure: ") + e.what());
    }
}

// YouTube 429 with the backup on: YouTube is asked once, then left alone; without the backup the 429 surfaces at once.
class YoutubeRateLimited : public yte::Http::IHttpClient {
public:
    std::atomic<int> youtubeRequests{0};
    yte::Http::HttpResponse send(const yte::Http::HttpRequest& r, const yte::CancellationToken& ct) override {
        if (r.url.find("youtube.com") != std::string::npos || r.url.find("googlevideo.com") != std::string::npos) {
            ++youtubeRequests;
            yte::Http::HttpResponse limited;
            limited.statusCode = 429;
            return limited;
        }
        return st::http::client()->send(r, ct);
    }
};

void checkRateLimit(const std::string& instance) {
    std::printf("[YouTube 429 is service-wide]\n");
    auto http = std::make_shared<YoutubeRateLimited>();
    try {
        st::youtube::MatchService ms(youtubeClient(http));
        AltSourceTestAccess::useBuiltins(ms.altSource(), false);
        ms.setAltSource("piped", instance);
        const auto r = ms.resolve(despacito(), false, false);
        const auto other = ms.stream(kRick, false, false, false);
        std::printf("        resolved %s via %s, then %s via %s; YouTube asked %d time(s)\n", r.match.videoId.c_str(),
                    r.stream.source.c_str(), kRick, other.source.c_str(), http->youtubeRequests.load());
        CHECK(r.stream.source.starts_with("piped ") && other.source.starts_with("piped ") && http->youtubeRequests == 1);
    } catch (const std::exception& e) {
        std::printf("  FAIL  429 with backup: %s\n", e.what());
        ++g_failures;
    }
    auto http2 = std::make_shared<YoutubeRateLimited>();
    st::youtube::MatchService off(youtubeClient(http2));
    off.setAltSource("off", "");
    bool limited = false;
    try {
        off.resolve(despacito(), false, false);
    } catch (const yte::Exceptions::RequestLimitExceededException&) {
        limited = true;
    } catch (const std::exception& e) {
        std::printf("        unexpected: %s\n", e.what());
    }
    CHECK(limited && http2->youtubeRequests == 1);
}

// Misbehaving instances cost bounded time and memory; a changed setting stops a walk in flight.
void checkHardening(MockInstance& mock) {
    const std::string base = mock.base();
    auto timed = [](const std::function<void()>& f, std::string& error) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            f();
            error = "(no error)";
        } catch (const std::exception& e) {
            error = e.what();
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    std::string err;
    double secs = 0;

    std::printf("[bounded requests]\n");
    secs = timed([&] { alt::AltSource::searchOn(alt::Kind::Piped, base + "/drip", "x", false); }, err);
    std::printf("        drip (1 byte/s): %.1f s, %s\n", secs, err.c_str());
    CHECK(has(err, "timed out") && secs < 11);
    secs = timed([&] { alt::AltSource::searchOn(alt::Kind::Piped, base + "/huge", "x", false); }, err);
    std::printf("        6 MB answer with Content-Length: %.1f s, %s\n", secs, err.c_str());
    CHECK(has(err, "too large") && secs < 3);
    secs = timed([&] { alt::AltSource::searchOn(alt::Kind::Piped, base + "/huge2", "x", false); }, err);
    std::printf("        unbounded answer: %.1f s, %s\n", secs, err.c_str());
    CHECK(has(err, "too large") && secs < 5);
    secs = timed([&] { alt::AltSource::streamFrom(alt::Kind::Piped, base + "/norange", kVideo, false, false); }, err);
    std::printf("        stream ignoring Range (64 MB): %.1f s, %s; server got %lld bytes out before the close\n", secs,
                err.c_str(), static_cast<long long>(mock.floodedBytes()));
    CHECK(has(err, "ignores range") && secs < 10 && mock.floodedBytes() < (64ll << 20));

    std::printf("[stream URLs outside the instance]\n");
    secs = timed([&] { alt::AltSource::streamFrom(alt::Kind::Piped, base + "/evil", kVideo, false, false); }, err);
    std::printf("        %s\n", err.c_str());
    CHECK(has(err, "outside the instance"));

    std::printf("[a changed setting stops a walk in flight]\n");
    alt::AltSource src;
    AltSourceTestAccess::useBuiltins(src, false);
    src.configure(alt::Kind::Piped, base + "/slow");
    std::string walk;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread walker([&] {
        try {
            src.stream(kVideo, false, false);
            walk = "served";
        } catch (const std::exception& e) {
            walk = e.what();
        }
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    src.configure(alt::Kind::Off, "");
    walker.join();
    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("        %.1f s: %s\n", secs, walk.c_str());
    CHECK(has(walk, "backup source changed") && secs < 8);
}

// Without a backup source nothing changes: YouTube's own error surfaces, no instance is contacted.
void checkOffUnchanged() {
    std::printf("[MatchService with the backup source off]\n");
    st::youtube::MatchService ms(youtubeClient(std::make_shared<YoutubeDown>()));
    ms.setAltSource("off", "http://127.0.0.1:9");
    ms.forceAltSource(true);   // no effect while off
    bool youtubeError = false;
    try {
        ms.resolve(despacito(), false, false);
    } catch (const yte::Exceptions::HttpRequestException& e) {
        youtubeError = std::string(e.what()).find("simulated outage") != std::string::npos;
    } catch (const std::exception& e) {
        std::printf("        unexpected: %s\n", e.what());
    }
    CHECK(youtubeError);
}

void printReports() {
    if (g_reports.empty()) return;
    std::printf("\n%-10s %-36s %-7s", "kind", "instance", "search");
    for (const auto& c : g_streamColumns) std::printf(" %-22s", c.c_str());
    std::printf("\n");
    for (const auto& r : g_reports) {
        std::printf("%-10s %-36s %-7s", r.kind.c_str(), r.instance.c_str(), r.search.substr(0, 4).c_str());
        for (const auto& s : r.streams) std::printf(" %-22s", s.substr(0, 4).c_str());
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::string mode = argc > 1 ? argv[1] : "";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr, 0))
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (fs::path(exe).parent_path() / L"altsource-profile").c_str());
    st::log::init();

    if (mode == "serve") {
        MockInstance mock(youtubeClient());
        const int port = argc > 2 ? std::atoi(argv[2]) : 8765;
        if (!mock.start(static_cast<uint16_t>(port))) {
            std::printf("cannot listen on 127.0.0.1:%d\n", port);
            return 1;
        }
        std::printf("mock Piped/Invidious instance on %s (Ctrl+C to stop)\n", mock.base().c_str());
        std::fflush(stdout);
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }

    testBasics();
    testPipedParsing();
    testInvidiousParsing();
    testRanking();

    if (mode == "mock") {
        MockInstance mock(youtubeClient());
        if (!mock.start(0)) {
            std::printf("  FAIL  mock instance did not start\n");
            return 1;
        }
        std::printf("\nmock instance: %s\n", mock.base().c_str());
        checkInstance(alt::Kind::Piped, mock.base(), true, {kVideo});
        checkInstance(alt::Kind::Invidious, mock.base(), true, {kVideo});
        checkInstance(alt::Kind::Piped, mock.base() + "/muxedonly", true, {kVideo});   // itag 18 + HEAD for the length
        checkInstance(alt::Kind::Invidious, mock.base() + "/muxedonly", true, {kVideo});
        checkMatchService("piped", mock.base(), true, despacito());
        checkMatchService("invidious", mock.base(), true, despacito());
        checkOffUnchanged();
        checkRateLimit(mock.base());
        checkHardening(mock);
        std::printf("  (mock served %d media range request(s))\n", mock.mediaRequests());
        mock.stop();
    } else if (mode == "live") {
        std::vector<std::pair<alt::Kind, std::string>> targets;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const auto colon = a.find(':');
            const auto url = colon == std::string::npos ? std::nullopt : alt::normalizeInstance(a.substr(colon + 1));
            if (url) targets.emplace_back(alt::parseKind(a.substr(0, colon)), *url);
        }
        const bool builtin = targets.empty();
        if (builtin)
            for (const auto kind : {alt::Kind::Piped, alt::Kind::Invidious})
                for (const auto& i : alt::AltSource::builtinInstances(kind)) targets.emplace_back(kind, i);
        std::printf("\nlive instances (%s)\n", builtin ? "built-in list" : "given");
        for (const auto& [kind, url] : targets) checkInstance(kind, url, false, {kVideo, kRick});
        for (const auto& track : {despacito(), rick()}) {
            if (builtin) {
                checkMatchService("piped", "", false, track);
                checkMatchService("invidious", "", false, track);
            } else {
                for (const auto& [kind, url] : targets) checkMatchService(alt::kindName(kind), url, false, track);
            }
        }
        printReports();
    }

    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    st::log::shutdown();
    return g_failures ? 1 : 0;
}
