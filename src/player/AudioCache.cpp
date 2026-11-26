#include "player/AudioCache.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>

namespace st::player {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int64_t kHeadTail = 64 * 1024;   // sampledHash: the first and last bytes...
constexpr int64_t kBlock = 4096;           // ... and blocks of this size
constexpr int kBlocks = 16;                // ... spread over the file
constexpr int64_t kMinStream = 16 * 1024;  // smaller "streams" are not songs
constexpr size_t kMaxTrackIds = 8;
constexpr size_t kMaxPendingSponsor = 16;
constexpr DWORD kWriteChunk = 1 << 20;
constexpr wchar_t kIndexFile[] = L"index.json";

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// YouTube video ids are 11 characters of [A-Za-z0-9_-]; anything else never becomes a file name.
bool validVideoId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

const char* extensionFor(const std::string& mime) {
    if (mime == "audio/mp4") return ".m4a";
    if (mime == "audio/webm") return ".webm";
    if (mime == "audio/mpeg") return ".mp3";
    return ".bin";
}

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
};

// The playback buffer is a mapped temporary file: a page that can't be read back (a full disk) must fail the copy,
// not the process. No C++ objects in here (SEH).
bool guardedCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit File(HANDLE v) : h(v) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

bool readAt(HANDLE h, int64_t offset, void* dst, size_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return ReadFile(h, dst, static_cast<DWORD>(size), &got, &ov) && got == size;
}

// Writes the whole stream and flushes it to the disk (the rename that follows must never publish a torn file).
bool writeAll(const fs::path& path, const uint8_t* data, int64_t length, std::string& error) {
    File f(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!f.ok()) {
        error = std::format("create failed ({})", GetLastError());
        return false;
    }
    for (int64_t pos = 0; pos < length;) {
        const DWORD want = static_cast<DWORD>(std::min<int64_t>(kWriteChunk, length - pos));
        DWORD wrote = 0;
        if (!WriteFile(f.h, data + pos, want, &wrote, nullptr) || wrote != want) {
            error = std::format("write failed ({})", GetLastError());
            return false;
        }
        pos += wrote;
    }
    if (!FlushFileBuffers(f.h)) {
        error = std::format("flush failed ({})", GetLastError());
        return false;
    }
    return true;
}

json toJson(const CachedSong& s) {
    json j{{"k", s.key},          {"v", s.videoId},      {"i", s.itag},       {"f", s.file},   {"n", s.length},
           {"h", std::format("{:016x}", s.hash)},      {"m", s.mimeType},   {"c", s.codec},  {"s", s.source},
           {"b", s.bitrateKbps},  {"p", s.profiles},     {"t", s.trackIds},   {"a", s.addedMs}, {"u", s.usedMs},
           {"mt", s.match.title}, {"mc", s.match.channel}, {"md", s.match.durationSec}, {"ms", s.match.score},
           {"mm", s.match.manual}, {"th", s.match.thumbnailUrl}};
    if (s.loudnessDb) j["l"] = *s.loudnessDb;
    if (!s.sponsorKey.empty()) {
        j["sk"] = s.sponsorKey;
        j["sj"] = s.sponsorJson;
    }
    return j;
}

