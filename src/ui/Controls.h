#pragma once
// Basic controls, painted exactly per ShadeTube-Design/components/spec.md.
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"
#include "ui/Widget.h"

#include <functional>
#include <string>

namespace st::ui {

using gfx::TextStyle;

// ---------------------------------------------------------------------------------------------------
// Label: a piece of text with a color role. Single line (ellipsis) or wrapping.
enum class Tone { Primary, Secondary, Tertiary, Accent, OnAccent, Error, Inverse };
Color toneColor(Tone t);

class Label : public Widget {
public:
    Label(std::wstring text = {}, TextStyle style = gfx::type::body, Tone tone = Tone::Primary);
    void setText(std::wstring text);
    void setText(std::string_view utf8);
    void setStyle(const TextStyle& s);
    void setTone(Tone t) { tone_ = t; invalidate(); }
    void setColor(Color c) { custom_ = c; hasCustom_ = true; invalidate(); }
    void setWrap(bool wrap, int maxLines = 0);
    void setAlign(gfx::TextAlign a);
    void setVAlign(gfx::VAlign v) { valign_ = v; }
    const std::wstring& text() const { return text_.text(); }
    float preferredHeight(float width) override;
    float naturalWidth();
    void paint(Canvas& c) override;
    std::wstring tooltip() const override;
    gfx::Text& textObj() { return text_; }

private:
    gfx::Text text_;
    Tone tone_;
    Color custom_{};
    bool hasCustom_ = false;
    gfx::VAlign valign_ = gfx::VAlign::Center;
    gfx::TextOptions opts_{};
};

// ---------------------------------------------------------------------------------------------------
enum class ButtonKind {
    Primary,      // accent pill
    Secondary,    // hairline pill
    Ghost,        // text-only pill
    Icon,         // circular icon button (size = rect)
    IconOutline,  // circular icon button with hairline ring (detail header actions)
    Caption,      // window caption (min/max/close)
    CaptionClose,
    Nav,          // sidebar navigation item
    SidebarItem,  // sidebar playlist row (label + mono count)
    Chip,
    Link,         // "Tümünü gör →"
    Tab,
};

class Button : public Widget {
public:
    explicit Button(ButtonKind kind, std::wstring label = {}, std::string icon = {});

    std::function<void()> onClick;
    std::function<void(const MouseEvent&)> onContext;   // right click

    void setLabel(std::wstring label);
    void setIcon(std::string icon);
    void setActive(bool active);         // toggled state (shuffle on, nav selected, chip selected)
    bool active() const { return active_; }
    void setTrailing(std::wstring trailing);   // SidebarItem count / Chip close etc.
    void setIconSize(float s) { iconSize_ = s; }
    ButtonKind kind() const { return kind_; }

    // Width the button wants (pills size to their label).
    float naturalWidth();

    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;

    // Keyboard: every kind except the window caption buttons is a Tab stop. Enter / Space = click; Shift+F10 /
    // the menu key = onContext; Left / Right move between the Tab buttons of one row (selecting them).
    bool activatable() const override { return true; }
    bool onActivate() override;
    bool onKeyDown(const KeyEvent& e) override;
    FocusShape focusShape() const override;

protected:
    ButtonKind kind_;
    gfx::Text label_;
    gfx::Text trailing_;
    std::string icon_;
    float iconSize_ = 0;   // 0 = default for the kind
    bool active_ = false;
    Anim hover_, press_, activeA_;
};

// ---------------------------------------------------------------------------------------------------
// Circular play/pause button with the 9-frame morph and spring press (motion-spec).
class PlayButton : public Widget {
public:
    enum class Look { Accent, Light };   // Light = fg.primary fill (player bar)
    explicit PlayButton(Look look = Look::Accent);
    std::function<void()> onClick;
    void setPlaying(bool playing);
    bool playing() const { return playing_; }
    void setLoading(bool loading);
    void setLook(Look l) { look_ = l; }

    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, motion::fast); }
    void onMouseLeave() override { hover_.to(0, motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override;
    FocusShape focusShape() const override { return FocusShape::Circle; }

private:
    Look look_;
    bool playing_ = false;
    bool loading_ = false;
    Anim morph_, hover_, press_;
};

// ---------------------------------------------------------------------------------------------------
class Toggle : public Widget {
public:
    Toggle() { focusable = true; }
    std::function<void(bool)> onChange;
    void setOn(bool on, bool animate = true);
    bool on() const { return on_; }
    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent&) override { return enabled(); }
    void onMouseUp(const MouseEvent& e) override;
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Enter / Space flip it (same as a click).
    bool activatable() const override { return true; }
    bool onActivate() override;
    Rect focusRect() const override { return rect().center(40, 22); }   // the track
    FocusShape focusShape() const override { return FocusShape::Pill; }

private:
    bool on_ = false;
    Anim knob_;
};

