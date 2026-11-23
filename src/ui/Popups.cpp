#include "ui/Popups.h"

#include "core/I18n.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace st::ui {

using gfx::colors;
using gfx::accent;
namespace type = gfx::type;
using overlay::Elevation;

// ===================================================================================================
// Menu

namespace {
constexpr float kMinW = 240, kMaxW = 360, kItemH = 34, kPadY = 6, kSepH = 13, kPadX = 12;
constexpr float kInset = 6;                      // highlight inset from the panel's edges
constexpr float kIcon = 16, kIconGap = 10;       // icon column
constexpr float kTrail = 16, kTrailGap = 8;      // check mark / submenu chevron column
constexpr float kHintGap = 24;                   // between the label and the shortcut hint
constexpr float kEdge = 8;                       // distance kept from the window's edges
constexpr float kSubGap = 2;                     // between a panel and its submenu
constexpr double kOpenMs = 150, kCloseMs = 100, kSubOpenMs = 140, kSubCloseMs = 90;
constexpr double kHoverOpenMs = 200, kHoverCloseMs = 300;   // submenu hover timers
constexpr double kTypeAheadMs = 800;

// An item's tooltip: an invisible child over the item, so the Window's hover delay and placement apply. The pointer
// still drives the menu's own highlight.
class TipZone : public Widget {
public:
    TipZone(Menu* menu, std::wstring tip) : menu_(menu) { setTooltip(std::move(tip)); }
    void paint(Canvas&) override {}
    void onMouseMove(const MouseEvent& e) override {
        MouseEvent m = e;
        m.pos = menu_->fromWindow(e.windowPos);
        menu_->onMouseMove(m);
    }

private:
    Menu* menu_;
};

// The label as shown: a submenu item drops a trailing ellipsis ("Çalma listesine ekle…" + chevron).
std::wstring shownLabel(const MenuItem& it) {
    std::wstring s = it.label;
    if (it.submenu) {
        while (!s.empty() && (s.back() == L'…' || s.back() == L' ')) s.pop_back();
        if (s.size() >= 3 && s.compare(s.size() - 3, 3, L"...") == 0) s.resize(s.size() - 3);
    }
    return s;
}

// Case and accent insensitive "starts with" in the user's locale (type-ahead: "s" finds "Şarkı radyosu").
bool startsWith(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.empty() || text.size() < prefix.size()) return false;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, text.data(),
                           static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), nullptr,
                           nullptr, 0) == CSTR_EQUAL;
}
} // namespace

struct Menu::Panel {
    std::vector<MenuItem> items;
    std::vector<gfx::Text> labels, hints;
    std::vector<Widget*> tips;     // per item: its tooltip zone (children of the menu), nullptr = none
    Panel* parent = nullptr;       // the panel this submenu belongs to
    int parentItem = -1;           // ...and the item that opened it
    Rect box{};
    int hot = -1;
    bool upward = false, leftward = false;   // root: opened above / left of the anchor; submenu: on the left
    bool iconCol = false, trailCol = false;
    float hintW = 0;
    Anim open;                     // 0 -> 1 opening, -> 0 a submenu closing
    Anim hlY, hlA;                 // highlight: position (glides between items) and opacity
    bool closing = false;
};

Menu* Menu::open(Window* window, Point anchor, std::vector<MenuItem> items) {
    if (!window || items.empty()) return nullptr;
    auto* menu = static_cast<Menu*>(window->pushOverlay(std::make_unique<Menu>(std::move(items), anchor), true));
    // Opened from the keyboard (Enter on a focused button, menu key): highlight the first item so Enter works right
    // away. A mouse-opened menu waits for the pointer, as before.
    if (window->focusVisible()) menu->moveHot(1, true);
    return menu;
}

Menu::Menu(std::vector<MenuItem> items, Point anchor) : anchor_(anchor) {
    panels_.push_back(makePanel(std::move(items), nullptr, -1));
    panels_.back()->open.to(1, overlay::duration(kOpenMs), Ease::Decelerate);
}

Menu::~Menu() = default;

