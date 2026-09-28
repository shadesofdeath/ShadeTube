#include "ui/Popups.h"

#include "core/I18n.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <unordered_map>

namespace st::ui {

using gfx::colors;
using gfx::accent;
namespace type = gfx::type;

// ===================================================================================================
// Menu

namespace {
constexpr float kMenuW = 240, kItemH = 34, kPadY = 6, kSepH = 13;
}

void Menu::open(Window* window, Point anchor, std::vector<MenuItem> items) {
    if (!window || items.empty()) return;
    auto* menu = static_cast<Menu*>(window->pushOverlay(std::make_unique<Menu>(std::move(items), anchor), true));
    // Opened from the keyboard (Enter on a focused button, menu key): highlight the first item so Enter works right
    // away. A mouse-opened menu waits for the pointer, as before.
    if (window->focusVisible()) menu->moveHot(1, true);
}

bool Menu::usable(int i) const {
    return i >= 0 && i < static_cast<int>(items_.size()) && !items_[i].separator && items_[i].enabled;
}

void Menu::moveHot(int dir, bool fromEdge) {
    const int n = static_cast<int>(items_.size());
    int i = (fromEdge || hot_ < 0) ? (dir > 0 ? -1 : n) : hot_;
    for (int k = 0; k < n; ++k) {
        i = (i + dir + n) % n;
        if (usable(i)) {
            hot_ = i;
            break;
        }
    }
    invalidate();
}

void Menu::activate(int i) {
    if (!usable(i)) return;
    auto action = items_[i].action;
    close();
    if (action) action();
}

Menu::Menu(std::vector<MenuItem> items, Point anchor) : items_(std::move(items)), anchor_(anchor) {
    for (auto& it : items_) labels_.emplace_back(it.label, type::secondary);
    open_.to(1, 160, Ease::Decelerate);
}

void Menu::layout() {
    float h = kPadY * 2;
    for (auto& it : items_) h += it.separator ? kSepH : kItemH;
    const Rect r = rect();
    leftward_ = anchor_.x + kMenuW > r.w - 8;
    upward_ = anchor_.y + h > r.h - 8;
    const float x = leftward_ ? anchor_.x - kMenuW : anchor_.x;
    const float y = upward_ ? std::max(8.f, anchor_.y - h) : anchor_.y;
    box_ = {std::max(8.f, x), y, kMenuW, h};
}

Rect Menu::itemRect(int i) const {
    float y = box_.y + kPadY;
    for (int k = 0; k < i; ++k) y += items_[k].separator ? kSepH : kItemH;
    return {box_.x + 6, y, box_.w - 12, items_[i].separator ? kSepH : kItemH};
}

int Menu::itemAt(Point p) const {
    if (!box_.contains(p)) return -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        if (!items_[i].separator && items_[i].enabled && itemRect(i).contains(p)) return i;
    return -1;
}

Widget* Menu::hitTest(Point p) { return visible() ? this : nullptr; }   // full-window: click outside closes

bool Menu::onMouseDown(const MouseEvent& e) {
    if (!box_.contains(e.pos)) {
        close();
        return false;
    }
    return true;
}

void Menu::onMouseUp(const MouseEvent& e) {
    const int i = itemAt(e.pos);
    if (i < 0) return;
    activate(i);
}

void Menu::onMouseMove(const MouseEvent& e) {
    const int i = itemAt(e.pos);
    if (i != hot_) {
        hot_ = i;
        invalidate();
    }
}

bool Menu::onKeyDown(const KeyEvent& e) {
    switch (e.vk) {
    case VK_ESCAPE: close(); return true;
    case VK_DOWN: moveHot(1, false); return true;
    case VK_UP: moveHot(-1, false); return true;
    case VK_TAB: moveHot(e.shift ? -1 : 1, false); return true;   // the Window hands Tab over (no tab stops here)
    case VK_HOME:
    case VK_PRIOR: moveHot(1, true); return true;
    case VK_END:
    case VK_NEXT: moveHot(-1, true); return true;
    case VK_RETURN:
    case VK_SPACE:
        if (!e.repeat) activate(hot_);
        return true;
    default: return true;   // swallow while open
    }
}

void Menu::close() {
    if (closing_) return;
    closing_ = true;
    if (auto* w = window()) w->removeOverlay(this);
}

void Menu::paint(Canvas& c) {
    const auto& col = colors();
    const float t = open_;
    const float s = 0.96f + 0.04f * t;
    const Point origin{leftward_ ? box_.right() : box_.x, upward_ ? box_.bottom() : box_.y};
    c.pushOpacity(t);
    c.pushScale(s, origin);
    c.shadow(box_, 2, 32, 16, col.shadowMenu);
    c.fillRounded(box_, 2, col.bgOverlay);
    c.strokeRounded(box_, 2, col.hairStrong);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& it = items_[i];
        const Rect r = itemRect(i);
        if (it.separator) {
            c.hline(box_.x, box_.right(), r.cy(), col.hairDefault);
            continue;
        }
        if (i == hot_) c.fillRounded(r, 2, col.overlayHover);
        Color fg = it.destructive ? col.error : (i == hot_ ? col.fgPrimary : col.fgSecondary);
        if (!it.enabled) fg = col.fgDisabled;
        float x = r.x + 6;
        if (!it.icon.empty()) {
            c.icon(it.icon, {x, r.cy() - 8, 16, 16}, it.checked ? accent().base : fg);
            x += 16 + 10;
        }
        c.text(labels_[i], {x, r.y, r.right() - x - 8, r.h}, fg, gfx::VAlign::Center);
        if (!it.shortcut.empty())
            c.text(it.shortcut, type::monoMeta, {r.x, r.y, r.w - 6, r.h}, col.fgTertiary, gfx::TextAlign::Trailing,
                   gfx::VAlign::Center);
        else if (it.checked && it.icon.empty())
            c.icon("check", {r.right() - 22, r.cy() - 8, 16, 16}, accent().base);
    }
    c.popTransform();
    c.popLayer();
}

