// Download sync engine and its UI (see DownloadSync.h; rules and planning: SyncRules.h).
#include "app/DownloadSync.h"

#include "app/AppContext.h"
#include "app/Blacklist.h"
#include "app/Components.h"
#include "app/Downloads.h"
#include "app/PageWidgets.h"
#include "app/Router.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Window.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <thread>

namespace st::app {

namespace sync {
namespace {

using namespace catalog;
using CT = YoutubeExplode::CancellationToken;
using CTS = YoutubeExplode::CancellationTokenSource;
using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

constexpr int64_t kGiB = 1024ll * 1024 * 1024;

struct Engine {
    State state;
    std::filesystem::path file;
    bool loaded = false;
    // The pass in progress.
    bool running = false;
    std::deque<std::string> pass;         // rule ids still to list
    std::string listing;                  // the rule being listed ("" = none)
    int spotifyListed = 0;                // Spotify listings in this pass (the next one waits a moment first)
    bool capHit = false;                  // a plan in this pass left tracks out for the storage cap
    std::shared_ptr<CTS> cts;             // cancels listings (logout)
    int64_t spotifyRetryAt = 0;           // after a 429: no Spotify listing before this (unix seconds)
    // Conditions.
    bool capReached = false;              // the last pass hit the storage cap
    bool capToasted = false;
    bool metered = false;
    bool netChecking = false;
    int64_t netCheckedAt = 0;             // steadyMs()
    int64_t startedAt = 0;                // unix seconds (startup delay)
    Lifetime life;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
};

Engine& engine() {
    static Engine e;
    return e;
}

void notify() {
    auto& e = engine();
    std::erase_if(e.listeners, [](const auto& l) { return l.first.expired(); });
    auto snapshot = e.listeners;
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

// ---- What the download queue holds ----------------------------------------------------------------------------------

struct Snapshot {
    std::unordered_map<std::string, ItemInfo> items;
    ItemLookup lookup() const {
        return [this](const std::string& id) {
            const auto it = items.find(id);
            return it == items.end() ? ItemInfo{} : it->second;
        };
    }
};

Snapshot snapshot() {
    Snapshot s;
    const auto& items = ctx().downloads.items();
    s.items.reserve(items.size());
    for (const auto& d : items) {
        ItemInfo i;
        switch (d.state) {
        case DlState::Queued: i.state = ItemInfo::St::Queued; break;
        case DlState::Downloading: i.state = ItemInfo::St::Downloading; break;
        case DlState::Done: i.state = ItemInfo::St::Done; break;
        case DlState::Failed: i.state = ItemInfo::St::Failed; break;
        }
        i.synced = d.synced;
        i.sizeBytes = d.sizeBytes;
        i.durationMs = d.track.durationMs;
        s.items.emplace(d.track.id, i);
    }
    return s;
}

// Sync downloads on disk + the estimate of the queued ones (what the storage cap counts).
int64_t usedBytes(const Snapshot& s) {
    const int kbps = Settings::get().downloadMp3Kbps;
    int64_t n = 0;
    for (const auto& [id, i] : s.items) {
        if (!i.synced) continue;
        if (i.state == ItemInfo::St::Done) n += i.sizeBytes;
        else if (i.state == ItemInfo::St::Queued || i.state == ItemInfo::St::Downloading) n += estimateBytes(i.durationMs, kbps);
    }
    return n;
}

bool allowedNow() {
    const auto& s = Settings::get();
    return !s.syncPaused && (!engine().metered || s.syncOnMetered);
}

// Attempts are kept only for tracks some rule wants and that are not downloaded yet.
void save() {
    auto& e = engine();
    if (!e.loaded) return;
    const auto wanted = wantedIds(e.state.rules);
    const Snapshot snap = snapshot();
    std::erase_if(e.state.attempts, [&](const auto& a) {
        const auto it = snap.items.find(a.first);
        return !wanted.contains(a.first) || (it != snap.items.end() && it->second.state == ItemInfo::St::Done);
    });
    if (!saveState(e.state, e.file)) ST_LOG_WARN("sync", "could not write sync.json");
}

// ---- Network ---------------------------------------------------------------------------------------------------------

// Worker thread: whether Windows calls the internet connection metered (fixed / variable cost, roaming, over the data
// limit). Unknown (no profile, API failure) = not metered.
bool queryMetered() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool metered = false;
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        if (auto profile = NetworkInformation::GetInternetConnectionProfile()) {
            const auto cost = profile.GetConnectionCost();
            const auto type = cost.NetworkCostType();
            metered = type == NetworkCostType::Fixed || type == NetworkCostType::Variable || cost.Roaming() ||
                      cost.OverDataLimit();
        }
    } catch (...) {
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return metered;
}

void refreshNetwork() {
    auto& e = engine();
    const int64_t t = steadyMs();
    if (e.netChecking || (e.netCheckedAt != 0 && t - e.netCheckedAt < 60000)) return;
    e.netChecking = true;
    async(Priority::Low, e.life.ref(), [] { return queryMetered(); }, [](Result<bool> r) {
        auto& e = engine();
        e.netChecking = false;
        e.netCheckedAt = steadyMs();
        const bool metered = r && *r;
        if (metered == e.metered) return;
        e.metered = metered;
        ST_LOG_INFO("sync", "connection {}", metered ? "metered" : "not metered");
        notify();
        ctx().downloads.tick();   // resume waiting sync downloads right away
    });
}

// ---- Listing ---------------------------------------------------------------------------------------------------------

struct Listing {
    std::vector<Track> tracks;
    std::string name;                 // "" = keep the rule's
    std::vector<Image> images;
};

// Worker thread: every page of a Spotify playlist / Liked Songs, a short pause between pages (rate limits).
Listing listSpotifyPlaylist(spotify::Api* api, const std::string& uri, bool liked, const CT& ct) {
    Listing l;
    Playlist meta;
    std::string owner;
    int offset = 0, total = 0;
    for (int page = 0; page < 200; ++page) {   // at most 20 000 rows
        ct.throwIfCancellationRequested();
        const bool first = page == 0 && !liked;
        auto p = api->playlistTracks(uri, offset, 100, ct, first ? &meta : nullptr, first ? &owner : nullptr);
        if (page == 0) total = p.total;
        const int next = p.nextOffset >= 0 ? p.nextOffset : offset + static_cast<int>(p.items.size());
        for (auto& t : p.items) l.tracks.push_back(std::move(t));
        if (next <= offset || next >= total) break;
        offset = next;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!liked) {
        l.name = meta.name;
        l.images = meta.images;
    }
    return l;
}

// Worker thread: an album (Spotify or MusicBrainz) with its tracks pointing at it.
Listing listAlbum(spotify::Api* api, const std::string& id, const CT& ct) {
    Album a = source::album(api, id, ct);
    Listing l;
    l.name = a.name;
    l.images = a.images;
    for (auto& t : a.tracks) {
        if (t.album.id.empty() && t.album.name.empty()) t.album = {a.id, a.name, a.images};
        if (t.album.images.empty()) t.album.images = a.images;
        l.tracks.push_back(std::move(t));
    }
    return l;
}

void listNext();

void finishPass() {
    auto& e = engine();
    e.running = false;
    e.listing.clear();
    e.pass.clear();
    e.cts.reset();
    e.capReached = e.capHit;
    if (e.capHit && !e.capToasted) {
        e.capToasted = true;
        toast(tr(L"Senkron depolama sınırına ulaştı; yeni şarkılar indirilmiyor"), false, true);
    }
    if (!e.capHit) e.capToasted = false;
    e.capHit = false;
    save();
    notify();
}

// Drops sync downloads no rule wants any more (their files only with "Listeden çıkan şarkıları sil"). `protect`: ids
// a suspicious listing would have dropped (kept until the next listing confirms).
void applyDrops(const std::vector<std::string>& protect) {
    auto& e = engine();
    auto wanted = wantedIds(e.state.rules);
    wanted.insert(protect.begin(), protect.end());
    auto& dm = ctx().downloads;
    std::vector<std::pair<std::string, ItemInfo>> items;
    const Snapshot snap = snapshot();
    items.reserve(snap.items.size());
    for (const auto& [id, info] : snap.items) items.emplace_back(id, info);
    const Drop d = dropped(items, wanted, Settings::get().syncRemoveDropped);
    for (const auto& id : d.cancel) {
        const auto* it = dm.item(id);
        if (!it) continue;
        if (it->state == DlState::Failed) dm.remove(id);
        else dm.cancel(id);
    }
    for (const auto& id : d.remove) dm.remove(id);
    if (!d.cancel.empty() || !d.remove.empty())
        ST_LOG_INFO("sync", "dropped {} queued and {} downloaded sync item(s)", d.cancel.size(), d.remove.size());
}

// Queues what the rule is missing (within the storage cap). Records one attempt per queued track.
void queueMissing(const std::vector<Track>& tracks) {
    auto& e = engine();
    const auto& s = Settings::get();
    const Snapshot snap = snapshot();
    Limits lim;
    lim.capBytes = static_cast<int64_t>(s.syncCapGb) * kGiB;
    lim.usedBytes = usedBytes(snap);
    lim.mp3Kbps = s.downloadMp3Kbps;
    const int64_t now = nowUnix();
    const Plan p = plan(tracks, snap.lookup(), [](const Track& t) { return blacklist::isBlocked(t); }, e.state.attempts,
                        lim, now);
    if (p.skippedCap > 0) e.capHit = true;
    for (const auto& t : p.toQueue) {
        auto& a = e.state.attempts[t.id];
        ++a.count;
        a.last = now;
    }
    if (!p.toQueue.empty()) {
        ST_LOG_INFO("sync", "queueing {} track(s) (~{} MB){}", p.toQueue.size(), p.queuedBytes / 1048576,
                    p.skippedCap ? " - storage cap reached" : "");
        ctx().downloads.enqueueSynced(p.toQueue);
    }
}

void onListed(const std::string& id, Listing l, int64_t startedAt) {
    auto& e = engine();
    e.listing.clear();
    if (Rule* r = findRule(e.state, id)) {
        // Blocked tracks and the songs the user took out are not part of what the rule keeps (and so never count as
        // missing).
        std::vector<Track> tracks;
        tracks.reserve(l.tracks.size());
        for (auto& t : l.tracks)
            if (!blacklist::isBlocked(t) && !isExcluded(*r, t.id)) tracks.push_back(std::move(t));
        auto ids = ruleTrackIds(tracks);
        std::vector<std::string> protect;
        if (suspiciousShrink(r->trackIds.size(), ids.size())) {
            ST_LOG_WARN("sync", "{}: listing came back empty (had {}); not dropping anything this time", id, r->trackIds.size());
            protect = r->trackIds;
        }
        ST_LOG_INFO("sync", "{}: {} track(s)", id, ids.size());
        r->trackIds = std::move(ids);
        // The listing's start: a change made while it ran is newer and triggers the next listing.
        r->lastSync = startedAt;
        r->error = ListError::None;
        r->errorDetail.clear();
        r->failures = 0;
        r->retryAt = 0;
        if (!l.name.empty()) r->name = l.name;
        if (!l.images.empty()) r->images = l.images;
        queueMissing(tracks);
        applyDrops(protect);
    }
    save();
    notify();
    Dispatcher::post([] { listNext(); });
}

void onListFailed(const std::string& id, ListError kind, const std::string& detail) {
    auto& e = engine();
    e.listing.clear();
    if (Rule* r = findRule(e.state, id)) {
        ++r->failures;
        r->error = kind;
        r->errorDetail = detail;
        r->retryAt = nowUnix() + backoffSeconds(r->failures, kind == ListError::RateLimited);
        ST_LOG_WARN("sync", "{}: listing failed ({}), retry in {} s", id, detail, r->retryAt - nowUnix());
        if (kind == ListError::RateLimited) e.spotifyRetryAt = r->retryAt;   // every Spotify rule waits
    }
    save();
    notify();
    Dispatcher::post([] { listNext(); });
}

ListError classify(const std::exception_ptr& ep, std::string& detail) {
    try {
        std::rethrow_exception(ep);
    } catch (const spotify::ApiError& ex) {
        detail = ex.what();
        if (ex.status == 429) return ListError::RateLimited;
        if (ex.status == 404 || ex.status == 403) return ListError::NotFound;
        return ListError::Network;
    } catch (const std::exception& ex) {
        detail = ex.what();
    } catch (...) {
        detail = "unknown error";
    }
    return ListError::Network;
}

void beginListing(const Rule& r) {
    auto& e = engine();
    const std::string id = r.id;
    const int64_t startedAt = nowUnix();
    auto& lib = ctx().library;
    // Local collections: already in memory.
    if (r.id == kLocalLikedId) {
        Listing l;
        l.tracks = lib.liked();
        return onListed(id, std::move(l), startedAt);
    }
    if (r.kind == Kind::Playlist && !isSpotifyRule(r)) {
        const auto* p = lib.playlist(id);
        const auto* tracks = lib.playlistTracks(id);
        if (!p || !tracks) return onListFailed(id, ListError::NotFound, "local playlist not found");
        Listing l;
        l.tracks = *tracks;
        l.name = p->name;
        l.images = p->images;
        return onListed(id, std::move(l), startedAt);
    }
    // Spotify lists and albums (Spotify / MusicBrainz): on a worker.
    spotify::Api* api = source::activeApi();
    const bool spotify = isSpotifyRule(r);
    const bool pause = spotify && e.spotifyListed++ > 0;
    const Kind kind = r.kind;
    const CT ct = e.cts ? e.cts->token() : CT{};
    auto cts = e.cts;
    async(
        Priority::Low, e.life.ref(),
        [api, id, kind, pause, ct] {
            if (pause) std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // one collection at a time, gently
            if (kind == Kind::Album) return listAlbum(api, id, ct);
            if (!api) throw spotify::ApiError(401, "not logged in");
            return listSpotifyPlaylist(api, id, kind == Kind::Liked, ct);
        },
        [id, startedAt, cts](Result<Listing> res) {
            if (res) return onListed(id, std::move(*res), startedAt);
            if (cts && cts->isCancellationRequested()) {   // logged out / removed meanwhile: not the rule's fault
                auto& en = engine();
                en.listing.clear();
                if (en.running && en.cts == cts) en.cts = std::make_shared<CTS>();   // the rest of the pass goes on
                notify();
                return Dispatcher::post([] { listNext(); });
            }
            std::string detail;
            const ListError kind = classify(res.error(), detail);
            onListFailed(id, kind, detail);
        });
}

void listNext() {
    auto& e = engine();
    if (!e.running) return;
    while (!e.pass.empty()) {
        const std::string id = e.pass.front();
        e.pass.pop_front();
        const Rule* r = findRule(e.state, id);
        if (!r) continue;
        if (isSpotifyRule(*r) && (!source::loggedIn() || nowUnix() < e.spotifyRetryAt)) continue;
        e.listing = id;
        notify();
        return beginListing(*r);
    }
    finishPass();
}

void startPass(std::vector<std::string> ids) {
    auto& e = engine();
    if (e.running || ids.empty()) return;
    e.running = true;
    e.pass.assign(ids.begin(), ids.end());
    e.spotifyListed = 0;
    e.capHit = false;
    e.cts = std::make_shared<CTS>();
    ST_LOG_INFO("sync", "pass over {} rule(s)", ids.size());
    notify();
    listNext();
}

// Some of the rule's tracks are neither downloaded nor on their way, and could be (not waiting for a retry, not
// given up, and the storage cap has room).
bool pending(const Rule& r, const Snapshot& snap, int64_t now) {
    auto& e = engine();
    if (e.capReached) return false;
    for (const auto& id : r.trackIds) {
        const auto it = snap.items.find(id);
        if (it == snap.items.end()) return true;
        if (it->second.state == ItemInfo::St::Failed) {
            const auto a = e.state.attempts.find(id);
            if (a == e.state.attempts.end() || retryDue(a->second, now)) return true;
        }
    }
    return false;
}

// Housekeeping (UI thread, every ~2 s).
void tick() {
    auto& e = engine();
    if (!e.loaded || e.state.rules.empty()) return;
    refreshNetwork();
    if (e.running || !allowedNow()) return;
    const int64_t now = nowUnix();
    if (now - e.startedAt < kStartupDelaySec) return;
    std::vector<std::string> due;
    std::optional<Snapshot> snap;
    for (const auto& r : e.state.rules) {
        if (isSpotifyRule(r) && (!source::loggedIn() || now < e.spotifyRetryAt)) continue;
        bool isPending = false;
        if (r.lastSync != 0 && now - r.lastSync >= kPendingRelistSec && now >= r.retryAt) {
            if (!snap) snap = snapshot();
            isPending = pending(r, *snap, now);
        }
        if (ruleDue(r, now, isPending)) due.push_back(r.id);
    }
    startPass(std::move(due));
}

// A change here (library, likes, playlist edits): mark the rules it may concern.
void markChanged(const std::string& id) {
    if (Rule* r = findRule(engine().state, id)) r->changedAt = nowUnix();
}

void onLibraryChanged() {
    auto& e = engine();
    const int64_t now = nowUnix();
    for (auto& r : e.state.rules)
        if (!isSpotifyRule(r) && r.kind != Kind::Album) r.changedAt = now;   // local lists: listing them is free
    // Spotify Liked Songs: a like / unlike shows in the snapshot's ids at once.
    auto* sess = ctx().session;
    Rule* liked = findRule(e.state, kSpotifyLikedId);
    if (!liked || !sess || !sess->loggedIn() || !sess->library().loaded || liked->lastSync == 0) return;
    const auto& ids = sess->library().likedIds;
    const std::unordered_set<std::string> have(liked->trackIds.begin(), liked->trackIds.end());
    bool changed = std::any_of(liked->trackIds.begin(), liked->trackIds.end(), [&](const std::string& id) { return !ids.contains(id); });
    if (!changed)
        for (const auto& id : ids)
            if (!have.contains(id) && !blacklist::isTrackBlocked(id) && !isExcluded(*liked, id)) {
                changed = true;
                break;
            }
    if (changed && liked->changedAt <= liked->lastSync) liked->changedAt = now;
}

// Liked Songs are named in the UI language of the moment (the stored name is only a fallback for the others).
std::wstring displayName(const Rule& r) {
    if (r.kind == Kind::Liked) return tr(L"Beğenilen Şarkılar");
    return r.name.empty() ? std::wstring(L"?") : toWide(r.name);
}

Route routeOf(const Rule& r) {
    if (r.kind == Kind::Liked) return {RouteKind::Liked};
    return {r.kind == Kind::Album ? RouteKind::Album : RouteKind::Playlist, r.id};
}

void removeRule(const std::string& id, bool deleteFiles) {
    auto& e = engine();
    const Rule* r = findRule(e.state, id);
    if (!r) return;
    const std::vector<std::string> ids = r->trackIds;
    const std::wstring name = displayName(*r);
    std::erase_if(e.state.rules, [&](const Rule& x) { return x.id == id; });
    std::erase(e.pass, id);
    if (e.listing == id && e.cts) e.cts->cancel();
    const auto wanted = wantedIds(e.state.rules);
    auto& dm = ctx().downloads;
    std::vector<std::string> keep;
    int deleted = 0;
    for (const auto& tid : ids) {
        const auto* it = dm.item(tid);
        if (!it || !it->synced || wanted.contains(tid)) continue;
        switch (it->state) {
        case DlState::Queued:
        case DlState::Downloading: dm.cancel(tid); break;
        case DlState::Failed: dm.remove(tid); break;
        case DlState::Done:
            if (deleteFiles) {
                dm.remove(tid);
                ++deleted;
            } else {
                keep.push_back(tid);
            }
            break;
        }
    }
    dm.keepAsManual(keep);
    ST_LOG_INFO("sync", "rule {} removed: {} file(s) deleted, {} kept", id, deleted, keep.size());
    save();
    notify();
    if (deleteFiles && deleted > 0)
        toast(i18n::format(tr(L"\"{}\" artık senkronize edilmiyor; {} silindi"), {name, i18n::plural(L"{} şarkı", deleted)}));
    else
        toast(i18n::format(tr(L"\"{}\" artık senkronize edilmiyor"), {name}));
}

// "1,5 GB" / "320 MB" with the UI language's decimal separator.
std::wstring sizeText(int64_t bytes) {
    const double mb = std::max<int64_t>(0, bytes) / 1048576.0;
    wchar_t b[32];
    if (mb >= 1024) {
        swprintf(b, 32, L"%.1f", mb / 1024.0);
        std::wstring n = b;
        std::replace(n.begin(), n.end(), L'.', i18n::decimalSeparator());
        return i18n::format(tr(L"{} GB"), {n});
    }
    swprintf(b, 32, L"%.0f", mb);
    return i18n::format(tr(L"{} MB"), {b});
}

std::wstring errorText(ListError e) {
    switch (e) {
    case ListError::NotFound: return tr(L"Liste bulunamadı; silinmiş ya da gizli olabilir");
    case ListError::RateLimited: return tr(L"Spotify istekleri sınırladı; birazdan yeniden denenecek");
    case ListError::Network: return tr(L"Liste alınamadı; yeniden denenecek");
    default: return {};
    }
}

// "412 / 430 şarkı · 12 iniyor · 2 hata · 3 saat önce"
std::wstring statusLine(const Rule& r, const Status& st) {
    if (!st.sourceReady) return tr(L"Spotify'a bağlanınca senkronize edilir");
    if (r.lastSync == 0) {
        if (st.listing) return tr(L"Liste alınıyor…");
        if (st.error != ListError::None) return errorText(st.error);
        return tr(L"Senkron bekliyor");
    }
    const auto& p = st.progress;
    std::wstring s = i18n::plural(L"{} / {} şarkı", p.total, {i18n::number(p.done), i18n::number(p.total)});
    if (p.active > 0) s += L" · " + i18n::plural(L"{} indirilecek", p.active);
    if (p.failed > 0) s += L" · " + i18n::plural(L"{} hata", p.failed);
    if (!r.excluded.empty()) s += L" · " + i18n::plural(L"{} hariç", static_cast<long long>(r.excluded.size()));
    if (st.listing) s += L" · " + std::wstring(tr(L"güncelleniyor…"));
    else if (st.error != ListError::None) s += L" · " + errorText(st.error);
    else s += L" · " + relativeTime(r.lastSync);
    return s;
}

// ---- Excluded songs ---------------------------------------------------------------------------------------------------

// Sync downloads of `ids` no rule wants any more: queued / downloading ones are dropped, downloaded files deleted (the
// user asked for them to go). The user's own downloads are left alone.
void dropSyncDownloads(const std::vector<std::string>& ids) {
    auto& e = engine();
    const auto wanted = wantedIds(e.state.rules);
    auto& dm = ctx().downloads;
    for (const auto& id : ids) {
        e.state.attempts.erase(id);
        const auto* it = dm.item(id);
        if (!it || !it->synced || wanted.contains(id)) continue;
        if (it->state == DlState::Queued || it->state == DlState::Downloading) dm.cancel(id);
        else dm.remove(id);
    }
}

// Takes the songs out of every rule that keeps them. Returns how many songs that concerned.
int excludeEverywhere(const std::vector<Track>& tracks) {
    auto& e = engine();
    const int64_t now = nowUnix();
    std::vector<std::string> ids;
    for (const auto& t : tracks) {
        bool any = false;
        for (auto& r : e.state.rules)
            if (std::find(r.trackIds.begin(), r.trackIds.end(), t.id) != r.trackIds.end()) any = exclude(r, t, now) || any;
        if (any) ids.push_back(t.id);
    }
    if (ids.empty()) return 0;
    dropSyncDownloads(ids);
    ST_LOG_INFO("sync", "{} song(s) excluded", ids.size());
    save();
    notify();
    return static_cast<int>(ids.size());
}

// Lets songs back into the rules they were taken out of (all rules, or `onlyRule`); those rules are listed again soon.
// Returns how many rules changed.
int includeEverywhere(const std::vector<std::string>& ids, const std::string& onlyRule = {}) {
    auto& e = engine();
    int n = 0;
    const int64_t now = nowUnix();
    for (auto& r : e.state.rules) {
        if (!onlyRule.empty() && r.id != onlyRule) continue;
        bool any = false;
        for (const auto& id : ids) any = include(r, id) || any;
        if (any) {
            r.changedAt = now;   // the next listing puts them back in their place (and queues them)
            ++n;
        }
    }
    if (n == 0) return 0;
    save();
    notify();
    return n;
}

void showExcluded(const std::string& ruleId);

// ---- Downloads page row -----------------------------------------------------------------------------------------------

class RuleRow : public ui::Widget {
public:
    RuleRow(const Rule& r, const Status& st)
        : rule_(r), st_(st), title_(displayName(r), type::body),
          sub_(statusLine(r, st), type::caption) {
        focusable = true;
    }
    float preferredHeight(float) override { return 64; }
    // Keyboard (Tab stop): Enter / Space open the collection, the menu key / Shift+F10 its menu.
    bool activatable() const override { return true; }
    bool onActivate() override {
        open();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) return false;
        const Rect r = toWindow(rect());
        menu({r.right() - 36, r.bottom()});
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            menu(e.windowPos);
            return false;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return;
        if (btn_.contains(e.pos)) {
            const Rect b = toWindow(btn_);
            menu({b.x, b.bottom() + 6});
        } else if (rect().contains(e.pos)) {
            open();
        }
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool h = btn_.contains(e.pos);
        if (h != btnHover_) {
            btnHover_ = h;
            invalidate();
        }
    }
    void onMouseLeave() override {
        btnHover_ = false;
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const Rect art{r.x, r.cy() - 24, 48, 48};
        if (rule_.kind == Kind::Liked) {
            c.fillRounded(art, 2, accent().base);
            c.icon("heart-filled", art.center(20, 20), accent().onAccent);
        } else {
            drawArtwork(c, rule_.images, art, 2, rule_.kind == Kind::Album ? Placeholder::Album : Placeholder::Playlist);
        }
        const float x = art.right() + 14, w = r.right() - 48 - x;
        c.text(title_, {x, r.y + 12, w, 20}, col.fgPrimary, gfx::VAlign::Center);
        const bool err = st_.error != ListError::None && !st_.listing;
        c.text(sub_, {x, r.y + 32, w, 16}, err ? col.error : col.fgSecondary, gfx::VAlign::Center);
        // Download progress of the collection.
        const auto& p = st_.progress;
        if (p.total > 0 && p.done < p.total) {
            const Rect track{x, r.bottom() - 8, w, 3};
            c.fillPill(track, gfx::isLightTheme() ? col.hairStrong : col.bgOverlay);
            Rect fill = track;
            fill.w = std::max(3.f, track.w * p.done / static_cast<float>(p.total));
            c.fillPill(fill, accent().base);
        }
        btn_ = {r.right() - 36, r.cy() - 16, 32, 32};
        if (btnHover_) c.fillCircle({btn_.cx(), btn_.cy()}, 16, col.overlayHover);
        c.icon("more", btn_.center(16, 16), col.fgSecondary);
    }

private:
    void open() {
        const Route route = routeOf(rule_);
        ctx().router->navigate(route);   // destroys this row
    }
    void menu(gfx::Point at) {
        const std::string id = rule_.id;
        const Route route = routeOf(rule_);
        std::vector<ui::MenuItem> items{{tr(L"Aç"), "arrow-up-right", L"", [route] { ctx().router->navigate(route); }},
                                        {tr(L"Şimdi senkronize et"), "refresh", L"", [id] { syncNow(id); }}};
        if (st_.progress.failed > 0)
            items.push_back({tr(L"Hataları yeniden dene"), "refresh", L"", [id] { retryFailed(id); }});
        if (!rule_.excluded.empty())
            items.push_back({i18n::format(tr(L"Hariç tutulanlar ({})"), {i18n::number(static_cast<long long>(rule_.excluded.size()))}),
                             "list", L"", [id] { Dispatcher::post([id] { showExcluded(id); }); }});
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem off{tr(L"Senkronu kapat"), "close", L"", [id] { confirmDisable(id); }};
        off.destructive = true;
        items.push_back(std::move(off));
        ui::Menu::open(ctx().window, at, std::move(items));
    }