std::unique_ptr<Menu::Panel> Menu::makePanel(std::vector<MenuItem> items, Panel* parent, int parentItem) {
    auto p = std::make_unique<Panel>();
    p->items = std::move(items);
    p->parent = parent;
    p->parentItem = parentItem;
    for (auto& it : p->items) {
        p->labels.emplace_back(shownLabel(it), type::secondary);
        p->hints.emplace_back(it.shortcut, type::monoMeta);
        p->tips.push_back(it.tooltip.empty() || it.separator ? nullptr : add<TipZone>(this, it.tooltip));
        if (it.separator) continue;
        p->iconCol = p->iconCol || !it.icon.empty();
        p->trailCol = p->trailCol || it.checked || it.submenu;
    }
    for (auto& h : p->hints)
        if (!h.empty()) p->hintW = std::max(p->hintW, std::ceil(h.measure().w));
    return p;
}

void Menu::layoutPanel(Panel& p) {
    const Rect r = rect();
    float h = kPadY * 2, content = 0;
    for (size_t i = 0; i < p.items.size(); ++i) {
        const auto& it = p.items[i];
        h += it.separator ? kSepH : kItemH;
        if (it.separator) continue;
        content = std::max(content, std::ceil(p.labels[i].measure().w));
    }
    float w = kPadX + (p.iconCol ? kIcon + kIconGap : 0) + content + (p.hintW > 0 ? kHintGap + p.hintW : 0) +
              (p.trailCol ? kTrailGap + kTrail : 0) + kPadX;
    w = std::clamp(std::ceil(w), kMinW, std::max(kMinW, std::min(kMaxW, r.w - kEdge * 2)));
    float x, y;
    if (!p.parent) {
        p.leftward = anchor_.x + w > r.w - kEdge;
        p.upward = anchor_.y + h > r.h - kEdge;
        x = p.leftward ? anchor_.x - w : anchor_.x;
        y = p.upward ? std::max(kEdge, anchor_.y - h) : anchor_.y;
        x = std::max(kEdge, x);
    } else {
        // Beside its parent, first item level with the item that opened it; on the left when the right has no room.
        const Rect& pb = p.parent->box;
        const Rect item = itemRectOf(*p.parent, p.parentItem);
        p.leftward = pb.right() + kSubGap + w > r.w - kEdge && pb.x - kSubGap - w >= kEdge;
        x = p.leftward ? pb.x - kSubGap - w : std::min(pb.right() + kSubGap, r.w - kEdge - w);
        y = std::clamp(item.y - kPadY, kEdge, std::max(kEdge, r.h - kEdge - h));
    }
    p.box = {std::round(x), std::round(y), w, h};
    for (size_t i = 0; i < p.tips.size(); ++i)
        if (p.tips[i]) p.tips[i]->setRect(itemRectOf(p, static_cast<int>(i)));
}

void Menu::layout() {
    for (auto& p : panels_) layoutPanel(*p);   // parents first: a submenu is placed beside its parent
}

Rect Menu::itemRectOf(const Panel& p, int i) const {
    float y = p.box.y + kPadY;
    for (int k = 0; k < i; ++k) y += p.items[k].separator ? kSepH : kItemH;
    return {p.box.x + kInset, y, p.box.w - kInset * 2, p.items[i].separator ? kSepH : kItemH};
}

bool Menu::usable(const Panel& p, int i) const {
    return i >= 0 && i < static_cast<int>(p.items.size()) && !p.items[i].separator && p.items[i].enabled;
}

std::vector<Menu::Panel*> Menu::chain() const {
    std::vector<Panel*> out;
    for (auto& p : panels_)
        if (!p->closing) out.push_back(p.get());
    return out;
}

Menu::Panel* Menu::active() const {
    for (auto it = panels_.rbegin(); it != panels_.rend(); ++it)
        if (!(*it)->closing) return it->get();
    return nullptr;
}

Menu::Panel* Menu::panelAt(Point pos) const {
    for (auto it = panels_.rbegin(); it != panels_.rend(); ++it)
        if (!(*it)->closing && (*it)->box.contains(pos)) return it->get();
    return nullptr;
}

