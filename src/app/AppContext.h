#pragma once
// Services shared by the shell and pages (owned by App; accessed on the UI thread via app::ctx()).
#include "app/Downloads.h"
#include "catalog/Models.h"
#include "core/Async.h"
#include "core/Settings.h"
#include "gfx/Types.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "youtube/MatchService.h"

#include <nlohmann/json.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st::ui {
class Window;
}

namespace st::app {

class Router;
class Scrobbler;
class DiscordRpc;

struct HistoryItem {
    catalog::Track track;
    int64_t playedAt = 0;
};

// The user's library, stored on this PC (%LOCALAPPDATA%\ShadeTube\library.json). No account needed.
// UI thread only; saved by App's housekeeping when dirty (write-then-rename).
class Library {
public:
    void load();
    void saveIfDirty();

    // Liked songs (newest first)
    const std::vector<catalog::Track>& liked() const { return liked_; }
    bool isLiked(const std::string& trackId) const;   // Spotify heart when logged in, else local
    // Logged in only Spotify tracks can be hearted (a local file / MusicBrainz id is never sent to Spotify).
    bool canLike(const std::string& trackId) const;
    void setLiked(const catalog::Track& t, bool liked);
    void toggleLiked(const catalog::Track& t) { setLiked(t, !isLiked(t.id)); }
    // Likes several songs at once (drag and drop onto Liked Songs): one write, no toast. Songs that can't be liked
    // or are liked already are passed over. Returns how many were newly liked.
    int likeAll(const std::vector<catalog::Track>& tracks);

    // Saved albums / followed artists (newest first)
    const std::vector<catalog::Album>& albums() const { return albums_; }
    bool isSavedAlbum(const std::string& id) const;
    void setSavedAlbum(const catalog::Album& a, bool saved);
    const std::vector<catalog::Artist>& artists() const { return artists_; }
    bool isFollowing(const std::string& id) const;
    void setFollow(const catalog::Artist& a, bool follow);

    // Local playlists
    const std::vector<catalog::Playlist>& playlists() const { return playlists_; }
    const catalog::Playlist* playlist(const std::string& id) const;
    const std::vector<catalog::Track>* playlistTracks(const std::string& id) const;
    std::string createPlaylist(const std::wstring& name);          // returns id
    void renamePlaylist(const std::string& id, const std::wstring& name);
    void deletePlaylist(const std::string& id);
    int addToPlaylist(const std::string& id, const std::vector<catalog::Track>& tracks);   // returns #added
    void removeFromPlaylist(const std::string& id, const std::string& trackId);

    // Listening history (newest first, capped)
    const std::deque<HistoryItem>& history() const { return history_; }
    void recordPlay(const catalog::Track& t);
    std::vector<catalog::ArtistRef> topArtists(size_t n) const;   // most played
    std::vector<catalog::AlbumRef> recentAlbums(size_t n) const;  // unique, newest first

    // Change notifications. Subscriptions die with their Lifetime (pages renew/destroy theirs).
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }
    void notify();

private:
    void touch();
    void refreshPlaylistMeta(catalog::Playlist& p);

    std::vector<catalog::Track> liked_;
    std::unordered_set<std::string> likedIds_;
    std::vector<catalog::Album> albums_;
    std::vector<catalog::Artist> artists_;
    std::vector<catalog::Playlist> playlists_;
    std::unordered_map<std::string, std::vector<catalog::Track>> playlistTracks_;
    std::deque<HistoryItem> history_;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    bool dirty_ = false;
};

struct AppContext {
    youtube::MatchService* matcher = nullptr;
    player::Player* player = nullptr;
    ui::Window* window = nullptr;
    Router* router = nullptr;
    spotify::Session* session = nullptr;        // the user's Spotify session (nullptr only during teardown)
    Scrobbler* scrobbler = nullptr;             // Last.fm / ListenBrainz scrobbling (owned by App)
    DiscordRpc* discord = nullptr;              // Discord Rich Presence (owned by App)
    Library library;
    DownloadManager downloads;