std::optional<CachedSong> fromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    try {
        CachedSong s;
        s.key = j.value("k", "");
        s.videoId = j.value("v", "");
        s.itag = j.value("i", 0);
        s.file = j.value("f", "");
        s.length = j.value("n", int64_t{0});
        s.hash = std::stoull(j.value("h", std::string("0")), nullptr, 16);
        s.mimeType = j.value("m", "");
        s.codec = j.value("c", "");
        s.source = j.value("s", "");
        s.bitrateKbps = j.value("b", 0);
        for (const auto& p : j.value("p", json::array()))
            if (p.is_string()) s.profiles.push_back(p.get<std::string>());
        for (const auto& t : j.value("t", json::array()))
            if (t.is_string()) s.trackIds.push_back(t.get<std::string>());
        s.addedMs = j.value("a", int64_t{0});
        s.usedMs = j.value("u", int64_t{0});
        if (const auto l = j.find("l"); l != j.end() && l->is_number()) s.loudnessDb = l->get<double>();
        s.match.videoId = s.videoId;
        s.match.title = j.value("mt", "");
        s.match.channel = j.value("mc", "");
        s.match.durationSec = j.value("md", 0);
        s.match.score = j.value("ms", 0.0);
        s.match.manual = j.value("mm", false);
        s.match.thumbnailUrl = j.value("th", "");
        s.sponsorKey = j.value("sk", "");
        s.sponsorJson = j.value("sj", "");
        // Only what a store could have written: the file name follows from the key, nothing else is trusted.
        if (!validVideoId(s.videoId) || s.itag <= 0 || s.key != std::format("{}-{}", s.videoId, s.itag) ||
            s.file != s.key + extensionFor(s.mimeType) || s.length < kMinStream)
            return std::nullopt;
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void addUnique(std::vector<std::string>& list, const std::string& value, size_t max) {
    if (value.empty()) return;
    std::erase(list, value);
    list.push_back(value);   // newest last
    if (list.size() > max) list.erase(list.begin(), list.end() - static_cast<ptrdiff_t>(max));
}

} // namespace

youtube::StreamInfo CachedSong::stream() const {
    youtube::StreamInfo s;
    s.contentLength = length;
    s.mimeType = mimeType;
    s.codec = codec;
    s.bitrateKbps = bitrateKbps;
    s.itag = itag;
    s.loudnessDb = loudnessDb;
    s.source = "cache";
    return s;
}

AudioCache::AudioCache(fs::path dir) : dir_(std::move(dir)) {}

AudioCache::~AudioCache() {
    try {
        flush();
    } catch (...) {
    }
}

std::string AudioCache::profile(bool allowWebm, bool lowQuality) {
    return std::string(allowWebm ? "w" : "m") + (lowQuality ? "l" : "h");
}

std::optional<CachedSong> AudioCache::songFor(const youtube::Resolved& r, const std::string& profile, const std::string& trackId) {
    if (!validVideoId(r.match.videoId) || r.stream.itag <= 0 || r.stream.source == "soundcloud") return std::nullopt;
    CachedSong s;
    s.videoId = r.match.videoId;
    s.itag = r.stream.itag;
    s.key = std::format("{}-{}", s.videoId, s.itag);
    s.profiles = {profile};
    if (!trackId.empty()) s.trackIds = {trackId};
    s.mimeType = r.stream.mimeType;
    s.codec = r.stream.codec;
    s.source = r.stream.source;
    s.bitrateKbps = r.stream.bitrateKbps;
    s.loudnessDb = r.stream.loudnessDb;
    s.match = r.match;
    return s;
}

void AudioCache::setLimit(int64_t bytes) { limit_.store(std::max<int64_t>(0, bytes)); }

void AudioCache::setFreeSpaceProbe(std::function<int64_t()> probe) {
    std::lock_guard lock(mutex_);
    freeProbe_ = std::move(probe);
}

int64_t AudioCache::freeSpace() const {
    std::function<int64_t()> probe;
    {
        std::lock_guard lock(mutex_);
        probe = freeProbe_;
    }
    if (probe) return probe();
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(dir_.c_str(), &avail, nullptr, nullptr)) return -1;
    return static_cast<int64_t>(std::min<ULONGLONG>(avail.QuadPart, INT64_MAX));
}

int64_t AudioCache::nextUseLocked() {
    lastUse_ = std::max(nowMs(), lastUse_ + 1);
    return lastUse_;
}

std::wstring AudioCache::pathOf(const CachedSong& song) const { return (dir_ / toWide(song.file)).wstring(); }

