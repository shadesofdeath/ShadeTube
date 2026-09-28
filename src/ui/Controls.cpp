#include "ui/Controls.h"

#include "core/I18n.h"
#include "core/Utf.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace st::ui {

using gfx::colors;
using gfx::accent;
namespace type = gfx::type;

Color toneColor(Tone t) {
    const auto& c = colors();
    switch (t) {
    case Tone::Secondary: return c.fgSecondary;
    case Tone::Tertiary: return c.fgTertiary;
    case Tone::Accent: return accent().base;
    case Tone::OnAccent: return accent().onAccent;
    case Tone::Error: return c.error;
    case Tone::Inverse: return c.fgInverse;
    default: return c.fgPrimary;
    }
}

std::wstring formatDuration(int64_t ms, bool padMinutes) {
    if (ms < 0) ms = 0;
    const int64_t total = ms / 1000;
    const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
    wchar_t buf[32];
    if (h > 0) swprintf(buf, 32, L"%lld:%02lld:%02lld", h, m, s);
    else if (padMinutes) swprintf(buf, 32, L"%02lld:%02lld", m, s);
    else swprintf(buf, 32, L"%lld:%02lld", m, s);
    return buf;
}

// ---------------------------------------------------------------------------------------------------
// Label

Label::Label(std::wstring text, TextStyle style, Tone tone) : text_(std::move(text), style), tone_(tone) {
    hitTestVisible = false;
}

void Label::setText(std::wstring text) {
    if (text == text_.text()) return;
    text_.setText(std::move(text));
    requestLayout();
    invalidate();
}
void Label::setText(std::string_view utf8) { setText(st::toWide(utf8)); }

void Label::setStyle(const TextStyle& s) {
    text_.setStyle(s);
    requestLayout();
    invalidate();
}

void Label::setWrap(bool wrap, int maxLines) {
    opts_.wrap = wrap;
    opts_.maxLines = maxLines;
    text_.setOptions(opts_);
    requestLayout();
}

void Label::setAlign(gfx::TextAlign a) {
    opts_.align = a;
    text_.setOptions(opts_);
}

float Label::preferredHeight(float width) {
    if (!opts_.wrap) return text_.style().size * text_.style().lineHeight;
    return std::ceil(text_.measure(width).h);
}

float Label::naturalWidth() { return std::ceil(text_.measure().w); }

void Label::paint(Canvas& c) {
    c.text(text_, rect(), hasCustom_ ? custom_ : toneColor(tone_), valign_);
}

std::wstring Label::tooltip() const {
    // Show the full text when it is trimmed (hit-testing must be enabled by the owner for this to fire).
    return {};
}

// ---------------------------------------------------------------------------------------------------
// Button

Button::Button(ButtonKind kind, std::wstring label, std::string icon) : kind_(kind), icon_(std::move(icon)) {
    TextStyle style = type::body;
    switch (kind) {
    case ButtonKind::Primary:
    case ButtonKind::Secondary: style = type::body.withSize(13); break;
    case ButtonKind::Ghost: style = type::body.withSize(13); break;
    case ButtonKind::Nav: style = type::body.withWeight(500); break;
    case ButtonKind::SidebarItem: style = type::secondary; break;
    case ButtonKind::Chip: style = type::caption.withWeight(600); break;
    case ButtonKind::Link: style = type::secondary; break;
    case ButtonKind::Tab: style = type::body; break;
    default: break;
    }
    label_ = gfx::Text(std::move(label), style);
    trailing_ = gfx::Text({}, type::monoLabel);
    // Window caption buttons stay out of the Tab order (the system handles Alt+F4 / Alt+Space / Win+arrows).
    focusable = kind != ButtonKind::Caption && kind != ButtonKind::CaptionClose;
}

void Button::setLabel(std::wstring label) {
    label_.setText(std::move(label));
    requestLayout();
    invalidate();
}

