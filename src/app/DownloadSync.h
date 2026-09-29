#pragma once
// Download sync ("Çevrimdışı kullanılabilir"): Liked Songs, playlists and albums the user marked are kept downloaded.
// The rules and the pure planning live in app/SyncRules (sync.json); this is the engine and its UI.
//
// Engine (UI thread; listings on workers). A pass runs 45 s after startup, whenever a rule is due (SyncRules'
// ruleDue: periodic relisting, a change seen here — a like, a playlist edit, a local playlist change — or tracks still
// missing) and on "Şimdi senkronize et". It lists the rules one at a time (Spotify pages throttled, one collection at
// a time; a 429 stops the pass and backs off every Spotify rule), stores each listing's track ids, queues what is
// missing through the normal download queue (DownloadManager::enqueueSynced: after the user's own downloads, marked
// "sy") within the storage cap, and drops sync downloads no rule wants any more (their files only when
// "Listeden çıkan şarkıları sil" is on). Sync downloads wait while sync is paused or the connection is metered (unless
// allowed). Rules of an unavailable source (Spotify while logged out) keep their track ids, so nothing is dropped.
#include "app/SyncRules.h"
#include "core/Async.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/Popups.h"

#include <functional>
#include <string>
#include <vector>

namespace st::app {

namespace sync {

// A collection that can be kept offline.
struct Target {
    Kind kind = Kind::Playlist;
    std::string id;                        // rule id (see Rule::id)
    std::string name;
    std::vector<catalog::Image> images;
};
// Liked Songs as the active source sees them: Spotify's while logged in, else the local ones.
Target likedTarget();

bool isSynced(const std::string& ruleId);
// Adds the rule and syncs it right away (unless sync is paused). Toasts what happens.
void enable(const Target& t);
// Asks whether to keep the downloaded songs, then removes the rule (keep: they become the user's own downloads;
// delete: their files go, unless another rule or the user wants them).
void confirmDisable(const std::string& ruleId);
void syncNow(const std::string& ruleId = {});   // a forced pass over one rule ("" = all)
void retryFailed(const std::string& ruleId);    // clears the failure count of its tracks and syncs it

struct Status {
    bool synced = false;                   // a rule exists
    bool listing = false;                  // being listed right now
    bool sourceReady = true;               // false: a Spotify rule while logged out
    Progress progress;
    ListError error = ListError::None;
    int64_t lastSync = 0;
};
Status status(const std::string& ruleId);
// Why sync downloads wait, for the Downloads page ("" = they don't).
std::wstring waitReason();
int64_t syncedBytes();                     // sync downloads on disk
const std::vector<Rule>& rules();
// Fired on the UI thread when rules, their status or the pause state change. Dies with its Lifetime.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

// "Çevrimdışı kullanılabilir yap" / "Çevrimdışı senkronu kapat" for context menus.
ui::MenuItem menuItem(const Target& t);

// ---- Songs taken out of synced collections --------------------------------------------------------------------------
bool isWanted(const std::string& trackId);             // some rule keeps it offline
bool isExcludedAnywhere(const std::string& trackId);   // some rule has it excluded
// "Senkrondan çıkar": the songs leave every rule that keeps them (never downloaded again by them); their sync downloads
// go (queued ones dropped, files deleted) unless another rule still wants them. The user's own downloads stay.
void excludeTracks(const std::vector<catalog::Track>& tracks);
// "Senkrona geri al": back into the rules they were taken out of (downloaded with the next listing).
void includeTracks(const std::vector<std::string>& trackIds);
// Deletes the downloads of the songs (file + record, or drops them from the queue). Songs a synced collection keeps are
// excluded from it first, so they are not downloaded again.
void deleteDownloads(const std::vector<catalog::Track>& tracks);
// Track context menu entries: "Senkrondan çıkar", "Senkrona geri al", "İndirileni sil" (as they apply; may be empty).
std::vector<ui::MenuItem> trackMenuItems(const std::vector<catalog::Track>& tracks);

// Collection header toggle: the download icon in a hairline circle, with an accent ring of the download progress
// while syncing and the accent check once everything is there. Click = enable / confirmDisable.
class SyncButton : public ui::Button {
public:
    SyncButton();
    void setTarget(Target t);
    void paint(gfx::Canvas& c) override;
    std::wstring tooltip() const override;

private:
    void refresh();
    Target target_;
    Status st_;
    Lifetime life_;
};

// Downloads page: "Senkronize edilenler" (rules with their state + actions). Adds nothing when there are no rules.
void buildSyncSection(ui::Column* c);

} // namespace sync
} // namespace st::app