// ===================================================================================================
// Toasts

namespace {
std::unordered_map<Window*, Toasts*> g_toastHosts;
constexpr double kToastLife = 4000, kToastIn = 240, kToastOut = 180;
} // namespace

Toasts* Toasts::instanceFor(Window* w) {
    if (auto it = g_toastHosts.find(w); it != g_toastHosts.end()) return it->second;
    auto* host = static_cast<Toasts*>(w->pushOverlay(std::make_unique<Toasts>(), false));
    g_toastHosts[w] = host;
    return host;
}

void Toasts::show(Window* window, std::wstring message, ToastKind kind, std::wstring actionLabel,
                  std::function<void()> action) {
    if (!window) return;
    auto* host = instanceFor(window);
    Toast t{gfx::Text(std::move(message), type::secondary.withWeight(500)), gfx::Text(std::move(actionLabel), type::body.withSize(13)),
            std::move(action), kind, frame::realNow()};
    host->toasts_.push_back(std::move(t));
    if (host->toasts_.size() > 3) host->toasts_.erase(host->toasts_.begin());
    host->invalidate();
}

Rect Toasts::toastRect(size_t i) const {
    const Rect r = rect();
    const float w = 360, h = 56;
    const size_t fromBottom = toasts_.size() - 1 - i;
    return {r.cx() - w * 0.5f, r.bottom() - bottomInset - h - fromBottom * (h + 8), w, h};
}

Widget* Toasts::hitTest(Point p) {
    for (size_t i = 0; i < toasts_.size(); ++i)
        if (!toasts_[i].action.empty() && toastRect(i).contains(p)) return this;
    return nullptr;
}

bool Toasts::onMouseDown(const MouseEvent& e) { return true; }

void Toasts::onMouseUp(const MouseEvent& e) {
    for (size_t i = 0; i < toasts_.size(); ++i) {
        if (toastRect(i).contains(e.pos) && toasts_[i].onAction) {
            auto a = toasts_[i].onAction;
            toasts_[i].dismissed = true;
            toasts_[i].dismissedAt = frame::realNow();
            a();
            return;
        }
    }
}

void Toasts::paint(Canvas& c) {
    const auto& col = colors();
    const double now = frame::realNow();
    std::erase_if(toasts_, [&](const Toast& t) {
        return (t.dismissed && now - t.dismissedAt > kToastOut) || now - t.shownAt > kToastLife + kToastOut;
    });
    if (toasts_.empty()) return;
    frame::requestNext();
    for (size_t i = 0; i < toasts_.size(); ++i) {
        auto& t = toasts_[i];
        const double age = now - t.shownAt;
        float alpha = static_cast<float>(std::min(1.0, age / kToastIn));
        float dy = (1 - ease(Ease::Decelerate, alpha)) * 24;
        const double outStart = t.dismissed ? t.dismissedAt : t.shownAt + kToastLife;
        if (now > outStart) alpha = 1 - static_cast<float>(std::min(1.0, (now - outStart) / kToastOut));
        const Rect r = toastRect(i).offset(0, dy);
        c.pushOpacity(alpha);
        c.shadow(r, 2, 24, 8, col.shadowToast);
        c.fillRounded(r, 2, col.bgElevated);
        c.strokeRounded(r, 2, col.hairStrong);
        const Color ic = t.kind == ToastKind::Error     ? col.error
                         : t.kind == ToastKind::Warning ? col.warning
                         : t.kind == ToastKind::Success ? col.success
                                                        : accent().base;
        const char* icon = t.kind == ToastKind::Error     ? "error"
                           : t.kind == ToastKind::Warning ? "warning"
                           : t.kind == ToastKind::Success ? "check"
                                                          : "info";
        c.icon(icon, {r.x + 16, r.cy() - 8, 16, 16}, ic);
        float right = r.right() - 16;
        if (!t.action.empty()) {
            const float aw = std::ceil(t.action.measure().w);
            c.text(t.action, {right - aw, r.y, aw + 1, r.h}, accent().base, gfx::VAlign::Center);
            right -= aw + 16;
        }
        c.text(t.text, {r.x + 16 + 16 + 12, r.y, right - (r.x + 44), r.h}, col.fgPrimary, gfx::VAlign::Center);
        // 2 px progress line.
        const float prog = 1 - static_cast<float>(std::clamp(age / kToastLife, 0.0, 1.0));
        c.fillRect({r.x, r.bottom() - 2, r.w * prog, 2}, accent().base);
        c.popLayer();
    }
}

