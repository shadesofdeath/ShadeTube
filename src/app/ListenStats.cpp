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
#include <cstring>
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

const char* importSourceName(ImportSource s) {
    switch (s) {
    case ImportSource::LastFm: return "lastfm";
    case ImportSource::ListenBrainz: return "listenbrainz";
    default: return "spotify";
    }
}

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

// Room for one more element, growing by a sixteenth (at least 256) instead of the library's half: a big history that
// gains a song or a play keeps ~6 % of slack, not a third (+2.8 MB of tracks for 30 000 songs after the first new
// one; with the half the old and the new block are also both alive while it moves). Still amortized O(1).
template <class V>
void roomForOne(V& v) {
    if (v.size() == v.capacity()) v.reserve(v.size() + std::max<size_t>(256, v.size() / 16));
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
            roomForOne(albums);
            albums.push_back({t.albumId, t.albumName, t.image, firstArtist(t), keys.firstArtist, false});
        }
        if (!keys.albumId.empty()) albumByKey.try_emplace(keys.albumId, g);
        if (!keys.album.empty()) albumByKey.try_emplace(keys.album, g);
        trackAlbum[idx] = static_cast<int32_t>(g);
    };
    auto [it, added] = trackByKey.try_emplace(keys.track, static_cast<uint32_t>(tracks.size()));
    const uint32_t idx = it->second;
    if (added) {
        roomForOne(trackArtists);
        roomForOne(trackAlbum);
        trackArtists.emplace_back();
        trackAlbum.push_back(-1);
        auto& ta = trackArtists.back();
        for (size_t i = 0; i < t.artists.size() && i < keys.artists.size(); ++i) {
            if (keys.artists[i].empty()) continue;
            auto [g, newGroup] = artistByKey.try_emplace(keys.artists[i], static_cast<uint32_t>(artists.size()));
            if (newGroup) {
                roomForOne(artists);
                artists.push_back({t.artists[i].id, t.artists[i].name, {}, {}, {}, false});
            }
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
    if (added) {
        roomForOne(tracks);
        tracks.push_back(std::move(t));
    }
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
    roomForOne(plays_);
    if (plays_.empty() || plays_.back().startedAt <= p.startedAt) {
        plays_.push_back(p);
    } else {
        const auto at = std::upper_bound(plays_.begin(), plays_.end(), p.startedAt,
                                         [](int64_t t, const Play& q) { return t < q.startedAt; });
        plays_.insert(at, p);
    }
    if (p.imported) {
        ++importedCount_;
        ++importedCounts_[static_cast<size_t>(p.source)];
    } else {
        ++nativeCount_;
    }
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

// An imported file, read as a stream (SAX): no document tree and no copy of the index, so reading a big history (300 000
// imported plays) costs little more than the plays and songs themselves. Collects the file's tracks (each object built
// alone), its flat "plays" numbers and "sync"; nothing reaches the snapshot until the whole file has parsed.
class ImportedReader final : public json::json_sax_t {
public:
    std::vector<Track> tracks;
    std::vector<Play> plays;                       // track = the file's index (remapped by the caller)
    std::vector<ListenStats::ImportCursor> cursors;
    bool sawTracks = false, sawPlays = false, bad = false;

    bool null() override { return scalar(json(nullptr)); }
    bool boolean(bool v) override { return scalar(json(v)); }
    bool number_integer(number_integer_t v) override { return number(v, false) && scalar(json(v)); }
    bool number_unsigned(number_unsigned_t v) override {
        const auto n = static_cast<int64_t>(std::min<uint64_t>(v, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        return number(n, true) && scalar(json(v));
    }
    bool number_float(number_float_t v, const string_t&) override {
        if (inPlays()) return fail();
        return scalar(json(v));
    }
    bool string(string_t& v) override {
        if (inPlays()) return fail();
        return scalar(json(std::move(v)));
    }
    bool binary(binary_t&) override { return fail(); }
    bool start_object(std::size_t) override {
        ++depth_;
        if (inPlays()) return fail();
        if (building()) open(json::object());
        return true;
    }
    bool key(string_t& k) override {
        if (depth_ == 1)
            section_ = k == "tracks" ? Section::Tracks : k == "plays" ? Section::Plays : k == "sync" ? Section::Sync : Section::Other;
        else if (building() && !keys_.empty())
            keys_.back() = std::move(k);
        return true;
    }
    bool end_object() override { return closeContainer(); }
    bool start_array(std::size_t) override {
        ++depth_;
        if (depth_ == 2) {
            if (section_ == Section::Tracks) sawTracks = true;
            if (section_ == Section::Plays) sawPlays = true;
            return true;
        }
        if (inPlays()) return fail();
        if (building()) open(json::array());
        return true;
    }
    bool end_array() override { return closeContainer(); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return fail(); }

private:
    enum class Section { None, Tracks, Plays, Sync, Other };
    // Elements of "tracks" / "sync" (depth 3 and below) are built as small documents.
    bool building() const { return depth_ >= 3 && (section_ == Section::Tracks || section_ == Section::Sync); }
    bool inPlays() const { return section_ == Section::Plays && depth_ >= 2; }
    bool fail() {
        bad = true;
        return false;
    }
    void open(json v) {
        stack_.push_back(std::move(v));
        keys_.emplace_back();
    }
    bool closeContainer() {
        if (building() && !stack_.empty()) {
            json v = std::move(stack_.back());
            stack_.pop_back();
            keys_.pop_back();
            --depth_;
            return attach(std::move(v));
        }
        --depth_;
        return true;
    }
    bool scalar(json v) {
        if (!building() || stack_.empty()) return true;   // "v", "source", a play's number, a bare element: not kept
        return attach(std::move(v));
    }
    // A finished value: into its parent, or (an element of "tracks" / "sync") taken.
    bool attach(json v) {
        if (!stack_.empty()) {
            json& top = stack_.back();
            if (top.is_array()) top.push_back(std::move(v));
            else top[keys_.back()] = std::move(v);
            return true;
        }
        if (section_ == Section::Tracks) {
            tracks.push_back(trackFromJson(v));
        } else if (v.is_object() && v.contains("account") && v["account"].is_string() && v.contains("next") &&
                   v["next"].is_number_integer()) {
            cursors.push_back({v["account"].get<std::string>(), v["next"].get<int64_t>()});
        }
        return true;
    }
    // The flat plays: track index (unsigned), start delta, length; three at a time.
    bool number(int64_t v, bool isUnsigned) {
        if (section_ != Section::Plays || depth_ != 2) return true;
        switch (slot_) {
        case 0:
            if (!isUnsigned) return fail();
            cur_.track = static_cast<uint32_t>(std::min<int64_t>(v, std::numeric_limits<uint32_t>::max()));
            break;
        case 1:
            at_ += v;
            cur_.startedAt = at_;
            break;
        default:
            cur_.listenedMs = v;
            if (v >= listen::kMinPlayMs) plays.push_back(cur_);
            break;
        }
        slot_ = (slot_ + 1) % 3;
        return true;
    }

    int depth_ = 0;
    Section section_ = Section::None;
    std::vector<json> stack_;
    std::vector<std::string> keys_;
    Play cur_{};
    int slot_ = 0;
    int64_t at_ = 0;
};

// One imported file, interned into `out` (on failure `out` is left as it was). Missing, unreadable (locked / I/O error:
// retried briefly), unparsable, or read.
ReadResult readImportedOne(const std::filesystem::path& file, ListenStats::Snapshot& out, ImportSource source) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) return ReadResult::Unreadable;   // "not found" is no error
    if (!exists) return ReadResult::Missing;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(200));   // a scanner / backup holding it
        std::vector<char> buffer(256 * 1024);   // outlives the stream that reads through it
        std::ifstream f;
        f.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        f.open(file, std::ios::binary);
        if (!f) continue;
        ImportedReader r;
        const bool parsed = json::sax_parse(f, &r);
        if (f.bad()) continue;
        if (!parsed || r.bad || !r.sawTracks || !r.sawPlays) return ReadResult::Corrupt;
        std::vector<uint32_t> remap;
        remap.reserve(r.tracks.size());
        for (auto& t : r.tracks) remap.push_back(out.index.intern(std::move(t)));
        r.tracks = {};
        out.plays.reserve(out.plays.size() + r.plays.size());
        for (const Play& p : r.plays)
            if (p.track < remap.size()) out.plays.push_back({remap[p.track], p.startedAt, p.listenedMs, true, source});
        out.importedFiles[static_cast<size_t>(source)].cursors = std::move(r.cursors);
        return ReadResult::Ok;
    }
    return ReadResult::Unreadable;
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

// An imported companion into `s`: missing = none; unreadable or broken without a usable .old = keepFile (a broken
// one is kept as .bak first).
void readImported(const std::filesystem::path& file, ListenStats::Snapshot& s, ImportSource source) {
    auto& state = s.importedFiles[static_cast<size_t>(source)];
    const std::string name = file.filename().string();
    const ReadResult r = readImportedOne(file, s, source);
    if (r == ReadResult::Ok || r == ReadResult::Missing) {
        if (r == ReadResult::Missing && readImportedOne(sibling(file, L".old"), s, source) == ReadResult::Ok)
            ST_LOG_WARN(kTag, "{} missing; restored the previous copy (.old)", name);
        return;
    }
    if (r == ReadResult::Corrupt) {
        state.corrupt = true;
        std::error_code ec;
        std::filesystem::copy_file(file, sibling(file, L".bak"), std::filesystem::copy_options::skip_existing, ec);
        if (readImportedOne(sibling(file, L".old"), s, source) == ReadResult::Ok) {
            ST_LOG_WARN(kTag, "{} is corrupt; restored the previous copy (.old)", name);
            return;
        }
    }
    state.keepFile = true;
}

// A history is read (or imported) once and then grows a play at a time: its vectors give back what their growth left
// over (up to a third of each: 2.4 MB for 30 000 songs), on the worker that built them.
void compact(ListenStats::Snapshot& s) {
    s.index.tracks.shrink_to_fit();
    s.index.trackArtists.shrink_to_fit();
    s.index.trackAlbum.shrink_to_fit();
    s.index.artists.shrink_to_fit();
    s.index.albums.shrink_to_fit();
    s.plays.shrink_to_fit();
}

} // namespace

