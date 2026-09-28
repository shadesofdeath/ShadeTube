#pragma once
// Download manager: a background queue that resolves each track to a YouTube stream, downloads it and
// transcodes to MP3 (audio::downloadTrack), writes it under Music\ShadeTube\<Artist>\<Album>\ and records it
// in downloads.json. Users also group downloads into their own "collections" (virtual folders). UI-thread
// facing; the actual work runs on the worker pool. One download at a time (gentle on YouTube).
// With Settings::sponsorBlockEnabled, MP3 downloads are trimmed: the matched video's SponsorBlock segments
// (music_offtopic / sponsor / selfpromo / interaction) are cut out before encoding. A failed lookup never fails
// the download (it's just not trimmed). Passthrough (.m4a) downloads are never trimmed (no re-encode).
#include "catalog/Models.h"
#include "core/Async.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::app {

enum class DlState { Queued, Downloading, Done, Failed };

struct DownloadItem {
    catalog::Track track;
    DlState state = DlState::Queued;
    float progress = 0.f;        // 0..1 while Downloading
    std::string filePath;        // set when Done (UTF-8)
    std::string error;           // set when Failed
    int64_t sizeBytes = 0;
    int64_t addedAt = 0;
    int cutSegments = 0;         // SponsorBlock segments removed from the file (0 = none / not trimmed)
    int64_t cutMs = 0;           // audio removed by those cuts, ms
    bool synced = false;         // queued by the download sync (app/DownloadSync), not by the user; a manual download
                                 // or a folder add makes it the user's own (sync never deletes those)
};

struct Collection {
    std::string id;              // "col:<hex>"
    std::string name;
    std::vector<std::string> trackIds;   // ids of downloaded tracks in this collection
    int64_t createdAt = 0;
};

class DownloadManager {
public:
    void load();
    void saveIfDirty();

    // Queue one/many tracks (skips tracks already downloaded or queued). Optionally drop them into a
    // collection once finished.
    void enqueue(const catalog::Track& t, const std::string& collectionId = {});
    void enqueue(const std::vector<catalog::Track>& tracks, const std::string& collectionId = {});
    // Download sync (app/DownloadSync): queues `tracks` after everything else, marked as sync downloads (failed ones are
    // queued again; anything else already known is left as it is). One change notification for the whole batch.
    void enqueueSynced(const std::vector<catalog::Track>& tracks);
    // Makes sync downloads the user's own: sync cleanup never touches them again.
    void keepAsManual(const std::vector<std::string>& trackIds);
    // Sync downloads wait in the queue while this returns false (sync paused, metered connection). Empty = allowed.
    std::function<bool()> syncAllowed;
    void cancel(const std::string& trackId);          // remove from queue / stop the active one
    void remove(const std::string& trackId);          // delete the file + the record (+ from collections)

    bool isDownloaded(const std::string& trackId) const;
    bool isActive(const std::string& trackId) const;  // queued or downloading
    const DownloadItem* item(const std::string& trackId) const;
    const std::vector<DownloadItem>& items() const { return items_; }
    int activeCount() const;                            // queued + downloading
    int64_t totalBytes() const;

    // Collections (user folders).
    const std::vector<Collection>& collections() const { return collections_; }
    const Collection* collection(const std::string& id) const;
    std::string createCollection(const std::wstring& name);
    void renameCollection(const std::string& id, const std::wstring& name);
    void deleteCollection(const std::string& id);
    void addToCollection(const std::string& id, const std::string& trackId);
    void addToCollection(const std::string& id, const std::vector<catalog::Track>& tracks);   // queues if needed
    void removeFromCollection(const std::string& id, const std::string& trackId);
    std::vector<catalog::Track> collectionTracks(const std::string& id) const;

    // Pumps the queue (called from the app housekeeping tick, UI thread).
    void tick();

    // Worker-thread progress callback marshals here (UI thread) with 0..1 for the active download.
    void reportProgress(const std::string& trackId, float fraction);

    // Change notifications (die with their Lifetime).
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }
    void notify();

private:
    void touch();
    void startNext();
    DownloadItem* find(const std::string& trackId);
    const DownloadItem* find(const std::string& trackId) const;

    std::vector<DownloadItem> items_;
    std::vector<Collection> collections_;
    std::string activeId_;                 // trackId currently downloading ("" = idle)
    std::shared_ptr<std::atomic<bool>> cancelFlag_;   // for the active download
    Lifetime life_;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    bool dirty_ = false;
};

// Absolute file path a track would download to (Music\ShadeTube\<Artist>\<Album>\<Title>.<ext>).
std::wstring downloadTargetPath(const catalog::Track& t, bool mp3);

// "2 bölüm kesildi (54 sn)" / "7 bölüm kesildi (3 dk 12 sn)"; "" when nothing was cut.
std::wstring cutSummary(int segments, int64_t ms);

} // namespace st::app
