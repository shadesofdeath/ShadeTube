#pragma once
// Phone remote control (app/RemoteControl): the routes of the LAN server.
//
//   GET  /  /app.js  /app.css  /theme.css      the web remote (embedded assets/remote/*, theme from assets/tokens)
//   GET  /fonts/<file>.ttf  /icons/<name>.svg  /logo.svg      the app's own fonts and icons
//   GET  /api/i18n                              the page's texts in the UI language (no token: the pairing screen)
//   POST /api/pair      {"secret"}              -> {"token", "device": {id, name}}  (throttled)
//   ---- Authorization: Bearer <device token> from here on (unknown tokens are throttled) ----
//   GET  /api/state                             {"state", "queue"}: the newest events (for browsers that only poll)
//   GET  /api/events                            text/event-stream: "state", "queue", "pos" events
//   POST /api/cmd       {"cmd", ...}            a command (see RemoteControl.cpp)
//   GET  /api/search?q=  /api/album?id=  /api/playlist?id=  /api/artist?id=  /api/library
//   GET  /api/cover/<handle>                    a local cover the app registered (never a path)
//   POST /api/unpair                            the phone forgets itself
//
// Only embedded assets are served, by a fixed table (no file system access). The Host header must be an IPv4
// literal or localhost (DNS rebinding). Called on the server thread; API calls are handed to `onCall`, which answers
// later (from any thread) through the Reply.
//
// Standalone (no app headers): tests compile it directly.
#include "app/RemoteAuth.h"
#include "app/RemoteHttp.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace st::app::remote {

// Answers a request; call exactly once, from any thread (a reply for a connection that is gone is dropped).
using Reply = std::function<void(Response)>;

struct ApiCall {
    std::string name;        // "cmd", "search", "album", "playlist", "artist", "library", "cover"
    Device device;           // who asks
    nlohmann::json body;     // "cmd": the POSTed object
    std::string arg;         // search: the query, album / playlist / artist: the id, cover: the handle
};

struct Outcome {
    enum class Kind { Respond, Async, Stream };
    Kind kind = Kind::Respond;
    Response response;       // Respond
    Device device;           // Stream: the device the event stream is for
};

class Router {
public:
    struct Config {
        // An embedded file by its resource name ("remote/index.html", "fonts/JetBrainsMono.ttf"); empty = missing.
        std::function<std::string_view(std::wstring_view name)> asset;
        std::function<std::string()> strings;   // /api/i18n body (JSON)
        std::function<std::string()> state;     // /api/state body (JSON: the newest "state" and "queue" events)
    };
    Router(DeviceStore& store, Config config, Throttle::Config throttle = {});

    std::function<void(ApiCall, Reply)> onCall;          // server thread; must reply exactly once
    std::function<void(const Device&)> onPaired;         // server thread
    std::function<void(const Device&)> onUnpaired;       // server thread: a phone disconnected itself

    // `peer` = the client's address (throttle key), `nowMs` = a monotonic clock, `nowUnix` = the wall clock.
    Outcome handle(const Request& req, const std::string& peer, int64_t nowMs, int64_t nowUnix, Reply reply);

    DeviceStore& store() { return store_; }

private:
    Outcome serveStatic(const Request& req);
    Response file(const Request& req, std::wstring_view resource, const char* type, bool longCache);
    Response cached(const Request& req, const std::string& key, std::string body, const char* type, bool longCache);
    const std::string& themeCss();

    DeviceStore& store_;
    Config cfg_;
    Throttle throttle_;
    std::string themeCss_;
    std::unordered_map<std::string, std::string> etags_;   // asset -> ETag (computed once)
};

// /theme.css from the two token files: every color of `color.*` and every `radius.*` as a CSS variable
// (--bg-base, --fg-primary, --accent-on-accent, --radius-surface ...), dark by default, light under
// prefers-color-scheme: light.
std::string themeCss(std::string_view darkTokensJson, std::string_view lightTokensJson);
// Is `host` (a Host header) an IPv4 literal or localhost, with an optional port?
bool allowedHost(std::string_view host);

} // namespace st::app::remote
