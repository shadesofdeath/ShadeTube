// Listening statistics store: recording (position deltas), grouping, aggregation and listening.json.
#include "app/ListenStats.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace st::app {

using json = nlohmann::json;
using Track = ListenStats::Track;
using Play = ListenStats::Play;
using Index = ListenStats::Index;

namespace listen {

bool counts(int64_t listenedMs, int64_t durationMs) {
    if (durationMs > 0 && durationMs < kShortTrackMs) return listenedMs >= durationMs / 2;
    return listenedMs >= kStreamMs;
}

int64_t dayNumber(int64_t localSeconds) { return localSeconds >= 0 ? localSeconds / 86400 : -((-localSeconds + 86399) / 86400); }

int weekday(int64_t day) { return static_cast<int>(((day % 7) + 7 + 3) % 7); }   // 1970-01-01 was a Thursday

// Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian).
int64_t daysFromCivil(int year, int month, int dayOfMonth) {
    const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + dayOfMonth - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil(int64_t day, int& year, int& month, int& dayOfMonth) {
    const int64_t z = day + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    dayOfMonth = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

} // namespace listen

// ============================================================================================================
// Local clock

namespace {

// Offset (seconds) of the zone at a UTC instant, through Windows' dynamic time zone data (historic DST rules).
int64_t zoneOffset(const DYNAMIC_TIME_ZONE_INFORMATION& zone, int64_t unixUtc) {
    const int64_t ft = (unixUtc + 11'644'473'600LL) * 10'000'000LL;   // FILETIME: 100 ns since 1601
    FILETIME f{static_cast<DWORD>(ft & 0xffffffff), static_cast<DWORD>(static_cast<uint64_t>(ft) >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local)) return 0;
    FILETIME lf{};
    if (!SystemTimeToFileTime(&local, &lf)) return 0;
    const int64_t l = static_cast<int64_t>((static_cast<uint64_t>(lf.dwHighDateTime) << 32) | lf.dwLowDateTime);
    return l / 10'000'000LL - 11'644'473'600LL - unixUtc;
}

// Caches the offset per UTC day: both ends of a day agree on all but the (at most two) transition days a year, which
// are resolved per hour. Plays arrive in time order, so the last day answers most lookups without hashing.
LocalClock makeClock(DYNAMIC_TIME_ZONE_INFORMATION zone) {
    struct Cache {
        DYNAMIC_TIME_ZONE_INFORMATION zone;
        std::unordered_map<int64_t, int64_t> uniform;          // UTC day -> offset (the same all day)
        std::unordered_map<int64_t, int64_t> hourly;           // UTC hour -> offset (transition days)
        int64_t lastDay = std::numeric_limits<int64_t>::min();
        int64_t lastOffset = 0;
        bool lastUniform = false;
    };
    auto cache = std::make_shared<Cache>();
    cache->zone = zone;
    return [cache](int64_t utc) -> int64_t {
        Cache& c = *cache;
        const int64_t day = listen::dayNumber(utc);
        if (day == c.lastDay && c.lastUniform) return utc + c.lastOffset;
        if (day != c.lastDay) {
            c.lastDay = day;
            if (const auto it = c.uniform.find(day); it != c.uniform.end()) {
                c.lastOffset = it->second;
                c.lastUniform = true;
                return utc + c.lastOffset;
            }
            const int64_t a = zoneOffset(c.zone, day * 86400), b = zoneOffset(c.zone, day * 86400 + 86399);
            c.lastUniform = a == b;
            c.lastOffset = a;
            if (c.lastUniform) {
                c.uniform.emplace(day, a);
                return utc + a;
            }
        }
        const int64_t hour = utc >= 0 ? utc / 3600 : -((-utc + 3599) / 3600);
        auto [it, added] = c.hourly.try_emplace(hour, 0);
        if (added) it->second = zoneOffset(c.zone, utc);
        return utc + it->second;
    };
}

} // namespace

LocalClock systemLocalClock() {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    GetDynamicTimeZoneInformation(&zone);
    return makeClock(zone);
}

LocalClock timeZoneClock(const std::wstring& windowsZoneName) {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &zone) == ERROR_SUCCESS; ++i)
        if (windowsZoneName == zone.TimeZoneKeyName) return makeClock(zone);
    return systemLocalClock();
}

namespace {

constexpr const char* kTag = "stats";

// Grouping fold: case- and accent-insensitive ("Şımarık" == "SIMARIK"). Pure ASCII (most titles) skips the
// Unicode normalization.
std::wstring fold(std::string_view utf8) {
    if (std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
        std::wstring out(utf8.size(), L'\0');
        std::transform(utf8.begin(), utf8.end(), out.begin(), [](char c) {
            return static_cast<wchar_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        return out;
    }
    return foldForSearch(toWide(utf8));
}

const std::string& firstArtist(const Track& t) {
    static const std::string none;
    return t.artists.empty() ? none : t.artists[0].name;
}

// Fills what `into` lacks from `from` (the first sighting wins; later ones only complete it).
void complete(Track& into, const Track& from) {
    if (into.id.empty()) into.id = from.id;
    if (into.image.empty()) into.image = from.image;
    if (into.albumName.empty()) {
        into.albumName = from.albumName;
        into.albumId = from.albumId;
    } else if (into.albumId.empty() && into.albumName == from.albumName) {
        into.albumId = from.albumId;
    }
    if (into.durationMs <= 0) into.durationMs = from.durationMs;
    for (auto& a : into.artists)
        if (a.id.empty())
            for (const auto& b : from.artists)
                if (!b.id.empty() && b.name == a.name) a.id = b.id;
}

// --- listening.json -------------------------------------------------------------------------------------------

// JSON string with UTF-8 validation: an invalid sequence becomes U+FFFD, so the file always parses (nlohmann
// rejects invalid UTF-8, overlong forms and surrogates).
void putString(std::string& out, std::string_view s) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += static_cast<char>(c);
            } else if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        const size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok && n > 2) {
            const auto c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))
                ok = false;
        }
        if (ok) {
            out.append(s.data() + i, n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    out += '"';
}

void putInt(std::string& out, int64_t v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

void putTrack(std::string& out, const Track& t) {
    out += "{\"n\":";
    putString(out, t.name);
    if (!t.id.empty()) {
        out += ",\"id\":";
        putString(out, t.id);
    }
    if (!t.artists.empty()) {
        out += ",\"a\":[";
        for (size_t i = 0; i < t.artists.size(); ++i) {
            out += i ? ",[" : "[";
            putString(out, t.artists[i].id);
            out += ',';
            putString(out, t.artists[i].name);
            out += ']';
        }
        out += ']';
    }
    if (!t.albumId.empty() || !t.albumName.empty()) {
        out += ",\"al\":[";
        putString(out, t.albumId);
        out += ',';
        putString(out, t.albumName);
        out += ']';
    }
    if (!t.image.empty()) {
        out += ",\"img\":";
        putString(out, t.image);
    }
    if (t.durationMs > 0) {
        out += ",\"d\":";
        putInt(out, t.durationMs);
    }
    out += '}';
}

std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

Track trackFromJson(const json& j) {
    Track t;
    if (!j.is_object()) return t;
    t.id = str(j, "id");
    t.name = str(j, "n");
    if (const auto a = j.find("a"); a != j.end() && a->is_array())
        for (const auto& ar : *a)
            if (ar.is_array() && ar.size() == 2 && ar[0].is_string() && ar[1].is_string())
                t.artists.push_back({ar[0].get<std::string>(), ar[1].get<std::string>()});
    if (const auto al = j.find("al"); al != j.end() && al->is_array() && al->size() == 2 && (*al)[0].is_string() &&
                                      (*al)[1].is_string()) {
        t.albumId = (*al)[0].get<std::string>();
        t.albumName = (*al)[1].get<std::string>();
    }
    t.image = str(j, "img");
    if (const auto d = j.find("d"); d != j.end() && d->is_number_integer()) t.durationMs = std::max<int64_t>(0, d->get<int64_t>());
    return t;
}

} // namespace

// ============================================================================================================
// Index

ListenStats::Keys ListenStats::keysFor(const Track& t) {
    Keys k;
    k.firstArtist = fold(firstArtist(t));
    k.track = t.name.empty() ? L"id:" + toWide(t.id) : fold(t.name) + L'\x1f' + k.firstArtist;
    k.artists.reserve(t.artists.size());
    for (const auto& a : t.artists) k.artists.push_back(a.name.empty() ? std::wstring{} : fold(a.name));
    if (!t.albumId.empty()) k.albumId = L"id:" + toWide(t.albumId);
    if (!t.albumName.empty()) k.album = fold(t.albumName) + L'\x1f' + k.firstArtist;
    else k.album = k.albumId;
    return k;
}

uint32_t Index::intern(Track t, const Keys& keys) {
    // Album group: by album id first (a compilation's tracks have different first artists), then by name + first
    // artist (a download or another source without that id). Both keys point at the group from then on.
    auto albumGroup = [&](uint32_t idx) {
        if (keys.album.empty() && keys.albumId.empty()) return;
        auto found = keys.albumId.empty() ? albumByKey.end() : albumByKey.find(keys.albumId);
        if (found == albumByKey.end() && !keys.album.empty()) found = albumByKey.find(keys.album);
        uint32_t g;
        if (found != albumByKey.end()) {
            g = found->second;
            auto& group = albums[g];
            if (!keys.firstArtist.empty() && !group.artistKey.empty() && keys.firstArtist != group.artistKey) group.various = true;
        } else {
            g = static_cast<uint32_t>(albums.size());
            albums.push_back({t.albumId, t.albumName, t.image, firstArtist(t), keys.firstArtist, false});
        }
        if (!keys.albumId.empty()) albumByKey.try_emplace(keys.albumId, g);
        if (!keys.album.empty()) albumByKey.try_emplace(keys.album, g);
        trackAlbum[idx] = static_cast<int32_t>(g);
    };
    auto [it, added] = trackByKey.try_emplace(keys.track, static_cast<uint32_t>(tracks.size()));
    const uint32_t idx = it->second;
    if (added) {
        trackArtists.emplace_back();
        trackAlbum.push_back(-1);
        auto& ta = trackArtists.back();
        for (size_t i = 0; i < t.artists.size() && i < keys.artists.size(); ++i) {
            if (keys.artists[i].empty()) continue;
            auto [g, newGroup] = artistByKey.try_emplace(keys.artists[i], static_cast<uint32_t>(artists.size()));
            if (newGroup) artists.push_back({t.artists[i].id, t.artists[i].name, {}, {}, {}, false});
            if (std::find(ta.begin(), ta.end(), g->second) == ta.end()) ta.push_back(g->second);
        }
        albumGroup(idx);
    } else {
        if (trackAlbum[idx] < 0) albumGroup(idx);   // the first sighting had no album
        complete(tracks[idx], t);
    }
    // Complete the groups from this sighting (an id / cover an earlier source did not have).
    const auto& ta = trackArtists[idx];
    for (size_t i = 0; i < t.artists.size() && i < keys.artists.size(); ++i) {
        if (keys.artists[i].empty() || t.artists[i].id.empty()) continue;
        const auto g = artistByKey.find(keys.artists[i]);
        if (g != artistByKey.end() && artists[g->second].id.empty() && std::find(ta.begin(), ta.end(), g->second) != ta.end())
            artists[g->second].id = t.artists[i].id;
    }
    if (const int32_t al = trackAlbum[idx]; al >= 0) {
        auto& g = albums[static_cast<size_t>(al)];
        if (g.id.empty()) g.id = t.albumId;
        if (g.image.empty()) g.image = t.image;
    }
    if (added) tracks.push_back(std::move(t));
    return idx;
}

// ============================================================================================================
// ListenStats

ListenStats::ListenStats(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) file_ = paths::appData() / L"listening.json";
}

