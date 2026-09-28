#pragma once
// Songs dropped on the window from Windows Explorer (app/DragDrop): the dropped files and folders become tracks the
// player can play, even when they lie outside the Yerel dosyalar folders.
//
// collect() runs on a worker: folders go through the local library's scanner (local::scan: the same walk rules,
// tag reader and cover extraction), single files through local::readFile, at most kMaxFiles per drop; the result is
// sorted like the library (album artist, album, disc, track number, title, path). Ids are the local library's
// ("local:<hash of the path>"), so a dropped file that is also in the library is the same track.
//
// Store remembers the files that were dropped (%LOCALAPPDATA%\ShadeTube\dropped-files.json, in the local library's
// index format, the newest kRemember), so a restored queue, the history and the stats keep playing them after a
// restart. Their extracted covers live in cache\dropped-covers and are pruned with the list.
//
// Standalone (core + catalog + app/LocalLibrary's local:: functions): tests/localfiles compiles it directly.
#include "app/LocalLibrary.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app::dropped {

inline constexpr size_t kMaxFiles = 2'000;   // per drop
inline constexpr size_t kRemember = 5'000;   // files remembered across drops

struct Collected {
    std::vector<local::Entry> entries;       // sorted (sortEntries)
    std::vector<std::wstring> folders;       // the dropped folders (normalized), in drop order
    int skipped = 0;                         // dropped files that aren't audio or couldn't be read
    bool capped = false;                     // stopped at maxFiles
};

// Quick check while a drag hovers (no disk access beyond one attribute read): an audio file by its extension, or a
// folder.
bool droppable(const std::wstring& path);
// Blocking (worker thread; initializes COM for itself). `coverDir` "" = no covers.
Collected collect(const std::vector<std::wstring>& paths, const std::filesystem::path& coverDir, size_t maxFiles = kMaxFiles);
void sortEntries(std::vector<local::Entry>& entries);
// Adds `added` to `list` (a file already listed moves to the end with its new tags) and keeps the newest `cap`.
void mergeRemembered(std::vector<local::Entry>& list, const std::vector<local::Entry>& added, size_t cap = kRemember);

// ---- Drop rules (pure; tests/localfiles checks them) -------------------------------------------------------------
enum class DropRow { Liked, SpotifyPlaylist, LocalPlaylist };
// The songs of `tracks` a sidebar row takes when they are dropped on it: Liked Songs the ones `likeable` accepts, a
// Spotify playlist its Spotify songs, a local playlist every song with an id. Never radio stations.
std::vector<catalog::Track> droppableOn(DropRow row, const std::vector<catalog::Track>& tracks,
                                        const std::function<bool(const std::string& id)>& likeable);
// Drop slot in the queue: the order index the songs go in front of, from the pointer's `y` below the top of the
// upcoming list (scrolling included) with rows of `rowH`: the nearest row boundary, clamped to
// [current + 1, orderSize].
int queueDropIndex(float y, float rowH, int currentOrderIndex, int orderSize);

// The remembered files. UI thread.
class Store {
public:
    static Store& get();
    static std::filesystem::path file();        // <data>\dropped-files.json
    static std::filesystem::path coverDir();    // <cache>\dropped-covers

    void load();                                            // reads the list on a worker (once)
    void remember(const std::vector<local::Entry>& entries);   // then saves (and prunes covers) on a worker
    // The file of a remembered "local:" id, ready for Win32 file APIs; "" when unknown or gone.
    std::wstring pathFor(const std::string& trackId) const;

private:
    Store() = default;
    void save();

    std::vector<local::Entry> entries_;                    // oldest first
    std::unordered_map<std::string, std::wstring> paths_;  // track id -> file
    bool loaded_ = false, loading_ = false;
    std::vector<local::Entry> pending_;                    // remembered while the list was still loading
    Lifetime life_;
};

} // namespace st::app::dropped
