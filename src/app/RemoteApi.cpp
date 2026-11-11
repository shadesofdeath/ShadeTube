#include "app/RemoteApi.h"

#include <algorithm>
#include <cstdio>

namespace st::app::remote {

using json = nlohmann::json;

namespace {

// The page's security policy: everything from this server, covers also from https/http image hosts (Spotify, Cover
// Art Archive, podcast and station artwork) and blob: URLs (local covers fetched with the token).
constexpr const char* kCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; font-src 'self'; connect-src 'self'; "
    "img-src 'self' blob: data: https: http:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

std::string etagOf(std::string_view body) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (unsigned char c : body) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "\"%016llx\"", static_cast<unsigned long long>(h));
    return buf;
}

bool isGetOrHead(const Request& r) { return r.method == "GET" || r.method == "HEAD"; }

Response methodNotAllowed(const char* allow) {
    Response r = Response::error(405);
    r.headers.push_back({"Allow", allow});
    return r;
}

Outcome respond(Response r) {
    Outcome o;
    o.response = std::move(r);
    return o;
}

bool validName(std::string_view s, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::wstring widen(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::string kebab(std::string_view camel) {
    std::string s;
    for (char c : camel) {
        if (c >= 'A' && c <= 'Z') {
            s += '-';
            s += static_cast<char>(c - 'A' + 'a');
        } else {
            s += c;
        }
    }
    return s;
}

// "--bg-base:#0F0D0B;" for every color string under color.<group>.<name>, and the radii.
std::string cssVariables(std::string_view tokensJson) {
    const json j = json::parse(tokensJson, nullptr, false);
    std::string out;
    if (!j.is_object()) return out;
    const auto safe = [](const std::string& v) {
        return std::all_of(v.begin(), v.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '#' || c == '(' ||
                   c == ')' || c == ',' || c == '.' || c == ' ' || c == '%';
        });
    };
    if (const auto colors = j.find("color"); colors != j.end() && colors->is_object()) {
        for (const auto& [group, values] : colors->items()) {
            if (!values.is_object()) continue;
            for (const auto& [name, v] : values.items()) {
                if (!v.is_string()) continue;
                const std::string value = v.get<std::string>();
                if ((value.rfind('#', 0) != 0 && value.rfind("rgba(", 0) != 0) || !safe(value)) continue;
                out += "--" + kebab(group) + "-" + kebab(name) + ":" + value + ";";
            }
        }
    }
    if (const auto radius = j.find("radius"); radius != j.end() && radius->is_object())
        for (const auto& [name, v] : radius->items())
            if (v.is_number()) out += "--radius-" + kebab(name) + ":" + std::to_string(v.get<int>()) + "px;";
    return out;
}

} // namespace

std::string themeCss(std::string_view darkTokensJson, std::string_view lightTokensJson) {
    std::string css = ":root{color-scheme:dark;" + cssVariables(darkTokensJson) + "}\n";
    const std::string light = cssVariables(lightTokensJson);
    if (!light.empty()) css += "@media (prefers-color-scheme: light){:root{color-scheme:light;" + light + "}}\n";
    return css;
}