std::filesystem::path ListenStats::importedFileFor(const std::filesystem::path& file, ImportSource source) {
    const wchar_t* suffix = source == ImportSource::LastFm         ? L"-lastfm"
                            : source == ImportSource::ListenBrainz ? L"-listenbrainz"
                                                                   : L"-imported";
    return file.parent_path() / (file.stem().wstring() + suffix + file.extension().wstring());
}

namespace {

// Every imported file into `s` (after this PC's plays); a file that throws counts as unreadable.
void readAllImported(const std::filesystem::path& mainFile, const std::array<std::filesystem::path, kImportSources>* files,
                     ListenStats::Snapshot& s) {
    for (size_t k = 0; k < kImportSources; ++k) {
        const auto source = static_cast<ImportSource>(k);
        const size_t before = s.plays.size();
        try {
            readImported(files ? (*files)[k] : ListenStats::importedFileFor(mainFile, source), s, source);
        } catch (const std::exception&) {
            s.plays.resize(before);
            s.importedFiles[k].keepFile = true;
        }
    }
}

// Sorts the plays and applies the imported bound per source (a file written by a build with a larger bound).
void finishSnapshot(ListenStats::Snapshot& s) {
    std::stable_sort(s.plays.begin(), s.plays.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });
    std::array<size_t, kImportSources> counts{};
    for (const auto& p : s.plays)
        if (p.imported) ++counts[static_cast<size_t>(p.source)];
    for (size_t k = 0; k < kImportSources; ++k) {
        if (counts[k] <= listen::kMaxImportedPlays) continue;
        size_t drop = counts[k] - listen::kMaxImportedPlays;
        std::erase_if(s.plays, [&drop, k](const Play& p) {
            if (!drop || !p.imported || static_cast<size_t>(p.source) != k) return false;
            --drop;
            return true;
        });
    }
}

} // namespace

