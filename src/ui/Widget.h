#pragma once
// Retained widget tree.
//
// Coordinates: every widget's `rect()` is expressed in its PARENT's content space. A widget may shift
// its children with contentOffset() (ScrollView returns {0, -scroll}); paint and hit-testing apply
// the offset, so scrolling never re-lays-out children. toWindow() maps a local rect to window DIPs.
//
// Layout: containers override layout() and position children with setRect(). Call requestLayout() when
// something that affects geometry changes; the Window re-runs layout for the whole tree before the next
// frame (cheap: layout is plain arithmetic, text measuring is cached).
#include "gfx/Canvas.h"
#include "gfx/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace st::ui {

using gfx::Canvas;
using gfx::Color;
using gfx::Point;
using gfx::Rect;

class Window;

enum class MouseButton { Left, Right, Middle, Back, Forward };

struct MouseEvent {
    Point pos;            // in the receiving widget's parent-content space (same space as rect())
    Point windowPos;      // window DIPs
    MouseButton button = MouseButton::Left;
    int clicks = 1;       // 2 = double click
    bool ctrl = false, shift = false, alt = false;
};

struct KeyEvent {
    UINT vk = 0;
    bool ctrl = false, shift = false, alt = false;
    bool repeat = false;
};

// Outline of the keyboard focus ring the Window draws around the focused widget (see Window.h "Focus").
enum class FocusShape {
    None,     // the widget shows focus itself (text fields: accent border)
    Rect,     // radius 2 (surfaces, rows, cards)
    Pill,     // pill buttons, chips, toggles, segmented controls
    Circle,   // icon buttons, play button, knob (circle of min(w, h) centered in focusRect())
};

class Widget {
public:
    Widget() = default;
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // --- tree ---------------------------------------------------------------------------------
    template <class T, class... Args>
    T* add(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = child.get();
        adopt(std::move(child));
        return raw;
    }
    Widget* adopt(std::unique_ptr<Widget> child);
    std::unique_ptr<Widget> release(Widget* child);
    void removeChild(Widget* child);   // destroyed (deferred to end of the current event if needed)
    void clearChildren();
    Widget* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    Window* window() const;

    // --- geometry ------------------------------------------------------------------------------
    const Rect& rect() const { return rect_; }
    void setRect(const Rect& r);       // runs layout() when the size changed or layout is pending
    virtual void layout() {}
    // Height this widget wants for a given width (used by stacks/scroll views). Default: current h.
    virtual float preferredHeight(float width) { return rect_.h; }
    virtual Point contentOffset() const { return {}; }
    // Where contentOffset() is heading (a ScrollView mid-animation returns its target), used by scrollIntoView().
    virtual Point settledContentOffset() const { return contentOffset(); }
    Rect toWindow(const Rect& local) const;   // local (parent-content) space -> window DIPs
    Point fromWindow(Point windowPos) const;  // window DIPs -> this widget's parent-content space
    void requestLayout();
    void invalidate();

    // --- painting --------------------------------------------------------------------------------
    virtual void paint(Canvas& c);            // default: paintChildren
    void paintChildren(Canvas& c);
    bool clipsChildren = false;

    // --- visibility / state ------------------------------------------------------------------------
    bool visible() const { return visible_; }
    void setVisible(bool v);
    bool enabled() const { return enabled_; }
    void setEnabled(bool e);
    bool hovered() const { return hovered_; }
    bool pressed() const { return pressed_; }
    bool focused() const;

    // --- input -----------------------------------------------------------------------------------
    // Returns the deepest widget accepting the point (pos in this widget's parent-content space).
    virtual Widget* hitTest(Point pos);
    bool hitTestVisible = true;        // false: mouse passes through this widget (children still hit)
    // Tab stop (Tab / Shift+Tab, tree order) that also takes focus when clicked. Hidden, disabled or zero-size
    // widgets are skipped by keyboard navigation.
    bool focusable = false;
    bool isDragRegion = false;         // title-bar: empty areas that move the window
    bool isMaximizeButton = false;     // Windows 11 snap layouts

    virtual void onMouseEnter() {}
    virtual void onMouseLeave() {}
    virtual bool onMouseDown(const MouseEvent&) { return false; }
    virtual void onMouseUp(const MouseEvent&) {}
    virtual void onMouseMove(const MouseEvent&) {}
    virtual bool onWheel(float deltaY, float deltaX, const MouseEvent&) { return false; }
    virtual bool onKeyDown(const KeyEvent&) { return false; }
    virtual bool onChar(wchar_t) { return false; }
    virtual void onFocusChanged(bool) {}
    virtual LPCWSTR cursor() const { return IDC_ARROW; }
    virtual std::wstring tooltip() const { return tooltip_; }
    void setTooltip(std::wstring t) { tooltip_ = std::move(t); }

    // --- keyboard focus ----------------------------------------------------------------------------
    // Key routing (Window): the focused widget, then its ancestors, get onKeyDown() before the overlay and
    // Window::onKey (global shortcuts). An *activatable* control (button, toggle, slider...) is only offered keys
    // while the focus ring is visible, i.e. after keyboard navigation: a mouse user who clicked a button and then
    // presses Space still toggles playback. Non-activatable focusables (text fields, the track table) keep every
    // key whenever they have focus, as before.
    virtual bool activatable() const { return false; }
    // Enter / Space on the keyboard-focused widget, offered after onKeyDown() declined the key. Return true when
    // handled. Key repeats never re-activate.
    virtual bool onActivate() { return false; }
    // Focus ring geometry, in parent-content space like rect(). Lists can return the current row.
    virtual Rect focusRect() const { return rect_; }
    virtual FocusShape focusShape() const { return FocusShape::Rect; }
    // Scroll containers override: make `contentRect` (this widget's content space) visible.
    virtual void revealRect(const Rect& contentRect) { (void)contentRect; }
    // Asks every scrolling ancestor to reveal `local` (parent-content space; default: focusRect()). Smooth with
    // the motion tokens, instant when reduce-motion is on.
    void scrollIntoView(const Rect& local);
    void scrollIntoView() { scrollIntoView(focusRect()); }
    // Focused with the ring visible (keyboard modality) - for controls that also restyle themselves.
    bool keyboardFocused() const;

    void focus();

private:
    friend class Window;
    Widget* parent_ = nullptr;
    Window* window_ = nullptr;          // set only on roots
    std::vector<std::unique_ptr<Widget>> children_;
    Rect rect_{};
    bool visible_ = true;
    bool enabled_ = true;
    bool hovered_ = false;
    bool pressed_ = false;
    bool layoutPending_ = true;
    std::wstring tooltip_;
};

} // namespace st::ui
