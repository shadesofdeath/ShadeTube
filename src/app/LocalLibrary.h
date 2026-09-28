#pragma once
// Yerel dosyalar: the user's own music folders (Settings::localFolders), Spotube's local library.
//
// local::scan() walks the folders recursively on worker threads (never the UI thread), reads the tags of new or
// changed files through the Windows property system (System.Title, Music.Artist, Music.AlbumArtist,
// Music.AlbumTitle, Music.TrackNumber, Music.DiscNumber, Media.Duration, Media.Year; the file name "Artist - Title"
// is the fallback) and copies embedded cover art (System.ThumbnailStream: ID3 APIC / MP4 covr / FLAC PICTURE /
// WM/Picture) once into a cover folder; files without one use the folder's cover.jpg / folder.jpg. The index
// (%LOCALAPPDATA%\ShadeTube\local-library.json: path, size, mtime + tags) lets a rescan skip unchanged files.
// The user's files are only ever read.
//
// A folder that can't be listed (unplugged drive, offline share, access denied) is not "empty": its indexed tracks
// are carried over unchanged and no cover is pruned, so they come back as soon as it is reachable again. Only a
// folder that is really gone from a reachable volume drops its tracks.
//
// Tracks get the id "local:<FNV-1a 64 of the lower-cased full path>" (stable across rescans); artist and album
// ids stay empty, so no Spotify / MusicBrainz action ever sees them. Covers are "file:///" catalog images
// (gfx::ImageCache reads them in place). Paths longer than MAX_PATH are opened with the "\\?\" prefix.
//
// LocalLibrary is the UI-thread side: the snapshot the page and the player's localFileResolvers read, scan
// scheduling, progress and change notifications. The local:: functions only need core + catalog, so
// tests/localfiles compiles this file directly.
#include "catalog/Models.h"
#include "core/Async.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app {

namespace local {

inline constexpr size_t kMaxFiles = 50'000;                  // per scan over all folders; the rest is skipped (logged)
inline constexpr int64_t kMaxFileBytes = int64_t{1} << 30;   // local playback reads the whole file into memory

struct Entry {
    std::wstring path;                   // full path (never "\\?\"-prefixed: ids and the index use it as is)
    int64_t size = 0;
    uint64_t mtime = 0;                  // last write time (FILETIME ticks)
    std::string title;                   // tag, else from the file name
    std::vector<std::string> artists;
    std::string albumArtist, album;
    int discNumber = 0;                  // 0 = untagged (sorts with disc 1)
    int trackNumber = 0;
    int year = 0;
    int durationMs = 0;                  // 0 = unknown (the player learns it from the decoder)
    bool tagged = false;                 // the title came from the file's tags
    std::wstring cover;                  // image file, "" = none
    bool embeddedCover = false;          // `cover` is our extracted copy (else the user's own folder image)
};

struct ScanStats {
    int files = 0;            // audio files in the result
    int reused = 0;           // unchanged since the previous index (not re-read)
    int read = 0;             // new or changed: tags read
    int failed = 0;           // unreadable, skipped
    int removed = 0;          // in the previous index, gone now
    int carried = 0;          // kept from the previous index because their folder couldn't be listed
    int onlineOnly = 0;       // cloud placeholders (OneDrive "online-only"): skipped, never downloaded by a scan
    bool capped = false;      // stopped at maxFiles
    bool incomplete = false;  // some folder couldn't be listed (its entries were carried over)
    bool cancelled = false;   // the result is partial: don't use it
    std::vector<std::wstring> missingRoots;   // listed folders that couldn't be reached at all
};

// Cancels a running scan: the flag, plus the scan's threads, so file I/O stuck on a stalled network share is
// interrupted (CancelSynchronousIo) instead of holding the app's exit.
class ScanCancel {
public:
    ScanCancel() = default;
    ScanCancel(const ScanCancel&) = delete;
    ScanCancel& operator=(const ScanCancel&) = delete;

    bool requested() const { return flag_.load(std::memory_order_relaxed); }
    void request();   // any thread

    // Registers the calling thread for its lifetime (scan() does it for its own threads).
    class ThreadScope {
    public:
        explicit ThreadScope(ScanCancel* cancel);
        ~ThreadScope();
        ThreadScope(const ThreadScope&) = delete;
        ThreadScope& operator=(const ThreadScope&) = delete;