    std::function<void(bool)> toggleNowPlaying; // full-screen Now Playing
    std::function<void(bool)> toggleQueue;      // side queue panel
    std::function<void()> openMiniPlayer;
    std::function<void(const catalog::Track&)> showMatchPicker;
    std::function<void()> startSpotifyLogin;    // opens the WebView2 login window
    std::function<void()> logoutSpotify;        // clears the session and returns to the connect screen
    std::function<void(bool)> showConnect;      // toggles the full-window Spotify connect overlay
    std::function<void()> syncDiscord;          // re-apply Settings (enabled / app id) + the playing track
    std::function<void()> applyTheme;           // re-resolve the theme mode (+ Windows app mode) and switch live
    std::function<void()> restartApp;           // quit and start again (UI language change)
    std::function<void()> quitApp;              // "Çıkış" (App::quit): updater / installer relaunch
    std::function<void()> toggleLyricsFullscreen;   // full-screen / karaoke lyrics view (set by the lyrics feature)
    std::function<void(int ms)> lyricsOffsetBy;     // shift the current track's lyrics timing (+ later, - earlier)

    // Theme: `--theme light|dark|system` (dev) wins over Settings::theme until the user picks one in Ayarlar.
    std::optional<ThemeMode> themeOverride;

    // Toast routing while the main window is not on screen (set by App once its windows exist; see toast()).
    std::function<bool(const std::wstring& message, bool error)> miniToast;   // false: mini player not visible
    std::function<void(const std::wstring& title, const std::wstring& text, bool error)> trayBalloon;

    // Sleep timer (UI thread): pause at a steady-clock deadline (0 = off) or when the current track ends.
    int64_t sleepDeadlineMs = 0;
    bool sleepAtTrackEnd = false;

    // Feature hooks. Features register them in their init function (see initFeatures()); App fires them on the UI
    // thread. Never unregistered (features live as long as the app).
    std::vector<std::function<void()>> playerChangedHooks;                     // state / track / queue changed
    std::vector<std::function<void(const catalog::Track&)>> trackChangedHooks; // a new track started
    std::vector<std::function<void()>> housekeepingHooks;                      // every ~2 s (+ periodic saves)
    std::vector<std::function<void()>> persistHooks;                           // save everything (exit)
    // Offline playback: asked after the downloads, first non-empty path wins (e.g. the local music library).
    std::vector<std::function<std::wstring(const std::string& trackId)>> localFileResolvers;
};

AppContext& ctx();

// Feature startup: each defined in its feature's file, called once by App (after the player is wired, before the
// first page / session restore) through initFeatures().
void initListenStats();        // app/StatsPage.cpp      (listening statistics)
void initPlaybackFeatures();   // app/Radio.cpp          (endless playback / radio + kara liste)
void initLocalLibrary();       // app/LocalFilesPage.cpp (Yerel dosyalar)
void initUpdater();            // app/AboutSettings.cpp  (update check)
void initInternetRadio();      // app/RadioPage.cpp      (radio-browser.info stations)
void initWinShell();           // app/WinShell.cpp       (taskbar buttons, app identity for the media flyout)
void initDownloadSync();       // app/DownloadSync.cpp   (collections kept offline)
void initDragDrop();           // app/DragDrop.cpp       (files dropped from Explorer, resolver for dropped files)
void initLyrics();             // app/LyricsService.cpp  (lyrics offset, full-screen lyrics)
void initPodcasts();           // app/PodcastsPage.cpp   (Podcastler: RSS feeds, progress, downloads)
inline void initFeatures() {
    initListenStats();
    initPlaybackFeatures();
    initLocalLibrary();
    initUpdater();
    initInternetRadio();
    initWinShell();
    initDownloadSync();
    initDragDrop();
    initLyrics();
    initPodcasts();
}

// Radio (app/Radio.cpp). Spotify's radio for a seed (spotify:track: / artist: / album: / playlist: URI) replaces the
// queue and starts playing; toasts on failure. Needs a Spotify session (radioAvailable()).
bool radioAvailable();
void startRadio(const std::string& seedUri, const std::wstring& seedName);