    Rule rule_;
    Status st_;
    gfx::Text title_, sub_;
    Rect btn_{};
    bool btnHover_ = false;
};

Status statusOf(const Rule& r, const Snapshot& snap) {
    Status st;
    st.synced = true;
    st.listing = engine().listing == r.id;
    st.sourceReady = !isSpotifyRule(r) || source::loggedIn();
    st.progress = progress(r, snap.lookup());
    st.error = r.error;
    st.lastSync = r.lastSync;
    return st;
}

// The songs taken out of a rule, newest first, each with "Geri al"; at most kShown of them (the rest are counted), and
// "Hepsini geri al" for all of them.
void showExcluded(const std::string& ruleId) {
    constexpr size_t kShown = 12;
    const Rule* r = findRule(engine().state, ruleId);
    if (!r || r->excluded.empty()) return;
    auto* d = ui::Dialog::open(
        ctx().window, tr(L"Hariç tutulan şarkılar"),
        i18n::format(tr(L"\"{}\" senkronize edilirken bu şarkılar indirilmez. Geri aldığın şarkı bir sonraki "
                        L"senkronda iner."),
                     {displayName(*r)}),
        560);
    if (!d) return;
    auto* body = d->body();
    const auto& ex = r->excluded;
    for (size_t shown = 0; shown < kShown && shown < ex.size(); ++shown) {
        const Excluded& x = ex[ex.size() - 1 - shown];
        auto* row = body->add<SettingRow>(toWide(x.name.empty() ? x.id : x.name), toWide(x.artists));
        auto* back = row->control<ui::Button>(110.f, ui::ButtonKind::Secondary, tr(L"Geri al"), "refresh");
        back->onClick = [d, ruleId, id = x.id] {
            includeEverywhere({id}, ruleId);
            toast(tr(L"Şarkı yeniden senkronize edilecek"));
            d->close();
            Dispatcher::post([ruleId] { showExcluded(ruleId); });   // the others, if any
        };
    }
    if (ex.size() > kShown)
        body->add<ui::Label>(i18n::plural(L"ve {} şarkı daha", static_cast<long long>(ex.size() - kShown)),
                             type::secondary, ui::Tone::Tertiary);
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"Hepsini geri al"), ui::ButtonKind::Primary, [ruleId] {
        std::vector<std::string> ids;
        if (const Rule* rr = findRule(engine().state, ruleId))
            for (const auto& x : rr->excluded) ids.push_back(x.id);
        includeEverywhere(ids, ruleId);
        toast(i18n::plural(L"{} şarkı yeniden senkronize edilecek", static_cast<long long>(ids.size())));
    });
}

} // namespace