bool allowedHost(std::string_view host) {
    if (const size_t colon = host.rfind(':'); colon != std::string_view::npos) {
        const std::string_view port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        host = host.substr(0, colon);
    }
    if (host == "localhost") return true;
    int parts = 0;
    size_t i = 0;
    while (i <= host.size()) {
        size_t j = i;
        int value = 0;
        while (j < host.size() && host[j] >= '0' && host[j] <= '9' && j - i < 3) value = value * 10 + (host[j++] - '0');
        if (j == i || value > 255) return false;
        ++parts;
        if (j == host.size()) break;
        if (host[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

Router::Router(DeviceStore& store, Config config, Throttle::Config throttle)
    : store_(store), cfg_(std::move(config)), throttle_(throttle) {}

const std::string& Router::themeCss() {
    if (themeCss_.empty() && cfg_.asset)
        themeCss_ = remote::themeCss(cfg_.asset(L"tokens/tokens.dark.json"), cfg_.asset(L"tokens/tokens.light.json"));
    return themeCss_;
}

Response Router::cached(const Request& req, const std::string& key, std::string body, const char* type, bool longCache) {
    auto it = etags_.find(key);
    if (it == etags_.end()) it = etags_.emplace(key, etagOf(body)).first;
    Response r;
    r.headers.push_back({"ETag", it->second});
    r.headers.push_back({"Cache-Control", longCache ? "public, max-age=86400" : "no-cache"});
    if (req.headerValue("if-none-match") == it->second) {
        r.status = 304;
        return r;
    }
    r.contentType = type;
    r.body = std::move(body);
    return r;
}

Response Router::file(const Request& req, std::wstring_view resource, const char* type, bool longCache) {
    const std::string_view data = cfg_.asset ? cfg_.asset(resource) : std::string_view{};
    if (data.empty()) return Response::error(404);
    std::string key;
    for (wchar_t c : resource) key += static_cast<char>(c & 0x7f);   // resource names are ASCII
    return cached(req, key, std::string(data), type, longCache);
}

Outcome Router::serveStatic(const Request& req) {
    const std::string& p = req.path;
    Response r;
    if (p == "/" || p == "/index.html") {
        r = file(req, L"remote/index.html", "text/html; charset=utf-8", false);
        r.headers.push_back({"Content-Security-Policy", kCsp});
    } else if (p == "/app.js") {
        r = file(req, L"remote/app.js", "text/javascript; charset=utf-8", false);
    } else if (p == "/app.css") {
        r = file(req, L"remote/app.css", "text/css; charset=utf-8", false);
    } else if (p == "/theme.css") {
        const std::string& css = themeCss();
        r = css.empty() ? Response::error(404) : cached(req, "theme.css", css, "text/css; charset=utf-8", false);
    } else if (p == "/fonts/BricolageGrotesque.ttf" || p == "/fonts/JetBrainsMono.ttf") {
        r = file(req, widen(std::string_view(p).substr(1)), "font/ttf", true);
    } else if (p == "/logo.svg") {
        r = file(req, L"logo/logo-mark.svg", "image/svg+xml", true);
    } else if (p.rfind("/icons/", 0) == 0 && p.size() > 11 && p.compare(p.size() - 4, 4, ".svg") == 0 &&
               validName(std::string_view(p).substr(7, p.size() - 11), 40)) {
        r = file(req, L"icons/24/" + widen(std::string_view(p).substr(7)), "image/svg+xml", true);
    } else {
        r = Response::error(404);
    }
    return respond(std::move(r));
}

Outcome Router::handle(const Request& req, const std::string& peer, int64_t nowMs, int64_t nowUnix, Reply reply) {
    if (!allowedHost(req.headerValue("host"))) return respond(Response::error(403));
    const std::string& p = req.path;
    if (p.rfind("/api/", 0) != 0) {
        if (!isGetOrHead(req)) return respond(methodNotAllowed("GET, HEAD"));
        return serveStatic(req);
    }

    if (p == "/api/i18n") {
        if (!isGetOrHead(req)) return respond(methodNotAllowed("GET, HEAD"));
        return respond(Response::json(200, cfg_.strings ? cfg_.strings() : std::string("{}")));
    }

    int64_t retry = 0;
    if (throttle_.blocked(peer, nowMs, &retry)) {
        Response r = Response::error(429);
        r.headers.push_back({"Retry-After", std::to_string(std::max<int64_t>(1, (retry + 999) / 1000))});
        return respond(std::move(r));
    }

    if (p == "/api/pair") {
        if (req.method != "POST") return respond(methodNotAllowed("POST"));
        const json body = json::parse(req.body, nullptr, false);
        const std::string secret = body.is_object() && body.contains("secret") && body["secret"].is_string()
                                       ? body["secret"].get<std::string>()
                                       : std::string{};
        auto paired = secret.empty() ? std::nullopt
                                     : store_.pair(secret, deviceNameFromUserAgent(req.headerValue("user-agent")), nowUnix);
        if (!paired) {
            throttle_.failure(peer, nowMs);
            return respond(Response::error(403));
        }
        throttle_.success(peer);
        if (onPaired) onPaired(paired->device);
        const json out = {{"token", paired->token}, {"device", {{"id", paired->device.id}, {"name", paired->device.name}}}};
        return respond(Response::json(200, out.dump()));
    }

    // Everything else needs a device token.
    const auto device = store_.authenticate(bearerToken(req.headerValue("authorization")), nowUnix);
    if (!device) {
        throttle_.failure(peer, nowMs);
        Response r = Response::error(401);
        r.headers.push_back({"WWW-Authenticate", "Bearer"});
        return respond(std::move(r));
    }
    throttle_.success(peer);

    if (p == "/api/state") {
        if (!isGetOrHead(req)) return respond(methodNotAllowed("GET, HEAD"));
        return respond(Response::json(200, cfg_.state ? cfg_.state() : std::string("{}")));
    }
    if (p == "/api/events") {
        if (req.method != "GET") return respond(methodNotAllowed("GET"));
        Outcome o;
        o.kind = Outcome::Kind::Stream;
        o.device = *device;
        return o;
    }
    if (p == "/api/unpair") {
        if (req.method != "POST") return respond(methodNotAllowed("POST"));
        store_.revoke(device->id);
        if (onUnpaired) onUnpaired(*device);
        return respond(Response::json(200, "{\"ok\":true}"));
    }

    ApiCall call;
    call.device = *device;
    if (p == "/api/cmd") {
        if (req.method != "POST") return respond(methodNotAllowed("POST"));
        if (req.headerValue("content-type").rfind("application/json", 0) != 0) return respond(Response::error(415));
        call.body = json::parse(req.body, nullptr, false);
        if (!call.body.is_object() || !call.body.contains("cmd") || !call.body["cmd"].is_string())
            return respond(Response::error(400));
        call.name = "cmd";
    } else if (p == "/api/search" || p == "/api/album" || p == "/api/playlist" || p == "/api/artist" || p == "/api/library") {
        if (req.method != "GET") return respond(methodNotAllowed("GET"));
        call.name = p.substr(5);
        if (call.name != "library") {
            call.arg = req.param(call.name == "search" ? "q" : "id");
            if (call.arg.empty() || call.arg.size() > 300) return respond(Response::error(400));
        }
    } else if (p.rfind("/api/cover/", 0) == 0) {
        if (!isGetOrHead(req)) return respond(methodNotAllowed("GET, HEAD"));
        call.name = "cover";
        call.arg = p.substr(11);
        if (call.arg.size() != 16 || !validName(call.arg, 16)) return respond(Response::error(404));
    } else {
        return respond(Response::error(404));
    }
    if (!onCall) return respond(Response::error(503));
    onCall(std::move(call), std::move(reply));
    Outcome o;
    o.kind = Outcome::Kind::Async;
    return o;
}

} // namespace st::app::remote
