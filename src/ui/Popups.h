#pragma once
// Overlay UI: context menus, toasts and modal dialogs (spec: Context menu / Toast / Modal dialog).
#include "gfx/Text.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::ui {

class Window;

struct MenuItem {
    std::wstring label;
    std::string icon;
    std::wstring shortcut;
    std::function<void()> action;
    bool destructive = false;
    bool separator = false;
    bool enabled = true;
    bool checked = false;

    static MenuItem sep() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

class Menu : public Widget {
public:
    // Opens a menu at `anchor` (window DIPs). Returns immediately; the menu closes itself.
    static void open(Window* window, Point anchor, std::vector<MenuItem> items);

    Menu(std::vector<MenuItem> items, Point anchor);
    void layout() override;
    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onWheel(float, float, const MouseEvent&) override { return true; }

private:
    // Keyboard: Up/Down (and Tab / Shift+Tab) move the highlight over enabled items, wrapping; Home/End (PageUp /
    // PageDown) jump to the first/last; Enter or Space activates; Escape closes.
    void moveHot(int dir, bool fromEdge);
    bool usable(int i) const;
    void activate(int i);
    void close();
    int itemAt(Point p) const;
    Rect itemRect(int i) const;
    std::vector<MenuItem> items_;
    std::vector<gfx::Text> labels_;
    Point anchor_;
    Rect box_{};
    int hot_ = -1;
    bool upward_ = false, leftward_ = false;
    Anim open_;
    bool closing_ = false;
};

enum class ToastKind { Info, Success, Warning, Error };

class Toasts : public Widget {
public:
    static void show(Window* window, std::wstring message, ToastKind kind = ToastKind::Info,
                     std::wstring actionLabel = {}, std::function<void()> action = {});
    Toasts() { hitTestVisible = false; }
    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;

    float bottomInset = 88 + 16;   // above the player bar

private:
    struct Toast {
        gfx::Text text;
        gfx::Text action;
        std::function<void()> onAction;
        ToastKind kind;
        double shownAt;
        bool dismissed = false;
        double dismissedAt = 0;
    };
    Rect toastRect(size_t i) const;
    std::vector<Toast> toasts_;
    static Toasts* instanceFor(Window* w);
};

// Modal dialog: scrim + centered panel. Content is a Column you fill; buttons go in the footer.
// Keyboard: focus is trapped inside and starts on the first field, else the first button (Window); Tab cycles;
// Escape cancels (closes); Enter presses the default button: the last Primary one, else the last button.
class Dialog : public Widget {
public:
    static Dialog* open(Window* window, std::wstring title, std::wstring body = {}, float width = 480);
    // Simple confirm helper.
    static void confirm(Window* window, std::wstring title, std::wstring body, std::wstring okLabel,
                        std::function<void()> onOk, bool destructive = false);

    Dialog(std::wstring title, std::wstring body, float width);
    Widget* body() { return body_; }   // add custom content here (laid out as a Column)
    Button* addButton(std::wstring label, ButtonKind kind, std::function<void()> onClick, bool closes = true);
    void close();
    std::function<void()> onClosed;

    void layout() override;
    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onWheel(float, float, const MouseEvent&) override { return true; }

private:
    Button* defaultButton() const;
    Rect panel_{};
    float width_;
    Label* title_ = nullptr;
    Label* text_ = nullptr;
    Widget* body_ = nullptr;
    std::vector<Button*> buttons_;
    Anim open_;
    bool closing_ = false;
};

} // namespace st::ui
