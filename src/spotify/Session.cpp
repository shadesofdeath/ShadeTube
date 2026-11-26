#include "spotify/Session.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "spotify/Auth.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace st::spotify {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
int64_t nowSec() { return nowMs() / 1000; }
} // namespace

namespace store {
std::string loadCookie() { return AccountStore::activeCookie(paths::appData()); }
} // namespace store

Session::Connected connectAccount(const std::string& spDc, const CT& ct) {
    Session::Connected c;
    c.token = fetchAccessToken(spDc, ct);
    // A throwaway Api: the session's own keeps serving the current account until the switch commits.
    Api api;
    api.setCredentials(c.token.accessToken, spDc);
    try {
        c.profile = api.me(ct);
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "profile fetch failed: {}", e.what());
    }
    return c;
}

Session::Session(std::filesystem::path dataRoot) : accounts_(dataRoot.empty() ? paths::appData() : std::move(dataRoot)) {}

std::function<Session::Connected()> Session::connectJob(std::string spDc) const {
    if (connector) return [fn = connector, spDc = std::move(spDc)] { return fn(spDc); };
    return [spDc = std::move(spDc)] { return connectAccount(spDc); };
}

void Session::loadAccounts(std::vector<std::wstring> perAccountFiles) {
    accounts_.setPerAccountFiles(std::move(perAccountFiles));
    accounts_.load();
    if (const auto* a = accounts_.active())
        ST_LOG_INFO("spotify", "{} account(s), {} signed in; active {}{}", accounts_.accounts().size(),
                    accounts_.signedIn().size(), a->id, accounts_.signedIn(a->id) ? "" : " (signed out)");
}