CachedSong* AudioCache::findLocked(const std::string& key) {
    for (auto& e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

void AudioCache::eraseLocked(size_t index, std::vector<std::wstring>& doomed) {
    doomed.push_back(pathOf(entries_[index]));
    bytes_ -= entries_[index].length;
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
    dirty_ = true;
}

bool AudioCache::makeRoomLocked(int64_t need, const std::string& keep, int64_t freeBytes, std::vector<std::wstring>& doomed) {
    // Evicting a file gives its bytes back to the volume, so the volume's share stays what it is now.
    int64_t budget = limit_.load();
    if (freeBytes >= 0) budget = std::min(budget, bytes_ + freeBytes - kMinFreeBytes);
    while (bytes_ + need > budget) {
        size_t oldest = entries_.size();
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].key != keep && (oldest == entries_.size() || entries_[i].usedMs < entries_[oldest].usedMs)) oldest = i;
        if (oldest == entries_.size()) break;
        ST_LOG_INFO("songcache", "evicting {} ({} KB, last played {} s ago)", entries_[oldest].key, entries_[oldest].length >> 10,
                    std::max<int64_t>(0, nowMs() - entries_[oldest].usedMs) / 1000);
        eraseLocked(oldest, doomed);
    }
    return bytes_ + need <= budget;
}

void AudioCache::deleteFiles(const std::vector<std::wstring>& files) {
    // A file the engine still plays is mapped with FILE_SHARE_DELETE: it goes once the track closes.
    for (const auto& f : files)
        if (!DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            ST_LOG_WARN("songcache", "could not delete {} ({})", toUtf8(f), GetLastError());
}

// ---- index -------------------------------------------------------------------------------------------------

void AudioCache::load() {
    std::lock_guard loadLock(loadMutex_);
    if (loaded_.load()) return;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::vector<CachedSong> kept;
    int dropped = 0;
    {
        std::ifstream f(dir_ / kIndexFile, std::ios::binary);
        const json j = f ? json::parse(f, nullptr, false) : json();
        if (j.is_object() && j.value("v", 0) == 1)
            for (const auto& e : j.value("songs", json::array())) {
                auto s = fromJson(e);
                if (!s || std::any_of(kept.begin(), kept.end(), [&](const CachedSong& k) { return k.key == s->key; })) {
                    ++dropped;
                    continue;
                }
                // The file must still be there with its length (the hash is checked when it is played).
                const auto size = fs::file_size(dir_ / toWide(s->file), ec);
                if (ec || static_cast<int64_t>(size) != s->length) {
                    ++dropped;
                    continue;
                }
                kept.push_back(std::move(*s));
            }
        else if (f)
            ST_LOG_WARN("songcache", "index.json unreadable: starting empty");
    }
    // Files no entry knows: ".part" leftovers of an interrupted write, files of dropped entries.
    std::vector<std::wstring> orphans;
    for (const auto& de : fs::directory_iterator(dir_, ec)) {
        const std::wstring name = de.path().filename().wstring();
        if (name == kIndexFile) continue;
        if (!std::any_of(kept.begin(), kept.end(), [&](const CachedSong& k) { return toWide(k.file) == name; }))
            orphans.push_back(de.path().wstring());
    }
    deleteFiles(orphans);
    {
        std::lock_guard lock(mutex_);
        entries_ = std::move(kept);
        bytes_ = 0;
        for (auto& e : entries_) {
            bytes_ += e.length;
            lastUse_ = std::max(lastUse_, e.usedMs);
            for (const auto& [video, sb] : pendingSponsor_)
                if (e.videoId == video) {
                    e.sponsorKey = sb.first;
                    e.sponsorJson = sb.second;
                }
        }
        ST_LOG_INFO("songcache", "{} song(s), {} MB in {}{}", entries_.size(), bytes_ >> 20, toUtf8(dir_.wstring()),
                    dropped + orphans.size() > 0 ? std::format(" ({} entries / {} files dropped)", dropped, orphans.size()) : std::string{});
    }
    loaded_.store(true);
    if (dropped > 0) dirty_.store(true);
    trim();
    flush();
}

void AudioCache::flush() {
    if (!loaded_.load() || !dirty_.exchange(false)) return;   // never replace the index before it was read
    std::lock_guard saveLock(saveMutex_);
    json songs = json::array();
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : entries_) songs.push_back(toJson(e));
    }
    const json j{{"v", 1}, {"songs", std::move(songs)}};
    const fs::path path = dir_ / kIndexFile;
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump();
        if (!f) {
            dirty_.store(true);
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ST_LOG_WARN("songcache", "index.json not written ({})", GetLastError());
        dirty_.store(true);
    }
}