void Button::setIcon(std::string icon) {
    icon_ = std::move(icon);
    invalidate();
}

void Button::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    activeA_.to(active ? 1.f : 0.f, motion::normal);
    invalidate();
}

void Button::setTrailing(std::wstring trailing) {
    trailing_.setText(std::move(trailing));
    invalidate();
}

float Button::naturalWidth() {
    const float text = label_.empty() ? 0 : std::ceil(label_.measure().w);
    const float icon = icon_.empty() ? 0 : (iconSize_ > 0 ? iconSize_ : 16);
    const float gap = (text > 0 && icon > 0) ? 8.f : 0.f;
    switch (kind_) {
    case ButtonKind::Primary:
    case ButtonKind::Secondary: return text + icon + gap + gfx::metrics::pillPadX * 2;
    case ButtonKind::Ghost: return text + icon + gap + 24;
    case ButtonKind::Chip: return text + 24;
    case ButtonKind::Link: return text + (icon > 0 ? icon + 4 : 0);
    case ButtonKind::Tab: return text;
    default: return rect().w;
    }
}

void Button::onMouseEnter() {
    hover_.to(1, motion::fast);
    invalidate();
}
void Button::onMouseLeave() {
    hover_.to(0, motion::fast);
    invalidate();
}

bool Button::onMouseDown(const MouseEvent& e) {
    if (!enabled()) return false;
    if (e.button == MouseButton::Right) {
        if (onContext) {
            onContext(e);
            return false;
        }
        return false;
    }
    if (e.button != MouseButton::Left) return false;
    press_.to(1, 80, Ease::Accelerate);
    return true;
}

void Button::onMouseUp(const MouseEvent& e) {
    press_.to(0, 160, Ease::Decelerate);
    if (hovered() && rect().contains(e.pos) && onClick) onClick();
}

bool Button::onActivate() {
    if (!enabled() || !onClick) return false;
    // The click's press feedback, then the click. The handler may replace onClick (or close this button's
    // dialog): call a copy. Widgets are deleted only after the event, so `this` stays valid meanwhile.
    press_.snap(1);
    press_.to(0, motion::normal, Ease::Decelerate);
    invalidate();
    const auto click = onClick;
    click();
    return true;
}

bool Button::onKeyDown(const KeyEvent& e) {
    // Context menu from the keyboard (menu key / Shift+F10), anchored under the button.
    if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && onContext && enabled()) {
        MouseEvent me;
        const Rect wr = toWindow(rect());
        me.windowPos = {wr.x + 8, wr.bottom()};
        me.pos = {rect().x + 8, rect().bottom()};
        me.button = MouseButton::Right;
        const auto context = onContext;
        context(me);
        return true;
    }
    // A row of Tab buttons: Left / Right move to the neighbouring tab and select it (wrapping).
    if (kind_ != ButtonKind::Tab || e.ctrl || e.alt || (e.vk != VK_LEFT && e.vk != VK_RIGHT)) return false;
    Widget* row = parent();
    if (!row) return false;
    std::vector<Button*> tabs;
    for (const auto& c : row->children()) {
        auto* b = dynamic_cast<Button*>(c.get());
        if (b && b->kind_ == ButtonKind::Tab && b->visible() && b->enabled()) tabs.push_back(b);
    }
    const auto it = std::find(tabs.begin(), tabs.end(), this);
    if (it == tabs.end() || tabs.size() < 2) return false;
    const int n = static_cast<int>(tabs.size());
    const int i = static_cast<int>(it - tabs.begin());
    Button* next = tabs[(i + (e.vk == VK_RIGHT ? 1 : n - 1)) % n];
    if (auto* w = window()) w->focusByKeyboard(next);
    next->onActivate();
    return true;
}

