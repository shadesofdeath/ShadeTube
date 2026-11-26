#pragma once
// Song cache: the streams of songs that were played, kept on disk (cache\audio), so the next play of a song starts
// from the file, without the network (also offline). See docs/DEVELOPMENT.md "Song cache".
//
//   - What: a YouTube (or backup-server) stream that arrived completely while it played, byte for byte as it came
//     (audio::StreamSource::onComplete), as "<videoId>-<itag>.m4a|.webm". The entry keeps what playing it needs
//     without resolving anything: the format, YouTube's loudness of the stream, the match (title, channel, duration,
//     score), the catalog tracks it played for and the SponsorBlock segments of the video (an opaque JSON the app
//     writes). Podcasts, radio, local files and downloads never come here (they live on disk already); SoundCloud
//     stand-ins neither (no video id).
//   - Formats: `profile` is the stream choice that picked the format (MatchService's flags: "w"/"m" = Opus allowed or
//     not, "h"/"l" = high / normal quality). find() serves an entry only for the profile that picked it, so a quality
//     change never serves another format; findFormat() learns that another profile picked the same itag; findAny() is
//     the offline fallback (any format the engine can decode).
//   - Safety: a file is written as "<name>.part", flushed and renamed; a partial download is never served. Every entry
//     carries the length and a sampled FNV-1a hash (head, tail and 16 blocks across the file), checked whenever it is
//     looked up: a file that no longer matches is dropped. load() drops entries whose file is gone and files no entry
//     knows (".part" leftovers of a crash).
//   - Size: least recently used entries go first to keep the files under the limit (Settings::audioCacheMb, 0 = off)
//     and to leave at least kMinFreeBytes free on the volume (a nearly full disk shrinks the cache). One stream may
//     take at most a quarter of the limit.
//   - Memory: the index only (~0.3 KB per song); stream bytes are copied from the playback buffer's mapped file and
//     played from a read-only mapping, both file cache pages.
//
// Thread-safe. File work (load, the lookups, store, remove*, clear, trim, flush) belongs on worker / download threads;
// stats(), hasVideo() and the SponsorBlock data are in-memory and fine on the UI thread (they wait for no file I/O).
// Until load() ran (the first worker lookup also runs it) the cache looks empty to them.
#include "youtube/MatchService.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace st::player {

struct CachedSong {
    std::string key;                       // "<videoId>-<itag>" (the file's stem)
    std::string videoId;
    int itag = 0;
    std::vector<std::string> profiles;     // stream choices that picked this format
    std::vector<std::string> trackIds;     // catalog tracks it played for (newest last, at most 8)
    std::string file;                      // file name in the cache folder
    int64_t length = 0;
    uint64_t hash = 0;                     // sampledHash()
    std::string mimeType, codec, source;   // source: who served it ("youtube", "piped <host>"...)
    int bitrateKbps = 0;
    std::optional<double> loudnessDb;      // YouTube's loudness vs. its -14 LUFS reference
    youtube::Match match;
    std::string sponsorKey, sponsorJson;   // SponsorBlock segments of the video and the categories they are for
    int64_t addedMs = 0, usedMs = 0;       // unix ms (usedMs: LRU order, strictly increasing within a run)

    // The stream as the player knows it (no URL: it plays from the file).
    youtube::StreamInfo stream() const;
};

class AudioCache {
public:
    static constexpr int64_t kMinFreeBytes = 1ll << 30;   // never fill the volume past this much free space

    explicit AudioCache(std::filesystem::path dir);
    ~AudioCache();
    AudioCache(const AudioCache&) = delete;
    AudioCache& operator=(const AudioCache&) = delete;

    const std::filesystem::path& dir() const { return dir_; }
    // "w"/"m" + "h"/"l", the stream choice of MatchService::stream(allowWebm, lowQuality).
    static std::string profile(bool allowWebm, bool lowQuality);
    // What a store of this resolution would keep (nullopt: not cacheable, e.g. a SoundCloud stand-in, no itag).
    static std::optional<CachedSong> songFor(const youtube::Resolved& r, const std::string& profile, const std::string& trackId);