ListenStats::Track ListenStats::fromCatalog(const catalog::Track& t) {
    Track s;
    s.id = t.id;
    s.name = t.name;
    s.artists = t.artists;
    s.albumId = t.album.id;
    s.albumName = t.album.name;
    if (const auto* img = catalog::pickImage(t.album.images, 300)) s.image = img->url;
    s.durationMs = std::max(0, t.durationMs);
    return s;
}

catalog::Track ListenStats::toCatalog(const Track& t) {
    catalog::Track c;
    c.id = t.id;
    c.name = t.name;
    c.artists = t.artists;
    c.album.id = t.albumId;
    c.album.name = t.albumName;
    if (!t.image.empty()) c.album.images.push_back({t.image, 0, 0});
    c.durationMs = static_cast<int>(std::min<int64_t>(t.durationMs, std::numeric_limits<int>::max()));
    return c;
}

void ListenStats::insertPlay(const Play& p) {
    if (plays_.empty() || plays_.back().startedAt <= p.startedAt) {
        plays_.push_back(p);
    } else {
        const auto at = std::upper_bound(plays_.begin(), plays_.end(), p.startedAt,
                                         [](int64_t t, const Play& q) { return t < q.startedAt; });
        plays_.insert(at, p);
    }
    ++(p.imported ? importedCount_ : nativeCount_);
    // Bounded in memory as on disk; trimmed in batches so appends stay amortized O(1).
    if (nativeCount_ > listen::kMaxPlays + 512) trimNative();
}

// Drops the oldest plays of this PC beyond kMaxPlays (imported ones have their own bound, applied by imports).
void ListenStats::trimNative() {
    size_t drop = nativeCount_ > listen::kMaxPlays ? nativeCount_ - listen::kMaxPlays : 0;
    if (!drop) return;
    nativeCount_ -= drop;
    std::erase_if(plays_, [&drop](const Play& p) {
        if (!drop || p.imported) return false;
        --drop;
        return true;
    });
}

// --- reading ---------------------------------------------------------------------------------------------------

