#include "lyrics/Lyrics.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace st::lyrics {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUserAgent = "ShadeTube/0.1 (https://github.com/shadesofdeath)";
constexpr int64_t kNegativeTtlSec = 7 * 24 * 3600;
constexpr int kCacheVersion = 2;   // 2: junk records (plausible()) are no longer cached as lyrics
constexpr int kDurationToleranceSec = 5;

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Parses "mm:ss", "mm:ss.x", "mm:ss.xx", "mm:ss.xxx" (also "mm:ss:xx") -> ms. -1 if not a timestamp.
int parseTimestamp(std::string_view t) {
    size_t i = 0;
    int minutes = 0, seconds = 0, frac = 0;
    size_t digits = 0;
    while (i < t.size() && isDigit(t[i]) && digits < 4) minutes = minutes * 10 + (t[i++] - '0'), ++digits;
    if (digits == 0 || i >= t.size() || t[i] != ':') return -1;
    ++i;
    digits = 0;
    while (i < t.size() && isDigit(t[i]) && digits < 2) seconds = seconds * 10 + (t[i++] - '0'), ++digits;
    if (digits == 0 || seconds > 59) return -1;
    if (i < t.size() && (t[i] == '.' || t[i] == ':')) {
        ++i;
        digits = 0;
        while (i < t.size() && isDigit(t[i])) {
            if (digits < 3) frac = frac * 10 + (t[i] - '0'), ++digits;
            ++i;
        }
        if (digits == 0) return -1;
        if (digits == 1) frac *= 100;
        else if (digits == 2) frac *= 10;
    }
    if (i != t.size()) return -1;
    return (minutes * 60 + seconds) * 1000 + frac;
}