// ---- Public API -----------------------------------------------------------------------------------------------------

Target likedTarget() {
    Target t;
    t.kind = Kind::Liked;
    t.id = source::loggedIn() ? kSpotifyLikedId : kLocalLikedId;
    t.name = toUtf8(tr(L"Beğenilen Şarkılar"));
    return t;
}

bool isSynced(const std::string& ruleId) { return !ruleId.empty() && findRule(engine().state, ruleId) != nullptr; }

void enable(const Target& t) {
    auto& e = engine();
    if (t.id.empty() || isSynced(t.id)) return;
    Rule r;
    r.id = t.id;
    r.kind = t.kind;
    r.name = t.name;
    r.images = t.images;
    r.addedAt = nowUnix();
    e.state.rules.push_back(std::move(r));
    ST_LOG_INFO("sync", "rule {} added", t.id);
    save();
    notify();
    const std::wstring name = toWide(t.name);
    if (Settings::get().syncPaused) {
        toast(i18n::format(tr(L"\"{}\" eklendi; senkron duraklatıldığı için şimdilik indirilmiyor"), {name}));
        return;
    }
    toast(i18n::format(tr(L"\"{}\" çevrimdışı kullanılabilir olacak"), {name}));
    if (!e.running) startPass({t.id});
    else e.pass.push_back(t.id);
}

