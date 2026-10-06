#include "youtube/SoundCloud.h"

#include "core/Http.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace st::youtube::soundcloud {

namespace yte = YoutubeExplode;
using json = nlohmann::json;
using CT = yte::CancellationToken;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kHome = "https://soundcloud.com/";
constexpr const char* kApi = "https://api-v2.soundcloud.com";
constexpr auto kClientIdTtl = std::chrono::hours(12);
constexpr auto kBudget = std::chrono::seconds(25);       // one find(): client_id, search, stream URL, probe
constexpr auto kRequest = std::chrono::seconds(8);
constexpr int kMaxDurationDiffMs = 3000;
constexpr double kMinimumScore = 70;                      // TrackMatcher: title 30 + artist 25 + duration 30 = 85

Clock::time_point requestDeadline(Clock::time_point end) { return std::min(Clock::now() + kRequest, end); }

std::string jstr(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int64_t jint(const json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    return 0;
}

// Lower-case words; ASCII punctuation separates them, non-ASCII bytes (ş, é, 日) belong to words (TrackMatcher's rule).
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (const unsigned char c : s) {
        if (c >= 0x80 || std::isalnum(c)) {
            cur += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string withParam(const std::string& url, const std::string& param) {
    return url + (url.find('?') == std::string::npos ? "?" : "&") + param;
}

} // namespace

// Plain scans rather than std::regex: the scripts are megabytes long.
std::vector<std::string> scriptUrls(std::string_view html) {
    static constexpr std::string_view kPrefix = "src=\"https://a-v2.sndcdn.com/assets/";
    std::vector<std::string> out;
    for (size_t pos = html.find(kPrefix); pos != std::string_view::npos; pos = html.find(kPrefix, pos + 1)) {
        const size_t begin = pos + 5;
        const size_t end = html.find('"', begin);
        if (end == std::string_view::npos) break;
        const std::string_view url = html.substr(begin, end - begin);
        if (url.ends_with(".js") && url.find_first_of(" <>'") == std::string_view::npos) out.emplace_back(url);
    }
    return out;
}

std::optional<std::string> parseClientId(std::string_view script) {
    static constexpr std::string_view kKey = "client_id";
    auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (size_t pos = script.find(kKey); pos != std::string_view::npos; pos = script.find(kKey, pos + 1)) {
        if (pos > 0 && (alnum(script[pos - 1]) || script[pos - 1] == '_')) continue;   // "some_client_id"
        size_t p = pos + kKey.size();
        while (p < script.size() && script[p] == ' ') ++p;
        if (p >= script.size() || script[p] != ':') continue;
        ++p;
        while (p < script.size() && script[p] == ' ') ++p;
        if (p >= script.size() || script[p] != '"') continue;
        const std::string_view value = script.substr(p + 1, 33);
        if (value.size() == 33 && value[32] == '"' && std::all_of(value.begin(), value.end() - 1, alnum)) return std::string(value.substr(0, 32));
    }
    return std::nullopt;
}

std::vector<Candidate> parseSearch(std::string_view body) {
    const json j = json::parse(body.begin(), body.end(), nullptr, false);
    const auto items = j.is_object() ? j.find("collection") : j.end();
    if (j.is_discarded() || !j.is_object() || items == j.end() || !items->is_array())
        throw alt::AltSourceError("unexpected SoundCloud search response");
    std::vector<Candidate> out;
    for (const auto& t : *items) {
        if (!t.is_object() || jstr(t, "kind") != "track") continue;
        Candidate c;
        c.id = jint(t, "id");
        c.title = jstr(t, "title");
        if (const auto u = t.find("user"); u != t.end() && u->is_object()) c.user = jstr(*u, "username");
        c.permalink = jstr(t, "permalink_url");
        c.durationMs = static_cast<int>(jint(t, "duration"));
        c.policy = jstr(t, "policy");
        c.trackAuthorization = jstr(t, "track_authorization");
        const auto media = t.find("media");
        if (media != t.end() && media->is_object())
            if (const auto tc = media->find("transcodings"); tc != media->end() && tc->is_array())
                for (const auto& x : *tc) {
                    if (!x.is_object() || x.value("snipped", false)) continue;
                    const auto format = x.find("format");
                    if (format == x.end() || !format->is_object()) continue;
                    const std::string mime = jstr(*format, "mime_type");
                    if (jstr(*format, "protocol") == "progressive" && mime.starts_with("audio/mpeg")) {
                        c.progressiveUrl = jstr(x, "url");
                        break;
                    }
                }
        if (c.id > 0 && !c.title.empty()) out.push_back(std::move(c));
    }
    return out;
}

const Candidate* pick(const std::vector<Candidate>& candidates, const catalog::Track& track,
                      const yte::Music::TrackMatcher& ranker) {
    if (track.durationMs <= 0) return nullptr;   // the duration check is what keeps covers / edits out
    yte::Music::TrackQuery query;
    query.title = track.name;
    for (const auto& a : track.artists) query.artists.push_back(a.name);
    query.duration = std::chrono::milliseconds(track.durationMs);
    query.album = track.album.name;
    // TrackMatcher ranks YouTube search results: each candidate goes in under a stand-in video id ("sc" + its index).
    std::vector<yte::Search::VideoSearchResult> results;
    std::vector<const Candidate*> byIndex;
    for (const auto& c : candidates) {
        if (c.policy != "ALLOW" || c.progressiveUrl.empty() || std::abs(c.durationMs - track.durationMs) > kMaxDurationDiffMs)
            continue;
        char id[16];
        std::snprintf(id, sizeof id, "sc%09zu", byIndex.size());
        results.emplace_back(yte::Videos::VideoId(id), c.title, yte::Common::Author("", c.user),
                             std::optional<yte::TimeSpan>(std::chrono::milliseconds(c.durationMs)), std::vector<yte::Common::Thumbnail>{});
        byIndex.push_back(&c);
    }
    // Uploads are user re-uploads: also reject titles that name more than the song ("A - Song / Other Song / Third").
    std::vector<std::string> known = words(track.name);
    for (const auto& a : track.artists)
        for (auto& w : words(a.name)) known.push_back(std::move(w));
    for (const char* noise : {"official", "audio", "video", "music", "lyrics", "lyric", "hd", "hq", "feat", "ft", "featuring",
                              "original", "version", "remastered", "remaster", "full", "song", "x", "and"})
        known.emplace_back(noise);
    for (const auto& r : ranker.rank(query, results)) {
        if (r.score < kMinimumScore) break;
        int extra = 0;
        for (const auto& w : words(r.video.title()))
            if (std::find(known.begin(), known.end(), w) == known.end() && !(w.size() == 4 && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c) != 0; })))
                ++extra;   // a year ("2011") is no other song
        if (extra > 2) continue;
        const size_t index = std::stoul(r.video.id().value().substr(2));
        return index < byIndex.size() ? byIndex[index] : nullptr;
    }
    return nullptr;
}