int Menu::itemAt(const Panel& p, Point pos) const {
    if (!p.box.contains(pos)) return -1;
    for (int i = 0; i < static_cast<int>(p.items.size()); ++i)
        if (usable(p, i) && itemRectOf(p, i).contains(pos)) return i;
    return -1;
}

void Menu::setHot(Panel& p, int i) {
    if (i == p.hot) return;
    p.hot = i;
    if (i >= 0) {
        const float y = itemRectOf(p, i).y;
        // Glides from where the highlight is (also mid-fade), appears in place when there was none.
        if (p.hlA.value() > 0.01f) p.hlY.to(y, overlay::duration(motion::fast), Ease::Standard);
        else p.hlY.snap(y);
        p.hlA.to(1, overlay::duration(80), Ease::Standard);
    } else {
        p.hlA.to(0, overlay::duration(motion::fast), Ease::Standard);
    }
    invalidate();
}

void Menu::moveHot(int dir, bool fromEdge) {
    Panel* p = active();
    if (!p) return;
    const int n = static_cast<int>(p->items.size());
    int i = (fromEdge || p->hot < 0) ? (dir > 0 ? -1 : n) : p->hot;
    for (int k = 0; k < n; ++k) {
        i = (i + dir + n) % n;
        if (usable(*p, i)) {
            setHot(*p, i);
            break;
        }
    }
    invalidate();
}

bool Menu::typeAhead(const KeyEvent& e) {
    if (e.ctrl || e.alt) return false;
    BYTE keys[256]{};
    if (!GetKeyboardState(keys)) return false;
    wchar_t buf[8]{};
    // Flag 4: leave the keyboard state alone (dead keys stay pending for the next real WM_CHAR).
    const int n = ToUnicodeEx(e.vk, MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC), keys, buf, 8, 4, GetKeyboardLayout(0));
    if (n != 1 || buf[0] <= L' ') return false;
    Panel* p = active();
    if (!p) return false;
    const double now = frame::realNow();
    if (now - typedAt_ > kTypeAheadMs) typed_.clear();
    typedAt_ = now;
    typed_ += buf[0];
    // The same letter again cycles through the items starting with it; a longer prefix stays on its first match.
    const bool cycle = typed_.size() > 1 && typed_.find_first_not_of(typed_[0]) == std::wstring::npos;
    const std::wstring prefix = cycle ? typed_.substr(0, 1) : typed_;
    const int count = static_cast<int>(p->items.size());
    const int start = typed_.size() == 1 || cycle ? p->hot + 1 : std::max(0, p->hot);
    for (int k = 0; k < count; ++k) {
        const int i = (start + k + count) % count;
        if (usable(*p, i) && startsWith(p->labels[i].text(), prefix)) {
            setHot(*p, i);
            break;
        }
    }
    return true;
}

void Menu::openSubmenu(Panel& p, int item, bool fromKeyboard) {
    if (!usable(p, item) || !p.items[item].submenu) return;
    pendingOpen_ = {};
    pendingClose_ = {};
    // Already open for this item: keyboard users go into it.
    for (auto& q : panels_) {
        if (q->closing || q->parent != &p || q->parentItem != item) continue;
        closeSubmenusAbove(q.get());
        if (fromKeyboard && q->hot < 0) moveHot(1, true);
        return;
    }
    closeSubmenusAbove(&p);
    setHot(p, item);
    auto items = p.items[item].submenu();
    if (items.empty()) return;
    panels_.push_back(makePanel(std::move(items), &p, item));
    Panel& sub = *panels_.back();
    layoutPanel(sub);
    sub.open.to(1, overlay::duration(kSubOpenMs), Ease::Decelerate);
    if (fromKeyboard) moveHot(1, true);
    invalidate();
}

