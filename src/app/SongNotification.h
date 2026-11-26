#pragma once
// Song-change notification (Ayarlar › Pencere ve sistem › "Şarkı değişince bildirim göster", off by default): when a
// song starts and no ShadeTube window is in view, a Windows notification shows its cover, title and artist with
// "Sonraki" and "Beğen" buttons. It replaces the previous one (one entry in the notification center, gone when the
// song would have ended) and is silent.
//
// This header is the pure part (standalone: tests compile SongNotification.cpp directly): when to notify and the toast
// XML. The app part (app/SongNotifier.cpp) watches the player, finds out whether a window is in view, writes the cover
// PNG and shows the toast through WinRT (the tray balloon when WinRT toasts are unavailable: no buttons, no cover).
//
// When not:
//   - a ShadeTube window is in view: the foreground window is ours, or one of ours (the main window, the mini player,
//     the video window) is shown, not minimized, on the current virtual desktop and not covered by the foreground app
//     (90 % of it under that window). The mini player on screen already shows the song (it also gets the app's toasts);
//   - the change came from Windows' media controls (media keys, the flyout, the lock screen, the taskbar thumbnail
//     buttons) within kMediaControlMs: Windows shows the new song in its own media overlay. Skips through ShadeTube's
//     global hotkeys do notify (nothing else shows the song then), after the debounce below;
//   - listening together as a guest while the host's changes come in a burst (kStormChanges within kStormWindowMs:
//     joining, catching up, the host skipping around);
//   - Windows says the user is busy (SHQueryUserNotificationState: a full-screen game / app, presentation mode, quiet
//     hours, a locked screen); Focus assist / Do not disturb itself is left to Windows, which holds the toast back;
//   - the same song again (repeat one, a re-resolve, "Yanlış eşleşme?"), a radio station or a podcast episode, a song
//     that is paused by then (the sleep timer stops at the end of a track).
// A change is looked at kDebounceMs after it happened and only if the song is still the current one, so skipping
// through several songs gives one notification for the one that stays.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace st::app::songnotify {

inline constexpr int kDebounceMs = 1500;
inline constexpr double kMediaControlMs = 3000;
inline constexpr double kStormWindowMs = 10000;
inline constexpr int kStormChanges = 2;

struct Input {
    bool enabled = false;               // the setting
    bool song = true;                   // a song (not a radio station or a podcast episode)
    bool playing = true;                // playing or on its way (not paused: e.g. the sleep timer's end of track)
    bool repeat = false;                // the same song as the change looked at before (repeat one, a re-resolve)
    bool inView = false;                // a ShadeTube window is in view (see above)
    double sinceMediaControlMs = 1e12;  // since a command from Windows' media controls
    bool guest = false;                 // a listen-together guest
    int recentChanges = 0;              // track changes in the last kStormWindowMs, this one included
    bool busy = false;                  // SHQueryUserNotificationState says not now
};

enum class Verdict { Show, Off, NotASong, Paused, Repeat, InView, WindowsShows, SyncStorm, Busy };
Verdict decide(const Input& in);
const char* name(Verdict v);   // for the log

// The toast: the cover as the app logo override (square), title, artist line, album as attribution; "Sonraki" always,
// "Beğen" when the song can be liked and isn't. Silent. Buttons carry the arguments "next" / "like"; the body "open".
struct Content {
    std::wstring title, artist, album;
    std::filesystem::path image;   // a PNG on disk ("" = none)
    bool likeButton = false;
    std::wstring nextLabel, likeLabel;   // translated
};
std::wstring toastXml(const Content& c);
std::wstring xmlEscape(std::wstring_view s);
// file:///C:/... with the characters a URI can't hold percent-encoded (spaces, #, %, non-ASCII stays as is).
std::wstring fileUri(const std::filesystem::path& p);

// ---- App part (app/SongNotifier.cpp; started by initSongNotifications) ---------------------------------------------
// App: a command from Windows' media controls (SMTC buttons, media keys, the taskbar thumbnail buttons) just ran.
void noteMediaControl();

} // namespace st::app::songnotify
