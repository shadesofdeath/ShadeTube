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
#include <cstdio>
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

// The text of a timed line and its enhanced-LRC words: "<mm:ss.xx>word <mm:ss.xx>word". A piece before the first
// word timestamp starts at the line's own time. Without word timestamps `words` stays empty. The words concatenate to
// the returned text (outer whitespace trimmed).
std::string parseWords(std::string_view s, int lineMs, std::vector<Word>& words) {
    struct Piece {
        int timeMs;
        std::string text;
    };
    std::vector<Piece> pieces{{lineMs, {}}};
    bool timed = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const auto close = s.find('>', i + 1);
            if (close != std::string_view::npos) {
                const int t = parseTimestamp(s.substr(i + 1, close - i - 1));
                if (t >= 0) {
                    timed = true;
                    pieces.push_back({t, {}});
                    i = close;
                    continue;
                }
            }
        }
        pieces.back().text.push_back(s[i]);
    }
    // Outer whitespace belongs to no word; pieces left empty (a closing "<mm:ss.xx>" end mark) are dropped.
    size_t first = 0;
    while (first < pieces.size() && trim(pieces[first].text).empty()) ++first;
    pieces.erase(pieces.begin(), pieces.begin() + static_cast<std::ptrdiff_t>(first));
    while (!pieces.empty() && trim(pieces.back().text).empty()) pieces.pop_back();
    if (!pieces.empty()) {
        auto& head = pieces.front().text;
        head.erase(0, head.find_first_not_of(" \t"));
        auto& tail = pieces.back().text;
        tail.erase(tail.find_last_not_of(" \t\r") + 1);
    }
    std::erase_if(pieces, [](const Piece& p) { return p.text.empty(); });
    std::string text;
    for (const auto& p : pieces) text += p.text;
    words.clear();
    if (timed && pieces.size() > 1)
        for (auto& p : pieces) words.push_back({p.timeMs, std::move(p.text)});
    return text;
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
    for (auto line : splitLines(text)) l.lines.push_back({-1, std::string(trim(line)), {}});
    // Trim leading/trailing blank lines; keep inner blanks as stanza breaks.
    while (!l.lines.empty() && l.lines.back().text.empty()) l.lines.pop_back();
    auto first = std::find_if(l.lines.begin(), l.lines.end(), [](const Line& x) { return !x.text.empty(); });
    l.lines.erase(l.lines.begin(), first);
    return l;
}

std::string formatTimestamp(int ms) {
    ms = std::max(0, ms);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02d:%02d.%02d", ms / 60000, ms / 1000 % 60, ms % 1000 / 10);
    return buf;
}

// ---- Disk cache ------------------------------------------------------------------------------
// {"v":2, "found":bool, "at":unix, "synced", "instrumental", "lines":[[ms,text]...], "words":{"<line>":[[ms,text]...]},
//  "src":"lrclib"|"spotify", "tried":["lrclib","spotify"], "lang":"tr", "tr":{"en":[text per line]}}. Entries without
//  "tried" were written by LRCLIB alone; "lang" / "tr" only when the source gave them.

struct CacheEntry {
    bool found = false;
    Lyrics lyrics;
    std::vector<std::string> tried;
    bool triedBy(const std::string& provider) const {
        return std::find(tried.begin(), tried.end(), provider) != tried.end();
    }
};