void Menu::closeSubmenusAbove(Panel* p) {
    // Everything opened from p (directly or deeper) is after it in panels_.
    bool after = p == nullptr;
    for (auto& q : panels_) {
        if (q.get() == p) {
            after = true;
            continue;
        }
        if (!after || q->closing || !q->parent) continue;
        q->closing = true;
        q->open.to(0, overlay::duration(kSubCloseMs), Ease::Accelerate);
        for (Widget*& tip : q->tips) {
            if (tip) removeChild(tip);
            tip = nullptr;
        }
    }
    if (pendingOpen_.panel && pendingOpen_.panel->closing) pendingOpen_ = {};
    if (pendingClose_.panel && pendingClose_.panel->closing) pendingClose_ = {};
    invalidate();
}

void Menu::activate(Panel& p, int i) {
    if (!usable(p, i)) return;
    if (p.items[i].submenu) {
        openSubmenu(p, i, true);
        return;
    }
    auto action = p.items[i].action;
    close();
    if (action) action();
}

Widget* Menu::hitTest(Point p) {   // full-window: click outside closes
    if (!visible()) return nullptr;
    // An item's tooltip zone of the panel on top there (the menu covers the window from 0,0: its children share its
    // coordinates).
    if (const Panel* top = closing_ ? nullptr : panelAt(p))
        for (Widget* tip : top->tips)
            if (tip && tip->rect().contains(p)) return tip;
    return this;
}

bool Menu::onMouseDown(const MouseEvent& e) {
    if (!panelAt(e.pos)) {
        close();
        return false;
    }
    return true;
}

void Menu::onMouseUp(const MouseEvent& e) {
    Panel* p = panelAt(e.pos);
    if (!p) return;
    const int i = itemAt(*p, e.pos);
    if (i < 0) return;
    if (p->items[i].submenu) {
        openSubmenu(*p, i, false);
        return;
    }
    activate(*p, i);
}

void Menu::onMouseMove(const MouseEvent& e) {
    if (closing_) return;
    Panel* p = panelAt(e.pos);
    if (!p) {
        // Outside: the deepest panel loses its highlight, unless it is the parent item of an open submenu.
        if (Panel* a = active(); a && a->hot >= 0) setHot(*a, -1);
        pendingOpen_ = {};
        return;
    }
    const int i = itemAt(*p, e.pos);
    const auto open = chain();
    const bool hasChild = open.back() != p;   // a submenu of p (or deeper) is open
    if (hasChild) {
        Panel* child = nullptr;
        for (Panel* q : open)
            if (q->parent == p) child = q;
        if (i >= 0 && child && i == child->parentItem) {
            pendingClose_ = {};   // back on the item of the open submenu
        } else if (!pendingClose_.panel || pendingClose_.panel != p) {
            pendingClose_ = {p, -1, frame::realNow() + kHoverCloseMs};
            if (auto* w = window()) w->invalidateAfter(kHoverCloseMs + 1);
        }
        if (i < 0 && child) {
            setHot(*p, child->parentItem);   // padding / separator: the open submenu's item stays lit
        } else {
            setHot(*p, i);
        }
    } else {
        pendingClose_ = {};
        setHot(*p, i);
        // Entering a submenu: its parents keep the item that opened it lit.
        for (Panel* q = p; q->parent; q = q->parent)
            if (q->parent->hot != q->parentItem) setHot(*q->parent, q->parentItem);
    }
    if (i >= 0 && p->items[i].submenu) {
        const bool already = std::any_of(open.begin(), open.end(), [&](Panel* q) { return q->parent == p && q->parentItem == i; });
        if (!already && (pendingOpen_.panel != p || pendingOpen_.item != i)) {
            pendingOpen_ = {p, i, frame::realNow() + kHoverOpenMs};
            if (auto* w = window()) w->invalidateAfter(kHoverOpenMs + 1);
        }
    } else if (pendingOpen_.panel == p) {
        pendingOpen_ = {};
    }
}

void Menu::onMouseLeave() {
    if (closing_) return;
    pendingOpen_ = {};
    if (Panel* a = active(); a && a->hot >= 0) setHot(*a, -1);
}