std::string SoundCloud::clientId(bool refresh, Clock::time_point deadline, const CT& ct) {
    {
        std::lock_guard lock(mutex_);
        if (!refresh && !clientId_.empty() && Clock::now() - clientIdAt_ < kClientIdTtl) return clientId_;
    }
    const auto home = alt::fetchText(kHome, requestDeadline(deadline), ct, 2u << 20);
    if (!home.ok()) throw alt::AltSourceError(std::format("soundcloud.com answered HTTP {}", home.status));
    auto scripts = scriptUrls(home.body);
    // The id sits in one of the last bundles (the app's own code, after the vendor libraries).
    for (auto it = scripts.rbegin(); it != scripts.rend(); ++it) {
        ct.throwIfCancellationRequested();
        if (Clock::now() >= deadline) break;
        const auto js = alt::fetchText(*it, requestDeadline(deadline), ct, 8u << 20);
        if (!js.ok()) continue;
        if (auto id = parseClientId(js.body)) {
            std::lock_guard lock(mutex_);
            clientId_ = *id;
            clientIdAt_ = Clock::now();
            return clientId_;
        }
    }
    throw alt::AltSourceError(std::format("no client_id in soundcloud.com's {} script(s)", scripts.size()));
}

alt::HttpResult SoundCloud::api(const std::string& url, Clock::time_point deadline, const CT& ct) {
    for (int attempt = 0;; ++attempt) {
        const std::string id = clientId(attempt > 0, deadline, ct);
        auto r = alt::fetchText(withParam(url, "client_id=" + id), requestDeadline(deadline), ct);
        if ((r.status == 401 || r.status == 403) && attempt == 0) {
            ST_LOG_INFO("soundcloud", "client_id refused (HTTP {}): reading a new one", r.status);
            continue;
        }
        if (!r.ok()) throw alt::AltSourceError(std::format("SoundCloud API answered HTTP {}", r.status));
        return r;
    }
}

Served SoundCloud::find(const catalog::Track& track, const yte::Music::TrackMatcher& ranker, const CT& ct) {
    const auto deadline = Clock::now() + kBudget;
    yte::Music::TrackQuery q;
    q.title = track.name;
    for (const auto& a : track.artists) q.artists.push_back(a.name);
    // "Artist - Title" finds re-uploads too ("Artist - Title (Official Audio)"); the ranking sorts them.
    const std::string query = yte::Music::TrackMatcher::buildQuery(q);
    const auto search = api(std::format("{}/search/tracks?q={}&limit=20", kApi, http::urlEncode(query)), deadline, ct);
    const auto candidates = parseSearch(search.body);
    const Candidate* best = pick(candidates, track, ranker);
    if (!best) {
        const auto previews = std::count_if(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.policy == "SNIP"; });
        throw alt::AltSourceError(std::format("SoundCloud: no full-length match among {} result(s){}", candidates.size(),
                                              previews ? std::format(" ({} only as 30 s previews)", previews) : std::string{}));
    }
    std::string transcoding = best->progressiveUrl;
    if (!best->trackAuthorization.empty()) transcoding = withParam(transcoding, "track_authorization=" + http::urlEncode(best->trackAuthorization));
    const auto resolved = api(transcoding, deadline, ct);
    const json j = json::parse(resolved.body, nullptr, false);
    const std::string url = j.is_object() ? jstr(j, "url") : std::string{};
    if (!url.starts_with("https://")) throw alt::AltSourceError("SoundCloud gave no stream URL");
    Served s;
    s.stream.url = url;
    s.stream.mimeType = "audio/mpeg";
    s.stream.codec = "mp3";
    s.stream.bitrate = 128000;   // progressive transcodings are mp3_1_0 / mp3_0_1: 128 kbps
    s.track = *best;
    ct.throwIfCancellationRequested();
    alt::probeStream(s.stream, deadline, ct);
    return s;
}

} // namespace st::youtube::soundcloud
