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

} // namespace listen

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
    // Bounded in memory as on disk; trimmed in batches so appends stay amortized O(1).
    if (plays_.size() > listen::kMaxPlays + 512)
        plays_.erase(plays_.begin(), plays_.end() - static_cast<std::ptrdiff_t>(listen::kMaxPlays));
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

// One file: missing, unreadable (locked / I/O error: retried briefly), unparsable, or parsed into `out`.
ReadResult readOne(const std::filesystem::path& file, ListenStats::Snapshot& out) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) return ReadResult::Unreadable;   // "not found" is no error
    if (!exists) return ReadResult::Missing;
    std::string text;
    bool read = false;
    for (int attempt = 0; attempt < 3 && !read; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(200));   // a scanner / backup holding it
        std::ifstream f(file, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        if (f.bad()) continue;
        text = std::move(ss).str();
        read = true;
    }
    if (!read) return ReadResult::Unreadable;
    json j = json::parse(text, nullptr, false);
    text = {};
    if (!j.is_object() || !j.contains("tracks") || !j.contains("plays")) return ReadResult::Corrupt;
    out = snapshotFromJson(j);
    return ReadResult::Ok;
}

} // namespace

ListenStats::Snapshot ListenStats::readFile(const std::filesystem::path& file) {
    try {
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
    } catch (const std::exception&) {
        Snapshot failed;
        failed.keepFile = true;
        return failed;
    }
}

void ListenStats::adopt(Snapshot snapshot) {
    if (loaded_) return;
    loaded_ = true;
    readOnly_ = snapshot.keepFile;
    mainBroken_ = snapshot.corrupt;
    if (snapshot.keepFile) ST_LOG_WARN(kTag, "listening.json unreadable; this session's plays are not saved");
    else if (snapshot.fromOld) ST_LOG_WARN(kTag, "listening.json missing or broken; restored the previous copy (.old)");
    else if (snapshot.corrupt) ST_LOG_WARN(kTag, "listening.json is corrupt; starting empty (file kept as .bak)");
    // What was recorded before the file was read (usually nothing) goes on top of the file's history.
    Index recorded = std::move(idx_);
    std::vector<Play> recordedPlays = std::move(plays_);
    idx_ = std::move(snapshot.index);
    plays_ = std::move(snapshot.plays);
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
    ST_LOG_INFO(kTag, "listening history: {} plays, {} tracks", plays_.size(), idx_.tracks.size());
    notify();
}

// --- writing ---------------------------------------------------------------------------------------------------