void Menu::runTimers() {
    const double now = frame::realNow();
    if (pendingClose_.panel && now >= pendingClose_.at) {
        Panel* p = pendingClose_.panel;
        pendingClose_ = {};
        closeSubmenusAbove(p);
    }
    if (pendingOpen_.panel && now >= pendingOpen_.at) {
        const Pending o = pendingOpen_;
        pendingOpen_ = {};
        if (!o.panel->closing && o.panel->hot == o.item) openSubmenu(*o.panel, o.item, false);
    }
    const double next = std::min(pendingClose_.panel ? pendingClose_.at : 1e300, pendingOpen_.panel ? pendingOpen_.at : 1e300);
    if (next < 1e300)
        if (auto* w = window()) w->invalidateAfter(next - now + 1);
}

bool Menu::onKeyDown(const KeyEvent& e) {
    Panel* p = active();
    if (!p) return true;
    const bool sub = p->parent != nullptr;
    switch (e.vk) {
    case VK_ESCAPE:
        if (sub) {
            closeSubmenusAbove(p->parent);
            setHot(*p->parent, p->parentItem);
        } else {
            close();
        }
        return true;
    case VK_DOWN: moveHot(1, false); return true;
    case VK_UP: moveHot(-1, false); return true;
    case VK_TAB: moveHot(e.shift ? -1 : 1, false); return true;   // the Window hands Tab over (no tab stops here)
    case VK_HOME:
    case VK_PRIOR: moveHot(1, true); return true;
    case VK_END:
    case VK_NEXT: moveHot(-1, true); return true;
    case VK_RIGHT:
        if (usable(*p, p->hot) && p->items[p->hot].submenu) openSubmenu(*p, p->hot, true);
        return true;
    case VK_LEFT:
        if (sub) {
            Panel* parent = p->parent;
            const int item = p->parentItem;
            closeSubmenusAbove(parent);
            setHot(*parent, item);
        }
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (!e.repeat) activate(*p, p->hot);
        return true;
    default:
        typeAhead(e);
        return true;   // swallow while open
    }
}

void Menu::close() {
    if (closing_) return;
    closing_ = true;
    pendingOpen_ = pendingClose_ = {};
    exit_.snap(1);
    exit_.to(0, overlay::duration(kCloseMs), Ease::Accelerate);
    if (auto* w = window()) w->removeOverlay(this);
}

bool Menu::exiting() const { return closing_ && exit_.running(); }

void Menu::prepareBackdrop(Canvas& c, Window& w) {
    Rect area{};
    for (auto& p : panels_) {
        if (p->box.empty()) continue;
        if (area.empty()) {
            area = p->box;
            continue;
        }
        const float l = std::min(area.x, p->box.x), t = std::min(area.y, p->box.y);
        const float r = std::max(area.right(), p->box.right()), b = std::max(area.bottom(), p->box.bottom());
        area = {l, t, r - l, b - t};
    }
    material_.capture(c, w, area, overlay::surfaceColor(Elevation::Menu));
}

void Menu::paint(Canvas& c) {
    if (!closing_) runTimers();
    // Submenus that have faded out.
    std::erase_if(panels_, [&](const std::unique_ptr<Panel>& p) {
        if (!p->closing || p->open.value() > 0.001f) return false;
        if (pendingOpen_.panel == p.get()) pendingOpen_ = {};
        if (pendingClose_.panel == p.get()) pendingClose_ = {};
        return true;
    });
    for (auto& p : panels_) paintPanel(c, *p);
}