fs::path cachePath(const std::string& key) {
    if (key.empty()) return {};
    std::string safe;
    for (char c : key) safe.push_back((isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_') ? c : '_');
    auto dir = paths::cacheDir() / L"lyrics";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (safe + ".json");
}

// nullopt: no usable cache entry (missing, corrupt, other version, expired negative).
std::optional<CacheEntry> readCache(const fs::path& p) {
    if (p.empty()) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != kCacheVersion) return std::nullopt;
    CacheEntry e;
    if (auto it = j.find("tried"); it != j.end() && it->is_array()) {
        for (const auto& t : *it)
            if (t.is_string()) e.tried.push_back(t.get<std::string>());
    } else {
        e.tried.push_back(kSourceLrclib);
    }
    e.found = j.value("found", false);
    if (!e.found) {
        const auto at = j.contains("at") && j["at"].is_number_integer() ? j["at"].get<int64_t>() : 0;
        if (nowUnix() - at > kNegativeTtlSec) return std::nullopt;   // expired negative
        return e;
    }
    Lyrics& l = e.lyrics;
    l.synced = j.value("synced", false);
    l.instrumental = j.value("instrumental", false);
    l.source = j.value("src", std::string(kSourceLrclib));
    if (auto it = j.find("lines"); it != j.end() && it->is_array()) {
        for (const auto& x : *it) {
            if (!x.is_array() || x.size() != 2 || !x[0].is_number_integer() || !x[1].is_string()) continue;
            l.lines.push_back({x[0].get<int>(), x[1].get<std::string>(), {}});
        }
    }
    if (auto it = j.find("words"); it != j.end() && it->is_object()) {
        for (const auto& [idx, list] : it->items()) {
            const size_t i = static_cast<size_t>(std::strtoul(idx.c_str(), nullptr, 10));
            if (i >= l.lines.size() || !list.is_array()) continue;
            for (const auto& w : list)
                if (w.is_array() && w.size() == 2 && w[0].is_number_integer() && w[1].is_string())
                    l.lines[i].words.push_back({w[0].get<int>(), w[1].get<std::string>()});
        }
    }
    l.language = j.value("lang", std::string());
    if (auto it = j.find("tr"); it != j.end() && it->is_object()) {
        for (const auto& [lang, list] : it->items()) {
            if (!list.is_array() || list.size() != l.lines.size()) continue;
            std::vector<std::string> lines;
            for (const auto& t : list) lines.push_back(t.is_string() ? t.get<std::string>() : std::string());
            l.translations[lang] = std::move(lines);
        }
    }
    return e;
}

void writeCache(const fs::path& p, const CacheEntry& e) {
    if (p.empty()) return;
    json j{{"v", kCacheVersion}, {"found", e.found}, {"at", nowUnix()}, {"tried", e.tried}};
    if (e.found) {
        const Lyrics& l = e.lyrics;
        j["synced"] = l.synced;
        j["instrumental"] = l.instrumental;
        j["src"] = l.source.empty() ? std::string(kSourceLrclib) : l.source;
        json lines = json::array();
        json words = json::object();
        for (size_t i = 0; i < l.lines.size(); ++i) {
            const auto& x = l.lines[i];
            lines.push_back(json::array({x.timeMs, x.text}));
            if (x.words.empty()) continue;
            json list = json::array();
            for (const auto& w : x.words) list.push_back(json::array({w.timeMs, w.text}));
            words[std::to_string(i)] = std::move(list);
        }
        j["lines"] = std::move(lines);
        if (!words.empty()) j["words"] = std::move(words);
        if (!l.language.empty()) j["lang"] = l.language;
        if (!l.translations.empty()) j["tr"] = l.translations;
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
        l.source = kSourceLrclib;
        return l;
    }
    if (auto synced = jstr(r, "syncedLyrics"); !trim(synced).empty()) {
        auto l = parseLrc(synced);
        l.source = kSourceLrclib;
        if (!l.lines.empty() && plausible(l)) return l;
    }
    if (auto plain = jstr(r, "plainLyrics"); !trim(plain).empty()) {
        auto l = plainToLyrics(plain);
        l.source = kSourceLrclib;
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

ProviderResult lrclib(const Query& q, const YoutubeExplode::CancellationToken& ct) {
    ProviderResult out;
    try {
        // 1) exact lookup
        std::string url = "https://lrclib.net/api/get?track_name=" + http::urlEncode(q.title) +
                          "&artist_name=" + http::urlEncode(q.artist);
        if (!q.album.empty()) url += "&album_name=" + http::urlEncode(q.album);
        if (q.durationSec > 0) url += "&duration=" + std::to_string(q.durationSec);
        auto resp = lrclibGet(url, ct);

        if (resp.statusCode == 200) {
            const json j = json::parse(resp.body, nullptr, false);
            if (auto l = fromRecord(j)) return {ProviderResult::Kind::Found, std::move(*l)};
            // Record exists but is empty: fall through to search.
        } else if (resp.statusCode != 404 && resp.statusCode != 400) {   // 400: incomplete query -> search
            ST_LOG_WARN("lyrics", "lrclib get HTTP {} for '{}'", resp.statusCode, q.title);
            return out;   // transient: don't cache
        }

        // 2) fuzzy search
        std::string surl = "https://lrclib.net/api/search?track_name=" + http::urlEncode(q.title);
        if (!q.artist.empty()) surl += "&artist_name=" + http::urlEncode(q.artist);
        resp = lrclibGet(surl, ct);
        if (resp.statusCode == 200) {
            const json j = json::parse(resp.body, nullptr, false);
            if (auto l = pickFromSearch(j, q.durationSec)) return {ProviderResult::Kind::Found, std::move(*l)};
            if (!j.is_discarded()) out.kind = ProviderResult::Kind::NotFound;
        } else if (resp.statusCode == 404) {
            out.kind = ProviderResult::Kind::NotFound;
        } else {
            ST_LOG_WARN("lyrics", "lrclib search HTTP {} for '{}'", resp.statusCode, q.title);
        }
        return out;
    } catch (const YoutubeExplode::Exceptions::HttpRequestException& e) {
        ST_LOG_WARN("lyrics", "lrclib request failed: {}", e.what());
        return out;
    }
}

bool better(const Lyrics& candidate, const std::optional<Lyrics>& current) {
    if (!current) return true;
    return candidate.synced && !current->synced && !current->instrumental;
}

// The online chain for one track: the cached answer, LRCLIB when it wasn't asked yet, then the fallback provider when
// that is still missing or unsynced. What was asked is remembered with the result.
std::optional<Lyrics> fetchOnline(const Query& q, const YoutubeExplode::CancellationToken& ct) {
    const fs::path cache = cachePath(q.cacheKey);
    CacheEntry entry = readCache(cache).value_or(CacheEntry{});
    std::optional<Lyrics> best;
    if (entry.found) best = entry.lyrics;
    bool changed = false;

    if (!entry.triedBy(kSourceLrclib)) {
        auto r = lrclib(q, ct);
        if (r.kind != ProviderResult::Kind::Failed) {
            entry.tried.push_back(kSourceLrclib);
            changed = true;
        }
        if (r.kind == ProviderResult::Kind::Found && better(r.lyrics, best)) best = std::move(r.lyrics);
    }
    const bool wantMore = !best || (!best->synced && !best->instrumental);
    if (wantMore && q.fallback && !q.fallbackName.empty() && !entry.triedBy(q.fallbackName)) {
        auto r = q.fallback(ct);
        if (r.kind != ProviderResult::Kind::Failed) {
            entry.tried.push_back(q.fallbackName);
            changed = true;
        }
        if (r.kind == ProviderResult::Kind::Found && !r.lyrics.lines.empty() && better(r.lyrics, best)) {
            if (r.lyrics.source.empty()) r.lyrics.source = q.fallbackName;
            best = std::move(r.lyrics);
        }
    }
    if (changed) {
        entry.found = best.has_value();
        if (best) entry.lyrics = *best;
        if (!best) ST_LOG_INFO("lyrics", "no lyrics for '{}' - '{}'", q.artist, q.title);
        writeCache(cache, entry);
    }
    return best;
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
            std::vector<Word> words;
            const std::string text = parseWords(line, times.front(), words);
            // Word times are absolute: a line repeated at several timestamps keeps them only for its first one.
            for (size_t i = 0; i < times.size(); ++i)
                out.lines.push_back({times[i], text, i == 0 ? words : std::vector<Word>{}});
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
    for (auto& l : out.lines) {
        l.timeMs = std::max(0, l.timeMs - offsetMs);
        for (auto& w : l.words) w.timeMs = std::max(0, w.timeMs - offsetMs);
    }
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const Line& a, const Line& b) { return a.timeMs < b.timeMs; });
    out.synced = true;
    return out;
}

std::string toLrc(const Lyrics& l, const std::string& title, const std::string& artist, const std::string& album) {
    std::string out;
    auto tag = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        std::string clean = v;
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        std::replace(clean.begin(), clean.end(), ']', ')');
        out += std::string("[") + key + ":" + clean + "]\n";
    };
    tag("ti", title);
    tag("ar", artist);
    tag("al", album);
    if (!out.empty()) out += "[by:ShadeTube]\n";
    for (const auto& line : l.lines) {
        if (!l.synced || line.timeMs < 0) {
            out += line.text + "\n";
            continue;
        }
        out += "[" + formatTimestamp(line.timeMs) + "]";
        if (line.words.empty()) {
            out += line.text;
        } else {
            for (const auto& w : line.words) out += "<" + formatTimestamp(w.timeMs) + ">" + w.text;
        }
        out += "\n";
    }
    return out;
}