FocusShape Button::focusShape() const {
    switch (kind_) {
    case ButtonKind::Icon:
    case ButtonKind::IconOutline: return FocusShape::Circle;
    case ButtonKind::Primary:
    case ButtonKind::Secondary:
    case ButtonKind::Ghost:
    case ButtonKind::Nav:
    case ButtonKind::SidebarItem:
    case ButtonKind::Chip: return FocusShape::Pill;
    default: return FocusShape::Rect;   // Link, Tab (and the caption buttons, never focused)
    }
}

LPCWSTR Button::cursor() const { return enabled() ? IDC_HAND : IDC_ARROW; }

void Button::paint(Canvas& c) {
    const auto& col = colors();
    const auto& acc = accent();
    const Rect r = rect();
    const float h = hover_, p = press_, a = activeA_;
    const float disabled = enabled() ? 1.f : 0.38f;
    const float iconSz = iconSize_ > 0 ? iconSize_ : 16.f;

    auto drawContent = [&](Color fg, float padX, gfx::TextAlign align) {
        const float textW = label_.empty() ? 0 : std::ceil(label_.measure().w);
        const float iconW = icon_.empty() ? 0 : iconSz;
        const float gap = (textW > 0 && iconW > 0) ? 8.f : 0.f;
        const float total = textW + iconW + gap;
        float x = align == gfx::TextAlign::Center ? r.cx() - total * 0.5f : r.x + padX;
        if (iconW > 0) {
            c.icon(icon_, {x, r.cy() - iconW * 0.5f, iconW, iconW}, fg);
            x += iconW + gap;
        }
        if (textW > 0) c.text(label_, {x, r.y, std::max(0.f, r.right() - x - (align == gfx::TextAlign::Center ? 0 : padX)), r.h}, fg, gfx::VAlign::Center);
    };

    switch (kind_) {
    case ButtonKind::Primary: {
        const float s = 1 - 0.02f * p;
        c.pushScale(s, {r.cx(), r.cy()});
        Color bg = Color::lerp(acc.base, acc.hover, h);
        bg = Color::lerp(bg, acc.pressed, p);
        c.fillPill(r, bg.mulAlpha(disabled));
        drawContent(acc.onAccent.mulAlpha(disabled), gfx::metrics::pillPadX, gfx::TextAlign::Center);
        c.popTransform();
        break;
    }
    case ButtonKind::Secondary: {
        const float s = 1 - 0.02f * p;
        c.pushScale(s, {r.cx(), r.cy()});
        c.fillPill(r, Color::lerp(col.overlayHover.withAlpha(0), col.overlayHover, h));
        if (p > 0) c.fillPill(r, col.overlayPressed.mulAlpha(p));
        c.strokePill(r, Color::lerp(col.hairControl, active_ ? acc.base : col.fgSecondary.withAlpha(0.5f), std::max(h * 0.6f, a)));
        drawContent(Color::lerp(col.fgPrimary, acc.base, a).mulAlpha(disabled), gfx::metrics::pillPadX,
                    gfx::TextAlign::Center);
        c.popTransform();
        break;
    }
    case ButtonKind::Ghost: {
        c.fillPill(r, col.overlayHover.mulAlpha(h));
        drawContent(Color::lerp(col.fgSecondary, col.fgPrimary, h).mulAlpha(disabled), 12, gfx::TextAlign::Center);
        break;
    }
    case ButtonKind::Icon:
    case ButtonKind::IconOutline: {
        const float s = 1 + 0.04f * h - 0.08f * p;
        c.pushScale(s, {r.cx(), r.cy()});
        const float d = std::min(r.w, r.h);
        const Rect circle = r.center(d, d);
        c.fillCircle({circle.cx(), circle.cy()}, d * 0.5f, col.overlayHover.mulAlpha(h));
        if (p > 0) c.fillCircle({circle.cx(), circle.cy()}, d * 0.5f, col.overlayPressed.mulAlpha(p));
        if (kind_ == ButtonKind::IconOutline)
            c.strokeCircle({circle.cx(), circle.cy()}, d * 0.5f, Color::lerp(col.hairControl, col.fgSecondary.withAlpha(0.6f), h));
        Color fg = Color::lerp(col.fgSecondary, col.fgPrimary, h);
        fg = Color::lerp(fg, acc.base, a);
        const float is = iconSize_ > 0 ? iconSize_ : (d >= 48 ? 18.f : 16.f);
        c.icon(icon_, circle.center(is, is), fg.mulAlpha(disabled));
        c.popTransform();
        break;
    }
    case ButtonKind::Caption:
    case ButtonKind::CaptionClose: {
        const Rect hit = r.center(40, 28);
        if (kind_ == ButtonKind::CaptionClose) {
            c.fillRounded(hit, 2, col.closeHover.mulAlpha(std::max(h, p)));
            c.icon(icon_, r.center(12, 12), Color::lerp(col.fgSecondary, col.closeHoverFg, h));
        } else {
            c.fillRounded(hit, 2, col.captionHover.mulAlpha(h));
            if (p > 0) c.fillRounded(hit, 2, col.overlayPressed.mulAlpha(p));
            c.icon(icon_, r.center(12, 12), Color::lerp(col.fgSecondary, col.fgPrimary, h));
        }
        break;
    }
    case ButtonKind::Nav: {
        c.fillPill(r, col.overlaySelected.mulAlpha(a));
        c.fillPill(r, col.overlayHover.mulAlpha(h * (1 - a)));
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, h), col.fgPrimary, a);
        const Color ic = Color::lerp(fg, acc.base, a);
        const float is = gfx::metrics::navIcon;
        c.icon(icon_, {r.x + 12, r.cy() - is * 0.5f, is, is}, ic);
        label_.setStyle(type::body.withWeight(active_ ? 600.f : 500.f));
        c.text(label_, {r.x + 12 + is + 12, r.y, r.w - 36 - is, r.h}, fg, gfx::VAlign::Center);
        break;
    }
    case ButtonKind::SidebarItem: {
        c.fillPill(r, col.overlaySelected.mulAlpha(a));
        c.fillPill(r, col.overlayHover.mulAlpha(h * (1 - a)));
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, h), col.fgPrimary, a);
        float right = r.right() - 12;
        if (!trailing_.empty()) {
            const float tw = std::ceil(trailing_.measure().w);
            c.text(trailing_, {right - tw, r.y, tw + 1, r.h}, col.fgTertiary, gfx::VAlign::Center);
            right -= tw + 10;
        }
        float x = r.x + 12;
        if (!icon_.empty()) {   // e.g. playing indicator
            c.icon(icon_, {x, r.cy() - 7, 14, 14}, acc.base);
            x += 20;
        }
        label_.setStyle(type::secondary.withWeight(active_ ? 600.f : 400.f));
        c.text(label_, {x, r.y, right - x, r.h}, fg, gfx::VAlign::Center);
        break;
    }
    case ButtonKind::Chip: {
        c.fillPill(r, acc.base.mulAlpha(a));
        c.fillPill(r, col.overlayHover.mulAlpha(h * (1 - a)));
        c.strokePill(r, col.hairControl.mulAlpha(1 - a));
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, h), acc.onAccent, a);
        c.text(label_, r.inset(12, 0), fg, gfx::VAlign::Center);
        break;
    }
    case ButtonKind::Link: {
        const Color fg = Color::lerp(col.fgSecondary, col.fgPrimary, h);
        const float tw = std::ceil(label_.measure().w);
        c.text(label_, {r.x, r.y, tw + 1, r.h}, fg, gfx::VAlign::Center);
        if (!icon_.empty()) c.icon(icon_, {r.x + tw + 4 + 2 * h, r.cy() - 7, 14, 14}, fg);
        break;
    }
    case ButtonKind::Tab: {
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, h), col.fgPrimary, a);
        c.text(label_, {r.x, r.y, r.w + 1, r.h - 2}, fg, gfx::VAlign::Center);
        if (a > 0.01f) {
            const float w = r.w * a;
            c.fillRect({r.cx() - w * 0.5f, r.bottom() - 2, w, 2}, acc.base);
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------------------------------
// PlayButton

PlayButton::PlayButton(Look look) : look_(look) { focusable = true; }

bool PlayButton::onActivate() {
    if (!enabled() || !onClick) return false;
    press_.snap(1);
    press_.to(0, motion::medium, Ease::Spring);   // the click's spring release
    invalidate();
    const auto click = onClick;
    click();
    return true;
}

void PlayButton::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    morph_.to(playing ? 1.f : 0.f, motion::medium, Ease::Linear);
    invalidate();
}