namespace {

std::filesystem::path sibling(const std::filesystem::path& file, const wchar_t* suffix) {
    auto p = file;
    p += suffix;
    return p;
}

ListenStats::Snapshot snapshotFromJson(const json& j) {
    ListenStats::Snapshot s;
    std::vector<uint32_t> remap;   // file track index -> index track (the same song twice in the file merges)
    if (const json& tracks = j["tracks"]; tracks.is_array()) {
        remap.reserve(tracks.size());
        for (const auto& t : tracks) remap.push_back(s.index.intern(trackFromJson(t)));
    }
    if (const json& plays = j["plays"]; plays.is_array()) {
        s.plays.reserve(plays.size());
        for (const auto& p : plays) {
            if (!p.is_array() || p.size() != 3 || !p[0].is_number_unsigned() || !p[1].is_number_integer() ||
                !p[2].is_number_integer())
                continue;
            const uint64_t t = p[0].get<uint64_t>();
            const int64_t ms = p[2].get<int64_t>();
            if (t >= remap.size() || ms < listen::kMinPlayMs) continue;
            s.plays.push_back({remap[static_cast<size_t>(t)], p[1].get<int64_t>(), ms});
        }
    }
    std::stable_sort(s.plays.begin(), s.plays.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });
    if (s.plays.size() > listen::kMaxPlays)
        s.plays.erase(s.plays.begin(), s.plays.end() - static_cast<std::ptrdiff_t>(listen::kMaxPlays));
    return s;
}

// listening-imported.json content, interned into `s` (whose index may already hold this PC's tracks). Plays are
// appended unsorted; returns false when the document isn't one.
bool importedFromJson(const json& j, ListenStats::Snapshot& s) {
    const auto tracks = j.find("tracks");
    const auto plays = j.find("plays");
    if (tracks == j.end() || plays == j.end() || !tracks->is_array() || !plays->is_array()) return false;
    std::vector<uint32_t> remap;
    remap.reserve(tracks->size());
    for (const auto& t : *tracks) remap.push_back(s.index.intern(trackFromJson(t)));
    const size_t n = plays->size() / 3;
    s.plays.reserve(s.plays.size() + n);
    int64_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        const json& t = (*plays)[i * 3];
        const json& d = (*plays)[i * 3 + 1];
        const json& ms = (*plays)[i * 3 + 2];
        if (!t.is_number_unsigned() || !d.is_number_integer() || !ms.is_number_integer()) return false;
        at += d.get<int64_t>();
        const uint64_t ti = t.get<uint64_t>();
        const int64_t len = ms.get<int64_t>();
        if (ti >= remap.size() || len < listen::kMinPlayMs) continue;
        s.plays.push_back({remap[static_cast<size_t>(ti)], at, len, true});
    }
    return true;
}

enum class ReadResult { Ok, Missing, Corrupt, Unreadable };

// Reads a whole file: missing, unreadable (locked / I/O error: retried briefly) or read into `text`.
ReadResult readText(const std::filesystem::path& file, std::string& text) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) return ReadResult::Unreadable;   // "not found" is no error
    if (!exists) return ReadResult::Missing;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(200));   // a scanner / backup holding it
        std::ifstream f(file, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        if (f.bad()) continue;
        text = std::move(ss).str();
        return ReadResult::Ok;
    }
    return ReadResult::Unreadable;
}

// One history file: missing, unreadable, unparsable, or parsed into `out`.
ReadResult readOne(const std::filesystem::path& file, ListenStats::Snapshot& out) {
    std::string text;
    if (const ReadResult r = readText(file, text); r != ReadResult::Ok) return r;
    json j = json::parse(text, nullptr, false);
    text = {};
    if (!j.is_object() || !j.contains("tracks") || !j.contains("plays")) return ReadResult::Corrupt;
    out = snapshotFromJson(j);
    return ReadResult::Ok;
}

// One imported file, interned into `out` (on failure `out` is left as it was).
ReadResult readImportedOne(const std::filesystem::path& file, ListenStats::Snapshot& out) {
    std::string text;
    if (const ReadResult r = readText(file, text); r != ReadResult::Ok) return r;
    json j = json::parse(text, nullptr, false);
    text = {};
    if (!j.is_object()) return ReadResult::Corrupt;
    ListenStats::Snapshot s;
    s.index = out.index;   // a copy: a half-read file must not leave tracks behind
    if (!importedFromJson(j, s)) return ReadResult::Corrupt;
    out.index = std::move(s.index);
    out.plays.insert(out.plays.end(), s.plays.begin(), s.plays.end());
    return ReadResult::Ok;
}

// The main file with the .old / .bak fallbacks (see readFile).
ListenStats::Snapshot readMain(const std::filesystem::path& file) {
    using Snapshot = ListenStats::Snapshot;
    Snapshot s;
    const ReadResult main = readOne(file, s);
    if (main == ReadResult::Ok) return s;
    if (main == ReadResult::Unreadable) {   // there but unreadable: start empty and never replace it
        Snapshot locked;
        locked.keepFile = true;
        return locked;
    }
    bool keep = false;
    if (main == ReadResult::Corrupt) {
        // Keep the broken file aside for a look (an existing .bak is not overwritten). If even that fails, the
        // file is never replaced this session.
        std::error_code ec;
        std::filesystem::copy_file(file, sibling(file, L".bak"), std::filesystem::copy_options::skip_existing, ec);
        keep = static_cast<bool>(ec);
    }
    // Missing (a crash between the two renames of write()) or broken: the previous generation.
    Snapshot old;
    const ReadResult prev = readOne(sibling(file, L".old"), old);
    if (prev == ReadResult::Ok) {
        old.fromOld = true;
        old.corrupt = main == ReadResult::Corrupt;
        old.keepFile = keep;
        return old;
    }
    Snapshot empty;
    empty.corrupt = main == ReadResult::Corrupt;
    empty.keepFile = keep;
    return empty;
}

// The imported companion into `s`: missing = none; unreadable or broken without a usable .old = importedKeepFile
// (a broken one is kept as .bak first).
void readImported(const std::filesystem::path& file, ListenStats::Snapshot& s) {
    const ReadResult r = readImportedOne(file, s);
    if (r == ReadResult::Ok || r == ReadResult::Missing) {
        if (r == ReadResult::Missing && readImportedOne(sibling(file, L".old"), s) == ReadResult::Ok)
            ST_LOG_WARN(kTag, "listening-imported.json missing; restored the previous copy (.old)");
        return;
    }
    if (r == ReadResult::Corrupt) {
        s.importedCorrupt = true;
        std::error_code ec;
        std::filesystem::copy_file(file, sibling(file, L".bak"), std::filesystem::copy_options::skip_existing, ec);
        if (readImportedOne(sibling(file, L".old"), s) == ReadResult::Ok) {
            ST_LOG_WARN(kTag, "listening-imported.json is corrupt; restored the previous copy (.old)");
            return;
        }
    }
    s.importedKeepFile = true;
}

} // namespace

std::filesystem::path ListenStats::importedFileFor(const std::filesystem::path& file) {
    return file.parent_path() / (file.stem().wstring() + L"-imported" + file.extension().wstring());
}

