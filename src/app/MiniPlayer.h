#pragma once
// Mini player: a compact tool window (no taskbar button, always on top unless turned off) that follows the player live,
// in three modes (app/MiniLayout, docs/DEVELOPMENT.md "Mini player modes"):
//   Strip  - a thin bar: cover 40, title / artist, prev · play · next, a progress line on the bottom edge.
//   Square - the cover alone; on hover the controls, title and the wave seek bar fade in over a scrim; resizable from
//            its corners and edges while staying square.
//   Wide   - cover 128, title / artist, shuffle · prev · play · next · repeat, heart, the current synced lyrics line
//            and the next one dimmed (app/LyricsService; graceful without synced lyrics), the wave seek bar.
// A mode switch (the mode button on hover, the context menu, Ctrl+1 / 2 / 3) animates the window to the new size with
// its anchored corner / edge in place. Dragging anywhere moves it (snapping to the work area's edges and corners with a
// small margin), it never ends up off screen, a double click returns to the main window, the mouse wheel sets the
// volume. Mode, always-on-top and each mode's last position / size are saved in Settings (miniMode, miniOnTop,
// miniPlaces). It follows the theme, the dynamic accent and "Arayüz ölçeği". Owned by App (UI thread).
#include "app/MiniLayout.h"
#include "core/Async.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace st::ui {
class Window;
}

namespace st::app {

class MiniView;
class MiniWindow;

class MiniPlayer {
public:
    // `anchor` (the main window) picks the monitor for the default bottom-right position.
    explicit MiniPlayer(HWND anchor);
    ~MiniPlayer();
    MiniPlayer(const MiniPlayer&) = delete;
    MiniPlayer& operator=(const MiniPlayer&) = delete;

    ui::Window* window() const;
    HWND hwnd() const;
    void show();                  // shows, brings to front and activates
    void hide();
    void sync();                  // player state / current track changed
    void savePosition();          // -> Settings::miniPlaces[mode] (+ miniX / miniY), only when changed
    void applyTheme();            // live theme switch: DWM frame + hairline border color, repaint
    // Compact toast inside the mini window (the main window's toasts are invisible while it is hidden).
    // Returns false when the mini player is not on screen (hidden / minimized).
    bool showToast(const std::wstring& message, bool error);

    mini::Mode mode() const { return mode_; }
    // Switches the mode: the window animates to the new size (instant with reduce motion or while hidden), keeping the
    // anchored corner / edge in place; going back to the mode it just came from, unmoved, returns to exactly where that
    // mode was. Saved.
    void setMode(mini::Mode m);
    bool onTop() const { return onTop_; }
    void setOnTop(bool on);       // Settings::miniOnTop (saved)
    // The context menu (right click, the menu key, Shift+F10): the modes, always on top, back to the main window.
    // A native popup menu like the tray's: it may extend past the small window. Posted (runs outside the message).
    void openMenu(POINT screen);

    std::function<void()> onExpand;                       // "Büyüt" / double click: back to the full window
    std::function<void()> onClose;                        // close button / Alt+F4
    std::function<void(UINT, WPARAM, LPARAM)> onMessage;  // forwarded window messages (single-instance activation)

private:
    friend class MiniWindow;
    friend class MiniView;

    // Window messages before ui::Window sees them (true = handled, `result` is returned).
    bool filterMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    float scale() const;          // DIP -> pixel of the window (monitor DPI x "Arayüz ölçeği")
    RECT windowRect() const;      // the settled rect (the animation's target while it runs)
    RECT workAreaOf(const RECT& r) const;
    POINT sizePx(mini::Mode m) const;
    // Moves the window to `target` (screen px), animated over `ms` when it is on screen and motion is allowed.
    void moveTo(const RECT& target, double ms);
    void stepMorph();
    void finishMorph();
    void refit();                 // back inside the work area (display / work area / DPI change, end of a drag)
    void applyZoom();             // "Arayüz ölçeği" changed while it was closed / hidden: same DIP size at the new scale
    void setPointerInside(bool inside);
    void checkPointerSoon();      // after a leave message: is the pointer still over the window?
    void beginResize(int edge);
    void resizeBy(POINT delta);
    void endResize();
    void finishResizeIfAny();
    void runMenu(POINT screen);
    POINT menuPointForKeyboard() const;   // the window's center (menu key, Shift+F10)
    void scheduleDemo();          // dev: SHADETUBE_MINI_DEMO
    void runDemoStep();

    std::unique_ptr<MiniWindow> window_;
    MiniView* view_ = nullptr;
    mini::Mode mode_ = mini::Mode::Wide;
    float squareSide_ = mini::kSquareDefault;   // DIPs
    bool onTop_ = true;
    // Mode switch animation (WM_TIMER steps; each SetWindowPos re-renders through WM_SIZE).
    bool morphing_ = false;
    RECT morphFrom_{}, morphTo_{};
    double morphStart_ = 0, morphMs_ = 0;
    // A round trip without moving the window returns to exactly where the other mode was (this run only).
    std::map<mini::Mode, RECT> leftAt_;
    RECT enteredAt_{};
    bool moving_ = false;         // inside the system move loop (dragging)
    bool ncTracking_ = false;     // TrackMouseEvent for the non-client (drag region) leave
    bool pointerInside_ = false;
    bool demoHover_ = false;      // dev: the pointer counts as inside (screenshots of the hover look)
    bool menuOpen_ = false;
    double menuClosedAt_ = 0;
    // Square resizing from a grip.
    int resizeEdge_ = 0;
    RECT resizeStart_{};
    // Dev demo steps (SHADETUBE_MINI_DEMO).
    struct DemoStep {
        double at;
        std::string action, arg;
    };
    std::vector<DemoStep> demo_;
    double createdAt_ = 0;
    Lifetime life_;               // posted closures check it
};

} // namespace st::app