// ---------------------------------------------------------------------------------------------------
// Horizontal slider: seek bar (buffered range, hover-grow, handle on hover) or volume slider.
class Slider : public Widget {
public:
    enum class Look { Seek, Volume, Plain };
    explicit Slider(Look look = Look::Seek);

    std::function<void(float)> onScrub;    // while dragging
    std::function<void(float)> onCommit;   // on release (seek)

    void setValue(float v);                // ignored while dragging
    float value() const { return dragging_ ? dragValue_ : value_; }
    void setBuffered(float b) { buffered_ = b; }
    bool dragging() const { return dragging_; }
    // Extra vertical hit area (the visual track is 2 px).
    float hitPadding = 8;
    // Keyboard step for Left/Right/Up/Down (fraction of the range); 0 = default (seek 2 %, volume/plain 5 %).
    // PageUp/PageDown move 10 %, Home/End jump to the ends. Keyboard changes commit immediately (onCommit).
    float keyStep = 0;

    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, motion::fast); }
    void onMouseLeave() override { hover_.to(0, motion::fast); }
    bool onWheel(float dy, float, const MouseEvent&) override;
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onKeyDown(const KeyEvent& e) override;
    // The ring circles the thumb (keyboard focus also shows the hover look: thicker track + thumb).
    Rect focusRect() const override;
    FocusShape focusShape() const override { return FocusShape::Circle; }
    // Fraction under the cursor while hovering (for time preview tooltips); -1 if not hovering.
    float hoverFraction() const { return hovered() ? hoverFrac_ : -1.f; }

private:
    float fractionAt(float x) const;
    Look look_;
    float value_ = 0, buffered_ = 0, dragValue_ = 0, hoverFrac_ = 0;
    bool dragging_ = false;
    Anim hover_;
};

// ---------------------------------------------------------------------------------------------------
// Hi-fi volume knob: 36 ring, 2x12 accent indicator, -135..+135 deg; drag vertically or scroll.
class Knob : public Widget {
public:
    Knob() { focusable = true; }
    std::function<void(float)> onChange;
    void setValue(float v);
    float value() const { return value_; }
    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent&) override {}
    bool onWheel(float dy, float, const MouseEvent&) override;
    void onMouseEnter() override { hover_.to(1, motion::normal); }
    void onMouseLeave() override { hover_.to(0, motion::normal); }
    LPCWSTR cursor() const override { return IDC_SIZENS; }
    std::wstring tooltip() const override;
    // Keyboard: Up/Right +5 %, Down/Left -5 %, PageUp/PageDown 10 %, Home/End; Enter / Space = press to mute
    // (and back to the previous level), like a hi-fi amplifier knob.
    bool activatable() const override { return true; }
    bool onKeyDown(const KeyEvent& e) override;
    bool onActivate() override;
    Rect focusRect() const override { return rect().center(36, 36); }
    FocusShape focusShape() const override { return FocusShape::Circle; }

private:
    float value_ = 0.7f;
    float unmuted_ = 0.7f;   // level restored by the mute press
    float dragStartValue_ = 0, dragStartY_ = 0;
    Anim angle_{0.7f}, hover_;
};

// ---------------------------------------------------------------------------------------------------
class Spinner : public Widget {
public:
    explicit Spinner(float size = 20) : size_(size) { hitTestVisible = false; }
    void paint(Canvas& c) override;

private:
    float size_;
};

// Separator hairline (horizontal).
class Hairline : public Widget {
public:
    Hairline() { hitTestVisible = false; }
    void paint(Canvas& c) override;
};

// Formats milliseconds as "m:ss" / "h:mm:ss" (tabular mono in the UI).
std::wstring formatDuration(int64_t ms, bool padMinutes = true);

} // namespace st::ui