ListenStats::Snapshot ListenStats::readFile(const std::filesystem::path& file) {
    Snapshot s;
    try {
        s = readMain(file);
    } catch (const std::exception&) {
        s = {};
        s.keepFile = true;
    }
    const size_t native = s.plays.size();
    try {
        readImported(importedFileFor(file), s);
    } catch (const std::exception&) {
        s.plays.resize(native);
        s.importedKeepFile = true;
    }
    if (s.plays.size() > native) {
        std::stable_sort(s.plays.begin(), s.plays.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });
        if (s.plays.size() - native > listen::kMaxImportedPlays) {   // written by a build with a larger bound
            size_t drop = s.plays.size() - native - listen::kMaxImportedPlays;
            std::erase_if(s.plays, [&drop](const Play& p) {
                if (!drop || !p.imported) return false;
                --drop;
                return true;
            });
        }
    }
    return s;
}

void ListenStats::adopt(Snapshot snapshot) {
    if (loaded_) return;
    loaded_ = true;
    readOnly_ = snapshot.keepFile;
    mainBroken_ = snapshot.corrupt;
    importedReadOnly_ = snapshot.importedKeepFile;
    importedBroken_ = snapshot.importedCorrupt;
    if (snapshot.keepFile) ST_LOG_WARN(kTag, "listening.json unreadable; this session's plays are not saved");
    else if (snapshot.fromOld) ST_LOG_WARN(kTag, "listening.json missing or broken; restored the previous copy (.old)");
    else if (snapshot.corrupt) ST_LOG_WARN(kTag, "listening.json is corrupt; starting empty (file kept as .bak)");
    if (snapshot.importedKeepFile) ST_LOG_WARN(kTag, "listening-imported.json unreadable; imports are off this session");
    // What was recorded before the file was read (usually nothing) goes on top of the file's history.
    Index recorded = std::move(idx_);
    std::vector<Play> recordedPlays = std::move(plays_);
    idx_ = std::move(snapshot.index);
    plays_ = std::move(snapshot.plays);
    importedCount_ = static_cast<size_t>(std::count_if(plays_.begin(), plays_.end(), [](const Play& p) { return p.imported; }));
    nativeCount_ = plays_.size() - importedCount_;
    if (!recordedPlays.empty() || cur_.active) {
        std::vector<int64_t> remap(recorded.tracks.size(), -1);
        auto remapped = [&](uint32_t i) {
            if (remap[i] < 0) remap[i] = idx_.intern(recorded.tracks[i]);
            return static_cast<uint32_t>(remap[i]);
        };
        for (const auto& p : recordedPlays) insertPlay({remapped(p.track), p.startedAt, p.listenedMs});
        if (cur_.active) cur_.track = remapped(cur_.track);
        if (!recordedPlays.empty()) dirty_ = true;
    }
    if (snapshot.fromOld) dirty_ = true;   // put the recovered history back in place
    ST_LOG_INFO(kTag, "listening history: {} plays ({} imported), {} tracks", plays_.size(), importedCount_, idx_.tracks.size());
    notify();
}

// --- writing ---------------------------------------------------------------------------------------------------

namespace {

std::atomic<uint64_t> g_generation{0};
std::mutex g_writeMutex;                                      // one writer per process at a time (worker or exit)
std::unordered_map<std::wstring, uint64_t> g_writtenGeneration;   // per file, under g_writeMutex

std::string serialize(const ListenStats::SaveJob& job) {
    std::string text;
    text.reserve(job.tracks.size() * 200 + job.plays.size() * (job.imported ? 18 : 26) + 64);
    text += job.imported ? "{\"v\":1,\"source\":\"spotify\",\"tracks\":[" : "{\"v\":1,\"tracks\":[";
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        if (i) text += ',';
        putTrack(text, job.tracks[i]);
    }
    text += "],\"plays\":[";
    if (job.imported) {
        // Flat and delta-coded (the plays are in time order): about 12 bytes a play instead of 26.
        int64_t prev = 0;
        for (size_t i = 0; i < job.plays.size(); ++i) {
            if (i) text += ',';
            putInt(text, job.plays[i].track);
            text += ',';
            putInt(text, job.plays[i].startedAt - prev);
            text += ',';
            putInt(text, job.plays[i].listenedMs);
            prev = job.plays[i].startedAt;
        }
    } else {
        for (size_t i = 0; i < job.plays.size(); ++i) {
            text += i ? ",[" : "[";
            putInt(text, job.plays[i].track);
            text += ',';
            putInt(text, job.plays[i].startedAt);
            text += ',';
            putInt(text, job.plays[i].listenedMs);
            text += ']';
        }
    }
    text += "]}";
    return text;
}

// Only the tracks `plays` use, renumbered in first-use order (the plays are rewritten to the new numbers).
std::vector<Track> usedTracks(const std::vector<Track>& all, std::vector<Play>& plays) {
    std::vector<Track> out;
    std::vector<int64_t> newIndex(all.size(), -1);
    for (auto& p : plays) {
        int64_t& n = newIndex[p.track];
        if (n < 0) {
            n = static_cast<int64_t>(out.size());
            out.push_back(all[p.track]);
        }
        p.track = static_cast<uint32_t>(n);
    }
    return out;
}

// Writes `text` to `tmp` and flushes it to the disk. False (and no .tmp left behind) on any failure.
bool writeFlushed(const std::filesystem::path& tmp, const std::string& text) {
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    ok = ok && FlushFileBuffers(h);
    ok = CloseHandle(h) && ok;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

} // namespace

ListenStats::SaveJob ListenStats::makeSaveJob() {
    SaveJob job;
    if (!loaded_ || readOnly_) return job;   // empty file = nothing to write
    job.file = file_;
    job.generation = ++g_generation;
    job.rotate = !mainBroken_;
    // The newest kMaxPlays plays of this PC, the current one (so far) included; only the tracks they use, renumbered.
    const bool withCurrent = currentCounts();
    const size_t keep = std::min(nativeCount_, listen::kMaxPlays - (withCurrent ? 1 : 0));
    job.plays.reserve(keep + 1);
    if (importedCount_ == 0) {
        job.plays.assign(plays_.end() - static_cast<std::ptrdiff_t>(keep), plays_.end());
    } else {
        for (auto it = plays_.rbegin(); it != plays_.rend() && job.plays.size() < keep; ++it)
            if (!it->imported) job.plays.push_back(*it);
        std::reverse(job.plays.begin(), job.plays.end());
    }
    if (withCurrent) job.plays.push_back(currentPlay());
    job.tracks = usedTracks(idx_.tracks, job.plays);
    dirty_ = false;
    return job;
}

ListenStats::SaveJob ListenStats::makeImportedSaveJob() {
    SaveJob job;
    if (!loaded_ || importedReadOnly_ || !importedDirty_) return job;
    job.file = importedFileFor(file_);
    job.generation = ++g_generation;
    job.rotate = !importedBroken_;
    job.imported = true;
    for (const auto& p : plays_)
        if (p.imported) job.plays.push_back(p);
    job.tracks = usedTracks(idx_.tracks, job.plays);
    importedDirty_ = false;
    return job;
}