void confirmDisable(const std::string& ruleId) {
    const Rule* r = findRule(engine().state, ruleId);
    if (!r) return;
    const Snapshot snap = snapshot();
    const bool anyFiles = std::any_of(r->trackIds.begin(), r->trackIds.end(), [&](const std::string& id) {
        const auto it = snap.items.find(id);
        return it != snap.items.end() && it->second.synced && it->second.state == ItemInfo::St::Done;
    });
    if (!anyFiles) return removeRule(ruleId, false);
    auto* d = ui::Dialog::open(
        ctx().window, tr(L"Çevrimdışı senkron kapatılsın mı?"),
        i18n::format(tr(L"\"{}\" artık güncel tutulmayacak. Senkronla indirilen şarkılar bu bilgisayarda kalsın mı? "
                        L"Kalanlar İndirilenler'de senin indirdiklerin gibi durur."),
                     {displayName(*r)}));
    if (!d) return;
    const std::string id = ruleId;
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"Şarkıları sil"), ui::ButtonKind::Secondary, [id] { removeRule(id, true); });
    d->addButton(tr(L"Şarkıları sakla"), ui::ButtonKind::Primary, [id] { removeRule(id, false); });
}

void syncNow(const std::string& ruleId) {
    auto& e = engine();
    if (Settings::get().syncPaused) {
        Settings::get().syncPaused = false;   // an explicit "sync now" also resumes
        Settings::get().markDirty();
    }
    std::vector<std::string> ids;
    for (auto& r : e.state.rules) {
        if (!ruleId.empty() && r.id != ruleId) continue;
        if (isSpotifyRule(r) && nowUnix() < e.spotifyRetryAt) continue;   // Spotify asked us to wait
        r.retryAt = 0;
        ids.push_back(r.id);
    }
    if (ids.empty()) {
        if (nowUnix() < e.spotifyRetryAt) toast(tr(L"Spotify istekleri sınırladı; birazdan yeniden denenecek"));
        return;
    }
    if (e.running) {
        for (const auto& id : ids)
            if (id != e.listing && std::find(e.pass.begin(), e.pass.end(), id) == e.pass.end()) e.pass.push_back(id);
    } else {
        startPass(std::move(ids));
    }
    ctx().downloads.tick();
}