void Menu::paintPanel(Canvas& c, Panel& p) {
    const auto& col = colors();
    const float t = std::clamp(p.open.value(), 0.f, 1.f);
    const float x = closing_ ? std::clamp(exit_.value(), 0.f, 1.f) : 1.f;
    const float alpha = t * x;
    if (alpha <= 0.001f) return;
    const bool group = alpha < 0.999f;
    if (group) c.pushOpacity(alpha);
    bool transformed = false;
    if (!p.parent) {
        // Scales from the corner at the anchor; closing shrinks a little towards it.
        const float s = (0.96f + 0.04f * t) * (0.98f + 0.02f * x);
        if (s < 0.9995f) {
            const Point origin{p.leftward ? p.box.right() : p.box.x, p.upward ? p.box.bottom() : p.box.y};
            c.pushScale(s, origin);
            transformed = true;
        }
    } else if (t < 0.999f) {
        // Slides out of its parent's side.
        const float dx = (1 - t) * 8 * (p.leftward ? 1.f : -1.f);
        c.pushTransform(D2D1::Matrix3x2F::Translation(dx, 0));
        transformed = true;
    }
    paintOverlaySurface(c, p.box, Elevation::Menu, &material_);
    const float rad = overlay::radius();
    // The highlight, gliding between items.
    if (const float ha = p.hlA.value(); ha > 0.001f && p.hot >= 0) {
        const Rect r = itemRectOf(p, p.hot);
        c.fillRounded({r.x, p.hlY.value(), r.w, kItemH}, rad, col.overlayHover.mulAlpha(ha));
    }
    for (int i = 0; i < static_cast<int>(p.items.size()); ++i) {
        const auto& it = p.items[i];
        const Rect r = itemRectOf(p, i);
        if (it.separator) {
            c.hline(r.x, r.right(), r.cy(), col.hairDefault);
            continue;
        }
        const bool hot = i == p.hot && it.enabled;
        Color fg = it.destructive ? col.error : (hot ? col.fgPrimary : col.fgSecondary);
        if (!it.enabled) fg = col.fgDisabled;
        float lx = p.box.x + kPadX;
        if (p.iconCol) {
            if (!it.icon.empty())
                c.icon(it.icon, {lx, r.cy() - kIcon * 0.5f, kIcon, kIcon}, it.checked && it.enabled ? accent().base : fg);
            lx += kIcon + kIconGap;
        }
        float right = p.box.right() - kPadX;
        if (p.trailCol) {
            const Rect tr{right - kTrail, r.cy() - kTrail * 0.5f, kTrail, kTrail};
            if (it.submenu) c.icon("chevron-right", tr, it.enabled ? (hot ? col.fgPrimary : col.fgTertiary) : col.fgDisabled);
            else if (it.checked) c.icon("check", tr, it.enabled ? accent().base : col.fgDisabled);
            right -= kTrail + kTrailGap;
        }
        if (!p.hints[i].empty()) {
            const float hw = std::ceil(p.hints[i].measure().w);
            c.text(p.hints[i], {right - hw, r.y, hw + 1, r.h}, it.enabled ? col.fgTertiary : col.fgDisabled, gfx::VAlign::Center);
        }
        if (p.hintW > 0) right -= p.hintW + kHintGap;
        c.text(p.labels[i], {lx, r.y, std::max(0.f, right - lx), r.h}, fg, gfx::VAlign::Center);
    }
    if (transformed) c.popTransform();
    if (group) c.popLayer();
}

int Menu::depth() const { return static_cast<int>(chain().size()); }

int Menu::highlighted(int level) const {
    const auto open = chain();
    return level >= 0 && level < static_cast<int>(open.size()) ? open[level]->hot : -1;
}

Rect Menu::panelRect(int level) const {
    const auto open = chain();
    return level >= 0 && level < static_cast<int>(open.size()) ? open[level]->box : Rect{};
}

Rect Menu::itemRect(int level, int item) const {
    const auto open = chain();
    if (level < 0 || level >= static_cast<int>(open.size())) return {};
    const Panel& p = *open[level];
    return item >= 0 && item < static_cast<int>(p.items.size()) ? itemRectOf(p, item) : Rect{};
}

// ===================================================================================================
// Toasts

