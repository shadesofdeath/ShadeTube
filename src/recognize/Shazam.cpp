#include "recognize/Shazam.h"

#include "core/Http.h"
#include "core/Log.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>

namespace st::recognize {

namespace {

using nlohmann::json;

constexpr char kTag[] = "recognize";

std::string str(const json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    return {};
}

double num(const json& j, const char* key) {
    if (!j.is_object()) return 0;
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : 0;
}

// The first URL in hub.options[].actions[] / hub.actions[] that opens Apple Music.
std::string appleMusicUrl(const json& track) {
    const auto hub = track.find("hub");
    if (hub == track.end() || !hub->is_object()) return {};
    auto scan = [](const json& actions) -> std::string {
        if (!actions.is_array()) return {};
        for (const auto& a : actions) {
            const std::string uri = str(a, "uri");
            if (uri.rfind("https://music.apple.com/", 0) == 0) return uri.substr(0, uri.find('?'));
        }
        return {};
    };
    if (const auto o = hub->find("options"); o != hub->end() && o->is_array())
        for (const auto& opt : *o)
            if (opt.is_object())
                if (auto u = scan(opt.value("actions", json::array())); !u.empty()) return u;
    return scan(hub->value("actions", json::array()));
}

} // namespace

std::string randomUuid() {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    uint64_t a, b;
    {
        std::lock_guard lock(m);
        a = rng();
        b = rng();
    }
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;   // RFC 4122 variant
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08X-%04X-%04X-%04X-%012llX", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFFF), static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

std::string buildUrl(const std::string& uuid1, const std::string& uuid2) {
    return "https://amp.shazam.com/discovery/v5/en-US/US/iphone/-/tag/" + uuid1 + "/" + uuid2 +
           "?sync=true&webv3=true&sampling=true&connected=&shazamapiversion=v3&sharehub=true&hubv5minorversion=v5.1"
           "&hidelb=true&video=v3";
}

std::string buildBody(const Signature& sig, int64_t timestampMs) {
    json body;
    body["timezone"] = "Europe/London";
    body["signature"] = {{"uri", sig.dataUri()}, {"samplems", static_cast<int64_t>(sig.seconds() * 1000)}};
    body["timestamp"] = timestampMs;
    body["context"] = json::object();
    body["geolocation"] = json::object();
    return body.dump();
}

Answer parseAnswer(std::string_view text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) throw std::runtime_error("the recognition answer is not a JSON object");
    Answer a;
    a.tagId = str(j, "tagid");
    a.retryMs = static_cast<int>(num(j, "retryms"));
    const auto matches = j.find("matches");
    const auto track = j.find("track");
    if (matches == j.end() || !matches->is_array() || matches->empty() || track == j.end() || !track->is_object())
        return a;
    const json& t = *track;
    Song s;
    s.key = str(t, "key");
    s.title = str(t, "title");
    s.artist = str(t, "subtitle");
    if (s.title.empty()) return a;   // a match without a song to show is no match
    s.isrc = str(t, "isrc");
    if (const auto g = t.find("genres"); g != t.end()) s.genre = str(*g, "primary");
    if (const auto img = t.find("images"); img != t.end()) {
        s.coverUrl = str(*img, "coverarthq");
        if (s.coverUrl.empty()) s.coverUrl = str(*img, "coverart");
    }
    if (const auto sec = t.find("sections"); sec != t.end() && sec->is_array())
        for (const auto& section : *sec) {
            if (str(section, "type") != "SONG") continue;
            const auto md = section.find("metadata");
            if (md == section.end() || !md->is_array()) continue;
            for (const auto& row : *md) {
                const std::string title = str(row, "title"), value = str(row, "text");
                if (title == "Album") s.album = value;
                else if (title == "Label") s.label = value;
                else if (title == "Released") s.year = value;
            }
        }
    s.appleMusicUrl = appleMusicUrl(t);
    s.shazamUrl = str(t, "url");
    if (s.shazamUrl.empty() && !s.key.empty()) s.shazamUrl = "https://www.shazam.com/track/" + s.key;
    const json& m = matches->front();
    s.offsetSec = num(m, "offset");
    s.timeSkew = num(m, "timeskew");
    s.frequencySkew = num(m, "frequencyskew");
    a.song = std::move(s);
    return a;
}

Answer identify(const Signature& sig, const YoutubeExplode::CancellationToken& ct) {
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    http::HttpRequest r;
    r.method = "POST";
    r.url = buildUrl(randomUuid(), randomUuid());
    r.body = buildBody(sig, now);
    r.headers = {{"Content-Type", "application/json"},
                 {"Accept", "*/*"},
                 {"Accept-Language", "en-US"},
                 {"X-Shazam-Platform", "IPHONE"},
                 {"X-Shazam-AppVersion", "14.1.0"},
                 {"User-Agent", "Dalvik/2.1.0 (Linux; U; Android 6.0.1; SM-G920F Build/MMB29K)"}};
    http::HttpResponse res;
    try {
        res = http::send(std::move(r), ct);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        throw ServiceError(0, e.what());
    }
    if (!res.isSuccessStatusCode()) {
        ST_LOG_WARN(kTag, "the service answered HTTP {} ({} bytes)", res.statusCode, res.body.size());
        throw ServiceError(res.statusCode, "HTTP " + std::to_string(res.statusCode));
    }
    try {
        return parseAnswer(res.body);
    } catch (const std::exception& e) {
        throw ServiceError(res.statusCode, e.what());
    }
}

} // namespace st::recognize