void retryFailed(const std::string& ruleId) {
    auto& e = engine();
    if (const Rule* r = findRule(e.state, ruleId))
        for (const auto& id : r->trackIds) e.state.attempts.erase(id);
    e.capReached = false;
    syncNow(ruleId);
}

Status status(const std::string& ruleId) {
    const Rule* r = findRule(engine().state, ruleId);
    if (!r) return {};
    return statusOf(*r, snapshot());
}

std::wstring waitReason() {
    auto& e = engine();
    const auto& s = Settings::get();
    if (s.syncPaused) return tr(L"Senkron duraklatıldı.");
    if (e.metered && !s.syncOnMetered) return tr(L"Ölçülü bir bağlantıdasın; senkron indirmeleri bekliyor.");
    if (e.capReached)
        return i18n::format(tr(L"Depolama sınırına ulaşıldı ({} / {}). Yeni şarkılar indirilmiyor."),
                            {sizeText(syncedBytes()), sizeText(static_cast<int64_t>(s.syncCapGb) * kGiB)});
    return {};
}

int64_t syncedBytes() {
    int64_t n = 0;
    for (const auto& d : ctx().downloads.items())
        if (d.synced && d.state == DlState::Done) n += d.sizeBytes;
    return n;
}

const std::vector<Rule>& rules() { return engine().state.rules; }

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { engine().listeners.emplace_back(std::move(owner), std::move(fn)); }