namespace {
std::unordered_map<Window*, Toasts*> g_toastHosts;
constexpr double kToastLife = 4000, kToastIn = 240, kToastOut = 180, kToastRestack = 220;
constexpr float kToastW = 360, kToastMinH = 56, kToastGap = 8, kToastPadX = 16;
constexpr size_t kMaxToasts = 3;
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
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = 3;
    Toast t{gfx::Text(std::move(message), type::secondary.withWeight(500), wrap),
            gfx::Text(std::move(actionLabel), type::body.withSize(13)), std::move(action), kind, frame::realNow()};
    host->toasts_.push_back(std::move(t));
    // A fourth one sends the oldest still showing on its way out.
    size_t live = 0;
    for (const auto& x : host->toasts_) live += x.dismissed ? 0 : 1;
    for (auto& x : host->toasts_) {
        if (live <= kMaxToasts) break;
        if (x.dismissed) continue;
        x.dismissed = true;
        x.dismissedAt = frame::realNow();
        --live;
    }
    host->updatedAt_ = -1;
    host->invalidate();
}

// Ages, heights and places of this frame's toasts (idempotent within a frame).
void Toasts::update() {
    const double now = frame::now();
    if (now == updatedAt_) return;
    updatedAt_ = now;
    const double out = overlay::duration(kToastOut), in = overlay::duration(kToastIn);
    std::erase_if(toasts_, [&](const Toast& t) {
        const double outStart = t.dismissed ? t.dismissedAt : t.shownAt + kToastLife;
        return now - outStart > out + 1;
    });
    const Rect r = rect();
    const float w = std::min(kToastW, r.w - 32);
    // Bottom to top: the newest at the bottom; a leaving toast keeps its place while the others close the gap.
    float lift = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        Toast& t = *it;
        const float textW = w - kToastPadX - 16 - 12 - kToastPadX - (t.action.empty() ? 0 : std::ceil(t.action.measure().w) + 16);
        t.h = std::max(kToastMinH, std::ceil(t.text.measure(std::max(40.f, textW)).h) + 32);
        const double outStart = t.dismissed ? t.dismissedAt : t.shownAt + kToastLife;
        const bool leaving = now >= outStart;
        if (!leaving) {
            if (t.lift.target() != lift) t.lift.to(lift, overlay::duration(kToastRestack), Ease::Standard);
            lift += t.h + kToastGap;
        }
        float dy = 0, alpha = 1;
        if (now < t.shownAt + in) {   // slides in: 24 DIP up + fade (decelerate)
            const float k = static_cast<float>((now - t.shownAt) / in);
            dy = (1 - ease(Ease::Decelerate, k)) * 24;
            alpha = std::clamp(k, 0.f, 1.f);
        }
        if (leaving) {                // slides out: fade + 12 DIP down (accelerate)
            const float k = out > 0 ? static_cast<float>(std::clamp((now - outStart) / out, 0.0, 1.0)) : 1.f;
            dy += ease(Ease::Accelerate, k) * 12;
            alpha *= 1 - k;
        }
        t.alpha = alpha;
        const float bottom = r.bottom() - bottomInset - t.lift.value() + dy;
        t.rect = {std::round(r.cx() - w * 0.5f), bottom - t.h, w, t.h};
    }
}

Widget* Toasts::hitTest(Point p) {
    update();
    for (const auto& t : toasts_)
        if (!t.action.empty() && !t.dismissed && t.alpha > 0.5f && t.rect.contains(p)) return this;
    return nullptr;
}

bool Toasts::onMouseDown(const MouseEvent&) { return true; }

void Toasts::onMouseUp(const MouseEvent& e) {
    for (auto& t : toasts_) {
        if (!t.dismissed && t.rect.contains(e.pos) && t.onAction) {
            auto a = t.onAction;
            t.dismissed = true;
            t.dismissedAt = frame::realNow();
            updatedAt_ = -1;
            invalidate();
            a();
            return;
        }
    }
}

void Toasts::prepareBackdrop(Canvas& c, Window& w) {
    update();
    if (toasts_.empty()) return material_.release();
    // The whole stack's column (with room for the slide-in below it): restacking stays inside one capture.
    Rect area = toasts_.front().rect;
    for (const auto& t : toasts_) {
        const float top = std::min(area.y, t.rect.y), bottom = std::max(area.bottom(), t.rect.bottom() + 24);
        area = {area.x, top, area.w, bottom - top};
    }
    material_.capture(c, w, area, overlay::surfaceColor(Elevation::Toast));
}