bool ListenStats::write(const SaveJob& job) {
    if (job.file.empty()) return false;
    try {
        const std::string text = serialize(job);
        std::lock_guard lock(g_writeMutex);
        uint64_t& last = g_writtenGeneration[job.file.wstring()];
        if (job.generation < last) return true;   // a newer save already landed
        const auto tmp = sibling(job.file, L".tmp");
        const std::string name = job.file.filename().string();
        if (!writeFlushed(tmp, text)) {
            ST_LOG_ERROR(kTag, "failed to write {}.tmp (error {})", name, GetLastError());
            return false;
        }
        // Keep the previous generation (readFile falls back to it), then move the new file in. Write-through: both
        // renames are on the disk before we return. A failed rotation only costs the fallback copy.
        if (job.rotate && GetFileAttributesW(job.file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(job.file.c_str(), sibling(job.file, L".old").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ST_LOG_WARN(kTag, "could not keep {}.old (error {})", name, GetLastError());
        if (!MoveFileExW(tmp.c_str(), job.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ST_LOG_ERROR(kTag, "failed to replace {} (error {})", name, GetLastError());
            DeleteFileW(tmp.c_str());
            return false;
        }
        last = job.generation;
        return true;
    } catch (const std::exception& e) {
        ST_LOG_ERROR(kTag, "save: {}", e.what());
        return false;
    }
}

bool ListenStats::save() {
    const SaveJob imported = makeImportedSaveJob();
    if (!imported.file.empty()) {
        if (write(imported)) importedBroken_ = false;
        else importedDirty_ = true;
    }
    const SaveJob job = makeSaveJob();
    if (job.file.empty()) return false;
    if (write(job)) {
        mainBroken_ = false;
        return !importedDirty_;
    }
    dirty_ = true;
    return false;
}

void ListenStats::saveAsync() {
    struct Jobs {
        SaveJob main, imported;
    };
    auto jobs = std::make_shared<Jobs>();
    jobs->imported = makeImportedSaveJob();
    jobs->main = makeSaveJob();
    if (jobs->main.file.empty() && jobs->imported.file.empty()) return;
    async(Priority::Low, life_.ref(),
          [jobs] {
              const bool imported = jobs->imported.file.empty() || write(jobs->imported);
              const bool main = jobs->main.file.empty() || write(jobs->main);
              return std::pair{main, imported};
          },
          [this, jobs](Result<std::pair<bool, bool>> r) {
              // Failures are retried by the next housekeeping save.
              if (!jobs->main.file.empty()) {
                  if (r && r->first) mainBroken_ = false;
                  else dirty_ = true;
              }
              if (!jobs->imported.file.empty()) {
                  if (r && r->second) importedBroken_ = false;
                  else importedDirty_ = true;
              }
          });
}

// --- recording ---------------------------------------------------------------------------------------------------

void ListenStats::trackStarted(const catalog::Track& t, int64_t positionMs, int64_t unixNow, int64_t wallMs) {
    const bool same = cur_.active && (t.id.empty() ? t.name == cur_.name : t.id == cur_.id);
    if (same && positionMs > 0 && positionMs + listen::kRestartMs >= cur_.lastPosMs) {
        // Reloaded at about the same spot: the same play goes on (the reload gap itself is not listening).
        cur_.lastPosMs = positionMs;
        cur_.lastWallMs = wallMs;
        cur_.lastPlaying = false;
        complete(idx_.tracks[cur_.track], fromCatalog(t));
        return;
    }
    finishCurrent(wallMs);
    cur_ = {};
    cur_.active = true;
    cur_.id = t.id;
    cur_.name = t.name;
    cur_.track = idx_.intern(fromCatalog(t));
    cur_.startedAt = unixNow;
    cur_.lastPosMs = positionMs;
    cur_.lastWallMs = wallMs;
}

void ListenStats::progress(int64_t positionMs, bool playing, int64_t durationMs, int64_t wallMs, int64_t unixNow) {
    if (!cur_.active) return;
    if (auto& t = idx_.tracks[cur_.track]; t.durationMs <= 0 && durationMs > 0) t.durationMs = durationMs;
    const int64_t delta = positionMs - cur_.lastPosMs;
    const int64_t elapsed = std::max<int64_t>(0, wallMs - cur_.lastWallMs);
    if (playing || cur_.lastPlaying) {
        if (delta > 0) {
            // Audio can't outrun the wall clock: whatever the position jumped beyond it is a seek (the 5 s arrow-key
            // step, SponsorBlock, a lyric line), not listening. A little slack covers sampling jitter.
            cur_.listenedMs += std::min(delta, elapsed + elapsed / listen::kClockJitterDiv);
        } else if (delta < 0 && cur_.lastPlaying) {
            // A backward seek: the audio before the jump was listening (bounded; samples come every ~2 s).
            cur_.listenedMs += std::min(elapsed, listen::kGapCreditMs);
        }
    }
    if (delta < 0 && positionMs < listen::kRestartMs && cur_.lastPosMs - positionMs > listen::kRestartMs) {
        // Back to the top of the same track (Previous restarts it with a seek, no track change): a new play.
        storeCurrent();
        cur_.startedAt = unixNow;
        cur_.listenedMs = 0;
    }
    cur_.lastPosMs = positionMs;
    cur_.lastWallMs = wallMs;
    cur_.lastPlaying = playing;
}

void ListenStats::stop(int64_t wallMs) { finishCurrent(wallMs); }

void ListenStats::finishCurrent(int64_t wallMs) {
    if (!cur_.active) return;
    cur_.active = false;
    if (cur_.lastPlaying) {
        // The player has already moved on: credit the stretch since the last sample (bounded, up to the track's end).
        int64_t tail = std::min(std::max<int64_t>(0, wallMs - cur_.lastWallMs), listen::kGapCreditMs);
        if (const int64_t d = idx_.tracks[cur_.track].durationMs; d > 0) tail = std::min(tail, std::max<int64_t>(0, d - cur_.lastPosMs));
        cur_.listenedMs += tail;
    }
    storeCurrent();
}

void ListenStats::storeCurrent() {
    if (cur_.listenedMs < listen::kMinPlayMs) return;
    const Play p = currentPlay();
    insertPlay(p);
    if (importing_) recordedDuringImport_.emplace_back(idx_.tracks[p.track], p);
    dirty_ = true;
    notify();
}

void ListenStats::addPlay(const catalog::Track& t, int64_t startedAt, int64_t listenedMs) {
    if (listenedMs < listen::kMinPlayMs) return;
    const Play p{idx_.intern(fromCatalog(t)), startedAt, listenedMs};
    insertPlay(p);
    if (importing_) recordedDuringImport_.emplace_back(idx_.tracks[p.track], p);
    dirty_ = true;
    notify();
}

void ListenStats::clear(int64_t unixNow) {
    Track current = cur_.active ? idx_.tracks[cur_.track] : Track{};
    idx_ = {};
    plays_ = {};
    if (cur_.active) {
        cur_.track = idx_.intern(std::move(current));
        cur_.startedAt = unixNow;
        cur_.listenedMs = 0;
    }
    cancelImport();     // a running import would bring its history back
    importedDirty_ = importedCount_ > 0 || importedReadOnly_;   // the imported file is emptied too
    nativeCount_ = importedCount_ = 0;
    loaded_ = true;     // a load still in flight must not bring the history back
    readOnly_ = false;  // the user chose to drop the old history
    importedReadOnly_ = false;
    dirty_ = true;
    ST_LOG_INFO(kTag, "listening history cleared");
    notify();
}

// --- importing -------------------------------------------------------------------------------------------------------

ListenStats::ImportBase ListenStats::beginImport() {
    ImportBase base;
    if (!canImport()) return base;   // empty importedFile = refused
    // A removal still waiting for its save: the job below reads the imported file, so empty it first (tiny write).
    if (importedDirty_) {
        const SaveJob pending = makeImportedSaveJob();
        if (!pending.file.empty() && !write(pending)) {
            importedDirty_ = true;
            return base;
        }
        importedBroken_ = false;
    }
    importing_ = true;
    base.token = ++importToken_;
    base.importedFile = importedFileFor(file_);
    base.plays.reserve(nativeCount_);
    for (const auto& p : plays_)
        if (!p.imported) base.plays.push_back(p);
    base.tracks = usedTracks(idx_.tracks, base.plays);
    recordedDuringImport_.clear();
    return base;
}

void ListenStats::cancelImport() {
    importing_ = false;
    ++importToken_;
    recordedDuringImport_.clear();
}

namespace {

// Is `c` (a new row) a play already in `known` (the same song's plays, ordered by start)? See listen::kSameEndS.
bool knownPlay(const std::vector<std::pair<int64_t, int64_t>>& known, int64_t startMs, int64_t lenMs) {
    constexpr int64_t kScanMs = 6 * 3600 * 1000LL;   // plays that could overlap start at most this much earlier
    const int64_t endMs = startMs + lenMs;
    auto it = std::lower_bound(known.begin(), known.end(), startMs - kScanMs,
                               [](const std::pair<int64_t, int64_t>& k, int64_t t) { return k.first < t; });
    for (; it != known.end() && it->first <= endMs + listen::kSameEndS * 1000; ++it) {
        const int64_t kEnd = it->first + it->second;
        const int64_t overlap = std::min(endMs, kEnd) - std::max(startMs, it->first);
        if (overlap > 0 && overlap * 2 >= std::min(lenMs, it->second)) return true;
        if (std::abs(endMs - kEnd) <= listen::kSameEndS * 1000 && std::abs(lenMs - it->second) <= listen::kSameLengthMs)
            return true;
    }
    return false;
}

} // namespace

ListenStats::ImportResult ListenStats::buildImport(const ImportBase& base, std::vector<ImportRow> rows) {
    ImportResult r;
    r.token = base.token;
    r.rows = rows.size();
    if (base.importedFile.empty()) {
        r.error = "import refused";
        return r;
    }
    try {
        Snapshot& s = r.snapshot;
        // This PC's plays, then what was imported before.
        std::vector<uint32_t> remap;
        remap.reserve(base.tracks.size());
        for (const auto& t : base.tracks) remap.push_back(s.index.intern(t));
        s.plays.reserve(base.plays.size());
        for (const auto& p : base.plays) s.plays.push_back({remap[p.track], p.startedAt, p.listenedMs, false});
        remap = {};
        readImported(base.importedFile, s);
        if (s.importedKeepFile) {
            r.error = "listening-imported.json unreadable";
            return r;
        }
        std::stable_sort(s.plays.begin(), s.plays.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });

        // The new rows, in time order.
        std::vector<Play> fresh;
        fresh.reserve(rows.size());
        for (auto& row : rows) {
            if (row.listenedMs < listen::kMinPlayMs) {
                ++r.tooShort;
                continue;
            }
            const int64_t start = row.endedAt - (row.listenedMs + 500) / 1000;
            fresh.push_back({s.index.intern(std::move(row.track)), start, row.listenedMs, true});
        }
        rows = {};
        std::stable_sort(fresh.begin(), fresh.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });

        // Everything known per song (only the songs the rows mention), then each row against it and the rows before.
        std::unordered_map<uint32_t, std::vector<std::pair<int64_t, int64_t>>> known;   // track -> (start ms, length ms)
        for (const auto& p : fresh) known.try_emplace(p.track);
        for (const auto& p : s.plays)
            if (auto it = known.find(p.track); it != known.end()) it->second.emplace_back(p.startedAt * 1000, p.listenedMs);
        std::vector<Play> added;
        added.reserve(fresh.size());
        for (const auto& p : fresh) {
            auto& k = known[p.track];
            const int64_t startMs = p.startedAt * 1000;
            if (knownPlay(k, startMs, p.listenedMs)) {
                ++r.duplicates;
                continue;
            }
            const auto at = std::upper_bound(k.begin(), k.end(), startMs,
                                             [](int64_t t, const std::pair<int64_t, int64_t>& e) { return t < e.first; });
            k.emplace(at, startMs, p.listenedMs);
            added.push_back(p);
        }
        fresh = {};
        known = {};
        r.added = added.size();
        if (added.empty()) {   // nothing new: nothing to write or install
            r.snapshot = {};
            r.ok = true;
            return r;
        }
        const auto mid = static_cast<std::ptrdiff_t>(s.plays.size());
        s.plays.insert(s.plays.end(), added.begin(), added.end());
        added = {};
        std::inplace_merge(s.plays.begin(), s.plays.begin() + mid, s.plays.end(),
                           [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });
        // The imported bound: the oldest go.
        size_t importedTotal = static_cast<size_t>(std::count_if(s.plays.begin(), s.plays.end(), [](const Play& p) { return p.imported; }));
        if (importedTotal > listen::kMaxImportedPlays) {
            size_t drop = importedTotal - listen::kMaxImportedPlays;
            std::erase_if(s.plays, [&drop](const Play& p) {
                if (!drop || !p.imported) return false;
                --drop;
                return true;
            });
        }

        SaveJob job;
        job.file = base.importedFile;
        job.generation = ++g_generation;
        job.rotate = !s.importedCorrupt;
        job.imported = true;
        for (const auto& p : s.plays)
            if (p.imported) job.plays.push_back(p);
        job.tracks = usedTracks(s.index.tracks, job.plays);
        if (!write(job)) {
            r.error = "writing listening-imported.json failed";
            r.snapshot = {};
            return r;
        }
        s.importedCorrupt = false;
        r.ok = true;
        ST_LOG_INFO(kTag, "import: {} rows, {} new plays, {} already known, {} too short", r.rows, r.added, r.duplicates, r.tooShort);
        return r;
    } catch (const std::exception& e) {
        ST_LOG_ERROR(kTag, "import: {}", e.what());
        r = {};
        r.token = base.token;
        r.error = e.what();
        return r;
    }
}

bool ListenStats::finishImport(ImportResult result) {
    if (!importing_ || result.token != importToken_) {   // superseded (a clear while it ran)
        // Its worker may have written the imported file after the clear emptied it: write what the store holds again.
        if (result.ok && result.added > 0 && loaded_ && !importedReadOnly_) importedDirty_ = true;
        return false;
    }
    importing_ = false;
    auto recorded = std::move(recordedDuringImport_);
    recordedDuringImport_.clear();
    if (!result.ok || result.added == 0) return false;   // nothing changed; what was recorded is already in place
    Track current = cur_.active ? idx_.tracks[cur_.track] : Track{};
    idx_ = std::move(result.snapshot.index);
    plays_ = std::move(result.snapshot.plays);
    importedCount_ = static_cast<size_t>(std::count_if(plays_.begin(), plays_.end(), [](const Play& p) { return p.imported; }));
    nativeCount_ = plays_.size() - importedCount_;
    for (auto& [t, p] : recorded) insertPlay({idx_.intern(std::move(t)), p.startedAt, p.listenedMs});
    if (cur_.active) cur_.track = idx_.intern(std::move(current));
    importedBroken_ = false;
    ST_LOG_INFO(kTag, "listening history: {} plays ({} imported), {} tracks", plays_.size(), importedCount_, idx_.tracks.size());
    notify();
    return true;
}

void ListenStats::removeImported() {
    if (importing_) cancelImport();
    if (importedCount_ == 0) return;
    std::erase_if(plays_, [](const Play& p) { return p.imported; });
    importedCount_ = 0;
    importedDirty_ = !importedReadOnly_;
    ST_LOG_INFO(kTag, "imported listening history removed");
    notify();
}

// --- aggregation ---------------------------------------------------------------------------------------------------

ListenStats::Tally ListenStats::newTally() const {
    Tally t;
    t.tracks.resize(idx_.tracks.size());
    t.artists.resize(idx_.artists.size());
    t.albums.resize(idx_.albums.size());
    return t;
}

bool ListenStats::tally(Tally& t, const Play& p) const {
    t.listenedMs += p.listenedMs;
    if (!listen::counts(p.listenedMs, idx_.tracks[p.track].durationMs)) return false;
    ++t.streams;
    auto bump = [&](Acc& a) {
        ++a.streams;
        a.ms += p.listenedMs;
        a.last = std::max(a.last, p.startedAt);
    };
    bump(t.tracks[p.track]);
    for (uint32_t a : idx_.trackArtists[p.track]) bump(t.artists[a]);
    if (const int32_t a = idx_.trackAlbum[p.track]; a >= 0) bump(t.albums[static_cast<size_t>(a)]);
    return true;
}

void ListenStats::tops(const Tally& t, size_t topN, std::vector<StatsTrack>& tracks, std::vector<StatsArtist>& artists,
                       std::vector<StatsAlbum>& albums, int& distinctTracks, int& distinctArtists) const {
    // Most streams, then more time, then the most recent. Returns (all with a stream, top n of them).
    auto top = [topN](const std::vector<Acc>& acc) {
        std::vector<uint32_t> idx;
        for (uint32_t i = 0; i < acc.size(); ++i)
            if (acc[i].streams > 0) idx.push_back(i);
        auto better = [&acc](uint32_t a, uint32_t b) {
            if (acc[a].streams != acc[b].streams) return acc[a].streams > acc[b].streams;
            if (acc[a].ms != acc[b].ms) return acc[a].ms > acc[b].ms;
            return acc[a].last > acc[b].last;
        };
        const size_t n = std::min(topN, idx.size());
        std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(n), idx.end(), better);
        return std::pair{idx, n};
    };
    {
        const auto [idx, n] = top(t.tracks);
        distinctTracks = static_cast<int>(idx.size());
        for (size_t i = 0; i < n; ++i) tracks.push_back({toCatalog(idx_.tracks[idx[i]]), t.tracks[idx[i]].streams, t.tracks[idx[i]].ms});
    }
    {
        const auto [idx, n] = top(t.artists);
        distinctArtists = static_cast<int>(idx.size());
        for (size_t i = 0; i < n; ++i) {
            const auto& g = idx_.artists[idx[i]];
            artists.push_back({{g.id, g.name}, t.artists[idx[i]].streams, t.artists[idx[i]].ms});
        }
    }
    {
        const auto [idx, n] = top(t.albums);
        for (size_t i = 0; i < n; ++i) {
            const auto& g = idx_.albums[idx[i]];
            StatsAlbum a;
            a.album.id = g.id;
            a.album.name = g.name;
            if (!g.image.empty()) a.album.images.push_back({g.image, 0, 0});
            a.artist = g.artist;
            a.variousArtists = g.various;
            a.streams = t.albums[idx[i]].streams;
            a.listenedMs = t.albums[idx[i]].ms;
            albums.push_back(std::move(a));
        }
    }
}

StatsSummary ListenStats::summarize(StatsPeriod period, int64_t unixNow, size_t topN, size_t recentN) const {
    StatsSummary s;
    const int64_t start = period == StatsPeriod::Week    ? unixNow - 7 * 86400
                          : period == StatsPeriod::Month ? unixNow - 30 * 86400
                                                         : std::numeric_limits<int64_t>::min();
    const bool withCurrent = currentCounts();
    if (period == StatsPeriod::All) s.periodStart = !plays_.empty() ? plays_.front().startedAt : withCurrent ? cur_.startedAt : 0;
    else s.periodStart = start;

    Tally t = newTally();
    const auto first = std::lower_bound(plays_.begin(), plays_.end(), start,
                                        [](const Play& q, int64_t v) { return q.startedAt < v; });
    for (auto it = first; it != plays_.end(); ++it) tally(t, *it);
    const Play current = currentPlay();
    const bool currentIn = withCurrent && cur_.startedAt >= start;
    if (currentIn) tally(t, current);
    s.listenedMs = t.listenedMs;
    s.streams = t.streams;

    // Recent streams, newest first (the current play is the newest once it counts).
    auto pushRecent = [&](const Play& p) {
        if (s.recent.size() >= recentN || !listen::counts(p.listenedMs, idx_.tracks[p.track].durationMs)) return;
        s.recent.push_back({toCatalog(idx_.tracks[p.track]), p.startedAt, p.listenedMs});
    };
    if (currentIn) pushRecent(current);
    for (auto it = plays_.rbegin(); it != std::make_reverse_iterator(first) && s.recent.size() < recentN; ++it) pushRecent(*it);

    tops(t, topN, s.topTracks, s.topArtists, s.topAlbums, s.distinctTracks, s.distinctArtists);
    return s;
}

namespace {

// Adds `p`'s listening to the heatmap: from its start on, split at every local hour boundary it crosses.
void spread(StatsHeatmap& h, const LocalClock& clock, int64_t startedAt, int64_t listenedMs) {
    int64_t at = startedAt, left = listenedMs;
    for (int guard = 0; left > 0 && guard < 48; ++guard) {
        const int64_t local = clock(at);
        const int64_t day = listen::dayNumber(local);
        const int64_t secOfDay = local - day * 86400;
        const int hour = static_cast<int>(secOfDay / 3600);
        const int64_t toNext = (3600 - secOfDay % 3600) * 1000;
        const int64_t part = std::min(left, toNext);
        h.ms[static_cast<size_t>(listen::weekday(day) * 24 + hour)] += part;
        left -= part;
        at += (part + 999) / 1000;
    }
    h.totalMs += listenedMs;
}

void finishHeatmap(StatsHeatmap& h) {
    h.maxMs = 0;
    for (int64_t v : h.ms) h.maxMs = std::max(h.maxMs, v);
}

// Local calendar year of a unix instant.
int localYear(const LocalClock& clock, int64_t unixUtc) {
    int y, m, d;
    listen::civil(listen::dayNumber(clock(unixUtc)), y, m, d);
    return y;
}

} // namespace

std::vector<int> ListenStats::years(const LocalClock& clock) const {
    std::vector<int> out;   // ascending while collecting
    auto add = [&](int64_t at) {
        const int y = localYear(clock, at);
        if (std::find(out.begin(), out.end(), y) == out.end()) out.push_back(y);
    };
    // Plays are in time order: jump to the first play of the next year instead of converting every one.
    for (auto it = plays_.begin(); it != plays_.end();) {
        const int y = localYear(clock, it->startedAt);
        if (out.empty() || out.back() != y) add(it->startedAt);
        // Anything before (next Jan 1 local, minus the largest UTC offset) is still this year or next year's first day.
        const int64_t nextYearUtc = listen::daysFromCivil(y + 1, 1, 1) * 86400 - 14 * 3600;
        auto next = std::lower_bound(it, plays_.end(), nextYearUtc, [](const Play& q, int64_t v) { return q.startedAt < v; });
        if (next == it) ++next;
        // Between `it` and `next` everything is year y (or earlier): walk the few plays around the boundary exactly.
        it = next;
        while (it != plays_.end() && localYear(clock, it->startedAt) == y) ++it;
    }
    if (currentCounts()) add(cur_.startedAt);
    std::sort(out.begin(), out.end(), std::greater<>());
    return out;
}

YearSummary ListenStats::summarizeYear(int year, const LocalClock& clock, size_t topN) const {
    YearSummary y;
    y.year = year;
    // UTC range that surely contains the local year (offsets are within +-14 h), filtered exactly by the clock.
    const int64_t fromDay = listen::daysFromCivil(year, 1, 1), toDay = listen::daysFromCivil(year + 1, 1, 1);
    const int64_t lo = fromDay * 86400 - 14 * 3600, hi = toDay * 86400 + 14 * 3600;
    auto first = std::lower_bound(plays_.begin(), plays_.end(), lo, [](const Play& q, int64_t v) { return q.startedAt < v; });

    // Artists streamed in an earlier year (for "new artists"): every stream before this year's range, plus the ones in
    // the boundary margin that the clock puts in an earlier year.
    std::vector<char> seenBefore(idx_.artists.size(), 0);
    for (auto it = plays_.begin(); it != first; ++it)
        if (listen::counts(it->listenedMs, idx_.tracks[it->track].durationMs))
            for (uint32_t a : idx_.trackArtists[it->track]) seenBefore[a] = 1;

    Tally t = newTally();
    std::vector<int64_t> streamDays;   // local day numbers with a stream, ascending
    std::vector<int64_t> dayExample;   // a stream's unix time on each of those days
    auto take = [&](const Play& p) {
        const int64_t local = clock(p.startedAt);
        const int64_t day = listen::dayNumber(local);
        if (day < fromDay) {   // an earlier year (boundary margin)
            if (listen::counts(p.listenedMs, idx_.tracks[p.track].durationMs))
                for (uint32_t a : idx_.trackArtists[p.track]) seenBefore[a] = 1;
            return;
        }
        if (day >= toDay) return;
        int yy, mm, dd;
        listen::civil(day, yy, mm, dd);
        y.monthMs[static_cast<size_t>(mm - 1)] += p.listenedMs;
        y.weekdayMs[static_cast<size_t>(listen::weekday(day))] += p.listenedMs;
        spread(y.heatmap, clock, p.startedAt, p.listenedMs);
        if (!tally(t, p)) return;
        if (!y.firstStream || p.startedAt < y.firstStream->startedAt)
            y.firstStream = StatsPlay{toCatalog(idx_.tracks[p.track]), p.startedAt, p.listenedMs};
        if (streamDays.empty() || streamDays.back() != day) {
            streamDays.push_back(day);
            dayExample.push_back(p.startedAt);
        }
    };
    for (auto it = first; it != plays_.end() && it->startedAt < hi; ++it) take(*it);
    if (currentCounts() && cur_.startedAt >= lo && cur_.startedAt < hi) take(currentPlay());
    finishHeatmap(y.heatmap);

    y.listenedMs = t.listenedMs;
    y.streams = t.streams;
    for (size_t a = 0; a < t.artists.size(); ++a)
        if (t.artists[a].streams > 0 && !seenBefore[a]) ++y.newArtists;
    for (size_t m = 0; m < 12; ++m)
        if (y.monthMs[m] > 0 && (y.topMonth < 0 || y.monthMs[m] > y.monthMs[static_cast<size_t>(y.topMonth)])) y.topMonth = static_cast<int>(m);
    for (size_t d = 0; d < 7; ++d)
        if (y.weekdayMs[d] > 0 && (y.topWeekday < 0 || y.weekdayMs[d] > y.weekdayMs[static_cast<size_t>(y.topWeekday)])) y.topWeekday = static_cast<int>(d);

    // The current play may be the only one out of time order: days are sorted before the streak walk.
    if (!std::is_sorted(streamDays.begin(), streamDays.end())) {
        std::vector<std::pair<int64_t, int64_t>> days;
        for (size_t i = 0; i < streamDays.size(); ++i) days.emplace_back(streamDays[i], dayExample[i]);
        std::sort(days.begin(), days.end());
        days.erase(std::unique(days.begin(), days.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), days.end());
        streamDays.clear();
        dayExample.clear();
        for (const auto& [d, ex] : days) {
            streamDays.push_back(d);
            dayExample.push_back(ex);
        }
    }
    y.activeDays = static_cast<int>(streamDays.size());
    for (size_t i = 0; i < streamDays.size();) {
        size_t j = i + 1;
        while (j < streamDays.size() && streamDays[j] == streamDays[j - 1] + 1) ++j;
        if (static_cast<int>(j - i) > y.longestStreak) {
            y.longestStreak = static_cast<int>(j - i);
            y.streakFrom = dayExample[i];
            y.streakTo = dayExample[j - 1];
        }
        i = j;
    }
    tops(t, topN, y.topTracks, y.topArtists, y.topAlbums, y.distinctTracks, y.distinctArtists);
    return y;
}

StatsHeatmap ListenStats::heatmap(StatsPeriod period, int64_t unixNow, const LocalClock& clock) const {
    StatsHeatmap h;
    const int64_t start = period == StatsPeriod::Week    ? unixNow - 7 * 86400
                          : period == StatsPeriod::Month ? unixNow - 30 * 86400
                                                         : std::numeric_limits<int64_t>::min();
    const auto first = std::lower_bound(plays_.begin(), plays_.end(), start,
                                        [](const Play& q, int64_t v) { return q.startedAt < v; });
    for (auto it = first; it != plays_.end(); ++it) spread(h, clock, it->startedAt, it->listenedMs);
    if (currentCounts() && cur_.startedAt >= start) spread(h, clock, cur_.startedAt, cur_.listenedMs);
    finishHeatmap(h);
    return h;
}

// --- notifications ---------------------------------------------------------------------------------------------------

void ListenStats::subscribe(Lifetime::Ref owner, std::function<void()> fn) {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    listeners_.emplace_back(std::move(owner), std::move(fn));
}

void ListenStats::notify() {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    const auto snapshot = listeners_;   // a listener may rebuild a page that re-subscribes
    for (const auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

} // namespace st::app