ui::MenuItem menuItem(const Target& t) {
    if (isSynced(t.id)) return {tr(L"Çevrimdışı senkronu kapat"), "downloaded", L"", [id = t.id] { confirmDisable(id); }};
    return {tr(L"Çevrimdışı kullanılabilir yap"), "download", L"", [t] { enable(t); }};
}

bool isWanted(const std::string& trackId) {
    for (const auto& r : engine().state.rules)
        if (std::find(r.trackIds.begin(), r.trackIds.end(), trackId) != r.trackIds.end()) return true;
    return false;
}

bool isExcludedAnywhere(const std::string& trackId) {
    for (const auto& r : engine().state.rules)
        if (isExcluded(r, trackId)) return true;
    return false;
}

void excludeTracks(const std::vector<Track>& tracks) {
    const int n = excludeEverywhere(tracks);
    if (n > 0)
        toast(n == 1 ? std::wstring(tr(L"Şarkı senkrondan çıkarıldı; yeniden indirilmeyecek"))
                     : i18n::plural(L"{} şarkı senkrondan çıkarıldı", n));
}

void includeTracks(const std::vector<std::string>& ids) {
    if (includeEverywhere(ids) > 0) toast(tr(L"Şarkı yeniden senkronize edilecek"));
}

void deleteDownloads(const std::vector<Track>& tracks) {
    auto& dm = ctx().downloads;
    std::vector<Track> had;
    for (const auto& t : tracks)
        if (dm.item(t.id)) had.push_back(t);
    if (had.empty()) return;
    // Songs a synced collection keeps would come back with its next listing: take them out of it first.
    const int excluded = excludeEverywhere(had);
    for (const auto& t : had) {
        const auto* it = dm.item(t.id);
        if (!it) continue;   // a sync download excludeEverywhere already dropped
        if (it->state == DlState::Queued || it->state == DlState::Downloading) dm.cancel(t.id);
        else dm.remove(t.id);
    }
    std::wstring msg = had.size() == 1 ? std::wstring(tr(L"İndirilen şarkı silindi"))
                                       : i18n::plural(L"{} indirilen şarkı silindi", static_cast<long long>(had.size()));
    if (excluded > 0) msg += L" · " + std::wstring(tr(L"senkrondan da çıkarıldı"));
    toast(msg);
}