// Removes enhanced-LRC word timestamps "<mm:ss.xx>" from lyric text.
std::string stripWordTimestamps(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const auto close = s.find('>', i + 1);
            if (close != std::string_view::npos && parseTimestamp(s.substr(i + 1, close - i - 1)) >= 0) {
                i = close;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return std::string(trim(out));
}

std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' || s[i] == '\r') {
            out.push_back(s.substr(start, i - start));
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

Lyrics plainToLyrics(std::string_view text) {
    Lyrics l;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    for (auto line : splitLines(text)) l.lines.push_back({-1, std::string(trim(line))});
    // Trim leading/trailing blank lines; keep inner blanks as stanza breaks.
    while (!l.lines.empty() && l.lines.back().text.empty()) l.lines.pop_back();
    auto first = std::find_if(l.lines.begin(), l.lines.end(), [](const Line& x) { return !x.text.empty(); });
    l.lines.erase(l.lines.begin(), first);
    return l;
}

// ---- Disk cache ------------------------------------------------------------------------------

fs::path cachePath(const std::string& key) {
    if (key.empty()) return {};
    std::string safe;
    for (char c : key) safe.push_back((isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_') ? c : '_');
    auto dir = paths::cacheDir() / L"lyrics";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (safe + ".json");
}

// nullopt: no usable cache entry. optional<optional<Lyrics>>: inner nullopt = cached "not found".
std::optional<std::optional<Lyrics>> readCache(const fs::path& p) {
    if (p.empty()) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != kCacheVersion) return std::nullopt;
    if (!j.value("found", false)) {
        const auto at = j.contains("at") && j["at"].is_number_integer() ? j["at"].get<int64_t>() : 0;
        if (nowUnix() - at > kNegativeTtlSec) return std::nullopt;   // expired negative
        return std::optional<Lyrics>{};
    }
    Lyrics l;
    l.synced = j.value("synced", false);
    l.instrumental = j.value("instrumental", false);
    if (auto it = j.find("lines"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_array() || e.size() != 2 || !e[0].is_number_integer() || !e[1].is_string()) continue;
            l.lines.push_back({e[0].get<int>(), e[1].get<std::string>()});
        }
    }
    return std::optional<Lyrics>{std::move(l)};
}

void writeCache(const fs::path& p, const std::optional<Lyrics>& l) {
    if (p.empty()) return;
    json j{{"v", kCacheVersion}, {"found", l.has_value()}, {"at", nowUnix()}};
    if (l) {
        j["synced"] = l->synced;
        j["instrumental"] = l->instrumental;
        json lines = json::array();
        for (const auto& x : l->lines) lines.push_back(json::array({x.timeMs, x.text}));
        j["lines"] = std::move(lines);
    }
    // Write to a temp file then rename, so a crash never leaves a truncated cache entry.
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::string s = j.dump();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

// ---- LRCLIB ----------------------------------------------------------------------------------

std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// LRCLIB is user-edited: some records are junk (Rick Astley "Never Gonna Give You Up", id 1009665, is
// "[00:00.00]probe"). Real lyrics have at least two lines or a sentence.
bool plausible(const Lyrics& l) {
    size_t lines = 0, chars = 0;
    for (const auto& x : l.lines) {
        if (trim(x.text).empty()) continue;
        ++lines;
        chars += x.text.size();
    }
    return lines >= 2 || chars >= 24;
}

// Converts an LRCLIB record to Lyrics; nullopt if it carries nothing usable.
std::optional<Lyrics> fromRecord(const json& r) {
    if (!r.is_object()) return std::nullopt;
    if (auto it = r.find("instrumental"); it != r.end() && it->is_boolean() && it->get<bool>()) {
        Lyrics l;
        l.instrumental = true;
        return l;
    }
    if (auto synced = jstr(r, "syncedLyrics"); !trim(synced).empty()) {
        auto l = parseLrc(synced);
        if (!l.lines.empty() && plausible(l)) return l;
    }
    if (auto plain = jstr(r, "plainLyrics"); !trim(plain).empty()) {
        auto l = plainToLyrics(plain);
        if (!l.lines.empty() && plausible(l)) return l;
    }
    return std::nullopt;
}

double recordDuration(const json& r) {
    auto it = r.find("duration");
    return it != r.end() && it->is_number() ? it->get<double>() : -1.0;
}

// Picks the best search result: synced within tolerance (closest duration) > instrumental/plain within
// tolerance. Without a known duration, the first synced (else first usable) result wins.
std::optional<Lyrics> pickFromSearch(const json& results, int durationSec) {
    if (!results.is_array()) return std::nullopt;
    const json* bestSynced = nullptr;
    const json* bestOther = nullptr;
    double bestSyncedDiff = 1e9, bestOtherDiff = 1e9;
    for (const auto& r : results) {
        if (!r.is_object()) continue;
        double diff = 0;
        if (durationSec > 0) {
            const double d = recordDuration(r);
            if (d < 0) continue;
            diff = std::abs(d - durationSec);
            if (diff > kDurationToleranceSec) continue;
        }
        if (!fromRecord(r)) continue;   // junk / empty record
        const bool hasSynced = !trim(jstr(r, "syncedLyrics")).empty();
        if (hasSynced && diff < bestSyncedDiff) bestSynced = &r, bestSyncedDiff = diff;
        else if (!hasSynced && diff < bestOtherDiff && fromRecord(r)) bestOther = &r, bestOtherDiff = diff;
    }
    if (bestSynced) return fromRecord(*bestSynced);
    if (bestOther) return fromRecord(*bestOther);
    return std::nullopt;
}

http::HttpResponse lrclibGet(const std::string& url, const YoutubeExplode::CancellationToken& ct) {
    return http::get(url, {{"User-Agent", kUserAgent}, {"Accept", "application/json"}}, ct);
}

} // namespace

// ---- Public API ------------------------------------------------------------------------------

Lyrics parseLrc(const std::string& lrcIn) {
    std::string_view lrc = lrcIn;
    if (lrc.size() >= 3 && lrc.substr(0, 3) == "\xEF\xBB\xBF") lrc.remove_prefix(3);

    Lyrics out;
    std::vector<std::string_view> untimed;   // text lines without timestamps (plain fallback)
    int offsetMs = 0;
    std::vector<int> times;

    for (auto raw : splitLines(lrc)) {
        std::string_view line = trim(raw);
        times.clear();
        bool metadata = false;
        while (!line.empty() && line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string_view::npos) break;
            const std::string_view tag = line.substr(1, close - 1);
            const int t = parseTimestamp(trim(tag));
            if (t >= 0) {
                times.push_back(t);
            } else if (times.empty()) {
                // [ar:..] [ti:..] [al:..] [by:..] [length:..] [offset:+/-ms] ...
                const auto colon = tag.find(':');
                if (colon == std::string_view::npos) break;   // "[Chorus]" etc. -> treat as text
                std::string key(trim(tag.substr(0, colon)));
                std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                if (key == "offset") {
                    const std::string v(trim(tag.substr(colon + 1)));
                    offsetMs = std::atoi(v.c_str());   // atoi handles leading '+'/'-'
                }
                metadata = true;
            } else {
                break;   // non-time tag after timestamps: part of the text
            }
            line.remove_prefix(close + 1);
        }
        if (!times.empty()) {
            const std::string text = stripWordTimestamps(line);
            for (int t : times) out.lines.push_back({t, text});
        } else if (!metadata) {
            untimed.push_back(line);
        }
    }

    if (out.lines.empty()) {
        // No timestamps at all: unsynced lyrics.
        std::string joined;
        for (auto l : untimed) {
            joined.append(l);
            joined.push_back('\n');
        }
        return plainToLyrics(joined);
    }

    // LRC semantics: a positive offset makes lyrics appear sooner.
    for (auto& l : out.lines) l.timeMs = std::max(0, l.timeMs - offsetMs);
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const Line& a, const Line& b) { return a.timeMs < b.timeMs; });
    out.synced = true;
    return out;
}

