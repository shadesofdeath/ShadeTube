#pragma once
// Top-level window hosting a widget tree, rendered with Direct2D.
//
// - Custom chrome: the client area covers the whole window (WM_NCCALCSIZE) while keeping the DWM shadow,
//   rounded corners, resize borders, Aero Snap and Windows 11 snap layouts (HTMAXBUTTON).
// - Rendering is on demand: invalidate() marks the window dirty; the App loop renders dirty windows
//   (and keeps rendering while animations run).
// - Overlays (menus, dialogs, toasts) live in a separate layer above the root, with optional modality.
#include "gfx/Device.h"
#include "ui/Anim.h"
#include "ui/Widget.h"

#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

namespace st::ui {

struct WindowOptions {
    std::wstring title = L"ShadeTube";
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, width = 1440, height = 900;   // DIPs
    int minWidth = 960, minHeight = 600;
    bool customChrome = true;
    bool resizable = true;
    bool topmost = false;
    bool toolWindow = false;     // no taskbar button and no minimize box (mini player)
    bool maximized = false;
};

class Overlay;

class Window {
public:
    explicit Window(const WindowOptions& options);
    virtual ~Window();

    HWND hwnd() const { return hwnd_; }
    void show(int cmd = SW_SHOW);
    void close();
    // Re-applies the theme-dependent native frame after a live gfx::Theme::load(): DWMWA_USE_IMMERSIVE_DARK_MODE and
    // the bg.base border/caption color, then relayout + repaint.
    void applyTheme();

    void setRoot(std::unique_ptr<Widget> root);
    Widget* root() const { return root_.get(); }

    // Overlays are painted above the root, topmost last. A modal overlay blocks input to everything below.
    Widget* pushOverlay(std::unique_ptr<Widget> overlay, bool modal);
    void removeOverlay(Widget* overlay);
    bool hasModal() const;

    void invalidate();
    void invalidateAfter(double ms);   // schedule a repaint (e.g. player position ticks)
    void scheduleLayout();
    // Hidden (tray / mini player) or minimized windows never need a frame and never schedule a wake-up, so a
    // window nobody can see costs 0% CPU. WM_PAINT / WM_SIZE on show/restore repaint it.
    bool needsFrame() const;           // dirty, animating or a scheduled repaint is due
    double nextWakeMs() const;         // absolute time of the next scheduled repaint (or +inf)
    bool isShown() const;              // visible and not minimized
    void render();
    // Renders the current UI into a PNG (dev screenshots / docs). Returns false on failure.
    bool renderToPng(const std::wstring& path);

    float scale() const { return target_ ? target_->scale() : 1.f; }
    Rect clientRect() const { return {0, 0, widthDip_, heightDip_}; }
    bool isMaximized() const;
    bool isMinimized() const;
    bool isActive() const { return active_; }
    void minimize();
    void toggleMaximize();
    HANDLE frameWaitable() const { return target_ ? target_->frameLatencyWaitable() : nullptr; }

    // Focus
    //
    // Keyboard navigation: Tab / Shift+Tab move through the focusable, visible, enabled, non-empty widgets in tree
    // order (depth first, parent before children), wrapping around. While a modal overlay (Menu, Dialog) is open
    // focus is trapped inside it; it is moved into the overlay (first tab stop) when the overlay opens and given
    // back to the previous widget when it closes. A keyboard-focused widget inside a ScrollView is scrolled into
    // view. Tab always belongs to the Window (text fields never swallow it).
    //
    // "Focus visible": the Window draws a 2 px accent ring (2 px offset, the widget's focusShape(), clipped by
    // its clipping ancestors) in a pass above the tree and the overlays, only after keyboard interaction (Tab or
    // a navigation key the focused widget handled). The next mouse click hides it. Clicking a focusable widget
    // still focuses it (Tab then continues from there) but never shows the ring.
    Widget* focusedWidget() const { return focus_; }
    // Programmatic / mouse: keeps the current focus-visible state. While a modal overlay is open, a widget outside
    // it (e.g. a page behind a Dialog focusing its search box) is not focused now but when the overlay closes.
    void setFocus(Widget* w);
    bool focusVisible() const { return focusVisible_; }   // keyboard modality: focus indicators are shown
    // Moves focus to the next/previous tab stop of the current scope (as Tab / Shift+Tab). False if none.
    bool moveFocus(bool forward);
    // Focuses `w` as keyboard navigation does: ring visible, scrolled into view.
    void focusByKeyboard(Widget* w);

