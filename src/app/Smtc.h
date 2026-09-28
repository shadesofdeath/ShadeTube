#pragma once
// Smtc - Windows System Media Transport Controls for the main window: the Windows media flyout
// (volume / quick settings), the lock screen card and the hardware media key overlay.
//
// Standalone module: depends only on catalog/Models.h, core/* and the Windows SDK. The C++/WinRT types live
// behind a pimpl, so including this header pulls in no WinRT headers.
//
// Threading: create, call and destroy on the UI thread (an STA; COM must already be initialized there).
// Button presses arrive from Windows on a background thread and are marshalled with st::Dispatcher::post,
// so onButton / onSeek always run on the UI thread, and never after this object is destroyed.
// Every method is a no-op until init() succeeded, and none of them throws.
#include "catalog/Models.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace st::app {

class Smtc {
public:
    enum class Button { Play, Pause, Next, Previous, Stop };

    Smtc();
    ~Smtc();
    Smtc(const Smtc&) = delete;
    Smtc& operator=(const Smtc&) = delete;

    // Binds the controls to `hwnd` (a top-level window; it may be hidden). Returns false (and logs why) when
    // SMTC is unavailable; the object then stays inert. Calling it again after success is a no-op.
    bool init(HWND hwnd);
    bool ready() const;

    // Title / artists / album and the album art (an http(s) cover, ~500 px, fetched lazily by Windows).
    // Re-sending the same track is cheap (ignored), so this may be called from any change notification.
    void setTrack(const catalog::Track& t);
    void setPlaying(bool playing);
    // Position and length in ms; call about once per second while playing and after seeks. Unchanged values
    // are dropped. durationMs <= 0 hides the timeline.
    void setTimeline(int64_t positionMs, int64_t durationMs);
    void setNavigation(bool canPrevious, bool canNext);
    // Nothing to play: clears the metadata and hides the session from the flyout (setTrack re-shows it).
    void clear();

    std::function<void(Button)> onButton;          // UI thread
    std::function<void(int64_t positionMs)> onSeek;  // UI thread; the flyout's seek bar (Windows 11)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace st::app