std::vector<ui::MenuItem> trackMenuItems(const std::vector<Track>& tracks) {
    std::vector<ui::MenuItem> items;
    auto& dm = ctx().downloads;
    std::vector<Track> downloaded, wanted;
    std::vector<std::string> excluded;
    for (const auto& t : tracks) {
        if (const auto* it = dm.item(t.id); it && it->state == DlState::Done) downloaded.push_back(t);
        if (isWanted(t.id)) wanted.push_back(t);
        else if (isExcludedAnywhere(t.id)) excluded.push_back(t.id);
    }
    if (!wanted.empty()) items.push_back({tr(L"Senkrondan çıkar"), "minus", L"", [wanted] { excludeTracks(wanted); }});
    if (!excluded.empty())
        items.push_back({tr(L"Senkrona geri al"), "refresh", L"", [excluded] { includeTracks(excluded); }});
    if (!downloaded.empty()) {
        ui::MenuItem del{downloaded.size() == 1
                             ? std::wstring(tr(L"İndirileni sil"))
                             : i18n::plural(L"{} indirileni sil", static_cast<long long>(downloaded.size())),
                         "trash", L"", [downloaded] {
                             if (downloaded.size() == 1) return deleteDownloads(downloaded);
                             ui::Dialog::confirm(
                                 ctx().window, tr(L"İndirilenler silinsin mi?"),
                                 i18n::plural(L"{} şarkının dosyası bu bilgisayardan silinecek. İstediğinde yeniden "
                                              L"indirebilirsin.",
                                              static_cast<long long>(downloaded.size())),
                                 tr(L"Sil"), [downloaded] { deleteDownloads(downloaded); }, true);
                         }};
        del.destructive = true;
        items.push_back(std::move(del));
    }
    return items;
}

// ---- Collection header toggle ---------------------------------------------------------------------------------------

SyncButton::SyncButton() : ui::Button(ui::ButtonKind::IconOutline, L"", "download") {
    onClick = [this] {
        if (target_.id.empty()) return;
        if (isSynced(target_.id)) confirmDisable(target_.id);
        else enable(target_);
    };
    sync::subscribe(life_.ref(), [this] { refresh(); });
    ctx().downloads.subscribe(life_.ref(), [this] { refresh(); });
}

void SyncButton::setTarget(Target t) {
    target_ = std::move(t);
    refresh();
}

void SyncButton::refresh() {
    st_ = status(target_.id);
    const auto& p = st_.progress;
    const bool complete = st_.synced && st_.lastSync != 0 && p.done >= p.total;
    setIcon(complete ? "check" : "download");
    setActive(st_.synced);
    invalidate();
}

void SyncButton::paint(gfx::Canvas& c) {
    ui::Button::paint(c);
    const auto& p = st_.progress;
    if (!st_.synced || p.total <= 0 || p.done >= p.total) return;
    // The share of the collection already on disk, as an accent arc over the hairline ring (tertiary while waiting).
    const Rect r = rect();
    const float d = std::min(r.w, r.h);
    const bool moving = (st_.listing || p.active > 0) && waitReason().empty();
    const float sweep = std::max(6.f, 360.f * p.done / static_cast<float>(p.total));
    c.arc({r.cx(), r.cy()}, d * 0.5f, -90.f, sweep, moving ? accent().base : colors().fgTertiary, 2.f);
}