// Shared helpers used by many pages.
void showTrackMenu(const std::vector<catalog::Track>& tracks, gfx::Point windowPos, const std::string& playlistId = {});
void showAddToPlaylistMenu(const std::vector<catalog::Track>& tracks, gfx::Point windowPos);
// Adds songs to the playlist `id` and reports with a toast: an editable Spotify playlist (logged in; only its Spotify
// songs, the others are noted) or a local playlist. Used by the menu above and by drag and drop.
void addToPlaylistWithToast(const std::string& id, const std::vector<catalog::Track>& tracks);
void showDownloadFolderMenu(const std::vector<catalog::Track>& tracks, gfx::Point windowPos);

// Sleep timer.
int64_t steadyMs();
void setSleepTimer(int minutes);           // 0 = off
void setSleepAtTrackEnd();
bool sleepTimerActive();
std::wstring sleepTimerLabel();            // "23 dk kaldı" / "Parça bitince" / ""
void showSleepTimerMenu(gfx::Point windowPos);
// Playlists. Logged in, these write to the user's Spotify account (create / rename / delete run on a worker and
// report with a toast); logged out they edit the local playlists. `id` is a local id or a spotify:playlist: URI.
// promptNewPlaylist: asks for a name, creates the playlist, adds `tracks` (Spotify: only Spotify tracks), then
// calls `created(id)` (Spotify: once the playlist exists).
void promptNewPlaylist(std::function<void(const std::string& id)> created = {}, std::vector<catalog::Track> tracks = {});
void promptRenamePlaylist(const std::string& id, const std::wstring& currentName);
// Confirm dialog, then delete (Spotify: removes it from the library — Spotify's "delete" for an owned playlist,
// unfollow otherwise). Leaves the playlist's page (-> Library) if it is open.
void confirmDeletePlaylist(const std::string& id, const std::wstring& name);
bool isEditableSpotifyPlaylist(const std::string& id);   // logged in, in the library, and the user may edit it
std::vector<std::string> spotifyTrackUris(const std::vector<catalog::Track>& tracks);   // spotify:track: ids only
// Toast in the main window. While the main window is hidden/minimized it goes to the visible mini player instead;
// with no app window on screen, errors and `important` messages become a (throttled) tray notification and the
// rest is dropped.
void toast(const std::wstring& message, bool error = false, bool important = false);
// Theme mode in effect (the --theme override, else Settings::theme) and the Ayarlar choice: persists, clears the
// override and applies live.
ThemeMode themeMode();
void setThemeMode(ThemeMode mode);
// Re-applies the theme-dependent native frame of a window (DWM dark title bar, border / caption color) and
// schedules a relayout + repaint. Used on a live theme switch for every window of the app.
void applyWindowTheme(ui::Window* window);
void copyText(const std::wstring& s);
// Formatting in the UI language (core/I18n.h: texts, month names, digit grouping); the examples are Turkish.
std::wstring greeting();                         // "Günaydın,\nne dinliyoruz?" (by time of day; two lines)
std::wstring trDate(int64_t unixSeconds, bool withYear = false);   // "27 EYL"
std::wstring relativeTime(int64_t unixSeconds);  // "3 gün önce"
std::wstring totalDuration(int64_t ms);          // "3 sa 12 dk"
std::wstring thousands(int64_t n);               // "1.234.567"
std::wstring compactCount(int64_t n);            // "12,4 B" / "1,2 Mn"
int64_t nowUnix();

// JSON (de)serialization of catalog models (library.json / session.json).
nlohmann::json toJson(const catalog::Track& t);
nlohmann::json toJson(const catalog::Album& a);
nlohmann::json toJson(const catalog::Artist& a);
catalog::Track trackFromJson(const nlohmann::json& j);
catalog::Album albumFromJson(const nlohmann::json& j);
catalog::Artist artistFromJson(const nlohmann::json& j);

} // namespace st::app
