#pragma once
// Spotube-style Spotify login: a WebView2 window shows accounts.spotify.com; when the user signs in, Spotify
// sets the long-lived `sp_dc` cookie on .spotify.com. We watch for it and hand it back — that cookie is all
// Auth needs to mint web-player tokens. No credentials ever pass through ShadeTube.
#include <functional>
#include <string>

namespace st::app {

class LoginWindow {
public:
    // Opens a modal WebView2 window centered over `parentHwnd`, its browser profile in `userDataDir` (a folder of its
    // own per login: Session::beginLogin). When the sp_dc cookie is captured, calls onDone(sp_dc) on the UI thread and
    // closes; if the user closes the window first, calls onDone("").
    // Requires the Evergreen WebView2 Runtime; if it's missing, onDone("") is invoked and a hint is logged.
    // Self-owning: the instance frees itself once finished. Call only from the UI (STA) thread.
    static void open(void* parentHwnd, std::wstring userDataDir, std::function<void(std::string spDc)> onDone);
};

} // namespace st::app