void Toasts::paint(Canvas& c) {
    update();
    if (toasts_.empty()) {
        material_.release();   // nothing on screen: the backdrop goes
        return;
    }
    frame::requestNext();   // progress lines and slides run at vsync while a toast is up
    const auto& col = colors();
    // Oldest first: a toast sliding in from below passes under the ones above it.
    for (auto& t : toasts_) {
        if (t.alpha <= 0.001f) continue;
        const Rect r = t.rect;
        c.pushOpacity(t.alpha);
        paintOverlaySurface(c, r, Elevation::Toast, &material_);
        const Color ic = t.kind == ToastKind::Error     ? col.error
                         : t.kind == ToastKind::Warning ? col.warning
                         : t.kind == ToastKind::Success ? col.success
                                                        : accent().base;
        const char* icon = t.kind == ToastKind::Error     ? "error"
                           : t.kind == ToastKind::Warning ? "warning"
                           : t.kind == ToastKind::Success ? "check"
                                                          : "info";
        c.icon(icon, {r.x + kToastPadX, r.cy() - 8, 16, 16}, ic);
        float right = r.right() - kToastPadX;
        if (!t.action.empty()) {
            const float aw = std::ceil(t.action.measure().w);
            c.text(t.action, {right - aw, r.y, aw + 1, r.h}, accent().base, gfx::VAlign::Center);
            right -= aw + 16;
        }
        const float tx = r.x + kToastPadX + 16 + 12;
        const float tw = std::max(40.f, right - tx);
        const float th = std::ceil(t.text.measure(tw).h);
        c.text(t.text, {tx, std::round(r.cy() - th * 0.5f), tw, th + 1}, col.fgPrimary);
        // 2 px progress line along the bottom edge.
        const double age = frame::now() - t.shownAt;
        const float prog = t.dismissed ? 0.f : 1 - static_cast<float>(std::clamp(age / kToastLife, 0.0, 1.0));
        if (prog > 0) c.fillRect({r.x, r.bottom() - 2, r.w * prog, 2}, accent().base);
        c.popLayer();
    }
}

// ===================================================================================================
// Dialog

namespace {
constexpr double kDialogCloseMs = 140;
}

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
    open_.to(1, overlay::duration(motion::medium), Ease::Decelerate);
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
    exit_.snap(1);
    exit_.to(0, overlay::duration(kDialogCloseMs), Ease::Accelerate);
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
    panel_ = {std::round(r.cx() - w * 0.5f), std::round(r.cy() - h * 0.5f), w, h};
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

void Dialog::prepareBackdrop(Canvas& c, Window& w) {
    material_.capture(c, w, panel_, overlay::surfaceColor(Elevation::Dialog));
}

void Dialog::paint(Canvas& c) {
    const auto& col = colors();
    const float t = std::clamp(open_.value(), 0.f, 1.f);
    const float x = closing_ ? std::clamp(exit_.value(), 0.f, 1.f) : 1.f;
    const float e = ease(Ease::Decelerate, t);
    c.fillRect(rect(), col.scrim.mulAlpha(t * x));
    const float alpha = t * x;
    if (alpha <= 0.001f) return;
    // Rises 8 DIP into place (and grows from 0.98) as it fades in; drops 6 DIP as it fades out.
    const float dy = (1 - e) * 8 + (1 - x) * 6;
    const float s = 0.98f + 0.02f * t;
    const bool group = alpha < 0.999f, moved = dy > 0.01f || s < 0.9995f;
    if (group) c.pushOpacity(alpha);
    if (moved)
        c.pushTransform(D2D1::Matrix3x2F::Scale(s, s, {panel_.cx(), panel_.cy()}) * D2D1::Matrix3x2F::Translation(0, dy));
    paintOverlaySurface(c, panel_, Elevation::Dialog, &material_);
    paintChildren(c);
    if (moved) c.popTransform();
    if (group) c.popLayer();
}

} // namespace st::ui