int activeLine(const Lyrics& l, int positionMs) {
    if (!l.synced || l.lines.empty()) return -1;
    auto it = std::upper_bound(l.lines.begin(), l.lines.end(), positionMs,
                               [](int pos, const Line& line) { return pos < line.timeMs; });
    return static_cast<int>(it - l.lines.begin()) - 1;
}

std::optional<Lyrics> fetch(const Query& q, const YoutubeExplode::CancellationToken& ct) {
    if (q.title.empty()) return std::nullopt;
    const fs::path cache = cachePath(q.cacheKey);
    if (auto cached = readCache(cache)) return *cached;

    try {
        // 1) exact lookup
        std::string url = "https://lrclib.net/api/get?track_name=" + http::urlEncode(q.title) +
                          "&artist_name=" + http::urlEncode(q.artist);
        if (!q.album.empty()) url += "&album_name=" + http::urlEncode(q.album);
        if (q.durationSec > 0) url += "&duration=" + std::to_string(q.durationSec);
        auto resp = lrclibGet(url, ct);

        bool definitiveMiss = false;
        if (resp.statusCode == 200) {
            const json j = json::parse(resp.body, nullptr, false);
            if (auto l = fromRecord(j)) {
                writeCache(cache, l);
                return l;
            }
            // Record exists but is empty: fall through to search.
        } else if (resp.statusCode != 404 && resp.statusCode != 400) {   // 400: incomplete query -> search
            ST_LOG_WARN("lyrics", "lrclib get HTTP {} for '{}'", resp.statusCode, q.title);
            return std::nullopt;   // transient: don't cache
        }

        // 2) fuzzy search
        std::string surl = "https://lrclib.net/api/search?track_name=" + http::urlEncode(q.title);
        if (!q.artist.empty()) surl += "&artist_name=" + http::urlEncode(q.artist);
        resp = lrclibGet(surl, ct);
        if (resp.statusCode == 200) {
            const json j = json::parse(resp.body, nullptr, false);
            if (auto l = pickFromSearch(j, q.durationSec)) {
                writeCache(cache, l);
                return l;
            }
            definitiveMiss = !j.is_discarded();
        } else if (resp.statusCode == 404) {
            definitiveMiss = true;
        } else {
            ST_LOG_WARN("lyrics", "lrclib search HTTP {} for '{}'", resp.statusCode, q.title);
        }

        if (definitiveMiss) {
            ST_LOG_INFO("lyrics", "no lyrics for '{}' - '{}'", q.artist, q.title);
            writeCache(cache, std::nullopt);
        }
        return std::nullopt;
    } catch (const YoutubeExplode::Exceptions::HttpRequestException& e) {
        ST_LOG_WARN("lyrics", "lrclib request failed: {}", e.what());
        return std::nullopt;
    }
}

} // namespace st::lyrics