// ---- lookups -----------------------------------------------------------------------------------------------

std::optional<AudioCache::Hit> AudioCache::verify(const CachedSong& song, const std::string& profileToAdd, const std::string& trackId) {
    const std::wstring path = pathOf(song);
    std::string problem;
    {
        File f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER size{};
        if (!f.ok()) problem = std::format("cannot open ({})", GetLastError());
        else if (!GetFileSizeEx(f.h, &size) || size.QuadPart != song.length)
            problem = std::format("length {} instead of {}", size.QuadPart, song.length);
        else {
            const auto hash = sampledHash(song.length, [&](int64_t off, void* dst, size_t n) { return readAt(f.h, off, dst, n); });
            if (!hash) problem = "read error";
            else if (*hash != song.hash) problem = "hash mismatch";
        }
    }
    std::vector<std::wstring> doomed;
    std::optional<Hit> hit;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].key != song.key) continue;
            if (!problem.empty()) {
                eraseLocked(i, doomed);
                break;
            }
            auto& e = entries_[i];
            e.usedMs = nextUseLocked();
            if (!profileToAdd.empty() && std::find(e.profiles.begin(), e.profiles.end(), profileToAdd) == e.profiles.end())
                e.profiles.push_back(profileToAdd);
            addUnique(e.trackIds, trackId, kMaxTrackIds);
            dirty_ = true;
            hit = Hit{e, path};
            break;
        }
    }
    if (!problem.empty()) {
        ST_LOG_WARN("songcache", "{} failed its check ({}): dropped", song.key, problem);
        deleteFiles(doomed);
        flush();
    }
    return hit;
}

std::optional<AudioCache::Hit> AudioCache::find(const std::string& videoId, const std::string& profile, const std::string& trackId) {
    if (limit_.load() <= 0 || videoId.empty()) return std::nullopt;
    load();
    std::optional<CachedSong> song;
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : entries_)
            if (e.videoId == videoId && std::find(e.profiles.begin(), e.profiles.end(), profile) != e.profiles.end()) {
                song = e;
                break;
            }
    }
    return song ? verify(*song, {}, trackId) : std::nullopt;
}

std::optional<AudioCache::Hit> AudioCache::findFormat(const std::string& videoId, int itag, const std::string& profile,
                                                      const std::string& trackId) {
    if (limit_.load() <= 0 || videoId.empty()) return std::nullopt;
    load();
    std::optional<CachedSong> song;
    {
        std::lock_guard lock(mutex_);
        if (const auto* e = findLocked(std::format("{}-{}", videoId, itag))) song = *e;
    }
    return song ? verify(*song, profile, trackId) : std::nullopt;
}

std::optional<AudioCache::Hit> AudioCache::findAny(const std::string& videoId, const std::string& trackId, bool webm) {
    if (limit_.load() <= 0 || (videoId.empty() && trackId.empty())) return std::nullopt;
    load();
    std::vector<CachedSong> candidates;
    {
        std::lock_guard lock(mutex_);
        auto collect = [&](auto&& pick) {
            for (const auto& e : entries_)
                if (pick(e) && (webm || e.mimeType != "audio/webm")) candidates.push_back(e);
        };
        if (!videoId.empty()) collect([&](const CachedSong& e) { return e.videoId == videoId; });
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.bitrateKbps > b.bitrateKbps; });
        // Nothing of that video (or no video known): the one this track played from last.
        if (candidates.empty() && !trackId.empty()) {
            collect([&](const CachedSong& e) { return std::find(e.trackIds.begin(), e.trackIds.end(), trackId) != e.trackIds.end(); });
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.usedMs > b.usedMs; });
        }
    }
    for (const auto& c : candidates)
        if (auto hit = verify(c, {}, trackId)) return hit;
    return std::nullopt;
}