std::string toPlainText(const Lyrics& l) {
    std::string out;
    for (size_t i = 0; i < l.lines.size(); ++i) {
        if (i) out += "\n";
        out += l.lines[i].text;
    }
    return out;
}

std::optional<Lyrics> parseSpotify(const std::string& body) {
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    const auto lit = j.find("lyrics");
    if (lit == j.end() || !lit->is_object()) return std::nullopt;
    const std::string syncType = jstr(*lit, "syncType");
    const bool synced = syncType == "LINE_SYNCED" || syncType == "SYLLABLE_SYNCED";
    Lyrics l;
    l.synced = synced;
    l.source = kSourceSpotify;
    l.language = jstr(*lit, "language");
    if (l.language == "und") l.language.clear();   // "undetermined"
    // Each kept line remembers its index in "lines": the alternatives' lines are indexed like that.
    std::vector<std::pair<Line, size_t>> kept;
    if (auto it = lit->find("lines"); it != lit->end() && it->is_array()) {
        for (size_t i = 0; i < it->size(); ++i) {
            const auto& x = (*it)[i];
            if (!x.is_object()) continue;
            int ms = -1;
            if (synced) {
                if (auto t = x.find("startTimeMs"); t != x.end()) {
                    if (t->is_string()) ms = std::atoi(t->get<std::string>().c_str());
                    else if (t->is_number_integer()) ms = t->get<int>();
                }
                if (ms < 0) continue;
            }
            std::string text(trim(jstr(x, "words")));
            if (text == "\xE2\x99\xAA") text.clear();   // "♪": an instrumental gap
            kept.push_back({{ms, std::move(text), {}}, i});
        }
    }
    if (synced) {
        std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first.timeMs < b.first.timeMs; });
    } else {
        while (!kept.empty() && kept.back().first.text.empty()) kept.pop_back();
        auto first = std::find_if(kept.begin(), kept.end(), [](const auto& x) { return !x.first.text.empty(); });
        kept.erase(kept.begin(), first);
    }
    if (std::none_of(kept.begin(), kept.end(), [](const auto& x) { return !x.first.text.empty(); })) return std::nullopt;
    if (auto alts = lit->find("alternatives"); alts != lit->end() && alts->is_array()) {
        for (const auto& a : *alts) {
            if (!a.is_object()) continue;
            const std::string lang = jstr(a, "language");
            const auto al = a.find("lines");
            if (lang.empty() || al == a.end() || !al->is_array()) continue;
            std::vector<std::string> lines;
            bool any = false;
            for (const auto& [line, src] : kept) {
                std::string t = !line.text.empty() && src < al->size() && (*al)[src].is_string()
                                    ? std::string(trim((*al)[src].get<std::string>()))
                                    : std::string();
                if (t == "\xE2\x99\xAA") t.clear();
                any |= !t.empty();
                lines.push_back(std::move(t));
            }
            if (any) l.translations[lang] = std::move(lines);
        }
    }
    l.lines.reserve(kept.size());
    for (auto& k : kept) l.lines.push_back(std::move(k.first));
    return l;
}

