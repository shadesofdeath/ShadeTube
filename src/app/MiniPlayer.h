#pragma once
// Mini player: a compact, always-on-top tool window (no taskbar button) that follows the player live.
// Spec: ShadeTube-Design/components/spec.md "Mini player" (art 88 radius 2, padding 16, title B 14/600, artist
// B 12, prev 32 · play 40 · next 32 right-aligned, 2 px seek line on the bottom edge, drag = whole surface) plus
// a "Büyüt" button next to the close button. 380x120 DIPs: the spec's 360 wide + room for "Büyüt".
// The window remembers its screen position in Settings::miniX/miniY. Owned by App (UI thread).
#include <functional>
#include <memory>
#include <string>

#include <windows.h>

namespace st::ui {
class Window;
}

namespace st::app {

class MiniView;

class MiniPlayer {
public:
    static constexpr int kWidth = 380, kHeight = 120;   // DIPs

    // `anchor` (the main window) picks the monitor for the default bottom-right position.
    explicit MiniPlayer(HWND anchor);
    ~MiniPlayer();
    MiniPlayer(const MiniPlayer&) = delete;
    MiniPlayer& operator=(const MiniPlayer&) = delete;

    ui::Window* window() const { return window_.get(); }
    HWND hwnd() const;
    void show();                  // shows, brings to front and activates
    void hide();
    void sync();                  // player state / current track changed
    void savePosition();          // -> Settings::miniX/miniY (screen pixels, only when changed)
    void applyTheme();            // live theme switch: DWM frame + hairline border color, repaint
    // Compact toast inside the mini window (the main window's toasts are invisible while it is hidden).
    // Returns false when the mini player is not on screen (hidden / minimized).
    bool showToast(const std::wstring& message, bool error);

    std::function<void()> onExpand;                       // "Büyüt": back to the full window
    std::function<void()> onClose;                        // close button / Alt+F4
    std::function<void(UINT, WPARAM, LPARAM)> onMessage;  // forwarded window messages (single-instance activation)

private:
    std::unique_ptr<ui::Window> window_;
    MiniView* view_ = nullptr;
};

} // namespace st::app