// ---- writes ------------------------------------------------------------------------------------------------

bool AudioCache::store(CachedSong song, const uint8_t* data, int64_t length) {
    const int64_t lim = limit_.load();
    if (lim <= 0 || !data || length < kMinStream || !validVideoId(song.videoId) || song.itag <= 0) return false;
    song.key = std::format("{}-{}", song.videoId, song.itag);
    song.file = song.key + extensionFor(song.mimeType);
    if (length > lim / 4) {
        ST_LOG_INFO("songcache", "{} not stored: {} MB is more than a quarter of the cache", song.key, length >> 20);
        return false;
    }
    load();
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (writing_.contains(song.key)) return false;
        if (auto* e = findLocked(song.key)) {   // stored already (the same song played twice): remember who asked
            for (const auto& p : song.profiles)
                if (std::find(e->profiles.begin(), e->profiles.end(), p) == e->profiles.end()) e->profiles.push_back(p);
            for (const auto& t : song.trackIds) addUnique(e->trackIds, t, kMaxTrackIds);
            e->usedMs = nextUseLocked();
            dirty_ = true;
            return false;
        }
        writing_.insert(song.key);
        generation = generation_;
    }
    struct Unmark {
        AudioCache& c;
        const std::string& key;
        ~Unmark() {
            std::lock_guard lock(c.mutex_);
            c.writing_.erase(key);
        }
    } unmark{*this, song.key};

    // Room first: on a nearly full disk the cache shrinks before anything is written.
    std::vector<std::wstring> doomed;
    bool fits = false;
    {
        const int64_t free = freeSpace();
        std::lock_guard lock(mutex_);
        fits = makeRoomLocked(length, song.key, free, doomed);
    }
    deleteFiles(doomed);
    doomed.clear();
    if (!fits) {
        ST_LOG_INFO("songcache", "{} not stored: no room (limit {} MB, {} MB free on the disk)", song.key, lim >> 20,
                    std::max<int64_t>(0, freeSpace()) >> 20);
        flush();
        return false;
    }
    const auto hash = sampledHash(length, [&](int64_t off, void* dst, size_t n) { return guardedCopy(dst, data + off, n); });
    if (!hash) {
        ST_LOG_WARN("songcache", "{} not stored: the buffer could not be read", song.key);
        return false;
    }
    const fs::path part = dir_ / toWide(song.key + ".part");
    const fs::path target = dir_ / toWide(song.file);
    std::string error;
    if (!writeAll(part, data, length, error) || !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error.empty()) error = std::format("rename failed ({})", GetLastError());
        DeleteFileW(part.c_str());
        ST_LOG_WARN("songcache", "{} not stored: {}", song.key, error);
        return false;
    }
    song.length = length;
    song.hash = *hash;
    song.addedMs = nowMs();
    bool kept = false;
    {
        const int64_t free = freeSpace();
        std::lock_guard lock(mutex_);
        if (generation != generation_ || limit_.load() <= 0) {
            doomed.push_back(target.wstring());   // cleared / turned off while it was written
        } else {
            if (song.sponsorKey.empty())
                for (const auto& [video, sb] : pendingSponsor_)
                    if (video == song.videoId) {
                        song.sponsorKey = sb.first;
                        song.sponsorJson = sb.second;
                    }
            song.usedMs = nextUseLocked();
            entries_.push_back(song);
            bytes_ += length;
            dirty_ = true;
            kept = true;
            makeRoomLocked(0, song.key, free, doomed);   // a lower limit set meanwhile
        }
    }
    deleteFiles(doomed);
    flush();
    if (kept) {
        const Stats st = stats();
        ST_LOG_INFO("songcache", "stored {} ({} KB, {} {} kbps, from {}); {} song(s), {} MB", song.key, length >> 10, song.mimeType,
                    song.bitrateKbps, song.source, st.count, st.bytes >> 20);
    }
    return kept;
}