void Session::notify() {
    // A listener may (re)subscribe while we notify — logging in rebuilds Home, whose ctor calls subscribe(),
    // which push_backs into listeners_. Iterate over a copy so that never invalidates our loop.
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    auto snapshot = listeners_;
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

void Session::restore() {
    const std::string id = accounts_.activeId();
    if (id.empty()) return;
    std::string cookie = accounts_.loadCookie(id);
    if (cookie.empty()) {
        // Signed out (its data stays on screen in MusicBrainz mode), unless another account is still signed in (a
        // sign out whose hand-over could not connect last time): that one takes over.
        const Account* next = nullptr;
        for (const auto& a : accounts_.signedIn())
            if (const Account* p = accounts_.find(a.id); !next || p->usedAt > next->usedAt) next = p;
        if (!next) return;
        state_ = SessionState::Connecting;
        notify();
        switchTo(next->id);
        return;
    }
    state_ = SessionState::Connecting;
    notify();
    connectLife_.renew();
    async(
        Priority::High, connectLife_.ref(), connectJob(cookie),
        [this, id, cookie](Result<Connected> r) {
            if (!r) {
                ST_LOG_WARN("spotify", "token fetch failed: {}", r.errorMessage());
                if (state_ == SessionState::Connecting) {
                    state_ = SessionState::LoggedOut;
                    notify();
                }
                return;
            }
            commit(id, cookie, std::move(*r), false);
        });
}

Session::LoginSlot Session::beginLogin() {
    LoginSlot slot;
    slot.id = accounts_.newLoginFolder();
    if (!slot.id.empty()) slot.webViewDir = accounts_.webViewDir(slot.id);
    return slot;
}

void Session::finishLogin(const LoginSlot& slot, std::string spDc, std::function<void(bool)> done) {
    if (slot.id.empty() || spDc.empty()) {
        discardLogin(slot.id);
        if (done) done(false);
        return;
    }
    const bool wasLoggedIn = loggedIn();
    if (!wasLoggedIn) {
        state_ = SessionState::Connecting;
        notify();
    }
    connectLife_.renew();
    switchingTo_.clear();
    async(
        Priority::High, connectLife_.ref(), connectJob(spDc),
        [this, slot, spDc, done, wasLoggedIn](Result<Connected> r) {
            if (!r) {
                ST_LOG_WARN("spotify", "login: token fetch failed: {}", r.errorMessage());
                discardLogin(slot.id);
                if (!wasLoggedIn && state_ == SessionState::Connecting) {
                    state_ = SessionState::LoggedOut;
                    notify();
                }
                if (done) done(false);
                return;
            }
            // The same Spotify user as an account already here: that account signs in again (its folder, its data);
            // this login's folder goes.
            std::string id = slot.id;
            bool add = true;
            if (const Account* known = accounts_.findByUser(r->profile.id)) {
                id = known->id;
                add = false;
                discardLogin(slot.id);
            }
            if (!accounts_.saveCookie(id, spDc)) ST_LOG_WARN("spotify", "login: the cookie could not be saved");
            commit(id, spDc, std::move(*r), add);
            if (done) done(true);
        });
}

void Session::switchTo(const std::string& accountId, std::function<void(bool)> done) {
    if (accountId == accounts_.activeId() && loggedIn()) {
        if (done) done(true);
        return;
    }
    std::string cookie = accounts_.loadCookie(accountId);
    if (!accounts_.find(accountId) || cookie.empty()) {
        if (done) done(false);
        return;
    }
    connectLife_.renew();   // a switch still connecting is superseded
    switchingTo_ = accountId;
    notify();
    async(
        Priority::High, connectLife_.ref(), connectJob(cookie),
        [this, accountId, cookie, done](Result<Connected> r) {
            switchingTo_.clear();
            if (!r) {
                ST_LOG_WARN("spotify", "switch to account {} failed: {}", accountId, r.errorMessage());
                if (state_ == SessionState::Connecting) state_ = SessionState::LoggedOut;
                notify();
                if (done) done(false);
                return;
            }
            commit(accountId, cookie, std::move(*r), false);
            if (done) done(true);
        });
}

void Session::clearAccountState() {
    life_.renew();   // drop in-flight library loads, edits and refreshes of the account that is leaving
    refreshing_ = false;
    spDc_.clear();
    token_ = {};
    profile_ = {};
    library_ = {};
    api_.setCredentials("", "");
    api_.setUsername({});
}

void Session::commit(const std::string& id, const std::string& cookie, Connected c, bool add) {
    const bool changes = id != accounts_.activeId();
    const std::string leaving = accounts_.activeId();
    if (changes && onAccountLeaving) onAccountLeaving();
    if (changes && !leaving.empty() && library_.loaded) parked_[leaving] = std::move(library_);
    clearAccountState();
    if (add) {
        Account a;
        a.id = id;
        accounts_.add(std::move(a), nowSec());   // the very first account takes over the profile's files here
    }
    accounts_.updateProfile(id, c.profile.id, c.profile.username, c.profile.name, c.profile.imageUrl);
    accounts_.setActive(id, nowSec());
    accounts_.save();

    spDc_ = cookie;
    token_ = std::move(c.token);
    api_.setCredentials(token_.accessToken, cookie);
    if (c.profile.valid()) {
        profile_ = std::move(c.profile);
        api_.setUsername(profile_.username);
    } else if (const Account* a = accounts_.find(id)) {   // the profile failed: what the list remembers
        profile_.id = a->user;
        profile_.name = a->name;
        profile_.username = a->username;
        profile_.imageUrl = a->image;
        api_.setUsername(a->username);
    }
    if (auto it = parked_.find(id); it != parked_.end()) {   // switching back: its library at once, then a reload
        library_ = std::move(it->second);
        parked_.erase(it);
    }
    state_ = SessionState::LoggedIn;
    ST_LOG_INFO("spotify", "account {} active{}", id, changes ? " (switched)" : "");
    if (changes && onAccountEntered) onAccountEntered();
    notify();
    if (!connector) reloadLibrary();
}

void Session::signOut(const std::string& accountId) {
    if (accountId.empty() || !accounts_.find(accountId)) return;
    if (accountId != accounts_.activeId()) {
        accounts_.signOut(accountId);
        parked_.erase(accountId);
        if (switchingTo_ == accountId) {
            connectLife_.renew();
            switchingTo_.clear();
        }
        notify();
        return;
    }
    connectLife_.renew();
    switchingTo_.clear();
    clearAccountState();
    accounts_.signOut(accountId);
    parked_.erase(accountId);
    // Another signed-in account takes over (the most recently used one); else logged out, the data of this one stays.
    const Account* next = nullptr;
    for (const auto& a : accounts_.signedIn())
        if (const Account* p = accounts_.find(a.id); !next || p->usedAt > next->usedAt) next = p;
    if (!next) {
        state_ = SessionState::LoggedOut;
        notify();
        return;
    }
    state_ = SessionState::Connecting;
    notify();
    switchTo(next->id);
}

void Session::logout() { signOut(accounts_.activeId()); }

bool Session::isSavedAlbum(const std::string& id) const {
    return std::any_of(library_.albums.begin(), library_.albums.end(), [&](const catalog::Album& a) { return a.id == id; });
}
bool Session::isFollowing(const std::string& id) const {
    return std::any_of(library_.artists.begin(), library_.artists.end(), [&](const catalog::Artist& a) { return a.id == id; });
}
static int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void Session::markAlbumSaved(const catalog::Album& a, bool saved) {
    if (saved) {
        if (!isSavedAlbum(a.id)) {
            library_.albums.insert(library_.albums.begin(), a);
            library_.entries[a.id].addedAt = unixNow();
        }
    } else {
        std::erase_if(library_.albums, [&](const catalog::Album& x) { return x.id == a.id; });
    }
}
void Session::markArtistFollowed(const catalog::Artist& a, bool follow) {
    if (follow) {
        if (!isFollowing(a.id)) {
            library_.artists.insert(library_.artists.begin(), a);
            library_.entries[a.id].addedAt = unixNow();
        }
    } else {
        std::erase_if(library_.artists, [&](const catalog::Artist& x) { return x.id == a.id; });
    }
}

void Session::reloadLibrary() {
    if (!api_.hasCredentials()) return;
    const int seq = editSeq_;
    async(
        Priority::High, life_.ref(),
        [this] {
            LibrarySnapshot s;
            // Each list is independent; a failure in one shouldn't blank the others.
            auto tryFetch = [](auto&& fn) {
                try {
                    fn();
                } catch (const std::exception& e) {
                    ST_LOG_WARN("spotify", "library fetch failed: {}", e.what());
                }
            };
            tryFetch([&] { s.playlists = api_.libraryPlaylists({}, &s.tree, &s.entries); });
            tryFetch([&] { s.albums = api_.libraryAlbums({}, &s.entries); });
            tryFetch([&] { s.artists = api_.libraryArtists({}, &s.entries); });
            // Liked Songs ids for heart state (paged; capped so a huge library can't stall login).
            tryFetch([&] {
                int off = 0;
                while (off < 10000) {
                    const auto pg = api_.playlistTracks(Api::kLikedSongsUri, off, 100);
                    for (const auto& t : pg.items)
                        if (!t.id.empty()) s.likedIds.insert(t.id);
                    const int next = pg.nextOffset >= 0 ? pg.nextOffset : off + static_cast<int>(pg.items.size());
                    if (!pg.hasMore || next <= off) break;
                    off = next;
                }
            });
            s.loaded = true;
            return s;
        },
        [this, seq](Result<LibrarySnapshot> r) {
            if (!r) return;
            library_ = std::move(*r);
            ST_LOG_INFO("spotify", "library: {} playlists, {} albums, {} artists", library_.playlists.size(),
                        library_.albums.size(), library_.artists.size());
            notify();
            // A playlist edit applied while this load was in flight may be missing from what it fetched (e.g. a
            // playlist created right after login): re-read the playlists, which now include it.
            if (editSeq_ != seq) reloadPlaylists();
        });
}

void Session::reloadPlaylists() {
    if (!api_.hasCredentials()) return;
    async(
        Priority::Low, life_.ref(),
        [this] {
            struct Out {
                std::vector<catalog::Playlist> playlists;
                PlaylistTree tree;
                LibraryEntries entries;
            } out;
            out.playlists = api_.libraryPlaylists({}, &out.tree, &out.entries);
            return out;
        },
        [this](auto r) {
            if (!r) {
                ST_LOG_WARN("spotify", "playlists reload failed: {}", r.errorMessage());
                return;
            }
            library_.playlists = std::move(r->playlists);
            library_.tree = std::move(r->tree);
            for (auto& [uri, e] : r->entries) library_.entries.insert_or_assign(uri, e);   // albums' / artists' stay
            notify();
        });
}

// ---- Playlist editing -----------------------------------------------------------------------------------------

const catalog::Playlist* Session::findPlaylist(const std::string& uri) const {
    for (const auto& p : library_.playlists)
        if (p.id == uri) return &p;
    return nullptr;
}

std::vector<catalog::Playlist> Session::editablePlaylists() const {
    std::vector<catalog::Playlist> out;
    for (const auto& p : library_.playlists)
        if (p.editable && p.id.rfind("spotify:playlist:", 0) == 0) out.push_back(p);
    return out;
}

void Session::applyEdit(const PlaylistEdit& e) {
    using K = PlaylistEdit::Kind;
    auto& pls = library_.playlists;
    auto it = std::find_if(pls.begin(), pls.end(), [&](const catalog::Playlist& p) { return p.id == e.uri; });
    switch (e.kind) {
    case K::Created: {
        if (it != pls.end()) break;
        catalog::Playlist p;
        p.id = e.uri;
        p.name = e.name;
        p.totalTracks = e.count;
        p.countKnown = true;
        p.ownerId = api_.username();
        p.ownerName = profile_.name;
        p.owned = p.editable = true;
        p.createdAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        library_.entries[p.id].addedAt = p.createdAt;   // "Son eklenenler" until the next reload says the same
        // The web player adds new playlists to the top of the rootlist; keep pinned pseudo-playlists (Liked Songs) first.
        auto pos = std::find_if(pls.begin(), pls.end(), [](const catalog::Playlist& x) { return x.id.rfind("spotify:playlist:", 0) == 0; });
        pls.insert(pos, std::move(p));
        break;
    }
    case K::Added:
        if (it != pls.end() && it->countKnown) it->totalTracks += e.count;
        break;
    case K::Removed:
        if (it != pls.end() && it->countKnown) it->totalTracks = std::max(0, it->totalTracks - e.count);
        break;
    case K::Moved: break;   // same rows, same count
    case K::Renamed:
        if (it != pls.end()) it->name = e.name;
        break;
    case K::Details:
        if (it != pls.end()) {
            if (!e.name.empty()) it->name = e.name;
            if (e.description) it->description = *e.description;
        }
        break;
    case K::Deleted:
        if (it != pls.end()) pls.erase(it);
        break;
    }
    ++editSeq_;
    notify();
    std::erase_if(editListeners_, [](const auto& l) { return l.first.expired(); });
    auto snapshot = editListeners_;   // a listener may subscribe while we iterate
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn(e);
}

void Session::createPlaylist(std::string name, std::vector<std::string> trackUris,
                             std::function<void(const std::string& uri, int added)> done) {
    if (!api_.hasCredentials() || name.empty()) {
        if (done) done({}, 0);
        return;
    }
    struct Out {
        std::string uri;
        int added = 0;
    };
    async(
        Priority::High, life_.ref(),
        [this, name, trackUris] {
            Out o;
            o.uri = api_.createPlaylist(name);
            if (!o.uri.empty() && !trackUris.empty() && api_.addToPlaylist(o.uri, trackUris))
                o.added = static_cast<int>(trackUris.size());
            return o;
        },
        [this, name, done](Result<Out> r) {
            const std::string uri = r ? r->uri : std::string();
            const int added = r ? r->added : 0;
            if (!uri.empty()) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Created;
                e.uri = uri;
                e.name = name;
                e.count = added;
                applyEdit(e);
            }
            if (done) done(uri, added);
        });
}

