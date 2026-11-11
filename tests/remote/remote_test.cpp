// remote_test: the phone remote control's standalone parts (app/RemoteHttp, RemoteAuth, RemoteApi, RemoteState,
// RemoteServer).
//
//   http     request parser: complete / byte-by-byte / pipelined input, bare LF, limits (431 / 414 / 413), malformed
//            requests (400 / 501 / 505), keep-alive rules, query strings, response serialization
//   sse      Server-Sent Events framing (multi-line data, CR / LF / CRLF, names, comments)
//   auth     random tokens, SHA-256 / base64url vectors, constant-time compare, User-Agent names, the DPAPI device store
//            (pairing, good / bad / rotated / revoked tokens, persistence, the device limit), the throttle
//   router   static assets + ETags, Host check, pairing, tokens, throttling (429), command / query validation, async
//            calls, streams, unpair, theme.css
//   state    state / queue / position JSON, cover mapping
//   lan      adapter ranking (physical over virtual, gateways)
//   live     a real Server on 127.0.0.1: WinHTTP pairing + state + commands + keep-alive, raw sockets for partial,
//            pipelined, slow (408) and oversized requests, the event stream (initial state, broadcasts, ticks,
//            heartbeats, closing), revocation, ports in use, stop
//
// Offline. Everything runs in a per-process temp folder (SHADETUBE_DATA_DIR points there too).
#include "app/RemoteApi.h"
#include "app/RemoteAuth.h"
#include "app/RemoteHttp.h"
#include "app/RemoteServer.h"
#include "app/RemoteState.h"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace st::app::remote;
using json = nlohmann::json;
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

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- http -------------------------------------------------------------------------------------------------------------

