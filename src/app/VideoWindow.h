#pragma once
// The video window (app/VideoMode): a resizable window of its own with the music video, letterboxed on black. Works on
// its own, also while the main window is minimized or in the tray (picture-in-picture use).
// - Controls over the picture (the song, previous / play-pause / next, always on top, full screen, close) fade out
//   2.5 s after the pointer last moved while the song plays; the pointer hides with them in full screen.
// - Drag the picture to move the window; double click (or F) toggles full screen on the window's monitor, Esc leaves
//   it; Space plays / pauses; the mini player's shortcuts (Ayarlar › Klavye) work here too.
// - Remembers its size, position and "always on top" (Settings::videoX / videoY / videoW / videoH / videoOnTop).
// Owned by video mode (UI thread); rendered by App's loop through AppContext::extraWindows.
#include <functional>
#include <memory>

#include <windows.h>

namespace st::ui {
class Window;
}

namespace st::app {

class VideoWindowView;

class VideoWindow {
public:
    explicit VideoWindow(HWND anchor);
    ~VideoWindow();
    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;

    ui::Window* window() const { return window_.get(); }
    HWND hwnd() const;
    void show();
    void savePlacement();   // -> Settings (normal rect; not while full screen)
    void sync();            // player state / track changed
    void setOnTop(bool on);
    bool onTop() const;
    void toggleFullscreen();
    void setFullscreen(bool on);
    bool fullscreen() const { return fullscreen_; }

    std::function<void()> onClose;   // close button / Alt+F4 / Ctrl+W

private:
    std::unique_ptr<ui::Window> window_;
    VideoWindowView* view_ = nullptr;
    bool fullscreen_ = false;
    WINDOWPLACEMENT saved_{sizeof(WINDOWPLACEMENT)};
    LONG savedStyle_ = 0;
};

} // namespace st::app