void PlayButton::setLoading(bool loading) {
    if (loading_ == loading) return;
    loading_ = loading;
    invalidate();
}

bool PlayButton::onMouseDown(const MouseEvent& e) {
    if (e.button != MouseButton::Left) return false;
    press_.to(1, 80, Ease::Accelerate);
    return true;
}

void PlayButton::onMouseUp(const MouseEvent& e) {
    press_.to(0, motion::medium, Ease::Spring);
    if (rect().contains(e.pos) && onClick) onClick();
}

void PlayButton::paint(Canvas& c) {
    const Rect r = rect();
    const float d = std::min(r.w, r.h);
    const float s = 1 + 0.04f * hover_ - 0.08f * press_;
    const auto& acc = accent();
    const auto& col = colors();
    const Color bg = look_ == Look::Accent ? Color::lerp(acc.base, acc.hover, hover_) : col.fgPrimary;
    const Color fg = look_ == Look::Accent ? acc.onAccent : col.fgInverse;
    c.pushScale(s, {r.cx(), r.cy()});
    c.fillCircle({r.cx(), r.cy()}, d * 0.5f, bg);
    const float is = d >= 56 ? 20.f : d >= 48 ? 16.f : 14.f;
    if (loading_) {
        const float t = static_cast<float>(std::fmod(frame::now(), 900.0) / 900.0);
        c.arc({r.cx(), r.cy()}, is * 0.55f, t * 360.f, 270.f, fg, 1.75f);
        frame::requestNext();
    } else {
        // 9 frames: 0 = play, 8 = pause.
        const int f = std::clamp(static_cast<int>(std::round(morph_.value() * 8)), 0, 8);
        char name[64];
        snprintf(name, sizeof name, "animations/play-pause-morph/frame-%02d", f);
        c.icon(name, r.center(is, is), fg);
    }
    c.popTransform();
}