static void testHttp() {
    std::printf("http parser:\n");
    {
        const std::string raw = "GET /api/search?q=a%20b&x=1 HTTP/1.1\r\nHost: 192.168.1.2:48484\r\nUser-Agent: T\r\n"
                                "X-Mixed-Case: Value \r\n\r\n";
        Request r;
        const auto pr = parseRequest(raw, r);
        CHECK(pr.status == ParseStatus::Done);
        CHECK(pr.consumed == raw.size());
        CHECK(r.method == "GET" && r.path == "/api/search" && r.query == "q=a%20b&x=1" && r.versionMinor == 1);
        CHECK(r.headerValue("host") == "192.168.1.2:48484");
        CHECK(r.headerValue("x-mixed-case") == "Value");
        CHECK(r.param("q") == "a b" && r.param("x") == "1" && r.param("missing").empty());
        CHECK(r.keepAlive());
    }
    {   // byte by byte: incomplete until the last byte, the body included
        const std::string raw = "POST /api/cmd HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n{\"cmd\":\"x\"}";
        Request r;
        bool early = false;
        for (size_t n = 0; n < raw.size(); ++n)
            if (parseRequest(std::string_view(raw).substr(0, n), r).status != ParseStatus::Incomplete) early = true;
        CHECK(!early);
        const auto pr = parseRequest(raw, r);
        CHECK(pr.status == ParseStatus::Done && r.body == "{\"cmd\":\"x\"}");
    }
    {   // pipelined: the first request only; the rest stays for the next call
        const std::string a = "GET /a HTTP/1.1\r\nHost: h\r\n\r\n";
        const std::string b = "GET /b HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
        const std::string both = a + b;
        Request r;
        auto pr = parseRequest(both, r);
        CHECK(pr.status == ParseStatus::Done && pr.consumed == a.size() && r.path == "/a");
        pr = parseRequest(std::string_view(both).substr(pr.consumed), r);
        CHECK(pr.status == ParseStatus::Done && r.path == "/b" && !r.keepAlive());
    }
    {   // bare LF, leading empty lines, HTTP/1.0 rules
        Request r;
        CHECK(parseRequest("\r\n\r\nGET /x HTTP/1.0\nConnection: keep-alive\n\n", r).status == ParseStatus::Done);
        CHECK(r.path == "/x" && r.versionMinor == 0 && r.keepAlive());
        CHECK(parseRequest("GET /x HTTP/1.0\r\n\r\n", r).status == ParseStatus::Done && !r.keepAlive());
        CHECK(parseRequest("GET /x HTTP/1.1\r\nHost: h\r\nConnection: Keep-Alive, Close\r\n\r\n", r).status == ParseStatus::Done &&
              !r.keepAlive());
    }
    {   // limits
        Limits l;
        Request r;
        std::string big = "GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(9000, 'a') + "\r\n\r\n";
        auto pr = parseRequest(big, r, l);
        CHECK(pr.status == ParseStatus::Error && pr.error == 431);
        std::string partial = "GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(9000, 'a');   // no end in sight
        pr = parseRequest(partial, r, l);
        CHECK(pr.status == ParseStatus::Error && pr.error == 431);
        std::string many = "GET / HTTP/1.1\r\nHost: h\r\n";
        for (int i = 0; i < 60; ++i) many += "X" + std::to_string(i) + ": 1\r\n";
        pr = parseRequest(many + "\r\n", r, l);
        CHECK(pr.status == ParseStatus::Error && pr.error == 431);
        pr = parseRequest("GET /" + std::string(3000, 'a') + " HTTP/1.1\r\nHost: h\r\n\r\n", r, l);
        CHECK(pr.status == ParseStatus::Error && pr.error == 414);
        pr = parseRequest("GET /" + std::string(3000, 'a'), r, l);   // a long target still arriving
        CHECK(pr.status == ParseStatus::Error && pr.error == 414);
        pr = parseRequest("POST /api/cmd HTTP/1.1\r\nHost: h\r\nContent-Length: 999999\r\n\r\n", r, l);   // before the body
        CHECK(pr.status == ParseStatus::Error && pr.error == 413);
        CHECK(parseRequest("GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(100, 'a'), r, l).status == ParseStatus::Incomplete);
    }
    {   // malformed
        Request r;
        auto err = [&](const std::string& raw) {
            const auto pr = parseRequest(raw, r);
            return pr.status == ParseStatus::Error ? pr.error : 0;
        };
        CHECK(err("GET /\r\nHost: h\r\n\r\n") == 400);                       // no version
        CHECK(err("GET  / HTTP/1.1\r\nHost: h\r\n\r\n") == 400);             // two spaces
        CHECK(err("GET http://evil/ HTTP/1.1\r\nHost: h\r\n\r\n") == 400);   // absolute-form
        CHECK(err("GET * HTTP/1.1\r\nHost: h\r\n\r\n") == 400);
        CHECK(err("GET / HTTP/2.0\r\nHost: h\r\n\r\n") == 505);
        CHECK(err("GET / HTTX/1.1\r\nHost: h\r\n\r\n") == 400);
        CHECK(err("GET / HTTP/1.1\r\n\r\n") == 400);                         // 1.1 without Host
        CHECK(err("GET / HTTP/1.1\r\nHost : h\r\n\r\n") == 400);             // space before the colon
        CHECK(err("GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n") == 400);   // obsolete folding
        CHECK(err("GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n") == 400);
        CHECK(err("GET / HTTP/1.1\r\nHost: h\x01\r\n\r\n") == 400);          // control character
        CHECK(err("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1x\r\n\r\n") == 400);
        CHECK(err("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab") == 400);
        CHECK(err("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == 501);
        CHECK(err("GET /%zz HTTP/1.1\r\nHost: h\r\n\r\n") == 400);
        CHECK(err("GET /%00 HTTP/1.1\r\nHost: h\r\n\r\n") == 400);
        CHECK(err("GET /a#b HTTP/1.1\r\nHost: h\r\n\r\n") == 400);
        CHECK(err("G\xC3\xA9T / HTTP/1.1\r\nHost: h\r\n\r\n") == 400);       // non-token method
        CHECK(err(std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11)) == 400);   // a TLS ClientHello, at once
        CHECK(err("GETGETGETGETGETGET / HTTP/1.1\r\n") == 400);                               // method too long
    }
    {   // helpers
        std::string out;
        CHECK(percentDecode("a%2Fb+c", true, out) && out == "a/b c");
        CHECK(percentDecode("a+b", false, out) && out == "a+b");
        CHECK(!percentDecode("%4", false, out) && !percentDecode("%", false, out));
        CHECK(queryParam("q=%C3%A7ay&x", "q") == "\xC3\xA7" "ay");
        CHECK(queryParam("q=%zz", "q").empty());
        CHECK(bearerToken("Bearer abc") == "abc" && bearerToken("bearer  abc ") == "abc" && bearerToken("Basic abc").empty());
    }
    {   // responses
        Response r = Response::json(200, "{}");
        const std::string s = serialize(r, true);
        CHECK(s.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        CHECK(s.find("Content-Length: 2\r\n") != std::string::npos);
        CHECK(s.find("Cache-Control: no-store\r\n") != std::string::npos);
        CHECK(s.find("Connection: keep-alive\r\n") != std::string::npos);
        CHECK(s.size() >= 2 && s.substr(s.size() - 2) == "{}");
        r.headers.push_back({"Cache-Control", "no-cache"});
        const std::string h = serialize(r, false, true);
        CHECK(h.find("no-store") == std::string::npos && h.find("Connection: close\r\n") != std::string::npos);
        CHECK(h.substr(h.size() - 4) == "\r\n\r\n");   // HEAD: no body, but the length of the GET body
        CHECK(h.find("Content-Length: 2\r\n") != std::string::npos);
        CHECK(serialize(Response::error(404), false).find("{\"error\":\"Not Found\"}") != std::string::npos);
    }
}

// ---- sse --------------------------------------------------------------------------------------------------------------

static void testSse() {
    std::printf("sse framing:\n");
    CHECK(sseEvent("state", "{\"a\":1}") == "event: state\ndata: {\"a\":1}\n\n");
    CHECK(sseEvent("x", "a\nb") == "event: x\ndata: a\ndata: b\n\n");
    CHECK(sseEvent("x", "a\r\nb\rc") == "event: x\ndata: a\ndata: b\ndata: c\n\n");
    CHECK(sseEvent("x", "") == "event: x\ndata: \n\n");
    CHECK(sseEvent("x", "a\n") == "event: x\ndata: a\ndata: \n\n");
    CHECK(sseEvent("bad\nname", "1") == "event: badname\ndata: 1\n\n");
    CHECK(sseEvent("", "1") == "data: 1\n\n");
    CHECK(sseComment("ping") == ": ping\n\n");
    CHECK(sseComment("a\nb") == ": ab\n\n");
    CHECK(sseHead().find("Content-Type: text/event-stream") != std::string::npos);
}

// ---- auth -------------------------------------------------------------------------------------------------------------

static void testAuth() {
    std::printf("auth:\n");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const uint8_t v[] = {0xfb, 0xff, 0x00, 0x10};
    CHECK(base64Url(v, 1) == "-w" && base64Url(v, 2) == "-_8" && base64Url(v, 3) == "-_8A" && base64Url(v, 4) == "-_8AEA");
    const std::string t1 = randomToken(16), t2 = randomToken(16);
    CHECK(t1.size() == 22 && t1 != t2);
    CHECK(randomToken(32).size() == 43);
    CHECK(constantTimeEquals("abc", "abc") && !constantTimeEquals("abc", "abd") && !constantTimeEquals("abc", "ab"));

    const char* kIphoneSafari = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
                                "Version/18.1 Mobile/15E148 Safari/604.1";
    const char* kIphoneChrome = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
                                "CriOS/131.0.6778.73 Mobile/15E148 Safari/604.1";
    const char* kAndroidChrome = "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 "
                                 "Mobile Safari/537.36";
    const char* kSamsung = "Mozilla/5.0 (Linux; Android 14; SM-S921B) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "SamsungBrowser/26.0 Chrome/122.0.0.0 Mobile Safari/537.36";
    const char* kFirefox = "Mozilla/5.0 (Android 14; Mobile; rv:132.0) Gecko/132.0 Firefox/132.0";
    const char* kEdge = "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile "
                        "Safari/537.36 EdgA/131.0.0.0";
    const char* kIpad = "Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) FxiOS/132.0 "
                        "Mobile/15E148 Safari/605.1.15";
    CHECK(deviceNameFromUserAgent(kIphoneSafari) == "iPhone \xC2\xB7 Safari");
    CHECK(deviceNameFromUserAgent(kIphoneChrome) == "iPhone \xC2\xB7 Chrome");
    CHECK(deviceNameFromUserAgent(kAndroidChrome) == "Android \xC2\xB7 Chrome");
    CHECK(deviceNameFromUserAgent(kSamsung) == "Android \xC2\xB7 Samsung Internet");
    CHECK(deviceNameFromUserAgent(kFirefox) == "Android \xC2\xB7 Firefox");
    CHECK(deviceNameFromUserAgent(kEdge) == "Android \xC2\xB7 Edge");
    CHECK(deviceNameFromUserAgent(kIpad) == "iPad \xC2\xB7 Firefox");
    CHECK(deviceNameFromUserAgent("curl/8.9.1").empty());
    CHECK(deviceNameFromUserAgent("").empty());

    const fs::path file = g_dir / L"remote.dat";
    {
        DeviceStore store(file);
        store.load();
        const std::string secret = store.pairingSecret();
        CHECK(secret.size() == 22);
        CHECK(fs::exists(file));
        CHECK(!store.pair("wrong-secret-wrong-sec", "x", 100));
        CHECK(!store.pair("", "x", 100));
        auto p = store.pair(secret, "iPhone \xC2\xB7 Safari", 100);
        CHECK(p && p->token.size() == 43 && p->device.id.size() == 16 && p->device.tokenHash == sha256Hex(p->token));
        if (!p) return;
        const std::string token = p->token;
        auto d = store.authenticate(token, 200);
        CHECK(d && d->id == p->device.id && d->name == "iPhone \xC2\xB7 Safari");
        CHECK(!store.authenticate(token + "x", 200));
        CHECK(!store.authenticate("", 200));
        CHECK(!store.authenticate(std::string(43, 'A'), 200));
        // The file holds neither the secret, nor the token, nor readable JSON.
        const std::string raw = slurp(file);
        CHECK(raw.find(secret) == std::string::npos && raw.find(token) == std::string::npos);
        CHECK(raw.find("devices") == std::string::npos && raw.find(p->device.tokenHash) == std::string::npos);

        // Rotation: the old secret stops working, paired devices stay.
        store.rotateSecret();
        CHECK(store.pairingSecret() != secret && store.pairingSecret().size() == 22);
        CHECK(!store.pair(secret, "y", 300));
        CHECK(store.authenticate(token, 300));
        auto p2 = store.pair(store.pairingSecret(), "Android \xC2\xB7 Chrome", 400);
        CHECK(p2 && store.devices().size() == 2);
        CHECK(store.devices().front().name == "Android \xC2\xB7 Chrome");   // most recently seen first

        // Persistence: a second store reads the same secret and devices (and lastSeen once saved).
        store.authenticate(token, 1000);
        store.saveIfDirty(1000, 0);
        DeviceStore again(file);
        again.load();
        CHECK(again.pairingSecret() == store.pairingSecret());
        CHECK(again.devices().size() == 2);
        CHECK(again.authenticate(token, 1001) && again.authenticate(p2->token, 1001));
        bool seen = false;
        for (const auto& dev : again.devices())
            if (dev.id == p->device.id) seen = dev.lastSeen == 1000;
        CHECK(seen);

        // Revocation.
        CHECK(again.revoke(p->device.id));
        CHECK(!again.revoke(p->device.id));
        CHECK(!again.authenticate(token, 1002) && again.authenticate(p2->token, 1002));
        DeviceStore third(file);
        third.load();
        CHECK(!third.authenticate(token, 1003) && third.devices().size() == 1);

        // The device limit: the one seen longest ago goes.
        for (int i = 0; i < 20; ++i) third.pair(third.pairingSecret(), "d" + std::to_string(i), 2000 + i);
        CHECK(third.devices().size() == DeviceStore::kMaxDevices);
        CHECK(!third.authenticate(p2->token, 3000));   // seen at 1002: the oldest
    }
    {   // an unreadable file starts over with a new secret
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "garbage";
        DeviceStore store(file);
        store.load();
        CHECK(store.pairingSecret().size() == 22 && store.devices().empty());
    }
    {   // throttle
        Throttle::Config cfg;
        cfg.maxFailures = 3;
        cfg.windowMs = 1000;
        cfg.blockMs = 500;
        cfg.maxBlockMs = 1500;
        Throttle t(cfg);
        int64_t retry = 0;
        t.failure("a", 0);
        t.failure("a", 10);
        CHECK(!t.blocked("a", 20));
        t.failure("a", 20);
        CHECK(t.blocked("a", 30, &retry) && retry == 490);
        CHECK(!t.blocked("b", 30));                     // per peer
        CHECK(!t.blocked("a", 520));                    // block over
        t.failure("a", 600);
        t.failure("a", 600);
        t.failure("a", 600);
        CHECK(t.blocked("a", 1500) && !t.blocked("a", 1600));   // second block: 1000 ms
        for (int i = 0; i < 3; ++i) t.failure("a", 2000);
        CHECK(t.blocked("a", 3400) && !t.blocked("a", 3500));   // third: capped at 1500 ms
        t.success("a");
        for (int i = 0; i < 3; ++i) t.failure("a", 5000);
        CHECK(t.blocked("a", 5400) && !t.blocked("a", 5500));   // a success resets the doubling
        t.failure("c", 0);
        t.failure("c", 0);
        t.failure("c", 2000);                           // the window restarted: only one counts
        CHECK(!t.blocked("c", 2001));
    }
}

// ---- router -----------------------------------------------------------------------------------------------------------

static std::map<std::wstring, std::string> g_assets = {
    {L"remote/index.html", "<!doctype html><title>t</title>"},
    {L"remote/app.js", "console.log(1)"},
    {L"remote/app.css", "body{}"},
    {L"fonts/JetBrainsMono.ttf", "FONT"},
    {L"logo/logo-mark.svg", "<svg/>"},
    {L"icons/24/play.svg", "<svg>play</svg>"},
    {L"tokens/tokens.dark.json", R"J({"color":{"bg":{"base":"#0F0D0B"},"accent":{"onAccent":"#000000","name":"Volt","glowRgba":"rgba(1,2,3,0.18)"},"x":{"bad":"#0F0;}body{"}},"radius":{"surface":2},"effects":{"menuShadow":{"type":"shadow","blurRadius":32,"offsetX":0,"offsetY":16,"color":"#000000","opacity":0.55},"glow":{"type":"glow","color":"accent.base"}},"motion":{"easing":{"standard":"cubic-bezier(0.2, 0, 0, 1)","evil":"x;}"},"duration":{"fast":120}}})J"},
    {L"tokens/tokens.light.json", R"({"color":{"bg":{"base":"#F4EFE6"}}})"},
};

static std::string_view fakeAsset(std::wstring_view name) {
    const auto it = g_assets.find(std::wstring(name));
    return it == g_assets.end() ? std::string_view{} : std::string_view(it->second);
}

static Request req(const std::string& method, const std::string& target, std::vector<Header> headers = {},
                   std::string body = {}) {
    Request r;
    r.method = method;
    r.target = target;
    const size_t q = target.find('?');
    percentDecode(target.substr(0, q), false, r.path);
    if (q != std::string::npos) r.query = target.substr(q + 1);
    r.headers.push_back({"host", "192.168.1.20:48484"});
    for (auto& h : headers) r.headers.push_back(std::move(h));
    r.body = std::move(body);
    return r;
}

static void testRouter() {
    std::printf("router:\n");
    const fs::path file = g_dir / L"router.dat";
    std::error_code ec;
    fs::remove(file, ec);
    DeviceStore store(file);
    store.load();
    Router::Config cfg;
    cfg.asset = fakeAsset;
    cfg.strings = [] { return std::string("{\"lang\":\"tr\"}"); };
    cfg.state = [] { return std::string("{\"v\":1}"); };
    Throttle::Config tc;
    tc.maxFailures = 4;
    Router router(store, cfg, tc);
    std::vector<ApiCall> calls;
    std::vector<Reply> replies;
    router.onCall = [&](ApiCall c, Reply r) {
        calls.push_back(std::move(c));
        replies.push_back(std::move(r));
    };
    std::vector<std::string> paired, unpaired;
    router.onPaired = [&](const Device& d) { paired.push_back(d.id); };
    router.onUnpaired = [&](const Device& d) { unpaired.push_back(d.id); };
    int64_t now = 1000;
    auto run = [&](const Request& r, const std::string& peer = "192.168.1.30") {
        return router.handle(r, peer, now, 1'700'000'000, [](Response) {});
    };

    // Static files.
    auto o = run(req("GET", "/"));
    CHECK(o.kind == Outcome::Kind::Respond && o.response.status == 200 && o.response.body == g_assets[L"remote/index.html"]);
    CHECK(o.response.contentType.rfind("text/html", 0) == 0);
    std::string etag, csp;
    for (const auto& h : o.response.headers) {
        if (h.name == "ETag") etag = h.value;
        if (h.name == "Content-Security-Policy") csp = h.value;
    }
    CHECK(!etag.empty() && csp.find("default-src 'none'") != std::string::npos);
    o = run(req("GET", "/", {{"if-none-match", etag}}));
    CHECK(o.response.status == 304 && o.response.body.empty());
    CHECK(run(req("HEAD", "/app.js")).response.status == 200);
    CHECK(run(req("GET", "/app.css")).response.status == 200);
    CHECK(run(req("GET", "/fonts/JetBrainsMono.ttf")).response.contentType == "font/ttf");
    CHECK(run(req("GET", "/fonts/BricolageGrotesque.ttf")).response.status == 404);   // not in the fake assets
    CHECK(run(req("GET", "/fonts/other.ttf")).response.status == 404);
    CHECK(run(req("GET", "/icons/play.svg")).response.body == "<svg>play</svg>");
    CHECK(run(req("GET", "/icons/../tokens/tokens.dark.json")).response.status == 404);
    CHECK(run(req("GET", "/icons/%2e%2e%2fx.svg")).response.status == 404);
    CHECK(run(req("GET", "/icons/Play.svg")).response.status == 404);
    CHECK(run(req("GET", "/remote/index.html")).response.status == 404);   // only the fixed names
    CHECK(run(req("GET", "/logo.svg")).response.status == 200);
    CHECK(run(req("POST", "/")).response.status == 405);
    {
        const auto css = run(req("GET", "/theme.css")).response;
        CHECK(css.status == 200 && css.body.find("--bg-base:#0F0D0B;") != std::string::npos);
        CHECK(css.body.find("--accent-on-accent:#000000;") != std::string::npos);
        CHECK(css.body.find("--accent-glow-rgba:rgba(1,2,3,0.18);") != std::string::npos);
        CHECK(css.body.find("Volt") == std::string::npos && css.body.find("}body{") == std::string::npos);
        CHECK(css.body.find("--radius-surface:2px;") != std::string::npos);
        CHECK(css.body.find("--effect-menu-shadow:0px 16px 32px rgba(0,0,0,0.55);") != std::string::npos);
        CHECK(css.body.find("--effect-glow") == std::string::npos && css.body.find("x;}") == std::string::npos);
        CHECK(css.body.find("--ease-standard:cubic-bezier(0.2, 0, 0, 1);") != std::string::npos);
        CHECK(css.body.find("--dur-fast:120ms;") != std::string::npos);
        CHECK(css.body.find("@media (prefers-color-scheme: light){:root{color-scheme:light;--bg-base:#F4EFE6;}}") != std::string::npos);
    }
    CHECK(run(req("GET", "/api/i18n")).response.body == "{\"lang\":\"tr\"}");

    // Host header (DNS rebinding).
    {
        Request r = req("GET", "/");
        r.headers[0].value = "evil.example:48484";
        CHECK(run(r).response.status == 403);
        r.headers[0].value = "localhost:48484";
        CHECK(run(r).response.status == 200);
        CHECK(allowedHost("127.0.0.1") && allowedHost("10.0.0.5:1") && !allowedHost("10.0.0") && !allowedHost("10.0.0.256") &&
              !allowedHost("1.2.3.4.5") && !allowedHost("1.2.3.4:") && !allowedHost("1.2.3.4:abc") && !allowedHost(""));
    }

    // No / bad token.
    CHECK(run(req("GET", "/api/state")).response.status == 401);
    CHECK(run(req("GET", "/api/state", {{"authorization", "Bearer nope"}})).response.status == 401);

    // Pairing.
    const std::string json_ct = "application/json";
    CHECK(run(req("GET", "/api/pair")).response.status == 405);
    o = run(req("POST", "/api/pair", {{"content-type", json_ct}}, R"({"secret":"0123456789012345678901"})"), "192.168.1.40");
    CHECK(o.response.status == 403);
    o = run(req("POST", "/api/pair", {{"user-agent", "Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) Version/18 Safari/604"}},
                json({{"secret", store.pairingSecret()}}).dump()),
            "192.168.1.40");
    CHECK(o.response.status == 200);
    const json pj = json::parse(o.response.body, nullptr, false);
    const std::string token = pj.is_object() ? pj.value("token", "") : "";
    const json pdev = pj.is_object() && pj.contains("device") ? pj["device"] : json::object();
    const std::string deviceId = pdev.value("id", "");
    CHECK(token.size() == 43 && pdev.value("name", "") == "iPhone \xC2\xB7 Safari" && paired.size() == 1);
    const Header auth{"authorization", "Bearer " + token};

    o = run(req("GET", "/api/state", {auth}));
    CHECK(o.response.status == 200 && o.response.body == "{\"v\":1}");
    o = run(req("GET", "/api/events", {auth}));
    CHECK(o.kind == Outcome::Kind::Stream && o.device.id == deviceId);
    CHECK(run(req("POST", "/api/events", {auth})).response.status == 405);

    // Commands and queries.
    CHECK(run(req("POST", "/api/cmd", {auth}, R"({"cmd":"next"})")).response.status == 415);
    CHECK(run(req("POST", "/api/cmd", {auth, {"content-type", json_ct}}, "[1]")).response.status == 400);
    CHECK(run(req("POST", "/api/cmd", {auth, {"content-type", json_ct}}, R"({"cmd":5})")).response.status == 400);
    CHECK(run(req("POST", "/api/cmd", {auth, {"content-type", json_ct}}, "{bad")).response.status == 400);
    o = run(req("POST", "/api/cmd", {auth, {"content-type", "application/json; charset=utf-8"}}, R"({"cmd":"seek","ms":5000})"));
    CHECK(o.kind == Outcome::Kind::Async && calls.size() == 1 && calls.back().name == "cmd" && calls.back().body["ms"] == 5000);
    CHECK(calls.back().device.id == deviceId);
    CHECK(run(req("GET", "/api/search", {auth})).response.status == 400);
    o = run(req("GET", "/api/search?q=Bar%C4%B1%C5%9F+Man%C3%A7o", {auth}));
    CHECK(o.kind == Outcome::Kind::Async && calls.back().name == "search" && calls.back().arg == "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o");
    o = run(req("GET", "/api/album?id=spotify%3Aalbum%3A1", {auth}));
    CHECK(o.kind == Outcome::Kind::Async && calls.back().name == "album" && calls.back().arg == "spotify:album:1");
    CHECK(run(req("GET", "/api/playlist", {auth})).response.status == 400);
    CHECK(run(req("GET", "/api/artist?id=" + std::string(400, 'a'), {auth})).response.status == 400);
    o = run(req("GET", "/api/library", {auth}));
    CHECK(o.kind == Outcome::Kind::Async && calls.back().name == "library");
    o = run(req("GET", "/api/cover/0123456789abcdef", {auth}));
    CHECK(o.kind == Outcome::Kind::Async && calls.back().name == "cover" && calls.back().arg == "0123456789abcdef");
    CHECK(run(req("GET", "/api/cover/..%2f..%2fwin.ini", {auth})).response.status == 404);
    CHECK(run(req("GET", "/api/cover/0123", {auth})).response.status == 404);
    CHECK(run(req("GET", "/api/nothing", {auth})).response.status == 404);
    // A token is needed for the cover too.
    CHECK(run(req("GET", "/api/cover/0123456789abcdef")).response.status == 401);

    // Throttling: 4 bad tokens from one peer block it (even with a good token), other peers are fine.
    for (int i = 0; i < 4; ++i) run(req("GET", "/api/state", {{"authorization", "Bearer bad"}}), "192.168.1.66");
    o = run(req("GET", "/api/state", {auth}), "192.168.1.66");
    CHECK(o.response.status == 429);
    bool retryAfter = false;
    for (const auto& h : o.response.headers) retryAfter = retryAfter || (h.name == "Retry-After" && std::stoi(h.value) >= 1);
    CHECK(retryAfter);
    CHECK(run(req("POST", "/api/pair", {}, json({{"secret", store.pairingSecret()}}).dump()), "192.168.1.66").response.status == 429);
    CHECK(run(req("GET", "/api/state", {auth}), "192.168.1.30").response.status == 200);
    CHECK(run(req("GET", "/"), "192.168.1.66").response.status == 200);   // the page itself stays reachable
    now += 31'000;
    CHECK(run(req("GET", "/api/state", {auth}), "192.168.1.66").response.status == 200);   // the block ran out
    // Guessing pairing secrets is throttled the same way.
    for (int i = 0; i < 4; ++i)
        run(req("POST", "/api/pair", {}, R"({"secret":"guessguessguessguess00"})"), "192.168.1.77");
    CHECK(run(req("POST", "/api/pair", {}, json({{"secret", store.pairingSecret()}}).dump()), "192.168.1.77").response.status == 429);

    // A rotated secret no longer pairs; the token keeps working.
    const std::string oldSecret = store.pairingSecret();
    store.rotateSecret();
    CHECK(run(req("POST", "/api/pair", {}, json({{"secret", oldSecret}}).dump())).response.status == 403);
    CHECK(run(req("GET", "/api/state", {auth})).response.status == 200);

    // The phone forgets itself: the token is gone.
    CHECK(run(req("POST", "/api/unpair", {auth})).response.status == 200 && unpaired.size() == 1);
    CHECK(run(req("GET", "/api/state", {auth})).response.status == 401);

    // No handler: 503.
    Router bare(store, cfg);
    auto p2 = store.pair(store.pairingSecret(), "x", 1);
    CHECK(p2.has_value());
    if (p2)
        CHECK(bare.handle(req("GET", "/api/library", {{"authorization", "Bearer " + p2->token}}), "1.1.1.1", 0, 1, [](Response) {})
                  .response.status == 503);
}

// ---- state ------------------------------------------------------------------------------------------------------------

static void testState() {
    std::printf("state json:\n");
    st::catalog::Track t;
    t.id = "spotify:track:1";
    t.name = "D\xC3\xB6nence";
    t.artists = {{"spotify:artist:1", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o"}, {"", "Kurtalan Ekspres"}};
    t.album.id = "spotify:album:1";
    t.album.name = "Sahibinden";
    t.album.images = {{"https://i.scdn.co/image/64", 64, 64}, {"https://i.scdn.co/image/640", 640, 640}};
    t.durationMs = 240000;
    std::vector<std::string> mapped;
    CoverMapper covers = [&](const std::string& url) {
        mapped.push_back(url);
        return std::string("/api/cover/0123456789abcdef");
    };
    NowPlaying s;
    s.track = &t;
    s.status = "playing";
    s.playing = true;
    s.positionMs = 12345;
    s.durationMs = 240000;
    s.shuffle = "smart";
    s.repeat = "one";
    s.liked = true;
    s.canLike = true;
    s.volume = 0.42f;
    s.accentDark = {hexColor(221 / 255.f, 1.f, 71 / 255.f), "#0F0D0B"};
    s.accentLight = {"#8DAD00", "#0F0D0B"};
    s.contextName = "Gece S\xC3\xBCr\xC3\xBC\xC5\x9F\xC3\xBC";
    const json j = json::parse(stateJson(s, covers), nullptr, false);
    CHECK(j.is_object());
    if (!j.is_object()) return;
    CHECK(j["playing"] == true && j["status"] == "playing" && j["positionMs"] == 12345 && j["durationMs"] == 240000);
    CHECK(j["shuffle"] == "smart" && j["repeat"] == "one" && j["liked"] == true && j["canSeek"] == true);
    CHECK(j["accent"]["dark"]["base"] == "#DDFF47" && j["accent"]["light"]["on"] == "#0F0D0B");
    CHECK(j["context"].get<std::string>() == s.contextName);
    CHECK(std::abs(j["volume"].get<double>() - 0.42) < 1e-6);
    CHECK(j["track"]["title"] == t.name && j["track"]["artist"] == "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o, Kurtalan Ekspres");
    CHECK(j["track"]["artists"].size() == 2 && j["track"]["artists"][0]["id"] == "spotify:artist:1");
    CHECK(j["track"]["cover"] == "https://i.scdn.co/image/640" && j["track"]["thumb"] == "https://i.scdn.co/image/640");
    CHECK(mapped.empty());   // https covers pass straight through

    // A local file's cover goes through the mapper; anything else that isn't http(s) too.
    t.album.images = {{"file:///C:/Users/x/AppData/Local/ShadeTube/cache/local-covers/a.jpg", 0, 0}};
    s.live = false;
    const json k = json::parse(stateJson(s, covers), nullptr, false);
    CHECK(k["track"]["cover"] == "/api/cover/0123456789abcdef" && mapped.size() == 2);
    CHECK(k.dump().find("file:") == std::string::npos && k.dump().find("AppData") == std::string::npos);
    CHECK(coverUrl(t.album.images, 64, nullptr).empty());

    // A station: no seeking.
    s.live = true;
    s.liveTitle = "Artist - Song";
    s.durationMs = 0;
    const json l = json::parse(stateJson(s, covers), nullptr, false);
    CHECK(l["live"] == true && l["canSeek"] == false && l["liveTitle"] == "Artist - Song");

    NowPlaying none;
    const json n = json::parse(stateJson(none, covers), nullptr, false);
    CHECK(n["track"].is_null() && n["playing"] == false && !n.contains("liked"));

    // Invalid UTF-8 never breaks the answer.
    st::catalog::Track bad = t;
    bad.name = "a\xFF" "b";
    s.track = &bad;
    CHECK(json::parse(stateJson(s, covers), nullptr, false).is_object());

    QueueView q;
    q.current = 4;
    q.items = {{4, &t}, {5, &bad}};
    q.more = 12;
    q.moreText = "ve 12 \xC5\x9F" "ark\xC4\xB1 daha";
    const json qj = json::parse(queueJson(q, covers), nullptr, false);
    CHECK(qj["current"] == 4 && qj["items"].size() == 2 && qj["items"][1]["i"] == 5 && qj["more"] == 12 && qj["moreText"] == q.moreText);
    q.more = 0;
    CHECK(!json::parse(queueJson(q, covers)).contains("moreText"));
    const json pos = json::parse(positionJson(-5, false, ""));
    CHECK(pos["p"] == 0 && pos["playing"] == false && !pos.contains("line"));
    CHECK(json::parse(positionJson(1000, true, "la la"))["line"] == "la la");

    st::catalog::Playlist pl;
    pl.name = "Liste";
    pl.ownerName = "Ben";
    const json pjson = playlistJson(pl, "liked", "12 \xC5\x9F" "ark\xC4\xB1", covers);
    CHECK(pjson["id"] == "liked" && pjson["count"] == "12 \xC5\x9F" "ark\xC4\xB1" && pjson["cover"] == "");
    st::catalog::Album al;
    al.id = "a";
    al.name = "Al";
    al.firstReleaseDate = "1997-06-14";
    CHECK(albumJson(al, covers)["year"] == "1997");
}

// ---- lan --------------------------------------------------------------------------------------------------------------

static void testLan() {
    std::printf("lan addresses:\n");
    std::vector<LanAddress> list(5);
    list[0].ip = "172.27.160.1";
    list[0].adapter = L"vEthernet (WSL)";
    list[0].description = L"Hyper-V Virtual Ethernet Adapter";
    list[0].ifType = 6;   // IF_TYPE_ETHERNET_CSMACD
    list[1].ip = "192.168.1.20";
    list[1].adapter = L"Wi-Fi";
    list[1].description = L"Intel(R) Wi-Fi 6 AX201 160MHz";
    list[1].ifType = 71;  // IF_TYPE_IEEE80211
    list[1].gateway = true;
    list[2].ip = "192.168.56.1";
    list[2].adapter = L"Ethernet 3";
    list[2].description = L"VirtualBox Host-Only Ethernet Adapter";
    list[2].ifType = 6;
    list[3].ip = "100.101.102.103";
    list[3].adapter = L"Tailscale";
    list[3].description = L"Tailscale Tunnel";
    list[3].ifType = 53;  // IF_TYPE_PROP_VIRTUAL
    list[4].ip = "10.0.0.7";
    list[4].adapter = L"Ethernet";
    list[4].description = L"Realtek PCIe GbE Family Controller";
    list[4].ifType = 6;
    list[4].gateway = true;
    rankAddresses(list);
    CHECK(list[0].ip == "192.168.1.20" && list[0].kind == LanAddress::Kind::WiFi && !list[0].isVirtual);
    CHECK(list[1].ip == "10.0.0.7" && list[1].kind == LanAddress::Kind::Ethernet);
    CHECK(list[2].isVirtual && list[3].isVirtual && list[4].isVirtual);
    CHECK(list.back().ip == "100.101.102.103");
    // The real machine: whatever it has, ranked and without loopback / link-local.
    const auto real = lanAddresses();
    for (const auto& a : real) {
        CHECK(a.ip.rfind("127.", 0) != 0 && a.ip.rfind("169.254.", 0) != 0);
        std::printf("  %-16s score %3d %s%ls\n", a.ip.c_str(), a.score, a.isVirtual ? "(virtual) " : "", a.adapter.c_str());
    }
    for (size_t i = 1; i < real.size(); ++i) CHECK(real[i - 1].score >= real[i].score);
}

// ---- live -------------------------------------------------------------------------------------------------------------

struct WinHttp {
    HINTERNET session = nullptr, connect = nullptr;
    WinHttp(uint16_t port) {
        session = WinHttpOpen(L"remote_test/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);
        connect = WinHttpConnect(session, L"127.0.0.1", port, 0);
    }
    ~WinHttp() {
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
    struct Result {
        int status = 0;
        std::string body;
        std::wstring headers;
    };
    Result request(const wchar_t* method, const std::wstring& path, const std::string& body = {}, std::wstring headers = {}) {
        Result r;
        HINTERNET h = WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!h) return r;
        if (!body.empty()) headers += L"Content-Type: application/json\r\n";
        const BOOL sent = WinHttpSendRequest(h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                             headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                             body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                             static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
        if (sent && WinHttpReceiveResponse(h, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX);
            r.status = static_cast<int>(status);
            DWORD hsize = 0;
            WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hsize, WINHTTP_NO_HEADER_INDEX);
            if (hsize) {
                r.headers.resize(hsize / sizeof(wchar_t));
                WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, r.headers.data(), &hsize,
                                    WINHTTP_NO_HEADER_INDEX);
            }
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(h, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(h, chunk.data(), avail, &got) || got == 0) break;
                r.body.append(chunk.data(), got);
            }
        }
        WinHttpCloseHandle(h);
        return r;
    }
};

// A raw TCP client for what WinHTTP won't send.
struct RawClient {
    SOCKET s = INVALID_SOCKET;
    explicit RawClient(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    }
    ~RawClient() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    void sendAll(std::string_view d) { send(s, d.data(), static_cast<int>(d.size()), 0); }
    // Reads until `pred(buffer)` or the connection closes / times out.
    template <class Pred>
    std::string readUntil(Pred pred) {
        std::string got;
        char buf[4096];
        while (!pred(got)) {
            const int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            got.append(buf, static_cast<size_t>(n));
        }
        return got;
    }
    std::string readAll() {
        return readUntil([](const std::string&) { return false; });
    }
};

static size_t countOf(const std::string& s, const std::string& part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) ++n;
    return n;
}

static void testLive() {
    std::printf("live server on 127.0.0.1:\n");
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const fs::path file = g_dir / L"live.dat";
    DeviceStore store(file);
    store.load();
    Router::Config cfg;
    cfg.asset = fakeAsset;
    cfg.strings = [] { return std::string("{}"); };
    Server* serverPtr = nullptr;
    cfg.state = [&] { return serverPtr ? serverPtr->latest("state") : std::string{}; };
    Router router(store, cfg);
    std::vector<std::thread> workers;
    std::mutex callsMu;
    std::vector<std::string> cmds;
    router.onCall = [&](ApiCall c, Reply reply) {
        // Answer later, from another thread (like the app's UI thread does).
        std::lock_guard lock(callsMu);
        cmds.push_back(c.body.is_object() ? c.body.value("cmd", c.name) : c.name);
        const bool slow = c.body.is_object() && c.body.value("cmd", "") == "never";
        workers.emplace_back([reply = std::move(reply), slow] {
            if (slow) return;   // never answered: the server times it out
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            reply(Response::json(200, "{\"ok\":true}"));
        });
    };
    std::atomic<int> paired{0};
    router.onPaired = [&](const Device&) { ++paired; };

    Server server(router);
    serverPtr = &server;
    std::atomic<int> ticks{0}, streamChanges{0};
    server.onTick = [&] { ++ticks; };
    server.onStreamsChanged = [&] { ++streamChanges; };
    Server::Options opt;
    opt.bindAddress = "127.0.0.1";
    opt.port = static_cast<uint16_t>(41000 + GetCurrentProcessId() % 4000);   // parallel runs pick other ports
    opt.portAttempts = 20;
    opt.requestTimeoutMs = 400;
    opt.replyTimeoutMs = 600;
    opt.tickMs = 100;
    opt.heartbeatMs = 300;
    opt.idleTimeoutMs = 2000;
    std::string error;
    CHECK(server.start(opt, &error));
    if (!server.running()) {
        std::printf("  could not start: %s\n", error.c_str());
        return;
    }
    const uint16_t port = server.port();
    std::printf("  port %u\n", port);
    server.broadcast("state", "{\"v\":1,\"n\":1}");
    server.broadcast("queue", "{\"items\":[]}");

    WinHttp http(port);
    {
        const auto r = http.request(L"GET", L"/");
        CHECK(r.status == 200 && r.body == g_assets[L"remote/index.html"]);
        CHECK(r.headers.find(L"Content-Security-Policy") != std::wstring::npos);
        CHECK(http.request(L"GET", L"/icons/play.svg").body == "<svg>play</svg>");
        CHECK(http.request(L"GET", L"/nothing").status == 404);
        CHECK(http.request(L"GET", L"/api/state").status == 401);
    }
    // Pairing, then the state and commands with the token (one keep-alive connection under WinHTTP).
    std::string token;
    {
        const auto r = http.request(L"POST", L"/api/pair", json({{"secret", store.pairingSecret()}}).dump());
        CHECK(r.status == 200 && paired == 1);
        const json j = json::parse(r.body, nullptr, false);
        token = j.is_object() ? j.value("token", "") : "";
        CHECK(token.size() == 43);
    }
    const std::wstring auth = L"Authorization: Bearer " + std::wstring(token.begin(), token.end()) + L"\r\n";
    {
        const auto r = http.request(L"GET", L"/api/state", {}, auth);
        CHECK(r.status == 200 && r.body == "{\"v\":1,\"n\":1}");
        const auto c = http.request(L"POST", L"/api/cmd", R"({"cmd":"toggle"})", auth);
        CHECK(c.status == 200 && c.body == "{\"ok\":true}");
        const auto never = http.request(L"POST", L"/api/cmd", R"({"cmd":"never"})", auth);
        CHECK(never.status == 504);
        const auto s = http.request(L"GET", L"/api/search?q=abc", {}, auth);
        CHECK(s.status == 200);
    }
    // Pipelining on one raw connection: both answered, in order.
    {
        RawClient c(port);
        const std::string a = "GET /icons/play.svg HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        const std::string b = "POST /api/cmd HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
                              "\r\nContent-Type: application/json\r\nContent-Length: 14\r\nConnection: close\r\n\r\n{\"cmd\":\"next\"}";
        c.sendAll(a + b);
        const std::string got = c.readAll();
        CHECK(countOf(got, "HTTP/1.1 200 OK") == 2);
        CHECK(got.find("<svg>play</svg>") < got.find("{\"ok\":true}"));
        CHECK(got.find("Connection: close") != std::string::npos);
    }
    // A request in three pieces.
    {
        RawClient c(port);
        c.sendAll("GET /app");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        c.sendAll(".js HTTP/1.1\r\nHo");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        c.sendAll("st: 127.0.0.1\r\nConnection: close\r\n\r\n");
        const std::string got = c.readAll();
        CHECK(got.rfind("HTTP/1.1 200 OK", 0) == 0 && got.find("console.log(1)") != std::string::npos);
    }
    // Too slow: 408. Garbage: 400. Too large: 413 / 431.
    {
        RawClient c(port);
        c.sendAll("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n");
        const std::string got = c.readAll();
        CHECK(got.rfind("HTTP/1.1 408", 0) == 0);
        RawClient g(port);
        g.sendAll("\x16\x03\x01 hello\r\n\r\n");
        CHECK(g.readAll().rfind("HTTP/1.1 400", 0) == 0);
        RawClient big(port);
        big.sendAll("POST /api/cmd HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n");
        CHECK(big.readAll().rfind("HTTP/1.1 413", 0) == 0);
        RawClient head(port);
        head.sendAll("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n");
        CHECK(head.readAll().rfind("HTTP/1.1 431", 0) == 0);
        RawClient host(port);
        host.sendAll("GET / HTTP/1.1\r\nHost: attacker.example\r\nConnection: close\r\n\r\n");
        CHECK(host.readAll().rfind("HTTP/1.1 403", 0) == 0);
    }
    // The event stream: initial state + queue, broadcasts, ticks while open, heartbeats, and a close.
    {
        RawClient c(port);
        c.sendAll("GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token + "\r\n\r\n");
        std::string got = c.readUntil([](const std::string& s) { return s.find("event: queue") != std::string::npos; });
        CHECK(got.rfind("HTTP/1.1 200 OK", 0) == 0 && got.find("text/event-stream") != std::string::npos);
        CHECK(got.find("event: state\ndata: {\"v\":1,\"n\":1}\n\n") != std::string::npos);
        for (int i = 0; i < 50 && server.streamCount() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(server.streamCount() == 1 && server.streamDevices().size() == 1);
        server.broadcast("state", "{\"v\":1,\"n\":2}");
        server.broadcast("state", "{\"v\":1,\"n\":3}");   // coalesced with the one before when still queued
        server.broadcast("pos", "{\"p\":1}");
        got = c.readUntil([](const std::string& s) { return s.find("event: pos") != std::string::npos; });
        CHECK(got.find("\"n\":3") != std::string::npos);
        const int before = ticks.load();
        got = c.readUntil([](const std::string& s) { return s.find(": ping") != std::string::npos; });
        CHECK(got.find(": ping\n\n") != std::string::npos);
        CHECK(ticks.load() > before);
        // A second stream starts with the newest state; "pos" is not replayed.
        RawClient d(port);
        d.sendAll("GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token + "\r\n\r\n");
        got = d.readUntil([](const std::string& s) { return s.find("event: queue") != std::string::npos; });
        CHECK(got.find("\"n\":3") != std::string::npos && got.find("event: pos") == std::string::npos);
        CHECK(server.latest("state") == "{\"v\":1,\"n\":3}" && server.latest("pos").empty());
    }
    for (int i = 0; i < 100 && server.streamCount() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(server.streamCount() == 0 && streamChanges.load() >= 2);
    {   // no stream, no ticks
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int t0 = ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        CHECK(ticks.load() == t0);
    }
    // Revocation closes the device's stream and its token stops working.
    {
        RawClient c(port);
        c.sendAll("GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token + "\r\n\r\n");
        c.readUntil([](const std::string& s) { return s.find("event: queue") != std::string::npos; });
        const auto devices = store.devices();
        CHECK(devices.size() == 1);
        if (!devices.empty()) {
            store.revoke(devices[0].id);
            server.dropDevice(devices[0].id);
        }
        const std::string rest = c.readAll();   // returns once the server closed it
        (void)rest;
        CHECK(http.request(L"GET", L"/api/state", {}, auth).status == 401);
    }
    // Many bad tokens: 429.
    {
        int last = 0;
        for (int i = 0; i < 10; ++i) last = http.request(L"GET", L"/api/state", {}, L"Authorization: Bearer nope\r\n").status;
        CHECK(last == 429);
    }
    // The port is taken: one attempt fails with inUse, more attempts take the next port.
    {
        Router other(store, cfg);
        Server second(other);
        Server::Options o2 = opt;
        o2.port = port;
        o2.portAttempts = 1;
        bool inUse = false;
        CHECK(!second.start(o2, nullptr, &inUse) && inUse);
        o2.portAttempts = 3;
        CHECK(second.start(o2) && second.port() > port);
        second.stop();
        CHECK(!second.running() && second.port() == 0);
    }
    const auto stopStart = std::chrono::steady_clock::now();
    server.stop();
    const auto stopMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopStart).count();
    std::printf("  stop took %lld ms\n", static_cast<long long>(stopMs));
    CHECK(stopMs < 1000);
    {
        RawClient c(port);
        CHECK(c.s == INVALID_SOCKET);   // nobody listens any more
    }
    server.broadcast("state", "{}");   // after stop: a no-op
    for (auto& w : workers) w.join();
    WSACleanup();
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_remote_test_" + std::to_wstring(GetCurrentProcessId()));   // runs may overlap
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // logs never reach the real profile

    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "http") testHttp();
    if (only.empty() || only == "sse") testSse();
    if (only.empty() || only == "auth") testAuth();
    if (only.empty() || only == "router") testRouter();
    if (only.empty() || only == "state") testState();
    if (only.empty() || only == "lan") testLan();
    if (only.empty() || only == "live") testLive();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall remote checks passed\n");
    return 0;
}