void AudioCache::remove(const std::string& key) {
    std::vector<std::wstring> doomed;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].key == key) {
                eraseLocked(i, doomed);
                break;
            }
    }
    deleteFiles(doomed);
    flush();
}

void AudioCache::removeVideo(const std::string& videoId) {
    if (videoId.empty()) return;
    load();
    std::vector<std::wstring> doomed;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = entries_.size(); i-- > 0;)
            if (entries_[i].videoId == videoId) eraseLocked(i, doomed);
    }
    if (doomed.empty()) return;
    ST_LOG_INFO("songcache", "{}: {} stream(s) dropped (downloaded)", videoId, doomed.size());
    deleteFiles(doomed);
    flush();
}

void AudioCache::clear() {
    load();
    std::vector<std::wstring> doomed;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        for (const auto& e : entries_) doomed.push_back(pathOf(e));
        entries_.clear();
        bytes_ = 0;
        pendingSponsor_.clear();
        dirty_ = true;
    }
    deleteFiles(doomed);
    flush();
    ST_LOG_INFO("songcache", "cleared ({} file(s))", doomed.size());
}

void AudioCache::trim() {
    if (!loaded_.load()) return;
    std::vector<std::wstring> doomed;
    {
        const int64_t free = freeSpace();
        std::lock_guard lock(mutex_);
        makeRoomLocked(0, {}, free, doomed);
    }
    if (doomed.empty()) return;
    deleteFiles(doomed);
    flush();
}

// ---- in-memory queries -------------------------------------------------------------------------------------

AudioCache::Stats AudioCache::stats() const {
    std::lock_guard lock(mutex_);
    return {static_cast<int>(entries_.size()), bytes_};
}

bool AudioCache::hasVideo(const std::string& videoId) const {
    std::lock_guard lock(mutex_);
    return std::any_of(entries_.begin(), entries_.end(), [&](const CachedSong& e) { return e.videoId == videoId; });
}

void AudioCache::setSponsor(const std::string& videoId, const std::string& key, std::string json) {
    if (videoId.empty() || key.empty()) return;
    std::lock_guard lock(mutex_);
    for (auto& e : entries_)
        if (e.videoId == videoId && (e.sponsorKey != key || e.sponsorJson != json)) {
            e.sponsorKey = key;
            e.sponsorJson = json;
            dirty_ = true;
        }
    std::erase_if(pendingSponsor_, [&](const auto& p) { return p.first == videoId; });
    pendingSponsor_.push_back({videoId, {key, std::move(json)}});
    if (pendingSponsor_.size() > kMaxPendingSponsor) pendingSponsor_.erase(pendingSponsor_.begin());
}

std::optional<std::string> AudioCache::sponsor(const std::string& videoId, const std::string& key) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e.videoId == videoId && e.sponsorKey == key) return e.sponsorJson;
    for (const auto& [video, sb] : pendingSponsor_)
        if (video == videoId && sb.first == key) return sb.second;
    return std::nullopt;
}

// ---- hash --------------------------------------------------------------------------------------------------

std::optional<uint64_t> AudioCache::sampledHash(int64_t length, const std::function<bool(int64_t, void*, size_t)>& read) {
    if (length <= 0) return std::nullopt;
    Fnv f;
    f.add(&length, sizeof length);
    std::vector<uint8_t> buf(static_cast<size_t>(kHeadTail));
    auto take = [&](int64_t offset, int64_t size) {
        size = std::min(size, length - offset);
        if (size <= 0) return true;
        if (!read(offset, buf.data(), static_cast<size_t>(size))) return false;
        f.add(buf.data(), static_cast<size_t>(size));
        return true;
    };
    if (!take(0, kHeadTail) || !take(std::max<int64_t>(0, length - kHeadTail), kHeadTail)) return std::nullopt;
    for (int i = 0; i < kBlocks; ++i)
        if (!take(length > kBlock ? (length - kBlock) * i / (kBlocks - 1) : 0, kBlock)) return std::nullopt;
    return f.h;
}

} // namespace st::player