// ---------------------------------------------------------------------------------------------------
// Toggle

void Toggle::setOn(bool on, bool animate) {
    on_ = on;
    if (animate) knob_.to(on ? 1.f : 0.f, motion::normal);
    else knob_.snap(on ? 1.f : 0.f);
    invalidate();
}

void Toggle::onMouseUp(const MouseEvent& e) {
    if (!rect().contains(e.pos)) return;
    setOn(!on_);
    if (onChange) onChange(on_);
}

bool Toggle::onActivate() {
    if (!enabled()) return false;
    setOn(!on_);
    if (onChange) onChange(on_);
    return true;
}

void Toggle::paint(Canvas& c) {
    const auto& col = colors();
    const auto& acc = accent();
    const Rect track = rect().center(40, 22);
    const float k = knob_;
    c.fillPill(track, acc.base.mulAlpha(k));
    c.strokePill(track, col.hairControl.mulAlpha(1 - k));
    const float x = track.x + 3 + 18 * k;
    c.fillCircle({x + 8, track.cy()}, 8, Color::lerp(col.fgSecondary, acc.onAccent, k).mulAlpha(enabled() ? 1 : 0.38f));
}

// ---------------------------------------------------------------------------------------------------
// Slider

Slider::Slider(Look look) : look_(look) { focusable = true; }