    // Called by widgets being destroyed so we never keep dangling hover/focus/capture pointers.
    void forget(Widget* w);
    // Widget::setVisible(false): focus inside `w` is dropped (a hidden control must not take Enter / Space or typed
    // characters); Tab continues from where it was.
    void widgetHidden(Widget* w);
    void deferDelete(std::unique_ptr<Widget> w);

    // Unhandled key presses (global shortcuts) and lifecycle hooks.
    std::function<bool(const KeyEvent&)> onKey;
    std::function<void()> onCloseRequested;   // default: DestroyWindow
    std::function<void(bool minimized)> onMinimizeChanged;
    std::function<void()> onDestroyed;
    std::function<void(MouseButton)> onNavButton;   // mouse back/forward buttons
    std::function<void(UINT, WPARAM, LPARAM)> onMessage;   // app-specific messages (tray, SMTC...)

    // Tooltip support (Window draws it in the overlay layer).
    void showTooltipFor(Widget* w);

    static Window* fromHwnd(HWND hwnd);
    LRESULT handleMessageThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

protected:
    virtual LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT hitTestNc(POINT screen);
    Widget* hitTest(Point p);
    void updateHover(Widget* hit);
    void onMouseMessage(UINT msg, WPARAM wp, LPARAM lp);
    Point toDip(LPARAM lp) const;
    MouseEvent makeEvent(Widget* target, Point windowPos, MouseButton b, WPARAM wp) const;
    void resized();
    void runLayout();
    void paintTooltip(Canvas& c);
    void paintFocusRing(Canvas& c);
    Widget* focusScope() const;                    // topmost modal overlay, else the root
    bool inScope(const Widget* w) const;           // attached under focusScope()
    bool dispatchKey(const KeyEvent& e);
    void restartRing();
    void applyPendingOverlayFocus();

    HWND hwnd_ = nullptr;
    WindowOptions options_;
    std::unique_ptr<gfx::WindowTarget> target_;
    std::unique_ptr<Widget> root_;
    struct OverlayEntry {
        std::unique_ptr<Widget> widget;
        bool modal;
    };
    std::vector<OverlayEntry> overlays_;
    std::vector<std::unique_ptr<Widget>> graveyard_;

    float widthDip_ = 0, heightDip_ = 0;
    bool dirty_ = true;
    bool layoutDirty_ = true;
    bool animating_ = false;
    bool active_ = true;
    bool trackingLeave_ = false;
    bool maxButtonHot_ = false;
    double wakeAt_ = 1e300;

    Widget* hover_ = nullptr;
    Widget* capture_ = nullptr;
    Widget* focus_ = nullptr;

    // Keyboard focus state
    bool focusVisible_ = false;
    Widget* navStart_ = nullptr;                   // where Tab starts when nothing is focused (last click, or the
                                                   // surviving parent of a destroyed focused widget)
    struct SavedFocus {
        Widget* overlay;
        Widget* previous;                          // focus to give back when the overlay closes (nulled if it dies)
    };
    std::vector<SavedFocus> savedFocus_;
    Widget* pendingOverlayFocus_ = nullptr;        // modal overlay whose first tab stop gets focus after layout
    UINT consumedVk_ = 0;                          // Enter / Space whose press was handled: its auto-repeat is too
    Anim ringAlpha_;
    // Pages rebuild their content wholesale (Library / Search tabs, Settings). When a keyboard-focused widget dies
    // that way, focus moves to the same-type tab stop that the rebuild puts at the same place (content coordinates
    // of the surviving parent), within a short window. See applyFocusRestore().
    struct LostFocus {
        Widget* parent = nullptr;                  // surviving parent (nulled if it dies too)
        const std::type_info* type = nullptr;
        Rect rect{};                               // focusRect in parent's content space
        double until = 0;
    } lost_;
    void applyFocusRestore();

    // Tooltip state
    Widget* tipWidget_ = nullptr;
    double tipHoverStart_ = 0;
    std::wstring tipText_;
    Point tipAnchor_{};
};

} // namespace st::ui
