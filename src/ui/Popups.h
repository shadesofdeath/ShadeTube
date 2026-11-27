#pragma once
// Overlay UI: context menus (with submenus), toasts and modal dialogs (spec: Context menu / Toast / Modal dialog), on
// the overlay family's surface (ui/Acrylic.h: outline, radius, layered shadow, acrylic material) with its motion
// (open / close animations, none with Hareketi azalt or Windows' animation effects off).
#include "gfx/Text.h"
#include "ui/Acrylic.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Widget.h"
#include "ui/Window.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::ui {

struct MenuItem {
    std::wstring label;
    std::string icon;
    std::wstring shortcut;   // right-aligned hint (the command's key combo, or a short note such as "Normal")
    std::function<void()> action;
    bool destructive = false;
    bool separator = false;
    bool enabled = true;
    bool checked = false;
    std::wstring tooltip;   // shown on hover (also over a disabled item: why it is disabled)
    // A submenu, built when it opens (hover, a click, Right / Enter / Space); `action` is not used then. The item
    // shows a chevron (a trailing "…" of its label is dropped: the chevron says the same).
    std::function<std::vector<MenuItem>()> submenu;

    static MenuItem sep() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

// Context menu / dropdown menu in the window's overlay layer (modal: a click outside closes it).
//
// Spec "Context menu": w 240 (grows to 360 for long labels), padY 6, items 34 high with padX 12, an icon column (16,
// gap 10) whenever any item has an icon, shortcut hints (mono 11, fg.tertiary) right-aligned, a trailing column for
// the check mark / submenu chevron, separators of 1 px hairline.default with 6 above and below. The hover highlight
// glides from item to item; disabled items are fg.disabled and never highlighted.
//
// Motion: opens with a fade and a 0.96 -> 1 scale from the corner at the anchor (150 ms, decelerate), closes faster
// (100 ms, fade + 0.98); a submenu slides 8 DIP out of its parent (140 ms) and fades out (90 ms).
//
// Keyboard (unchanged, plus submenus and type-ahead): Up / Down (and Tab / Shift+Tab) move the highlight over the
// enabled items, wrapping; Home / End (PageUp / PageDown) jump to the first / last; Enter or Space activates (or opens
// the submenu); Right opens a submenu, Left / Escape close it; Escape closes the menu. Typing letters jumps to the next
// item starting with them (accent and case insensitive; the same letter again cycles). Opened from the keyboard, the
// first item is highlighted.
class Menu : public Widget, public OverlayLayer {
public:
    // Opens a menu at `anchor` (window DIPs). Returns immediately; the menu closes itself.
    static Menu* open(Window* window, Point anchor, std::vector<MenuItem> items);

    Menu(std::vector<MenuItem> items, Point anchor);
    ~Menu() override;
    void layout() override;
    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onWheel(float, float, const MouseEvent&) override { return true; }
    void prepareBackdrop(Canvas& c, Window& w) override;
    bool exiting() const override;

    void close();
    // Dev / tests: open panels (1 = no submenu), the highlighted item of one (-1 = none), geometry in window DIPs.
    int depth() const;
    int highlighted(int level) const;
    Rect panelRect(int level) const;
    Rect itemRect(int level, int item) const;

private:
    struct Panel;
    std::unique_ptr<Panel> makePanel(std::vector<MenuItem> items, Panel* parent, int parentItem);
    void layoutPanel(Panel& p);
    std::vector<Panel*> chain() const;   // the open panels, root first (closing submenus left out)
    Panel* active() const;               // the deepest open panel: where the keys go
    Panel* panelAt(Point p) const;
    int itemAt(const Panel& p, Point pos) const;
    Rect itemRectOf(const Panel& p, int i) const;
    bool usable(const Panel& p, int i) const;
    void setHot(Panel& p, int i);
    void moveHot(int dir, bool fromEdge);
    bool typeAhead(const KeyEvent& e);
    void openSubmenu(Panel& p, int item, bool fromKeyboard);
    void closeSubmenusAbove(Panel* p);   // closes every open panel deeper than p (nullptr = all submenus)
    void activate(Panel& p, int i);
    void paintPanel(Canvas& c, Panel& p);
    void runTimers();

