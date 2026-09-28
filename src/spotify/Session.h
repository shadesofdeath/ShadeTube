#pragma once
// The user's Spotify session: owns the persisted sp_dc cookie (DPAPI-encrypted), the current web-player
// access token (auto-refreshed), the profile, and the Api. The WebView login only needs to hand us the
// sp_dc cookie via loginWithCookie(); everything else is here. UI-thread facing; network on workers.
#include "core/Async.h"
#include "spotify/Auth.h"
#include "spotify/SpotifyApi.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace st::spotify {

enum class SessionState { LoggedOut, Connecting, LoggedIn };

// The user's Spotify library, fetched once after login and refreshed on demand. The sidebar, Home and
// Library pages read it directly (no per-navigation refetch). "Liked Songs" is the pseudo-playlist that
// libraryV3 returns first among the playlists.
struct LibrarySnapshot {
    std::vector<catalog::Playlist> playlists;
    std::vector<catalog::Album> albums;
    std::vector<catalog::Artist> artists;
    std::unordered_set<std::string> likedIds;   // Liked Songs track ids (for heart state)
    bool loaded = false;   // true once the first fetch completes (even if some lists are empty)
};

// A successful write to one of the user's Spotify playlists, broadcast (UI thread) so open pages can follow it.
struct PlaylistEdit {
    enum class Kind { Created, Added, Removed, Renamed, Deleted };
    Kind kind = Kind::Added;
    std::string uri;
    std::string name;                        // Created / Renamed: the (new) name
    std::vector<std::string> removedUids;    // Removed: the playlist rows that are gone
    int count = 0;                           // Created / Added / Removed: rows added or removed
};

class Session {
public:
    Session();

    // Load a previously saved sp_dc (if any) and start minting a token in the background.
    void restore();

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

    // Called by the WebView login once it captures the cookie. Fetches a token + profile, then LoggedIn.
    void loginWithCookie(std::string spDc);
    void logout();

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
    // Rows by Track::uid (from Api::playlistTracks).
    void removeFromPlaylist(const std::string& uri, std::vector<std::string> uids, std::function<void(bool)> done = {});
    void renamePlaylist(const std::string& uri, std::string name, std::function<void(bool)> done = {});
    // Owned: Spotify's "delete"; someone else's: unfollow. Either way it leaves the library.
    void deletePlaylist(const std::string& uri, std::function<void(bool)> done = {});
    void subscribePlaylistEdits(Lifetime::Ref owner, std::function<void(const PlaylistEdit&)> fn) {
        editListeners_.emplace_back(std::move(owner), std::move(fn));
    }

    // Called from the app's housekeeping tick (UI thread): refreshes the token shortly before it expires.
    void maybeRefresh();

    // UI observers; die with their Lifetime. Fired on the UI thread when state/profile changes.
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }

private:
    void notify();
    void beginTokenFetch(Priority priority, bool alsoProfile);
    void applyEdit(const PlaylistEdit& e);   // snapshot update + notifications

    Api api_;
    UserProfile profile_;
    LibrarySnapshot library_;
    std::string spDc_;
    TokenBundle token_;
    SessionState state_ = SessionState::LoggedOut;
    bool refreshing_ = false;
    int editSeq_ = 0;   // bumped by every applied edit: a library load that overlapped one re-fetches the playlists
    Lifetime life_;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    std::vector<std::pair<Lifetime::Ref, std::function<void(const PlaylistEdit&)>>> editListeners_;
};

// sp_dc persistence (DPAPI, %LOCALAPPDATA%\ShadeTube\spotify.dat). Empty string on miss/failure.
namespace store {
void saveCookie(const std::string& spDc);
std::string loadCookie();
void clearCookie();
} // namespace store

} // namespace st::spotify