int activeLine(const Lyrics& l, int positionMs) {
    if (!l.synced || l.lines.empty()) return -1;
    auto it = std::upper_bound(l.lines.begin(), l.lines.end(), positionMs,
                               [](int pos, const Line& line) { return pos < line.timeMs; });
    return static_cast<int>(it - l.lines.begin()) - 1;
}

int64_t songTime(int64_t mediaMs, const Ranges& extra) {
    int64_t removed = 0;
    for (const auto& [start, end] : extra) {
        if (mediaMs <= start) break;
        removed += std::min(mediaMs, end) - start;
    }
    return std::max<int64_t>(0, mediaMs - removed);
}

int64_t mediaTime(int64_t songMs, const Ranges& extra) {
    int64_t t = std::max<int64_t>(0, songMs);
    for (const auto& [start, end] : extra) {
        if (start > t) break;
        t += end - start;
    }
    return t;
}

std::optional<Lyrics> fetch(const Query& q, const YoutubeExplode::CancellationToken& ct) {
    std::optional<Lyrics> local;
    if (!q.audioPath.empty()) {
        local = readLocal(q.audioPath);
        if (local && local->synced) return local;
    }
    if (q.title.empty()) return local;
    auto online = fetchOnline(q, ct);
    if (online && (online->synced || online->instrumental)) return online;
    if (local) return local;   // the user's own unsynced text beats an unsynced online one
    return online;
}