    std::vector<std::unique_ptr<Panel>> panels_;   // [0] = root; closing submenus stay until they faded out
    Point anchor_;
    Acrylic material_;
    Anim exit_;
    bool closing_ = false;
    // Hover timers: a submenu opens after resting on its item, an open one closes after the pointer rested on
    // another item of its parent.
    struct Pending {
        Panel* panel = nullptr;
        int item = -1;
        double at = 0;
    } pendingOpen_, pendingClose_;
    std::wstring typed_;
    double typedAt_ = 0;
};

enum class ToastKind { Info, Success, Warning, Error };

// Toasts above the player bar (spec "Toast": w 360, min-h 56, padX 16, status icon 16 gap 12, text B 13 up to three
// lines, action on the right, 2 px accent progress line). Up to three at once, the newest at the bottom; a fourth
// sends the oldest out. They slide in (24 DIP up + fade, 240 ms), slide out (fade + 12 DIP down, 180 ms) and the
// others glide to their new places.
class Toasts : public Widget, public OverlayLayer {
public:
    static void show(Window* window, std::wstring message, ToastKind kind = ToastKind::Info,
                     std::wstring actionLabel = {}, std::function<void()> action = {});
    Toasts() { hitTestVisible = false; }
    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void prepareBackdrop(Canvas& c, Window& w) override;

    float bottomInset = 88 + 16;   // above the player bar

    // Dev / tests: the window's toast layer (nullptr before its first toast) and its toasts (leaving ones included).
    static Toasts* hostOf(Window* w);
    int count() const { return static_cast<int>(toasts_.size()); }

private:
    struct Toast {
        gfx::Text text;
        gfx::Text action;
        std::function<void()> onAction;
        ToastKind kind;
        double shownAt;
        bool dismissed = false;
        double dismissedAt = 0;
        Anim lift;          // distance of its bottom edge above the stack's bottom (glides when the stack changes)
        float h = 56;
        Rect rect{};        // this frame (update())
        float alpha = 1;
    };
    void update();
    std::vector<Toast> toasts_;
    Acrylic material_;
    double updatedAt_ = -1;
    static Toasts* instanceFor(Window* w);
};

// Modal dialog: scrim + centered panel. Content is a Column you fill; buttons go in the footer.
// Keyboard: focus is trapped inside and starts on the first field, else the first button (Window); Tab cycles;
// Escape cancels (closes); Enter presses the default button: the last Primary one, else the last button.
// Motion: the scrim fades in while the panel fades and rises 8 DIP (240 ms, decelerate); closing fades both and lets
// the panel drop a little (140 ms).
class Dialog : public Widget, public OverlayLayer {
public:
    static Dialog* open(Window* window, std::wstring title, std::wstring body = {}, float width = 480);
    // Simple confirm helper.
    static void confirm(Window* window, std::wstring title, std::wstring body, std::wstring okLabel,
                        std::function<void()> onOk, bool destructive = false);

    Dialog(std::wstring title, std::wstring body, float width);
    Widget* body() { return body_; }   // add custom content here (laid out as a Column)
    // Footer buttons, right-aligned in the order added: the dismissal (Ghost "Vazgeç" / "Kapat") first, the dialog's
    // action (Primary, or a destructive Secondary) last.
    Button* addButton(std::wstring label, ButtonKind kind, std::function<void()> onClick, bool closes = true);
    // A footer button on the left end, apart from the dismiss / confirm pair: an alternative way to the same goal
    // ("Dosyadan…", "Otomatik seçime dön", "Video gösterme") or a destructive side action ("Sil"). Never the Enter
    // default.
    Button* addLeadingButton(std::wstring label, ButtonKind kind, std::function<void()> onClick, bool closes = true);
    void close();
    std::function<void()> onClosed;

    void layout() override;
    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onWheel(float, float, const MouseEvent&) override { return true; }
    void prepareBackdrop(Canvas& c, Window& w) override;
    bool exiting() const override { return closing_ && exit_.running(); }

private:
    Button* defaultButton() const;
    Rect panel_{};
    float width_;
    Label* title_ = nullptr;
    Label* text_ = nullptr;
    Widget* body_ = nullptr;
    std::vector<Button*> buttons_;
    std::vector<Button*> leading_;   // addLeadingButton(): left end of the footer
    Anim open_, exit_;
    Acrylic material_;
    bool closing_ = false;
};

} // namespace st::ui
