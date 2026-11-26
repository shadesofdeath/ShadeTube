#pragma once
// The user's Spotify session: the accounts saved on this PC (spotify/Accounts: one DPAPI-encrypted sp_dc per account),
// the active one's web-player access token (auto-refreshed), its profile and library, and the Api. The WebView login
// only hands over the sp_dc cookie (beginLogin / finishLogin); everything else is here. UI-thread facing; network on
// workers.
#include "core/Async.h"
#include "spotify/Accounts.h"
#include "spotify/Auth.h"
#include "spotify/SpotifyApi.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st::spotify {

enum class SessionState { LoggedOut, Connecting, LoggedIn };

// The user's Spotify library, fetched once after login and refreshed on demand. The sidebar, Home and
// Library pages read it directly (no per-navigation refetch). "Liked Songs" is the pseudo-playlist that
// libraryV3 returns first among the playlists.
struct LibrarySnapshot {
    std::vector<catalog::Playlist> playlists;   // flat, in libraryV3 order (folders flattened away)
    PlaylistTree tree;                          // the user's folders and which playlists they hold (rootlist)
    std::vector<catalog::Album> albums;
    std::vector<catalog::Artist> artists;
    std::unordered_set<std::string> likedIds;   // Liked Songs track ids (for heart state)
    LibraryEntries entries;   // libraryV3's added / played times and pins by URI (folders: "folder:<id>"); may be empty
    bool loaded = false;   // true once the first fetch completes (even if some lists are empty)
};

// A successful write to one of the user's Spotify playlists, broadcast (UI thread) so open pages can follow it.
struct PlaylistEdit {
    enum class Kind { Created, Added, Removed, Moved, Renamed, Details, Deleted };
    Kind kind = Kind::Added;
    std::string uri;
    std::string name;                        // Created / Renamed: the (new) name; Details: the new name or ""
    std::vector<std::string> removedUids;    // Removed: the playlist rows that are gone
    int count = 0;                           // Created / Added / Removed / Moved: rows added, removed or moved
    std::optional<std::string> description;  // Details: the new description ("" = cleared), unset = unchanged
    bool coverChanged = false;               // Details: a new cover or the cover removed (its URL comes with a reload)
};

class Session {
public:
    // `dataRoot`: the profile folder (default paths::appData()).
    explicit Session(std::filesystem::path dataRoot = {});

    // Reads the saved accounts (a single-account profile from before 0.11 becomes the first account). Call before
    // anything reads dataDir(). `perAccountFiles`: the app's files that belong to an account (see AccountStore).
    void loadAccounts(std::vector<std::wstring> perAccountFiles);
    // Restores the active account's saved sp_dc (if any) and starts minting a token in the background.
    void restore();

    // ---- Accounts ---------------------------------------------------------------------------------------------------
    const AccountStore& accounts() const { return accounts_; }
    const std::string& activeAccountId() const { return accounts_.activeId(); }
    // The active account's folder (its listening history, blocklist...); the profile root while there is no account.
    std::filesystem::path dataDir() const { return accounts_.dataDir(); }
    std::vector<Account> signedInAccounts() const { return accounts_.signedIn(); }
    // A quick switch is connecting to this account ("" = none). The current account stays on meanwhile.
    const std::string& switchingTo() const { return switchingTo_; }

    // Login (the WebView2 window): a fresh folder whose WebView2 profile holds this login's cookies, so the accounts
    // already here stay signed in and are not picked up again. finishLogin() with the captured sp_dc ("" = the window
    // was closed): the account it belongs to becomes active, a new one or a known one signed in again (same Spotify
    // user). `done(ok)` on the UI thread.
    struct LoginSlot {
        std::string id;
        std::filesystem::path webViewDir;
    };
    LoginSlot beginLogin();
    void finishLogin(const LoginSlot& slot, std::string spDc, std::function<void(bool ok)> done = {});
    // Quick switch to another signed-in account, no login: its token and profile are fetched first, then the session,
    // the library snapshot and the data folder swap in one step (onAccountLeaving / onAccountEntered around it).
    void switchTo(const std::string& accountId, std::function<void(bool ok)> done = {});
    // Signs an account out (its cookie and login profile are deleted; its folder stays for a later sign in). For the
    // active account: another signed-in account takes over when there is one, else the session is logged out.
    void signOut(const std::string& accountId);
    // The active account's whole Spotify state, as signOut(active).
    void logout();

    // Fired on the UI thread right before the active account changes (the old one is still active: save what belongs to
    // it) and right after (load the new one's). Only when dataDir() really changes.
    std::function<void()> onAccountLeaving;
    std::function<void()> onAccountEntered;

    SessionState state() const { return state_; }
    bool loggedIn() const { return state_ == SessionState::LoggedIn; }
    const UserProfile& profile() const { return profile_; }
    const LibrarySnapshot& library() const { return library_; }
    Api& api() { return api_; }

    // Heart / saved / following state + optimistic local update (the write to Spotify is fired separately).
    bool isLiked(const std::string& trackId) const { return library_.likedIds.count(trackId) > 0; }
    void markLiked(const std::string& trackId, bool liked) {
        if (liked) library_.likedIds.insert(trackId);
        else library_.likedIds.erase(trackId);
    }
    bool isSavedAlbum(const std::string& id) const;
    bool isFollowing(const std::string& id) const;
    void markAlbumSaved(const catalog::Album& a, bool saved);
    void markArtistFollowed(const catalog::Artist& a, bool follow);