ListenStats::Snapshot ListenStats::readFile(const std::filesystem::path& file) {
    Snapshot s;
    try {
        s = readMain(file);
    } catch (const std::exception&) {
        s = {};
        s.keepFile = true;
    }
    const size_t native = s.plays.size();
    readAllImported(file, nullptr, s);
    if (s.plays.size() > native) finishSnapshot(s);
    compact(s);
    return s;
}

void ListenStats::adopt(Snapshot snapshot) {
    if (loaded_) return;
    loaded_ = true;
    readOnly_ = snapshot.keepFile;
    mainBroken_ = snapshot.corrupt;
    for (size_t k = 0; k < kImportSources; ++k) {
        importedReadOnly_[k] = snapshot.importedFiles[k].keepFile;
        importedBroken_[k] = snapshot.importedFiles[k].corrupt;
        cursors_[k] = std::move(snapshot.importedFiles[k].cursors);
        if (importedReadOnly_[k])
            ST_LOG_WARN(kTag, "{} unreadable; imports are off this session",
                        importedFileFor(file_, static_cast<ImportSource>(k)).filename().string());
    }
    if (snapshot.keepFile) ST_LOG_WARN(kTag, "listening.json unreadable; this session's plays are not saved");
    else if (snapshot.fromOld) ST_LOG_WARN(kTag, "listening.json missing or broken; restored the previous copy (.old)");
    else if (snapshot.corrupt) ST_LOG_WARN(kTag, "listening.json is corrupt; starting empty (file kept as .bak)");
    // What was recorded before the file was read (usually nothing) goes on top of the file's history.
    Index recorded = std::move(idx_);
    std::vector<Play> recordedPlays = std::move(plays_);
    idx_ = std::move(snapshot.index);
    plays_ = std::move(snapshot.plays);
    countImported();
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

// The job's text. Its tracks are job.tracks, or - a worker's job over its own snapshot, which needs no copy of them -
// `order` picked from `all`.
std::string serialize(const ListenStats::SaveJob& job, const std::vector<Track>* all = nullptr,
                      const std::vector<uint32_t>* order = nullptr) {
    const size_t trackCount = all && order ? order->size() : job.tracks.size();
    std::string text;
    text.reserve(trackCount * 200 + job.plays.size() * (job.imported ? 18 : 26) + 64);
    if (job.imported) {
        text += "{\"v\":1,\"source\":\"";
        text += importSourceName(job.source);
        text += '"';
        if (!job.cursors.empty()) {
            text += ",\"sync\":[";
            for (size_t i = 0; i < job.cursors.size(); ++i) {
                text += i ? ",{\"account\":" : "{\"account\":";
                putString(text, job.cursors[i].account);
                text += ",\"next\":";
                putInt(text, job.cursors[i].next);
                text += '}';
            }
            text += ']';
        }
        text += ",\"tracks\":[";
    } else {
        text += "{\"v\":1,\"tracks\":[";
    }
    for (size_t i = 0; i < trackCount; ++i) {
        if (i) text += ',';
        putTrack(text, all && order ? (*all)[(*order)[i]] : job.tracks[i]);
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

// The same without copying them: the indexes into `all` of the tracks `plays` use, in first-use order.
std::vector<uint32_t> usedTrackOrder(size_t all, std::vector<Play>& plays) {
    std::vector<uint32_t> order;
    std::vector<int64_t> newIndex(all, -1);
    for (auto& p : plays) {
        int64_t& n = newIndex[p.track];
        if (n < 0) {
            n = static_cast<int64_t>(order.size());
            order.push_back(p.track);
        }
        p.track = static_cast<uint32_t>(n);
    }
    return order;
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

ListenStats::SaveJob ListenStats::makeSaveJob(bool includeCurrent) {
    SaveJob job;
    if (!loaded_ || readOnly_) return job;   // empty file = nothing to write
    job.file = file_;
    job.generation = ++g_generation;
    job.rotate = !mainBroken_;
    // The newest kMaxPlays plays of this PC, the current one (so far) included; only the tracks they use, renumbered.
    const bool withCurrent = includeCurrent && currentCounts();
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

std::vector<ListenStats::SaveJob> ListenStats::makeImportedSaveJobs() {
    std::vector<SaveJob> jobs;
    if (!loaded_) return jobs;
    for (size_t k = 0; k < kImportSources; ++k) {
        if (importedReadOnly_[k] || !importedDirty_[k]) continue;
        SaveJob job;
        job.source = static_cast<ImportSource>(k);
        job.file = importedFileFor(file_, job.source);
        job.generation = ++g_generation;
        job.rotate = !importedBroken_[k];
        job.imported = true;
        job.cursors = cursors_[k];
        job.plays.reserve(importedCounts_[k]);
        for (const auto& p : plays_)
            if (p.imported && p.source == job.source) job.plays.push_back(p);
        job.tracks = usedTracks(idx_.tracks, job.plays);
        importedDirty_[k] = false;
        jobs.push_back(std::move(job));
    }
    return jobs;
}

bool ListenStats::dirty() const {
    return dirty_ || std::any_of(importedDirty_.begin(), importedDirty_.end(), [](bool d) { return d; });
}

bool ListenStats::canImport() const {
    return loaded_ && !importing_ && std::none_of(importedReadOnly_.begin(), importedReadOnly_.end(), [](bool r) { return r; });
}

void ListenStats::countImported() {
    importedCounts_ = {};
    for (const auto& p : plays_)
        if (p.imported) ++importedCounts_[static_cast<size_t>(p.source)];
    importedCount_ = 0;
    for (size_t n : importedCounts_) importedCount_ += n;
    nativeCount_ = plays_.size() - importedCount_;
}

int64_t ListenStats::importCursor(ImportSource source, std::string_view account) const {
    const std::wstring want = fold(account);
    for (const auto& c : cursors_[static_cast<size_t>(source)])
        if (fold(c.account) == want) return c.next;
    return 0;
}

namespace {
bool writeText(const ListenStats::SaveJob& job, const std::string& text);
} // namespace

bool ListenStats::write(const SaveJob& job) {
    if (job.file.empty()) return false;
    try {
        return writeText(job, serialize(job));
    } catch (const std::exception& e) {
        ST_LOG_ERROR(kTag, "save: {}", e.what());
        return false;
    }
}

namespace {

// write() once the text is made: flushed .tmp, the previous file kept as .old, renamed in place.
bool writeText(const ListenStats::SaveJob& job, const std::string& text) {
    try {
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

} // namespace

bool ListenStats::save() {
    bool importedOk = true;
    for (const SaveJob& imported : makeImportedSaveJobs()) {
        const size_t k = static_cast<size_t>(imported.source);
        if (write(imported)) {
            importedBroken_[k] = false;
        } else {
            importedDirty_[k] = true;
            importedOk = false;
        }
    }
    const SaveJob job = makeSaveJob();
    if (job.file.empty()) return false;
    if (write(job)) {
        mainBroken_ = false;
        return importedOk;
    }
    dirty_ = true;
    return false;
}

void ListenStats::detach() {
    if (importing_) cancelImport();
    if (!loaded_) return;   // only what was recorded since the start: it goes along to the next history
    for (const SaveJob& imported : makeImportedSaveJobs())   // a removal / clear not saved yet
        if (!write(imported)) ST_LOG_WARN(kTag, "account switch: {} not saved", imported.file.filename().string());
    const SaveJob job = makeSaveJob(false);
    if (!job.file.empty() && !write(job)) ST_LOG_WARN(kTag, "account switch: listening.json not saved");
    life_.renew();   // a background save of this history still in flight is older than the one above
    Track current = cur_.active ? idx_.tracks[cur_.track] : Track{};
    idx_ = {};
    plays_ = {};
    if (cur_.active) cur_.track = idx_.intern(std::move(current));
    nativeCount_ = importedCount_ = 0;
    importedCounts_ = {};
    loaded_ = readOnly_ = mainBroken_ = dirty_ = false;
    importedDirty_ = importedReadOnly_ = importedBroken_ = {};
    for (auto& c : cursors_) c.clear();   // the next account's come with its files
    recordedDuringImport_.clear();
    ST_LOG_INFO(kTag, "listening history saved for the account switch");
    notify();
}

void ListenStats::saveAsync() {
    struct Jobs {
        SaveJob main;
        std::vector<SaveJob> imported;
        std::vector<char> importedOk;
        bool mainOk = false;
    };
    auto jobs = std::make_shared<Jobs>();
    jobs->imported = makeImportedSaveJobs();
    jobs->main = makeSaveJob();
    if (jobs->main.file.empty() && jobs->imported.empty()) return;
    async(Priority::Low, life_.ref(),
          [jobs] {
              for (const auto& j : jobs->imported) jobs->importedOk.push_back(write(j) ? 1 : 0);
              jobs->mainOk = jobs->main.file.empty() || write(jobs->main);
              return Unit{};
          },
          [this, jobs](Result<Unit> r) {
              // Failures are retried by the next housekeeping save.
              if (!jobs->main.file.empty()) {
                  if (r && jobs->mainOk) mainBroken_ = false;
                  else dirty_ = true;
              }
              for (size_t i = 0; i < jobs->imported.size(); ++i) {
                  const size_t k = static_cast<size_t>(jobs->imported[i].source);
                  if (r && i < jobs->importedOk.size() && jobs->importedOk[i]) importedBroken_[k] = false;
                  else importedDirty_[k] = true;
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
    for (size_t k = 0; k < kImportSources; ++k) {   // the imported files are emptied too
        importedDirty_[k] = importedCounts_[k] > 0 || importedReadOnly_[k] || !cursors_[k].empty();
        importedReadOnly_[k] = false;
        cursors_[k].clear();
    }
    nativeCount_ = importedCount_ = 0;
    importedCounts_ = {};
    loaded_ = true;     // a load still in flight must not bring the history back
    readOnly_ = false;  // the user chose to drop the old history
    dirty_ = true;
    ST_LOG_INFO(kTag, "listening history cleared");
    notify();
}

// --- importing -------------------------------------------------------------------------------------------------------

ListenStats::ImportBase ListenStats::beginImport(ImportSource source) {
    ImportBase base;
    if (!canImport()) return base;   // empty importedFile = refused
    // A removal still waiting for its save: the job below reads the imported files, so empty them first (tiny writes).
    bool failed = false;
    for (const SaveJob& pending : makeImportedSaveJobs()) {
        const size_t k = static_cast<size_t>(pending.source);
        if (write(pending)) {
            importedBroken_[k] = false;
        } else {
            importedDirty_[k] = true;
            failed = true;
        }
    }
    if (failed) return base;
    importing_ = true;
    base.token = ++importToken_;
    base.source = source;
    for (size_t k = 0; k < kImportSources; ++k) base.importedFiles[k] = importedFileFor(file_, static_cast<ImportSource>(k));
    base.importedFile = base.importedFiles[static_cast<size_t>(source)];
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

void ListenStats::ImportBatch::add(ImportRow row) {
    ++offered;
    int64_t start = 0, ms = 0;
    if (row.listenedMs <= 0 && row.startedAt > 0) {   // a scrobble: when it started, not how long (estimated later)
        start = row.startedAt;
    } else {
        if (row.listenedMs < listen::kMinPlayMs) {
            ++tooShort;
            return;
        }
        ms = row.listenedMs;
        start = row.endedAt - (row.listenedMs + 500) / 1000;
    }
    std::wstring key = keysFor(row.track).track;
    auto [it, added] = byKey_.try_emplace(std::move(key), static_cast<uint32_t>(tracks.size()));
    if (added) tracks.push_back(std::move(row.track));
    else complete(tracks[it->second], row.track);
    rows.push_back({it->second, start, ms});
}

namespace {

bool scrobbled(const Play& p) { return p.imported && p.source != ImportSource::Spotify; }

// A play for the duplicate check: start and length in ms; a scrobble's length is an estimate, so it is matched by its
// start, within half of `match`: the song's known length (0 = unknown) - a measured play's is its own.
struct KnownPlay {
    int64_t start = 0, len = 0, match = 0;
    bool estimated = false;
};

// Is `c` (a new row) a play already in `known` (the same song's plays, ordered by start)? Two measured plays (this PC,
// Spotify's history): overlapping by half the shorter one, or the same length ending at about the same time (see
// listen::kSameEndS). A scrobble on either side: both started within half the song's length (see
// listen::kScrobbleMatchMinMs; a song scrobbled twice is that far apart at least, as a scrobble needs half of it).
bool knownPlay(const std::vector<KnownPlay>& known, const KnownPlay& c) {
    constexpr int64_t kScanMs = 6 * 3600 * 1000LL;   // plays that could overlap start at most this much earlier
    const int64_t endMs = c.start + c.len;
    auto it = std::lower_bound(known.begin(), known.end(), c.start - kScanMs,
                               [](const KnownPlay& k, int64_t t) { return k.start < t; });
    const int64_t last = std::max(endMs + listen::kSameEndS * 1000, c.start + listen::kScrobbleMatchMaxMs);
    for (; it != known.end() && it->start <= last; ++it) {
        if (c.estimated || it->estimated) {
            const int64_t a = c.estimated ? c.match : c.len, b = it->estimated ? it->match : it->len;
            const int64_t length = a > 0 && b > 0 ? std::min(a, b) : std::max(a, b);
            const int64_t window = std::clamp(length / 2, listen::kScrobbleMatchMinMs, listen::kScrobbleMatchMaxMs);
            if (std::abs(it->start - c.start) <= window) return true;
            continue;
        }
        const int64_t kEnd = it->start + it->len;
        const int64_t overlap = std::min(endMs, kEnd) - std::max(c.start, it->start);
        if (overlap > 0 && overlap * 2 >= std::min(c.len, it->len)) return true;
        if (std::abs(endMs - kEnd) <= listen::kSameEndS * 1000 && std::abs(c.len - it->len) <= listen::kSameLengthMs)
            return true;
    }
    return false;
}

// "A feat. B", "A & B", "A, B", "A x B": the credit split at its separators (ASCII-case-insensitive), trimmed.
std::vector<std::string> splitCredit(const std::string& credit) {
    static const char* const kSeparators[] = {" feat. ", " featuring ", " feat ", " ft. ", " ft ", " & ", ", ",
                                              " x ", " \xC3\x97 ", " vs. ", " vs ", "; ", " / "};
    std::string lower = credit;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; });
    std::vector<std::string> parts;
    size_t from = 0;
    while (from < credit.size()) {
        size_t best = std::string::npos, len = 0;
        for (const char* sep : kSeparators) {
            const size_t at = lower.find(sep, from);
            if (at != std::string::npos && (best == std::string::npos || at < best)) {
                best = at;
                len = std::strlen(sep);
            }
        }
        const size_t end = best == std::string::npos ? credit.size() : best;
        std::string part = credit.substr(from, end - from);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        if (!part.empty()) parts.push_back(std::move(part));
        if (best == std::string::npos) break;
        from = best + len;
    }
    return parts;
}

std::wstring trackKey(const std::string& name, const std::string& artist) { return fold(name) + L'\x1f' + fold(artist); }

bool importId(const std::string& id) { return id.empty() || id.rfind("import:", 0) == 0; }

// Puts an imported song on the entries the history already has, so top lists, the year in review and smart lists merge
// them instead of listing the song twice: the same id (a MusicBrainz recording, a Spotify URI) is that song; artists and
// albums with a known id (MBIDs) take the known name; a credit "A feat. B" whose first part is a known artist (and the
// whole credit is not) is split, so the song files under A like the plays heard here (which credit the first artist).
// Names are compared folded (case and accents) by the index itself.
class Resolver {
public:
    explicit Resolver(const Index& idx) : idx_(idx) {
        for (uint32_t i = 0; i < idx.tracks.size(); ++i) learn(i);
        for (uint32_t g = 0; g < idx.artists.size(); ++g)
            if (!idx.artists[g].id.empty()) artistById_.try_emplace(idx.artists[g].id, g);
        for (uint32_t g = 0; g < idx.albums.size(); ++g)
            if (!idx.albums[g].id.empty()) albumById_.try_emplace(idx.albums[g].id, g);
    }
    void learn(uint32_t track) {
        if (track < idx_.tracks.size() && !importId(idx_.tracks[track].id)) trackById_.try_emplace(idx_.tracks[track].id, track);
    }
    void resolve(Track& t) const {
        if (!importId(t.id)) {
            if (const auto it = trackById_.find(t.id); it != trackById_.end()) {
                const Track& k = idx_.tracks[it->second];
                t.name = k.name;
                t.artists = k.artists;
                return;
            }
        }
        for (auto& a : t.artists)
            if (!a.id.empty())
                if (const auto it = artistById_.find(a.id); it != artistById_.end()) a.name = idx_.artists[it->second].name;
        if (!t.albumId.empty())
            if (const auto it = albumById_.find(t.albumId); it != albumById_.end()) t.albumName = idx_.albums[it->second].name;
        if (t.name.empty() || t.artists.size() != 1) return;
        const std::string credit = t.artists[0].name;
        if (idx_.trackByKey.contains(trackKey(t.name, credit))) return;
        std::vector<std::string> parts = splitCredit(credit);
        if (parts.size() < 2) return;
        const bool knownSong = idx_.trackByKey.contains(trackKey(t.name, parts[0]));
        const bool knownArtist = idx_.artistByKey.contains(fold(parts[0])) && !idx_.artistByKey.contains(fold(credit));
        if (!knownSong && !knownArtist) return;
        const std::string firstId = t.artists[0].id;
        t.artists.clear();
        for (size_t i = 0; i < parts.size(); ++i) t.artists.push_back({i == 0 ? firstId : std::string{}, std::move(parts[i])});
    }

private:
    const Index& idx_;
    std::unordered_map<std::string, uint32_t> trackById_, artistById_, albumById_;
};

// A scrobble's length (see listen::kScrobbleDefaultMs): the song's when known, else its longest measured play when that
// is long enough to be a real listen (known()), else a typical song's (operator()).
class LengthGuess {
public:
    LengthGuess(const Index& idx, const std::vector<Play>& plays) : idx_(idx), plays_(plays) {}
    int64_t known(uint32_t track) {
        if (track < idx_.tracks.size() && idx_.tracks[track].durationMs > 0) return idx_.tracks[track].durationMs;
        if (!measured_) {   // once: the longest measured play per song
            measured_ = true;
            longest_.assign(idx_.tracks.size(), 0);
            for (const auto& p : plays_)
                if (!scrobbled(p) && p.track < longest_.size()) longest_[p.track] = std::max(longest_[p.track], p.listenedMs);
        }
        return track < longest_.size() && longest_[track] >= listen::kScrobbleMeasuredMinMs ? longest_[track] : 0;
    }
    int64_t operator()(uint32_t track) {
        const int64_t k = known(track);
        return k > 0 ? k : listen::kScrobbleDefaultMs;
    }

private:
    const Index& idx_;
    const std::vector<Play>& plays_;
    std::vector<int64_t> longest_;
    bool measured_ = false;
};

// What makes a play of this length a stream (a scrobble means the song was listened to).
int64_t streamFloor(int64_t lengthMs) { return lengthMs > 0 && lengthMs < listen::kShortTrackMs ? (lengthMs + 1) / 2 : listen::kStreamMs; }

// `into` updated with `from` (an account's cursor only moves forward). True when something changed.
bool mergeCursors(std::vector<ListenStats::ImportCursor>& into, const std::vector<ListenStats::ImportCursor>& from) {
    bool changed = false;
    for (const auto& c : from) {
        if (c.account.empty() || c.next <= 0) continue;
        const std::wstring key = fold(c.account);
        auto it = std::find_if(into.begin(), into.end(), [&key](const ListenStats::ImportCursor& x) { return fold(x.account) == key; });
        if (it == into.end()) {
            into.push_back(c);
            changed = true;
            continue;
        }
        if (c.next > it->next) {
            it->next = c.next;
            changed = true;
        }
        if (it + 1 != into.end()) {   // the account imported last goes last (the import dialog pre-fills it)
            std::rotate(it, it + 1, into.end());
            changed = true;
        }
    }
    return changed;
}

} // namespace

ListenStats::ImportResult ListenStats::buildImport(const ImportBase& base, std::vector<ImportRow> rows) {
    ImportBatch batch;
    batch.rows.reserve(rows.size());
    for (auto& row : rows) batch.add(std::move(row));
    rows = {};
    return buildImport(base, std::move(batch));
}

ListenStats::ImportResult ListenStats::buildImport(const ImportBase& base, ImportBatch batch) {
    ImportResult r;
    r.token = base.token;
    r.source = base.source;
    r.file = base.importedFile;
    r.rows = batch.offered;
    r.tooShort = batch.tooShort;
    if (base.importedFile.empty()) {
        r.error = "import refused";
        return r;
    }
    const size_t src = static_cast<size_t>(base.source);
    const bool estimates = base.source != ImportSource::Spotify;
    try {
        Snapshot& s = r.snapshot;
        // This PC's plays, then what was imported before (every source: the same play may come from several).
        std::vector<uint32_t> remap;
        remap.reserve(base.tracks.size());
        for (const auto& t : base.tracks) remap.push_back(s.index.intern(t));
        s.plays.reserve(base.plays.size());
        for (const auto& p : base.plays) s.plays.push_back({remap[p.track], p.startedAt, p.listenedMs, false});
        remap = {};
        readAllImported({}, &base.importedFiles, s);
        for (size_t k = 0; k < kImportSources; ++k)
            if (s.importedFiles[k].keepFile) {
                r.error = base.importedFiles[k].filename().string() + " unreadable";
                r.snapshot = {};
                return r;
            }
        std::stable_sort(s.plays.begin(), s.plays.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });

        // The rows' songs: matched to the known ones, then interned.
        std::vector<uint32_t> songs(batch.tracks.size());
        {
            Resolver resolver(s.index);
            for (size_t i = 0; i < batch.tracks.size(); ++i) {
                resolver.resolve(batch.tracks[i]);
                songs[i] = s.index.intern(std::move(batch.tracks[i]));
                resolver.learn(songs[i]);
            }
        }
        batch.tracks = {};

        // The new rows, in time order; a scrobble gets its song's length for now (capped below).
        LengthGuess guess(s.index, s.plays);
        std::vector<Play> fresh;
        fresh.reserve(batch.rows.size());
        for (const auto& row : batch.rows) {
            const uint32_t t = songs[row.track];
            fresh.push_back({t, row.startedAt, row.listenedMs > 0 ? row.listenedMs : guess(t), true, base.source});
        }
        batch.rows = {};
        songs = {};
        std::stable_sort(fresh.begin(), fresh.end(), [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });

        // Everything known per song (only the songs the rows mention), then each row against it and the rows before.
        std::unordered_map<uint32_t, std::vector<KnownPlay>> known;
        for (const auto& p : fresh) known.try_emplace(p.track);
        for (const auto& p : s.plays)
            if (auto it = known.find(p.track); it != known.end())
                it->second.push_back({p.startedAt * 1000, p.listenedMs, scrobbled(p) ? guess.known(p.track) : p.listenedMs, scrobbled(p)});
        std::vector<Play> added;
        added.reserve(fresh.size());
        for (const auto& p : fresh) {
            auto& k = known[p.track];
            const KnownPlay c{p.startedAt * 1000, p.listenedMs, estimates ? guess.known(p.track) : p.listenedMs, estimates};
            if (knownPlay(k, c)) {
                ++r.duplicates;
                continue;
            }
            const auto at = std::upper_bound(k.begin(), k.end(), c.start, [](int64_t t, const KnownPlay& e) { return t < e.start; });
            k.insert(at, c);
            added.push_back(p);
        }
        fresh = {};
        known = {};
        r.added = added.size();

        // A scrobble lasted at most until the next play began (any source), and at least what makes it a stream.
        if (estimates)
            for (size_t i = 0; i < added.size(); ++i) {
                Play& p = added[i];
                int64_t next = std::numeric_limits<int64_t>::max();
                const auto later = std::upper_bound(s.plays.begin(), s.plays.end(), p.startedAt,
                                                    [](int64_t t, const Play& q) { return t < q.startedAt; });
                if (later != s.plays.end()) next = later->startedAt;
                for (size_t j = i + 1; j < added.size(); ++j)
                    if (added[j].startedAt > p.startedAt) {
                        next = std::min(next, added[j].startedAt);
                        break;
                    }
                const int64_t length = p.listenedMs;
                if (next != std::numeric_limits<int64_t>::max()) p.listenedMs = std::min(p.listenedMs, (next - p.startedAt) * 1000);
                p.listenedMs = std::max(p.listenedMs, streamFloor(length));
            }

        // The source's cursors: the stored ones moved on by this batch.
        std::vector<ImportCursor> cursors = s.importedFiles[src].cursors;
        const bool cursorsMoved = mergeCursors(cursors, batch.cursors);
        s.importedFiles[src].cursors = cursors;
        r.cursors = cursors;
        if (added.empty() && !cursorsMoved) {   // nothing new: nothing to write
            r.ok = true;
            r.installable = batch.install;
            if (!r.installable) r.snapshot = {};
            return r;
        }
        if (!added.empty()) {
            const auto mid = static_cast<std::ptrdiff_t>(s.plays.size());
            s.plays.insert(s.plays.end(), added.begin(), added.end());
            std::inplace_merge(s.plays.begin(), s.plays.begin() + mid, s.plays.end(),
                               [](const Play& a, const Play& b) { return a.startedAt < b.startedAt; });
        }
        added = {};
        // The imported bound (per source): the oldest go.
        size_t sourceTotal = static_cast<size_t>(std::count_if(s.plays.begin(), s.plays.end(),
                                                               [&](const Play& p) { return p.imported && p.source == base.source; }));
        if (sourceTotal > listen::kMaxImportedPlays) {
            size_t drop = sourceTotal - listen::kMaxImportedPlays;
            std::erase_if(s.plays, [&](const Play& p) {
                if (!drop || !p.imported || p.source != base.source) return false;
                --drop;
                return true;
            });
        }

        SaveJob job;
        job.file = base.importedFile;
        job.generation = ++g_generation;
        job.rotate = !s.importedFiles[src].corrupt;
        job.imported = true;
        job.source = base.source;
        job.cursors = cursors;
        job.plays.reserve(sourceTotal);
        for (const auto& p : s.plays)
            if (p.imported && p.source == base.source) job.plays.push_back(p);
        const std::vector<uint32_t> order = usedTrackOrder(s.index.tracks.size(), job.plays);   // no copy of the songs
        if (!writeText(job, serialize(job, &s.index.tracks, &order))) {
            r.error = "writing " + base.importedFile.filename().string() + " failed";
            r.snapshot = {};
            return r;
        }
        s.importedFiles[src].corrupt = false;
        compact(s);
        r.ok = r.wrote = true;
        r.installable = r.added > 0 || batch.install;
        if (!r.installable) r.snapshot = {};
        ST_LOG_INFO(kTag, "import ({}): {} rows, {} new plays, {} already known, {} too short", importSourceName(base.source),
                    r.rows, r.added, r.duplicates, r.tooShort);
        return r;
    } catch (const std::exception& e) {
        ST_LOG_ERROR(kTag, "import: {}", e.what());
        r = {};
        r.token = base.token;
        r.source = base.source;
        r.file = base.importedFile;
        r.error = e.what();
        return r;
    }
}

bool ListenStats::finishImport(ImportResult result) {
    const size_t src = static_cast<size_t>(result.source);
    if (!importing_ || result.token != importToken_) {   // superseded (a clear, a removal, an account switch while it ran)
        // Its worker may have written the imported file after the clear emptied it: write what the store holds again.
        // Another account's file (a switch) keeps what the import wrote: it shows when that account is back.
        if (result.ok && result.wrote && loaded_ && !importedReadOnly_[src] && result.file == importedFileFor(file_, result.source))
            importedDirty_[src] = true;
        return false;
    }
    importing_ = false;
    auto recorded = std::move(recordedDuringImport_);
    recordedDuringImport_.clear();
    if (!result.ok) {
        // A batched import that failed half way wrote some batches it can't install: the file gets the store's state.
        if (result.wrote && !importedReadOnly_[src]) importedDirty_[src] = true;
        return false;
    }
    if (result.wrote || result.installable) cursors_[src] = std::move(result.cursors);
    if (!result.installable) return false;   // nothing changed; what was recorded is already in place
    Track current = cur_.active ? idx_.tracks[cur_.track] : Track{};
    idx_ = std::move(result.snapshot.index);
    plays_ = std::move(result.snapshot.plays);
    countImported();
    for (auto& [t, p] : recorded) insertPlay({idx_.intern(std::move(t)), p.startedAt, p.listenedMs});
    if (cur_.active) cur_.track = idx_.intern(std::move(current));
    if (result.wrote) importedBroken_[src] = false;
    ST_LOG_INFO(kTag, "listening history: {} plays ({} imported), {} tracks", plays_.size(), importedCount_, idx_.tracks.size());
    notify();
    return true;
}

void ListenStats::removeImported(ImportSource source) {
    if (importing_) cancelImport();
    const size_t k = static_cast<size_t>(source);
    if (importedCounts_[k] == 0 && cursors_[k].empty()) return;
    std::erase_if(plays_, [source](const Play& p) { return p.imported && p.source == source; });
    cursors_[k].clear();
    countImported();
    importedDirty_[k] = !importedReadOnly_[k];
    // Plays the other sources also had were left out of them as duplicates of these: their next API import reads the
    // whole account again (forgetting where it got to), so they come back.
    for (size_t j = 0; j < kImportSources; ++j)
        if (j != k && !cursors_[j].empty()) {
            cursors_[j].clear();
            importedDirty_[j] = !importedReadOnly_[j];
        }
    ST_LOG_INFO(kTag, "imported listening history removed ({})", importSourceName(source));
    notify();
}

void ListenStats::removeImported() {
    if (importing_) cancelImport();
    bool any = false;
    for (size_t k = 0; k < kImportSources; ++k) {
        if (importedCounts_[k] == 0 && cursors_[k].empty()) continue;
        cursors_[k].clear();
        importedDirty_[k] = !importedReadOnly_[k];
        any = true;
    }
    if (!any) return;
    std::erase_if(plays_, [](const Play& p) { return p.imported; });
    countImported();
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
