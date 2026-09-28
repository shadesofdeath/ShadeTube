#include "app/DroppedFiles.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_set>

namespace st::app::dropped {

namespace fs = std::filesystem;

namespace {
// COM for the calling thread (MTA); a thread that is already STA keeps it (and we don't uninitialize it).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// Size + last write time for local::readFile / the index (it reads the rest).
bool statFile(local::Entry& e) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(local::openablePath(e.path).c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    e.size = (static_cast<int64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    e.mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return e.size > 0 && e.size <= local::kMaxFileBytes;
}
} // namespace

bool droppable(const std::wstring& path) {
    if (local::isAudioFile(path)) return true;
    const DWORD a = GetFileAttributesW(local::openablePath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void sortEntries(std::vector<local::Entry>& entries) {
    auto cmp = [](const std::wstring& a, const std::wstring& b) {
        if (a.empty() != b.empty()) return a.empty() ? 1 : -1;   // untagged last
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                               static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
               CSTR_EQUAL;
    };
    struct Key {
        std::wstring artist, album, title;
    };
    std::vector<Key> keys(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        keys[i] = {toWide(!e.albumArtist.empty() ? e.albumArtist : e.artists.empty() ? std::string{} : e.artists[0]),
                   toWide(e.album), toWide(e.title)};
    }
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (int c = cmp(keys[x].artist, keys[y].artist)) return c < 0;
        if (int c = cmp(keys[x].album, keys[y].album)) return c < 0;
        const int dx = std::max(entries[x].discNumber, 1), dy = std::max(entries[y].discNumber, 1);   // untagged = disc 1
        if (dx != dy) return dx < dy;
        if (entries[x].trackNumber != entries[y].trackNumber) return entries[x].trackNumber < entries[y].trackNumber;
        if (int c = cmp(keys[x].title, keys[y].title)) return c < 0;
        return lower(entries[x].path) < lower(entries[y].path);
    });
    std::vector<local::Entry> sorted;
    sorted.reserve(entries.size());
    for (size_t i : order) sorted.push_back(std::move(entries[i]));
    entries = std::move(sorted);
}

Collected collect(const std::vector<std::wstring>& paths, const fs::path& coverDir, size_t maxFiles) {
    ComScope com;
    Collected r;
    std::vector<std::wstring> files;
    for (const auto& p : paths) {
        const DWORD a = GetFileAttributesW(local::openablePath(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            ++r.skipped;
            continue;
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (std::wstring f = local::normalizeFolder(p); !f.empty()) r.folders.push_back(std::move(f));
        } else if (local::isAudioFile(p)) {
            files.push_back(p);
        } else {
            ++r.skipped;
        }
    }
    // Loose files first (in drop order), then the folders' contents, all under one cap; a file dropped both on its
    // own and inside a dropped folder is listed once.
    std::unordered_set<std::wstring> seen;
    for (const auto& f : files) {
        if (r.entries.size() >= maxFiles) {
            r.capped = true;
            break;
        }
        local::Entry e;
        e.path = f;
        if (!seen.insert(lower(f)).second) continue;
        if (!statFile(e) || !local::readFile(e, coverDir)) {
            ++r.skipped;
            ST_LOG_INFO("dragdrop", "dropped file unreadable, skipped: {}", toUtf8(f));
            continue;
        }
        r.entries.push_back(std::move(e));
    }
    if (!r.folders.empty() && r.entries.size() < maxFiles) {
        local::ScanOptions o;
        o.folders = r.folders;
        o.coverDir = coverDir;
        o.maxFiles = maxFiles - r.entries.size();
        local::ScanStats st;
        auto found = local::scan(o, {}, &st);
        r.capped = r.capped || st.capped;
        r.skipped += st.failed;
        for (auto& e : found)
            if (seen.insert(lower(e.path)).second) r.entries.push_back(std::move(e));
    } else if (!r.folders.empty()) {
        r.capped = true;
    }
    sortEntries(r.entries);
    ST_LOG_INFO("dragdrop", "dropped: {} paths -> {} tracks ({} folders, {} skipped{})", paths.size(), r.entries.size(),
                r.folders.size(), r.skipped, r.capped ? ", capped" : "");
    return r;
}

void mergeRemembered(std::vector<local::Entry>& list, const std::vector<local::Entry>& added, size_t cap) {
    // A file listed twice in `added` keeps its last occurrence.
    std::unordered_map<std::wstring, size_t> last;
    for (size_t i = 0; i < added.size(); ++i) last[lower(added[i].path)] = i;
    std::erase_if(list, [&](const local::Entry& e) { return last.contains(lower(e.path)); });
    for (size_t i = 0; i < added.size(); ++i)
        if (last[lower(added[i].path)] == i) list.push_back(added[i]);
    if (list.size() > cap) list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(cap));
}

std::vector<catalog::Track> droppableOn(DropRow row, const std::vector<catalog::Track>& tracks,
                                        const std::function<bool(const std::string& id)>& likeable) {
    std::vector<catalog::Track> out;
    for (const auto& t : tracks) {
        if (t.id.empty() || t.id.rfind("radio:", 0) == 0) continue;
        const bool takes = row == DropRow::Liked             ? likeable && likeable(t.id)
                           : row == DropRow::SpotifyPlaylist ? t.id.rfind("spotify:track:", 0) == 0
                                                             : true;
        if (takes) out.push_back(t);
    }
    return out;
}

int queueDropIndex(float y, float rowH, int currentOrderIndex, int orderSize) {
    const int first = currentOrderIndex + 1;
    if (rowH <= 0) return std::max(first, orderSize);
    const int k = static_cast<int>(std::floor(y / rowH + 0.5f));
    return std::clamp(first + k, std::min(first, orderSize), orderSize);
}

// ---- Store ----------------------------------------------------------------------------------------------

Store& Store::get() {
    static Store s;
    return s;
}

fs::path Store::file() { return paths::appData() / L"dropped-files.json"; }
fs::path Store::coverDir() { return paths::cacheDir() / L"dropped-covers"; }

void Store::load() {
    if (loaded_ || loading_) return;
    loading_ = true;
    async(
        Priority::Low, life_.ref(), [] { return local::loadIndex(file()); },
        [this](Result<std::vector<local::Entry>> r) {
            loading_ = false;
            loaded_ = true;
            entries_ = r ? std::move(*r) : std::vector<local::Entry>{};
            if (!pending_.empty()) {   // dropped before the list was read: newer than anything in it
                mergeRemembered(entries_, pending_);
                pending_.clear();
                save();
            }
            paths_.clear();
            for (const auto& e : entries_) paths_[local::trackIdFor(e.path)] = e.path;
            ST_LOG_INFO("dragdrop", "{} dropped files remembered", entries_.size());
        });
}

void Store::remember(const std::vector<local::Entry>& entries) {
    if (entries.empty()) return;
    for (const auto& e : entries) paths_[local::trackIdFor(e.path)] = e.path;
    if (!loaded_) {
        mergeRemembered(pending_, entries);
        load();
        return;
    }
    mergeRemembered(entries_, entries);
    save();
}

void Store::save() {
    // The id map keeps what was forgotten by the cap until the next start: harmless, and the queue may hold them.
    background(Priority::Low, [entries = entries_] {
        local::IndexMeta meta;
        meta.scannedAt = static_cast<int64_t>(time(nullptr));
        if (!local::saveIndex(file(), entries, meta)) ST_LOG_WARN("dragdrop", "saving dropped-files.json failed");
        local::pruneCovers(coverDir(), entries);
    });
}

std::wstring Store::pathFor(const std::string& trackId) const {
    const auto it = paths_.find(trackId);
    if (it == paths_.end()) return {};
    const std::wstring open = local::openablePath(it->second);
    const DWORD a = GetFileAttributesW(open.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? open : std::wstring();
}

} // namespace st::app::dropped