bool Slider::onKeyDown(const KeyEvent& e) {
    if (e.ctrl || e.alt || dragging_ || !enabled()) return false;
    const float step = keyStep > 0 ? keyStep : (look_ == Look::Seek ? 0.02f : 0.05f);
    float v = value_;
    switch (e.vk) {
    case VK_LEFT:
    case VK_DOWN: v -= step; break;
    case VK_RIGHT:
    case VK_UP: v += step; break;
    case VK_NEXT: v -= 0.1f; break;    // PageDown
    case VK_PRIOR: v += 0.1f; break;   // PageUp
    case VK_HOME: v = 0; break;
    case VK_END: v = 1; break;
    default: return false;
    }
    value_ = std::clamp(v, 0.f, 1.f);
    if (onCommit) onCommit(value_);
    invalidate();
    return true;
}

Rect Slider::focusRect() const {
    const Rect r = rect();
    const float cy = look_ == Look::Seek ? r.y + 1 : r.cy();   // same geometry as paint()
    const float d = look_ == Look::Seek ? 8.f : 12.f;           // the full-size thumb
    return {r.x + r.w * value() - d * 0.5f, cy - d * 0.5f, d, d};
}

void Slider::setValue(float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (v == value_) return;
    value_ = v;
    if (!dragging_) invalidate();
}

float Slider::fractionAt(float x) const {
    const Rect r = rect();
    return r.w > 0 ? std::clamp((x - r.x) / r.w, 0.f, 1.f) : 0.f;
}

Widget* Slider::hitTest(Point p) {
    const Rect r = rect();
    const Rect hit{r.x, r.y - hitPadding, r.w, r.h + hitPadding * 2};
    return (visible() && hit.contains(p)) ? this : nullptr;
}

bool Slider::onMouseDown(const MouseEvent& e) {
    if (e.button != MouseButton::Left) return false;
    dragging_ = true;
    dragValue_ = fractionAt(e.pos.x);
    if (onScrub) onScrub(dragValue_);
    invalidate();
    return true;
}

void Slider::onMouseMove(const MouseEvent& e) {
    hoverFrac_ = fractionAt(e.pos.x);
    if (dragging_) {
        dragValue_ = hoverFrac_;
        if (onScrub) onScrub(dragValue_);
    }
    invalidate();
}

void Slider::onMouseUp(const MouseEvent& e) {
    if (!dragging_) return;
    dragging_ = false;
    value_ = fractionAt(e.pos.x);
    if (onCommit) onCommit(value_);
    invalidate();
}

bool Slider::onWheel(float dy, float, const MouseEvent&) {
    if (look_ != Look::Volume) return false;
    value_ = std::clamp(value_ + dy * 0.05f, 0.f, 1.f);
    if (onCommit) onCommit(value_);
    invalidate();
    return true;
}

void Slider::paint(Canvas& c) {
    const auto& col = colors();
    const auto& acc = accent();
    const Rect r = rect();
    // Keyboard focus shows the hover state (thicker track + handle) so the value being changed is visible.
    const float h = std::max({hover_.value(), dragging_ ? 1.f : 0.f, keyboardFocused() ? 1.f : 0.f});
    const float trackH = 2 + 2 * h;
    const float cy = look_ == Look::Seek ? r.y + 1 : r.cy();   // seek sits on the bar's top edge
    const Rect track{r.x, cy - trackH * 0.5f, r.w, trackH};
    c.fillRect(track, col.hairDefault);
    if (look_ == Look::Seek && buffered_ > 0) c.fillRect({r.x, track.y, r.w * std::clamp(buffered_, 0.f, 1.f), trackH}, acc.tint25);
    const float v = value();
    c.fillRect({r.x, track.y, r.w * v, trackH}, look_ == Look::Plain ? col.fgPrimary : acc.base);
    const float handle = (look_ == Look::Seek ? 8.f : 12.f) * (0.6f + 0.4f * h);
    if (h > 0.01f || look_ != Look::Seek)
        c.fillCircle({r.x + r.w * v, cy}, handle * 0.5f, (look_ == Look::Plain ? col.fgPrimary : acc.base).mulAlpha(look_ == Look::Seek ? h : 1.f));
}