    // Byte limit (any thread; 0 = off: nothing is stored or served). A lower limit takes effect at the next store or
    // trim().
    void setLimit(int64_t bytes);
    int64_t limit() const { return limit_.load(); }

    // Reads the index and drops what it can't trust (once; later calls return at once).
    void load();
    bool loaded() const { return loaded_.load(); }

    struct Hit {
        CachedSong song;
        std::wstring path;   // the verified file
    };
    // The video's format for `profile`, verified (length + hash). A file that fails the check is dropped. Marks the
    // entry used (LRU) and remembers `trackId` (may be "").
    std::optional<Hit> find(const std::string& videoId, const std::string& profile, const std::string& trackId);
    // The video's format `itag` (another profile picked the same one): adds `profile` to it.
    std::optional<Hit> findFormat(const std::string& videoId, int itag, const std::string& profile, const std::string& trackId);
    // Offline fallback: any verified format of the video, or with videoId "" of the video `trackId` last played from;
    // the highest bit rate first, Opus / WebM only with `webm`.
    std::optional<Hit> findAny(const std::string& videoId, const std::string& trackId, bool webm);

    // A complete stream (download thread; `data` is valid during the call). The file is written next to its final
    // name, flushed and renamed; then the limit is enforced. false: not stored (off, too large, low disk, an I/O error,
    // already there, cleared meanwhile).
    bool store(CachedSong song, const uint8_t* data, int64_t length);

    void remove(const std::string& key);
    void removeVideo(const std::string& videoId);   // a downloaded song: its streams go (never kept twice)
    void clear();
    void trim();     // evicts down to the limit and the free-space floor
    void flush();    // writes the index if it changed (UI-thread changes only mark it)
    bool dirty() const { return dirty_.load(); }

    struct Stats {
        int count = 0;
        int64_t bytes = 0;
    };
    Stats stats() const;
    bool hasVideo(const std::string& videoId) const;
    // SponsorBlock segments of a video (opaque JSON for the categories `key`): kept with its entries, and with one
    // stored later in this run (the lookup usually lands before the stream is complete).
    void setSponsor(const std::string& videoId, const std::string& key, std::string json);
    std::optional<std::string> sponsor(const std::string& videoId, const std::string& key) const;

    // FNV-1a 64 over the length, the first and last 64 KB and 16 blocks of 4 KB spread over the file. `read(offset,
    // dst, size)` fills dst from the stream; false = unreadable.
    static std::optional<uint64_t> sampledHash(int64_t length, const std::function<bool(int64_t, void*, size_t)>& read);

    // Tests: replaces the free-space query of the cache's volume (bytes free; < 0 = unknown).
    void setFreeSpaceProbe(std::function<int64_t()> probe);

private:
    std::optional<Hit> verify(const CachedSong& song, const std::string& profileToAdd, const std::string& trackId);
    // mutex_ held: drops entries (least recently used first, never `keep`) until `need` more bytes fit under the limit
    // and the room the volume leaves (`freeBytes`, measured outside the lock; < 0 = unknown). Their files are collected
    // for deletion outside the lock. false: `need` does not fit.
    bool makeRoomLocked(int64_t need, const std::string& keep, int64_t freeBytes, std::vector<std::wstring>& doomed);
    void eraseLocked(size_t index, std::vector<std::wstring>& doomed);
    CachedSong* findLocked(const std::string& key);
    std::wstring pathOf(const CachedSong& song) const;
    int64_t freeSpace() const;
    int64_t nextUseLocked();
    static void deleteFiles(const std::vector<std::wstring>& files);

    const std::filesystem::path dir_;
    std::atomic<int64_t> limit_{0};
    std::atomic<bool> loaded_{false};
    std::atomic<bool> dirty_{false};
    std::mutex loadMutex_;    // load()
    std::mutex saveMutex_;    // the index file
    mutable std::mutex mutex_;
    // ---- guarded by mutex_
    std::vector<CachedSong> entries_;
    int64_t bytes_ = 0;
    int64_t lastUse_ = 0;
    uint64_t generation_ = 0;                     // clear() bumps it: a store that began before is dropped
    std::unordered_set<std::string> writing_;     // keys being written now
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> pendingSponsor_;   // videoId -> (key, json)
    std::function<int64_t()> freeProbe_;
};

} // namespace st::player