// ===================================================================================================
// Dialog

Dialog* Dialog::open(Window* window, std::wstring title, std::wstring body, float width) {
    if (!window) return nullptr;
    return static_cast<Dialog*>(window->pushOverlay(std::make_unique<Dialog>(std::move(title), std::move(body), width), true));
}

void Dialog::confirm(Window* window, std::wstring title, std::wstring body, std::wstring okLabel,
                     std::function<void()> onOk, bool destructive) {
    auto* d = open(window, std::move(title), std::move(body));
    if (!d) return;
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    d->addButton(std::move(okLabel), destructive ? ButtonKind::Secondary : ButtonKind::Primary, std::move(onOk));
}

Dialog::Dialog(std::wstring title, std::wstring body, float width) : width_(width) {
    title_ = add<Label>(std::move(title), type::title, Tone::Primary);
    title_->setWrap(true);
    if (!body.empty()) {
        text_ = add<Label>(std::move(body), type::bodyRegular, Tone::Secondary);
        text_->setWrap(true);
    }
    body_ = add<Column>(8.f);
    open_.to(1, motion::medium, Ease::Decelerate);
}

Button* Dialog::addButton(std::wstring label, ButtonKind kind, std::function<void()> onClick, bool closes) {
    auto* b = add<Button>(kind, std::move(label));
    b->onClick = [this, onClick = std::move(onClick), closes] {
        if (closes) close();
        if (onClick) onClick();
    };
    buttons_.push_back(b);
    requestLayout();
    return b;
}

void Dialog::close() {
    if (closing_) return;
    closing_ = true;
    if (onClosed) onClosed();
    if (auto* w = window()) w->removeOverlay(this);
}

void Dialog::layout() {
    const Rect r = rect();
    const float pad = 28, w = std::min(width_, r.w - 32);
    const float inner = w - pad * 2;
    float h = pad;
    const float titleH = title_->preferredHeight(inner);
    h += titleH;
    float textH = 0;
    if (text_) {
        textH = text_->preferredHeight(inner);
        h += 10 + textH;
    }
    const float bodyH = body_->preferredHeight(inner);
    const float maxBody = r.h - 160 - h;
    const float bodyShown = std::min(bodyH, std::max(0.f, maxBody));
    if (bodyShown > 0) h += 20 + bodyShown;
    if (!buttons_.empty()) h += 28 + 40;
    h += pad;
    panel_ = {r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h};
    float y = panel_.y + pad;
    title_->setRect({panel_.x + pad, y, inner, titleH});
    y += titleH;
    if (text_) {
        y += 10;
        text_->setRect({panel_.x + pad, y, inner, textH});
        y += textH;
    }
    if (bodyShown > 0) {
        y += 20;
        body_->setRect({panel_.x + pad, y, inner, bodyShown});
        y += bodyShown;
    }
    float bx = panel_.right() - pad;
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
        const float bw = std::max(88.f, (*it)->naturalWidth());
        bx -= bw;
        (*it)->setRect({bx, panel_.bottom() - pad - 40, bw, 40});
        bx -= 8;
    }
}

bool Dialog::onMouseDown(const MouseEvent& e) {
    if (!panel_.contains(e.pos)) close();   // click on the scrim dismisses
    return true;
}

Button* Dialog::defaultButton() const {
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it)
        if ((*it)->kind() == ButtonKind::Primary && (*it)->visible() && (*it)->enabled()) return *it;
    return buttons_.empty() ? nullptr : buttons_.back();   // e.g. a destructive confirm ("Sil", Secondary)
}

bool Dialog::onKeyDown(const KeyEvent& e) {
    // Reached when the focused widget did not take the key: a keyboard-focused button handles Enter itself, a
    // field submits (or passes Enter on when it has no submit handler).
    if (e.vk == VK_ESCAPE) {   // cancel
        close();
        return true;
    }
    if (e.vk == VK_RETURN && !e.ctrl && !e.alt) {
        if (e.repeat) return true;
        Button* b = defaultButton();
        if (b && b->enabled() && b->onClick) {
            const auto click = b->onClick;   // closes the dialog (deferred delete) then runs the action
            click();
            return true;
        }
    }
    return false;
}

void Dialog::paint(Canvas& c) {
    const auto& col = colors();
    const float t = open_;
    c.fillRect(rect(), col.scrim.mulAlpha(t));
    c.pushOpacity(t);
    c.pushScale(0.98f + 0.02f * t, {panel_.cx(), panel_.cy()});
    c.shadow(panel_, 2, 64, 32, col.shadowDialog);
    c.fillRounded(panel_, 2, col.bgRaised);
    c.strokeRounded(panel_, 2, col.hairStrong);
    paintChildren(c);
    c.popTransform();
    c.popLayer();
}

} // namespace st::ui