// ---------------------------------------------------------------------------------------------------
// Knob

void Knob::setValue(float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (v == value_) return;
    value_ = v;
    angle_.to(v, motion::normal);
    invalidate();
}

bool Knob::onMouseDown(const MouseEvent& e) {
    if (e.button != MouseButton::Left) return false;
    dragStartValue_ = value_;
    dragStartY_ = e.windowPos.y;
    return true;
}

void Knob::onMouseMove(const MouseEvent& e) {
    if (!pressed()) return;
    const float v = std::clamp(dragStartValue_ + (dragStartY_ - e.windowPos.y) / 150.f, 0.f, 1.f);
    value_ = v;
    angle_.snap(v);
    if (onChange) onChange(v);
    invalidate();
}

bool Knob::onWheel(float dy, float, const MouseEvent&) {
    setValue(value_ + dy * 0.05f);
    if (onChange) onChange(value_);
    return true;
}

bool Knob::onKeyDown(const KeyEvent& e) {
    if (e.ctrl || e.alt || !enabled()) return false;
    float v = value_;
    switch (e.vk) {
    case VK_UP:
    case VK_RIGHT: v += 0.05f; break;
    case VK_DOWN:
    case VK_LEFT: v -= 0.05f; break;
    case VK_PRIOR: v += 0.1f; break;
    case VK_NEXT: v -= 0.1f; break;
    case VK_HOME: v = 0; break;
    case VK_END: v = 1; break;
    default: return false;
    }
    setValue(v);
    if (onChange) onChange(value_);
    return true;
}

bool Knob::onActivate() {
    if (!enabled()) return false;
    if (value_ > 0.001f) {
        unmuted_ = value_;
        setValue(0);
    } else {
        setValue(unmuted_ > 0.001f ? unmuted_ : 0.5f);
    }
    if (onChange) onChange(value_);
    return true;
}

std::wstring Knob::tooltip() const { return i18n::format(tr(L"Ses %{}"), std::lround(value_ * 100)); }

void Knob::paint(Canvas& c) {
    const auto& col = colors();
    const auto& acc = accent();
    const Rect r = rect().center(36, 36);
    const Point center{r.cx(), r.cy()};
    c.strokeCircle(center, 18, Color::lerp(col.hairControl, col.fgSecondary, hover_));
    // Tick marks at min/max (hi-fi cue).
    const float v = angle_;
    const float deg = -135.f + 270.f * v;
    // Level arc just inside the ring.
    if (v > 0.001f) c.arc(center, 15.5f, -135.f, 270.f * v, acc.tint25, 1.5f);
    const float rad = (deg - 90.f) * std::numbers::pi_v<float> / 180.f;
    const Point a{center.x + std::cos(rad) * 3.f, center.y + std::sin(rad) * 3.f};
    const Point b{center.x + std::cos(rad) * 13.f, center.y + std::sin(rad) * 13.f};
    c.line(a, b, value_ <= 0.001f ? col.fgTertiary : acc.base, 2.f);
}

// ---------------------------------------------------------------------------------------------------

void Spinner::paint(Canvas& c) {
    const Rect r = rect().center(size_, size_);
    const float t = static_cast<float>(std::fmod(frame::now(), 900.0) / 900.0);
    c.arc({r.cx(), r.cy()}, size_ * 0.5f - 1, t * 360.f, 270.f, accent().base, 1.75f);
    frame::requestNext();
}

void Hairline::paint(Canvas& c) { c.hline(rect().x, rect().right(), rect().y, colors().hairDefault); }

} // namespace st::ui