void Session::addToPlaylist(const std::string& uri, std::vector<std::string> trackUris, std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.empty() || trackUris.empty()) {
        if (done) done(false);
        return;
    }
    const int n = static_cast<int>(trackUris.size());
    async(
        Priority::High, life_.ref(), [this, uri, trackUris] { return api_.addToPlaylist(uri, trackUris); },
        [this, uri, n, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Added;
                e.uri = uri;
                e.count = n;
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::insertIntoPlaylist(const std::string& uri, std::vector<std::string> trackUris, PlaylistPosition at,
                                 std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.empty() || trackUris.empty()) {
        if (done) done(false);
        return;
    }
    const int n = static_cast<int>(trackUris.size());
    async(
        Priority::High, life_.ref(), [this, uri, trackUris, at] { return api_.insertIntoPlaylist(uri, trackUris, at); },
        [this, uri, n, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Added;
                e.uri = uri;
                e.count = n;
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::moveInPlaylist(const std::string& uri, std::vector<std::string> uids, PlaylistPosition to,
                             std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.empty() || uids.empty()) {
        if (done) done(false);
        return;
    }
    const int n = static_cast<int>(uids.size());
    async(
        Priority::High, life_.ref(), [this, uri, uids, to] { return api_.moveInPlaylist(uri, uids, to); },
        [this, uri, n, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Moved;
                e.uri = uri;
                e.count = n;
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::updatePlaylistDetails(const std::string& uri, PlaylistDetailsChange change, std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.rfind("spotify:playlist:", 0) != 0 || (change.name && change.name->empty())) {
        if (done) done(false);
        return;
    }
    if (change.empty()) {
        if (done) done(true);
        return;
    }
    PlaylistEdit e;
    e.kind = PlaylistEdit::Kind::Details;
    e.uri = uri;
    e.name = change.name.value_or(std::string());
    e.description = change.description;
    e.coverChanged = change.cover != PlaylistDetailsChange::Cover::Keep;
    async(
        Priority::High, life_.ref(), [this, uri, change = std::move(change)] { return api_.updatePlaylistDetails(uri, change); },
        [this, e, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                applyEdit(e);
                if (e.coverChanged) reloadPlaylists();   // the library's cards show the new picture
            }
            if (done) done(ok);
        });
}

void Session::removeFromPlaylist(const std::string& uri, std::vector<std::string> uids, std::function<void(bool)> done) {
    std::erase_if(uids, [](const std::string& u) { return u.empty(); });
    if (!api_.hasCredentials() || uri.empty() || uids.empty()) {
        if (done) done(false);
        return;
    }
    async(
        Priority::High, life_.ref(), [this, uri, uids] { return api_.removeFromPlaylist(uri, uids); },
        [this, uri, uids, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Removed;
                e.uri = uri;
                e.removedUids = uids;
                e.count = static_cast<int>(uids.size());
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::renamePlaylist(const std::string& uri, std::string name, std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.empty() || name.empty()) {
        if (done) done(false);
        return;
    }
    async(
        Priority::High, life_.ref(), [this, uri, name] { return api_.renamePlaylist(uri, name); },
        [this, uri, name, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Renamed;
                e.uri = uri;
                e.name = name;
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::deletePlaylist(const std::string& uri, std::function<void(bool)> done) {
    if (!api_.hasCredentials() || uri.rfind("spotify:playlist:", 0) != 0) {
        if (done) done(false);
        return;
    }
    async(
        Priority::High, life_.ref(), [this, uri] { return api_.deletePlaylist(uri); },
        [this, uri, done](Result<bool> r) {
            const bool ok = r && *r;
            if (ok) {
                PlaylistEdit e;
                e.kind = PlaylistEdit::Kind::Deleted;
                e.uri = uri;
                applyEdit(e);
            }
            if (done) done(ok);
        });
}

void Session::discardLogin(const std::string& id) {
    if (!accounts_.discard(id)) pendingDiscards_.push_back(id);   // WebView2 may hold it a moment longer
}

void Session::maybeRefresh() {
    // Login folders that did not become accounts: retried while WebView2's browser process lets go of them.
    std::erase_if(pendingDiscards_, [this](const std::string& id) { return accounts_.discard(id); });
    if (spDc_.empty() || refreshing_) return;
    if (state_ != SessionState::LoggedIn && state_ != SessionState::Connecting) return;
    if (!token_.expired(nowMs())) return;
    refreshing_ = true;
    const std::string cookie = spDc_;
    async(
        Priority::Low, life_.ref(), [cookie] { return fetchAccessToken(cookie); },
        [this, cookie](Result<TokenBundle> r) {
            refreshing_ = false;
            if (r) {
                token_ = *r;
                api_.setCredentials(token_.accessToken, cookie);
            } else {
                ST_LOG_WARN("spotify", "token refresh failed: {}", r.errorMessage());
            }
        });
}

} // namespace st::spotify