namespace {

std::atomic<uint64_t> g_generation{0};
std::mutex g_writeMutex;                                      // one writer per process at a time (worker or exit)
std::unordered_map<std::wstring, uint64_t> g_writtenGeneration;   // per file, under g_writeMutex

std::string serialize(const ListenStats::SaveJob& job) {
    std::string text;
    text.reserve(job.tracks.size() * 200 + job.plays.size() * 26 + 64);
    text += "{\"v\":1,\"tracks\":[";
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        if (i) text += ',';
        putTrack(text, job.tracks[i]);
    }
    text += "],\"plays\":[";
    for (size_t i = 0; i < job.plays.size(); ++i) {
        text += i ? ",[" : "[";
        putInt(text, job.plays[i].track);
        text += ',';
        putInt(text, job.plays[i].startedAt);
        text += ',';
        putInt(text, job.plays[i].listenedMs);
        text += ']';
    }
    text += "]}";
    return text;
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
    // The newest kMaxPlays plays, the current one (so far) included; only the tracks they use, renumbered.
    const bool withCurrent = cur_.active && cur_.listenedMs >= listen::kMinPlayMs;
    const size_t keep = std::min(plays_.size(), listen::kMaxPlays - (withCurrent ? 1 : 0));
    job.plays.assign(plays_.end() - static_cast<std::ptrdiff_t>(keep), plays_.end());
    if (withCurrent) job.plays.push_back({cur_.track, cur_.startedAt, cur_.listenedMs});
    std::vector<int64_t> newIndex(idx_.tracks.size(), -1);
    for (auto& p : job.plays) {
        int64_t& n = newIndex[p.track];
        if (n < 0) {
            n = static_cast<int64_t>(job.tracks.size());
            job.tracks.push_back(idx_.tracks[p.track]);
        }
        p.track = static_cast<uint32_t>(n);
    }
    dirty_ = false;
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
        if (!writeFlushed(tmp, text)) {
            ST_LOG_ERROR(kTag, "failed to write listening.json.tmp (error {})", GetLastError());
            return false;
        }
        // Keep the previous generation (readFile falls back to it), then move the new file in. Write-through: both
        // renames are on the disk before we return. A failed rotation only costs the fallback copy.
        if (job.rotate && GetFileAttributesW(job.file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(job.file.c_str(), sibling(job.file, L".old").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ST_LOG_WARN(kTag, "could not keep listening.json.old (error {})", GetLastError());
        if (!MoveFileExW(tmp.c_str(), job.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ST_LOG_ERROR(kTag, "failed to replace listening.json (error {})", GetLastError());
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
    const SaveJob job = makeSaveJob();
    if (job.file.empty()) return false;
    if (write(job)) {
        mainBroken_ = false;
        return true;
    }
    dirty_ = true;
    return false;
}

void ListenStats::saveAsync() {
    auto job = std::make_shared<const SaveJob>(makeSaveJob());
    if (job->file.empty()) return;
    async(Priority::Low, life_.ref(), [job] { return write(*job); },
          [this](Result<bool> r) {
              if (r && *r) mainBroken_ = false;
              else dirty_ = true;   // retried by the next housekeeping save
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
    insertPlay({cur_.track, cur_.startedAt, cur_.listenedMs});
    dirty_ = true;
    notify();
}

void ListenStats::addPlay(const catalog::Track& t, int64_t startedAt, int64_t listenedMs) {
    if (listenedMs < listen::kMinPlayMs) return;
    insertPlay({idx_.intern(fromCatalog(t)), startedAt, listenedMs});
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
    loaded_ = true;     // a load still in flight must not bring the history back
    readOnly_ = false;  // the user chose to drop the old history
    dirty_ = true;
    ST_LOG_INFO(kTag, "listening history cleared");
    notify();
}

// --- aggregation ---------------------------------------------------------------------------------------------------

StatsSummary ListenStats::summarize(StatsPeriod period, int64_t unixNow, size_t topN, size_t recentN) const {
    StatsSummary s;
    const int64_t start = period == StatsPeriod::Week    ? unixNow - 7 * 86400
                          : period == StatsPeriod::Month ? unixNow - 30 * 86400
                                                         : std::numeric_limits<int64_t>::min();
    const bool withCurrent = cur_.active && cur_.listenedMs >= listen::kMinPlayMs;
    if (period == StatsPeriod::All) s.periodStart = !plays_.empty() ? plays_.front().startedAt : withCurrent ? cur_.startedAt : 0;
    else s.periodStart = start;

    struct Acc {
        int streams = 0;
        int64_t ms = 0, last = 0;
    };
    std::vector<Acc> tr(idx_.tracks.size()), ar(idx_.artists.size()), al(idx_.albums.size());
    auto add = [&](const Play& p) {
        s.listenedMs += p.listenedMs;
        if (!listen::counts(p.listenedMs, idx_.tracks[p.track].durationMs)) return;
        ++s.streams;
        auto bump = [&](Acc& a) {
            ++a.streams;
            a.ms += p.listenedMs;
            a.last = std::max(a.last, p.startedAt);
        };
        bump(tr[p.track]);
        for (uint32_t a : idx_.trackArtists[p.track]) bump(ar[a]);
        if (const int32_t a = idx_.trackAlbum[p.track]; a >= 0) bump(al[static_cast<size_t>(a)]);
    };
    const auto first = std::lower_bound(plays_.begin(), plays_.end(), start,
                                        [](const Play& q, int64_t t) { return q.startedAt < t; });
    for (auto it = first; it != plays_.end(); ++it) add(*it);
    const Play current{cur_.track, cur_.startedAt, cur_.listenedMs};
    const bool currentIn = withCurrent && cur_.startedAt >= start;
    if (currentIn) add(current);

    // Recent streams, newest first (the current play is the newest once it counts).
    auto pushRecent = [&](const Play& p) {
        if (s.recent.size() >= recentN || !listen::counts(p.listenedMs, idx_.tracks[p.track].durationMs)) return;
        s.recent.push_back({toCatalog(idx_.tracks[p.track]), p.startedAt, p.listenedMs});
    };
    if (currentIn) pushRecent(current);
    for (auto it = plays_.rbegin(); it != std::make_reverse_iterator(first) && s.recent.size() < recentN; ++it) pushRecent(*it);

    // Top lists: most streams, then more time, then the most recent. Returns (all with a stream, top n of them).
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
        const auto [idx, n] = top(tr);
        s.distinctTracks = static_cast<int>(idx.size());
        for (size_t i = 0; i < n; ++i) s.topTracks.push_back({toCatalog(idx_.tracks[idx[i]]), tr[idx[i]].streams, tr[idx[i]].ms});
    }
    {
        const auto [idx, n] = top(ar);
        s.distinctArtists = static_cast<int>(idx.size());
        for (size_t i = 0; i < n; ++i) {
            const auto& g = idx_.artists[idx[i]];
            s.topArtists.push_back({{g.id, g.name}, ar[idx[i]].streams, ar[idx[i]].ms});
        }
    }
    {
        const auto [idx, n] = top(al);
        for (size_t i = 0; i < n; ++i) {
            const auto& g = idx_.albums[idx[i]];
            StatsAlbum a;
            a.album.id = g.id;
            a.album.name = g.name;
            if (!g.image.empty()) a.album.images.push_back({g.image, 0, 0});
            a.artist = g.artist;
            a.variousArtists = g.various;
            a.streams = al[idx[i]].streams;
            a.listenedMs = al[idx[i]].ms;
            s.topAlbums.push_back(std::move(a));
        }
    }
    return s;
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