// ---- Offsets ---------------------------------------------------------------------------------

Offsets& Offsets::get() {
    static Offsets store(paths::appData() / L"lyrics-offsets.json");
    return store;
}

Offsets::Offsets(fs::path file) : file_(std::move(file)) {}

void Offsets::loadLocked() {
    if (loaded_) return;
    loaded_ = true;
    std::ifstream f(file_, std::ios::binary);
    if (!f) return;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object()) return;
    const auto it = j.find("offsets");
    if (it == j.end() || !it->is_object()) return;
    for (const auto& [id, v] : it->items()) {
        if (!v.is_array() || v.size() != 2 || !v[0].is_number_integer() || !v[1].is_number_integer()) continue;
        const int ms = std::clamp(v[0].get<int>(), -kMaxMs, kMaxMs);
        if (ms != 0) entries_[id] = {ms, v[1].get<int64_t>()};
    }
}

void Offsets::saveLocked() {
    json offsets = json::object();
    for (const auto& [id, e] : entries_) offsets[id] = json::array({e.ms, e.at});
    const std::string s = json{{"v", 1}, {"offsets", std::move(offsets)}}.dump();
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) return;
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        ST_LOG_WARN("lyrics", "could not write lyrics offsets: {}", ec.message());
        fs::remove(tmp, ec);
    }
}

int Offsets::offsetMs(const std::string& trackId) {
    std::lock_guard lock(mutex_);
    loadLocked();
    const auto it = entries_.find(trackId);
    return it == entries_.end() ? 0 : it->second.ms;
}

int Offsets::set(const std::string& trackId, int ms) {
    if (trackId.empty()) return 0;
    std::lock_guard lock(mutex_);
    loadLocked();
    ms = std::clamp(ms, -kMaxMs, kMaxMs);
    if (ms == 0) entries_.erase(trackId);
    else entries_[trackId] = {ms, nowUnix()};
    while (entries_.size() > kMaxEntries) {
        auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                       [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
        entries_.erase(oldest);
    }
    saveLocked();
    return ms;
}

} // namespace st::lyrics