    // (Re)fetch playlists / saved albums / followed artists in the background; notifies when each arrives.
    void reloadLibrary();
    // Playlists only (libraryV3 + rootlist counts) — much cheaper than reloadLibrary() (no Liked Songs paging).
    void reloadPlaylists();

    // ---- Playlist editing (writes to the user's Spotify account) -------------------------------------------
    // Each call writes on a worker thread. On success the snapshot is updated in place (exact, no refetch: the
    // library views update at once and can't flicker back to stale server data), session listeners and
    // playlist-edit listeners are notified, then `done` runs. On failure (logged) only `done(false / "")` runs.
    // Everything runs on the UI thread; nothing runs after logout() or once the Session is gone.
    const catalog::Playlist* findPlaylist(const std::string& uri) const;
    // Playlists the user can add songs to (owned or collaborative), in library order. Excludes Liked Songs.
    std::vector<catalog::Playlist> editablePlaylists() const;
    // `done(uri, added)`: uri = the new playlist ("" on failure), added = how many of `trackUris` were added.
    void createPlaylist(std::string name, std::vector<std::string> trackUris,
                        std::function<void(const std::string& uri, int added)> done = {});
    void addToPlaylist(const std::string& uri, std::vector<std::string> trackUris, std::function<void(bool)> done = {});
    // The same at a position (an undo puts removed songs back where they were).
    void insertIntoPlaylist(const std::string& uri, std::vector<std::string> trackUris, PlaylistPosition at,
                            std::function<void(bool)> done = {});
    // Rows by Track::uid (from Api::playlistTracks).
    void removeFromPlaylist(const std::string& uri, std::vector<std::string> uids, std::function<void(bool)> done = {});
    // Rows (uids, in their new order) moved together to `to`. Pages reorder their own rows before the write.
    void moveInPlaylist(const std::string& uri, std::vector<std::string> uids, PlaylistPosition to,
                        std::function<void(bool)> done = {});
    void renamePlaylist(const std::string& uri, std::string name, std::function<void(bool)> done = {});
    // Name / description / cover (owner only). A new or removed cover also reloads the playlists (its image URLs).
    void updatePlaylistDetails(const std::string& uri, PlaylistDetailsChange change, std::function<void(bool)> done = {});
    // Owned: Spotify's "delete"; someone else's: unfollow. Either way it leaves the library.
    void deletePlaylist(const std::string& uri, std::function<void(bool)> done = {});
    void subscribePlaylistEdits(Lifetime::Ref owner, std::function<void(const PlaylistEdit&)> fn) {
        editListeners_.emplace_back(std::move(owner), std::move(fn));
    }

    // Called from the app's housekeeping tick (UI thread): refreshes the token shortly before it expires.
    void maybeRefresh();

    // UI observers; die with their Lifetime. Fired on the UI thread when state/profile changes.
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }

    // What a connect brings back from a worker: the token and the profile (empty when only the profile failed).
    struct Connected {
        TokenBundle token;
        UserProfile profile;
    };
    // Tests: used instead of connectAccount() (runs on a worker; throw to fail). Unset = the real connect. With it set
    // no library load follows a connect (that would go to Spotify).
    std::function<Connected(const std::string& spDc)> connector;

private:
    std::function<Connected()> connectJob(std::string spDc) const;
    void notify();
    void applyEdit(const PlaylistEdit& e);   // snapshot update + notifications
    // Makes `id` the signed-in active account with these credentials (UI thread). `add`: a new account (from a login)
    // goes into the list first. Fires the account hooks when the data folder changes.
    void commit(const std::string& id, const std::string& cookie, Connected c, bool add);
    void clearAccountState();   // credentials, profile and library of the account that is leaving
    void discardLogin(const std::string& id);

    AccountStore accounts_;
    Api api_;
    UserProfile profile_;
    LibrarySnapshot library_;
    std::string spDc_;
    TokenBundle token_;
    SessionState state_ = SessionState::LoggedOut;
    bool refreshing_ = false;
    int editSeq_ = 0;   // bumped by every applied edit: a library load that overlapped one re-fetches the playlists
    std::string switchingTo_;
    std::vector<std::string> pendingDiscards_;   // login folders WebView2 still held (retried by maybeRefresh)
    // The library snapshots of accounts switched away from this run: switching back shows them at once (a reload
    // follows). Dropped at sign out.
    std::unordered_map<std::string, LibrarySnapshot> parked_;
    Lifetime life_;          // library loads, edits, token refreshes of the active account
    Lifetime connectLife_;   // the connect in flight (a newer switch / login / sign out drops it)
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    std::vector<std::pair<Lifetime::Ref, std::function<void(const PlaylistEdit&)>>> editListeners_;
};

// Worker thread: mints a token for `spDc` and reads its profile (a profile failure leaves it empty). Throws when no
// token can be had.
Session::Connected connectAccount(const std::string& spDc, const CT& ct = {});

// Live tests: the cookie of the active account in %LOCALAPPDATA%\ShadeTube (read only, DPAPI). "" when none.
namespace store {
std::string loadCookie();
} // namespace store

} // namespace st::spotify