    private:
        ScanCancel* cancel_;
        void* thread_ = nullptr;   // HANDLE
    };

private:
    std::atomic<bool> flag_{false};
    std::mutex mutex_;
    std::vector<void*> threads_;
};

struct ScanOptions {
    std::vector<std::wstring> folders;
    std::filesystem::path coverDir;                       // extracted covers go here ("" = no covers)
    size_t maxFiles = kMaxFiles;
    int readThreads = 3;                                  // tag readers for the new / changed files
    std::function<void(int done, int total)> progress;    // any worker thread; total = files to read
    std::shared_ptr<ScanCancel> cancel;                   // requested: scan() returns early, stats.cancelled
};

// What local-library.json stores next to the files.
struct IndexMeta {
    int64_t scannedAt = 0;                   // unix seconds of the scan that wrote it
    std::vector<std::wstring> folders;       // the folders that scan covered
    bool capped = false;                     // it stopped at kMaxFiles
};

// Extensions scanned (lower case, with the dot): the formats Media Foundation decodes on Windows 10/11.
const std::vector<std::wstring>& audioExtensions();
bool isAudioFile(const std::wstring& path);
std::string trackIdFor(const std::wstring& path);   // "local:0123456789abcdef"
std::string fileUrl(const std::wstring& path);      // "file:///C:/Music/a%20b.jpg" (what gfx::ImageCache loads)
// The path for Win32 file APIs: "\\?\" (or "\\?\UNC\") prefixed when it is too long for the plain form.
std::wstring openablePath(const std::wstring& path);

// Tags + embedded cover of one file into `e` (path/size/mtime already set). COM must be initialized on the calling
// thread. Returns false when the file can't be opened at all (or `cancel` was requested meanwhile).
bool readFile(Entry& e, const std::filesystem::path& coverDir, const ScanCancel* cancel = nullptr);
// Blocking (worker thread): walks the folders, keeps `previous` entries whose size + mtime still match and reads
// the rest. Result sorted by artist, album, disc, track number, title.
std::vector<Entry> scan(const ScanOptions& o, const std::vector<Entry>& previous, ScanStats* stats = nullptr);
catalog::Track toTrack(const Entry& e);

std::vector<Entry> loadIndex(const std::filesystem::path& file, IndexMeta* meta = nullptr);
bool saveIndex(const std::filesystem::path& file, const std::vector<Entry>& entries, const IndexMeta& meta);   // tmp + rename
// Deletes extracted covers that no entry references any more. Returns the number removed.
int pruneCovers(const std::filesystem::path& coverDir, const std::vector<Entry>& entries);

// Path for Settings::localFolders: absolute, normalized, no trailing separator (except a drive root); "" when it
// can't be stored (e.g. a name with an unpaired UTF-16 surrogate, which the UTF-8 settings / index can't hold).
std::wstring normalizeFolder(const std::wstring& path);
// `path` equals or lies inside `folder` (both normalized, case-insensitive).
bool isUnder(const std::wstring& path, const std::wstring& folder);
// A scan of `parent` reaches the folder `child`: it lies inside `parent` and no folder on the way is one the walk
// skips (junction / symlink, hidden, system, "." prefixed). Touches the disk (attributes of each level).
bool walkedFrom(const std::wstring& parent, const std::wstring& child);

} // namespace local

// ---------------------------------------------------------------------------------------------------
// UI thread only.
class LocalLibrary {
public:
    static LocalLibrary& get();

    void start();     // loads the index on a worker (tracks show at once); an incremental rescan follows a few s later
    void tick();      // housekeeping: runs that startup rescan when due
    void rescan();    // incremental, background; while one runs, one more is queued
    void cancel();    // app exit: stops a running scan (a partial result is never applied or saved)

    bool loaded() const { return loaded_; }
    bool scanning() const { return scanning_; }
    int scanDone() const { return scanDone_; }
    int scanTotal() const { return scanTotal_; }   // files to read in the running scan (0 = still walking the folders)
    int64_t lastScanAt() const { return lastScanAt_; }   // unix seconds, 0 = never
    const local::ScanStats& lastStats() const { return stats_; }   // of the last finished scan (empty before it)
    bool capped() const { return stats_.capped || (!scanned_ && indexCapped_); }   // the library stops at kMaxFiles
    // The folder was covered by a finished scan (this run, or the one that wrote the index).
    bool scannedFolder(const std::wstring& folder) const;
    // The last scan couldn't reach this listed folder (its tracks are the ones indexed before).
    bool folderUnavailable(const std::wstring& folder) const;

    const std::vector<catalog::Track>& tracks() const { return tracks_; }
    const std::vector<local::Entry>& entries() const { return *entries_; }   // parallel to tracks() (same order)
    int64_t totalMs() const { return totalMs_; }
    uint64_t generation() const { return generation_; }   // bumps whenever tracks() changes
    // The file of a "local:" id, ready for Win32 file APIs (see local::openablePath); "" when unknown.
    std::wstring pathFor(const std::string& trackId) const;
    int countUnder(const std::wstring& folder) const;      // tracks inside a (normalized) folder

    // Settings::localFolders. Both save the settings and rescan. addFolder: false when the folder is already
    // scanned (itself, or it is walked from a listed folder); listed folders the new one walks into are dropped.
    // removeFolder cancels a running scan (it may still be reading that folder).
    bool addFolder(const std::wstring& path);
    void removeFolder(const std::wstring& path);

    // Change notifications (tracks, scan state / progress). Subscriptions die with their Lifetime.
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }

    struct Snapshot;   // built on a worker: entries + tracks + id -> path

private:
    LocalLibrary() = default;
    void notify();
    void apply(std::shared_ptr<Snapshot> s);
    void startScan();

    std::shared_ptr<const std::vector<local::Entry>> entries_ = std::make_shared<std::vector<local::Entry>>();
    std::vector<catalog::Track> tracks_;
    std::unordered_map<std::string, std::wstring> paths_;   // track id -> file
    int64_t totalMs_ = 0;
    uint64_t generation_ = 0;
    bool loaded_ = false;
    bool started_ = false;
    bool scanning_ = false;
    bool scanned_ = false;         // a scan finished in this run (stats_ is valid)
    bool rescanQueued_ = false;
    int64_t startupScanDue_ = 0;   // GetTickCount64 ms, 0 = done / not scheduled
    int scanDone_ = 0, scanTotal_ = 0;
    int64_t lastScanAt_ = 0;
    bool indexCapped_ = false;
    std::vector<std::wstring> scannedFolders_;
    local::ScanStats stats_;
    std::shared_ptr<local::ScanCancel> cancel_;
    Lifetime life_;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
};

} // namespace st::app
