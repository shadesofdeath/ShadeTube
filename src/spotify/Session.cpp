#include "spotify/Session.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "spotify/Auth.h"

#include <windows.h>
// <windows.h> must precede <wincrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace st::spotify {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::filesystem::path cookieFile() { return paths::appData() / L"spotify.dat"; }
} // namespace

namespace store {

void saveCookie(const std::string& spDc) {
    DATA_BLOB in{static_cast<DWORD>(spDc.size()), reinterpret_cast<BYTE*>(const_cast<char*>(spDc.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ShadeTube sp_dc", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN("spotify", "CryptProtectData failed: {}", GetLastError());
        return;
    }
    std::ofstream f(cookieFile(), std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
}

std::string loadCookie() {
    std::ifstream f(cookieFile(), std::ios::binary);
    if (!f) return {};
    std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (enc.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(enc.size()), reinterpret_cast<BYTE*>(enc.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN("spotify", "CryptUnprotectData failed: {}", GetLastError());
        return {};
    }
    std::string spDc(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return spDc;
}

void clearCookie() {
    std::error_code ec;
    std::filesystem::remove(cookieFile(), ec);
}

} // namespace store

Session::Session() = default;

void Session::notify() {
    // A listener may (re)subscribe while we notify — logging in rebuilds Home, whose ctor calls subscribe(),
    // which push_backs into listeners_. Iterate over a copy so that never invalidates our loop.
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    auto snapshot = listeners_;
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

void Session::restore() {
    spDc_ = store::loadCookie();
    if (spDc_.empty()) return;
    state_ = SessionState::Connecting;
    notify();
    beginTokenFetch(Priority::High, /*alsoProfile=*/true);
}

void Session::loginWithCookie(std::string spDc) {
    if (spDc.empty()) return;
    spDc_ = std::move(spDc);
    store::saveCookie(spDc_);
    state_ = SessionState::Connecting;
    notify();
    beginTokenFetch(Priority::High, /*alsoProfile=*/true);
}

void Session::beginTokenFetch(Priority priority, bool alsoProfile) {
    const std::string cookie = spDc_;
    async(
        priority, life_.ref(), [cookie] { return fetchAccessToken(cookie); },
        [this, cookie, alsoProfile](Result<TokenBundle> r) {
            if (!r) {
                ST_LOG_WARN("spotify", "token fetch failed: {}", r.errorMessage());
                if (state_ == SessionState::Connecting) {
                    state_ = SessionState::LoggedOut;
                    notify();
                }
                return;
            }
            token_ = *r;
            api_.setCredentials(token_.accessToken, cookie);
            if (!alsoProfile) {
                if (state_ != SessionState::LoggedIn) {
                    state_ = SessionState::LoggedIn;
                    notify();
                }
                return;
            }
            async(
                Priority::High, life_.ref(), [this] { return api_.me(); },
                [this](Result<UserProfile> pr) {
                    if (pr && pr->valid()) profile_ = *pr;
                    state_ = SessionState::LoggedIn;
                    notify();
                    reloadLibrary();
                });
        });
}

bool Session::isSavedAlbum(const std::string& id) const {
    return std::any_of(library_.albums.begin(), library_.albums.end(), [&](const catalog::Album& a) { return a.id == id; });
}
bool Session::isFollowing(const std::string& id) const {
    return std::any_of(library_.artists.begin(), library_.artists.end(), [&](const catalog::Artist& a) { return a.id == id; });
}
void Session::markAlbumSaved(const catalog::Album& a, bool saved) {
    if (saved) {
        if (!isSavedAlbum(a.id)) library_.albums.insert(library_.albums.begin(), a);
    } else {
        std::erase_if(library_.albums, [&](const catalog::Album& x) { return x.id == a.id; });
    }
}
void Session::markArtistFollowed(const catalog::Artist& a, bool follow) {
    if (follow) {
        if (!isFollowing(a.id)) library_.artists.insert(library_.artists.begin(), a);
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
            tryFetch([&] { s.playlists = api_.libraryPlaylists({}, &s.tree); });
            tryFetch([&] { s.albums = api_.libraryAlbums(); });
            tryFetch([&] { s.artists = api_.libraryArtists(); });
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
            std::pair<std::vector<catalog::Playlist>, PlaylistTree> out;
            out.first = api_.libraryPlaylists({}, &out.second);
            return out;
        },
        [this](Result<std::pair<std::vector<catalog::Playlist>, PlaylistTree>> r) {
            if (!r) {
                ST_LOG_WARN("spotify", "playlists reload failed: {}", r.errorMessage());
                return;
            }
            library_.playlists = std::move(r->first);
            library_.tree = std::move(r->second);
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

void Session::maybeRefresh() {
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

void Session::logout() {
    life_.renew(); // drop any in-flight continuations
    store::clearCookie();
    spDc_.clear();
    token_ = {};
    profile_ = {};
    library_ = {};
    api_.setCredentials("", "");
    api_.setUsername({});
    state_ = SessionState::LoggedOut;
    notify();
}

} // namespace st::spotify