std::wstring SyncButton::tooltip() const {
    if (!st_.synced) return tr(L"Çevrimdışı kullanılabilir yap");
    const auto& p = st_.progress;
    if (st_.lastSync == 0) return tr(L"Çevrimdışı kullanılabilir · senkronize ediliyor…");
    std::wstring s = i18n::plural(L"Çevrimdışı kullanılabilir · {} / {} şarkı", p.total,
                                  {i18n::number(p.done), i18n::number(p.total)});
    if (const auto why = waitReason(); !why.empty() && p.done < p.total) s += L"\n" + why;
    return s;
}

// ---- Downloads page section -------------------------------------------------------------------------------------------

void buildSyncSection(ui::Column* c) {
    auto& e = engine();
    if (e.state.rules.empty()) return;
    c->add<SectionHeader>(tr(L"Senkronize edilenler"), std::to_wstring(e.state.rules.size()));
    // Actions: sync everything now, pause / resume.
    auto* bar = c->add<ui::Box>();
    const bool paused = Settings::get().syncPaused;
    auto* now = bar->add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Şimdi senkronize et"), "refresh");
    auto* pause = bar->add<ui::Button>(ui::ButtonKind::Ghost, paused ? tr(L"Sürdür") : tr(L"Duraklat"), paused ? "play" : "pause");
    const std::wstring why = waitReason();
    auto* note = why.empty() ? nullptr : bar->add<ui::Label>(why, type::secondary, ui::Tone::Tertiary);
    now->onClick = [] { syncNow(); };
    pause->onClick = [paused] {
        Settings::get().syncPaused = !paused;
        Settings::get().markDirty();
        notify();
        ctx().downloads.tick();
    };
    bar->onPreferredHeight = [](float) { return 48.f; };
    bar->onLayout = [now, pause, note](ui::Box& b) {
        const float nw = now->naturalWidth(), pw = pause->naturalWidth();
        now->setRect({0, 4, nw, 40});
        pause->setRect({nw + 10, 4, pw, 40});
        if (note) note->setRect({nw + pw + 28, 4, std::max(0.f, b.rect().w - nw - pw - 28), 40});
    };
    const Snapshot snap = snapshot();
    for (const auto& r : e.state.rules) c->add<RuleRow>(r, statusOf(r, snap));
}

} // namespace sync

// ---- Ayarlar › İNDİRME ----------------------------------------------------------------------------------------------

void buildDownloadSyncRows(ui::Column* c, const std::function<void()>& rebuild) {
    auto& s = Settings::get();
    auto changed = [] {
        sync::notify();
        ctx().downloads.tick();
    };
    settingsToggle(c, tr(L"Çevrimdışı senkron"),
                   tr(L"Çevrimdışı kullanılabilir yaptığın Beğenilen Şarkılar, çalma listeleri ve albümler arka planda "
                      L"indirilir ve güncel tutulur. Bir listenin başlığındaki indirme düğmesiyle ekle."),
                   !s.syncPaused, [changed](bool v) {
                       Settings::get().syncPaused = !v;
                       changed();
                   });
    settingsToggle(c, tr(L"Ölçülü bağlantıda da senkronize et"),
                   tr(L"Kapalıyken kotalı (ölçülü) bir bağlantıdayken senkron indirmeleri bekler; kendi başlattığın "
                      L"indirmeler etkilenmez."),
                   s.syncOnMetered, [changed](bool v) {
                       Settings::get().syncOnMetered = v;
                       changed();
                   });
    settingsToggle(c, tr(L"Listeden çıkan şarkıları sil"),
                   tr(L"Senkronize edilen listelerin hiçbirinde kalmayan bir şarkının dosyası silinir. Kendi "
                      L"indirdiklerin ve klasörlerine eklediklerin hiç silinmez."),
                   s.syncRemoveDropped, [](bool v) { Settings::get().syncRemoveDropped = v; });
    auto* row = c->add<SettingRow>(
        tr(L"Senkron için depolama sınırı"),
        i18n::format(tr(L"Senkronize edilen şarkılar şu an {} yer kaplıyor. Sınıra ulaşınca yeni şarkı indirilmez."),
                     {sync::sizeText(sync::syncedBytes())}));
    static const int kCaps[] = {0, 1, 5, 10, 20};
    int cur = 0;
    for (int i = 0; i < 5; ++i)
        if (kCaps[i] == s.syncCapGb) cur = i;
    auto gb = [](int n) { return i18n::format(tr(L"{} GB"), {std::to_wstring(n)}); };
    auto* seg = row->control<Segmented>(
        300.f, std::vector<std::wstring>{tr(L"Sınırsız"), gb(1), gb(5), gb(10), gb(20)}, cur);
    seg->onChange = [changed](int i) {
        Settings::get().syncCapGb = kCaps[std::clamp(i, 0, 4)];
        Settings::get().markDirty();
        sync::engine().capReached = false;   // re-evaluated by the next pass
        changed();
    };
    (void)rebuild;
}

// ---- Startup --------------------------------------------------------------------------------------------------------

void initDownloadSync() {
    auto& e = sync::engine();
    e.file = paths::appData() / L"sync.json";
    e.state = sync::loadState(e.file);
    e.loaded = true;
    e.startedAt = nowUnix();
    ST_LOG_INFO("sync", "{} rule(s)", e.state.rules.size());
    ctx().downloads.syncAllowed = [] { return sync::allowedNow(); };
    ctx().housekeepingHooks.push_back([] { sync::tick(); });
    ctx().persistHooks.push_back([] { sync::save(); });
    ctx().library.subscribe(e.life.ref(), [] { sync::onLibraryChanged(); });
    if (auto* sess = ctx().session) {
        sess->subscribe(e.life.ref(), [] {
            auto& en = sync::engine();
            // Logged out while a Spotify list was being fetched: stop it (the rule keeps its last listing).
            if (!source::loggedIn() && en.cts && !en.listing.empty() && en.listing.rfind("spotify:", 0) == 0)
                en.cts->cancel();
            sync::onLibraryChanged();
            sync::notify();
        });
        sess->subscribePlaylistEdits(e.life.ref(), [](const spotify::PlaylistEdit& ed) { sync::markChanged(ed.uri); });
    }
}

} // namespace st::app
