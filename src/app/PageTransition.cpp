#include "app/PageTransition.h"

#include "app/AppContext.h"
#include "core/Settings.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace st::app {

namespace {

PageTransition* g_current = nullptr;   // the main window's page host (there is one)

// A cover armed by the card / row that is about to navigate.
struct Armed {
    SharedCover cover;
    double at = 0;
};
std::optional<Armed> g_armed;

// Where a page was opened from by a flying cover: Back from `page` to `from` flies it back to a cover with `key` on
// that page (nearest `rect`, where it was), Forward again from the same cover.
struct Origin {
    Route page, from;
    std::string key;
    Rect rect;
};
std::vector<Origin> g_origins;   // newest last, a few dozen

void remember(const Route& page, const Route& from, const SharedCover& c) {
    if (c.key.empty()) return;
    std::erase_if(g_origins, [&](const Origin& o) { return o.page == page && o.from == from; });
    g_origins.push_back({page, from, c.key, c.rect});
    if (g_origins.size() > 32) g_origins.erase(g_origins.begin());
}

const Origin* findOrigin(const Route& page, const Route& from) {
    for (auto it = g_origins.rbegin(); it != g_origins.rend(); ++it)
        if (it->page == page && it->from == from) return &*it;
    return nullptr;
}

// Dev: SHADETUBE_MOTION_SCALE=<factor> stretches page transitions (slow motion for screenshots of their middle).
double motionScale() {
    static const double scale = [] {
        wchar_t v[16] = {};
        if (GetEnvironmentVariableW(L"SHADETUBE_MOTION_SCALE", v, 16) == 0) return 1.0;
        return std::clamp(_wtof(v), 1.0, 50.0);
    }();
    return scale;
}

float dipScale() { return ctx().window ? ctx().window->scale() : 1.f; }

// Dev (docs/DEVELOPMENT.md 5.4): SHADETUBE_NAV_DEMO="<step>;<step>…", each <action>@<ms>[=<arg>] run <ms> after the
// shell opened: click=<x>,<y> (a real left click posted at window DIPs: move, down, up), back, forward. With
// SHADETUBE_MOTION_SCALE and --screenshot it captures the middle of a transition.
void scheduleNavDemo() {
    static bool once = false;
    wchar_t spec[1024] = {};
    if (once || !GetEnvironmentVariableW(L"SHADETUBE_NAV_DEMO", spec, 1024)) return;
    once = true;
    struct Step {
        std::wstring action, arg;
    };
    static std::vector<std::pair<UINT_PTR, Step>> steps;
    const std::wstring all = spec;
    for (size_t start = 0; start < all.size();) {
        const size_t end = std::min(all.find(L';', start), all.size());
        const std::wstring s = all.substr(start, end - start);
        start = end + 1;
        const size_t at = s.find(L'@');
        if (at == std::wstring::npos) continue;
        const size_t eq = s.find(L'=', at);
        Step step{s.substr(0, at), eq == std::wstring::npos ? std::wstring() : s.substr(eq + 1)};
        const UINT ms = static_cast<UINT>(std::max(1, _wtoi(s.c_str() + at + 1)));
        const UINT_PTR id = SetTimer(nullptr, 0, ms, [](HWND, UINT, UINT_PTR timer, DWORD) {
            KillTimer(nullptr, timer);
            const auto it = std::find_if(steps.begin(), steps.end(), [&](const auto& p) { return p.first == timer; });
            if (it == steps.end()) return;
            const Step st = it->second;
            steps.erase(it);
            auto* w = ctx().window;
            if (!w || !ctx().router) return;
            if (st.action == L"back") ctx().router->back();
            else if (st.action == L"forward") ctx().router->forward();
            else if (st.action == L"click") {
                const size_t comma = st.arg.find(L',');
                if (comma == std::wstring::npos) return;
                const float s = w->scale();
                const int x = static_cast<int>(_wtof(st.arg.c_str()) * s), y = static_cast<int>(_wtof(st.arg.c_str() + comma + 1) * s);
                const LPARAM lp = MAKELPARAM(x, y);
                PostMessageW(w->hwnd(), WM_MOUSEMOVE, 0, lp);
                PostMessageW(w->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, lp);
                PostMessageW(w->hwnd(), WM_LBUTTONUP, 0, lp);
            }
        });
        if (id) steps.emplace_back(id, std::move(step));
    }
}

int pixelsFor(const Rect& r, float scale) { return static_cast<int>(std::ceil(std::max(r.w, r.h) * scale)); }

// Something the flight can draw from the first frame: a painter, or an image that is already loaded.
bool flyable(const SharedCover& c) { return !c.rect.empty() && (c.paint || (!c.images.empty() && c.ready)); }

// At least half of `r` is inside `area`.
bool mostlyInside(const Rect& r, const Rect& area) {
    const Rect i = r.intersect(area);
    return !r.empty() && i.w * i.h >= r.w * r.h * 0.5f;
}

// The covers in `root` (CoverSource widgets) showing `key`, the one nearest `hint` that is on screen in `area`.
bool findCover(ui::Widget& root, const std::string& key, const Rect& hint, const Rect& area, SharedCover& out) {
    float best = 1e30f;
    bool found = false;
    std::function<void(ui::Widget&)> walk = [&](ui::Widget& w) {
        if (!w.visible()) return;
        if (auto* src = dynamic_cast<CoverSource*>(&w)) {
            SharedCover c;
            if (src->coverFor(key, c) && mostlyInside(c.rect, area)) {
                const float dx = c.rect.cx() - hint.cx(), dy = c.rect.cy() - hint.cy();
                const float d = dx * dx + dy * dy;
                if (d < best) {
                    best = d;
                    out = std::move(c);
                    found = true;
                }
            }
        }
        for (const auto& child : w.children()) walk(*child);
    };
    walk(root);
    return found;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// pagetx

namespace pagetx {

bool enabled() {
    if (ui::motion::reduced() || !Settings::get().pageTransitions) return false;
    BOOL on = TRUE;   // Windows › Accessibility › Visual effects › Animation effects
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) && !on) return false;
    return true;
}

void arm(SharedCover cover) { g_armed = Armed{std::move(cover), ui::frame::realNow()}; }

float presence(const void* owner) { return g_current && owner ? g_current->presenceOf(owner) : 1.f; }

bool coverIncoming() { return g_current && g_current->coverIncoming(); }

SharedCover imageCover(const ui::Widget& owner, const Rect& local, float radius, const std::vector<catalog::Image>& images) {
    SharedCover s;
    s.rect = owner.toWindow(local);
    s.radius = radius;
    s.images = images;
    s.owner = &owner;
    if (!images.empty()) s.key = images.front().url;
    const int px = pixelsFor(local, dipScale());
    const auto* img = catalog::pickImage(images, px);
    s.ready = img && !img->url.empty() && gfx::ImageCache::get().request(img->url, px) != nullptr;
    return s;
}

void drawLikedTile(gfx::Canvas& c, const Rect& r, float radius) {
    const auto& acc = gfx::accent();
    c.fillRounded(r, radius, acc.base);
    const float s = std::max(16.f, std::round(std::min(r.w, r.h) * 0.31f));
    c.icon("heart-filled", r.center(s, s), acc.onAccent);
}

SharedCover likedCover(const ui::Widget& owner, const Rect& local, float radius) {
    SharedCover s;
    s.rect = owner.toWindow(local);
    s.radius = radius;
    s.owner = &owner;
    s.key = "liked";
    s.paint = [](gfx::Canvas& c, const Rect& r, float rad) { drawLikedTile(c, r, rad); };
    return s;
}

} // namespace pagetx

// ---------------------------------------------------------------------------------------------------
// PageTransition

PageTransition::PageTransition() {
    g_current = this;
    scheduleNavDemo();
}

PageTransition::~PageTransition() {
    if (g_current == this) g_current = nullptr;
}

float PageTransition::presenceOf(const void* owner) const {
    if (!flight_.active()) return 1.f;
    if (owner == src_.owner) return 0.f;
    if (owner == dst_.owner) return flight_.targetPresence();
    return 1.f;
}

bool PageTransition::coverIncoming() const {
    using P = ui::CoverFlight::Phase;
    return target_ == Target::Slot && (flight_.phase() == P::Fly || flight_.phase() == P::Hold);
}

void PageTransition::dropOutgoing(ui::Widget& host) {
    if (!out_) return;
    // Destroyed before the next frame (the page may still be inside an event of its own, e.g. the click that left it).
    if (auto* w = host.window()) w->deferDelete(std::move(out_));
    else out_.reset();
}

void PageTransition::begin(ui::Widget& host, Page* outgoing, Page* incoming, const Router::Change& change) {
    const double now = ui::frame::now();
    // An interrupted change: its outgoing page goes now; the page that was arriving leaves from the opacity it had.
    float fromAlpha = 1;
    if (motion_.running(now)) fromAlpha = std::max(0.f, motion_.incoming(now).alpha);
    dropOutgoing(host);
    flight_.cancel();
    target_ = Target::None;
    src_ = dst_ = {};
    std::optional<Armed> armed;
    armed.swap(g_armed);   // an armed cover belongs to this change only
    hostWin_ = host.toWindow(host.rect());

    const bool animate = outgoing && pagetx::enabled();
    if (!animate) {
        motion_.stop();
        if (outgoing) host.removeChild(outgoing);   // released (focus forgotten) and destroyed after the event
        if (!outgoing && pagetx::enabled()) {       // the first page fades in
            nav_ = ui::PageNav::Replace;
            start_ = ui::frame::realNow();
            scale_ = motionScale();
            motion_.start(nav_, false, start_, scale_);
        }
        return;
    }

    using Nav = Router::Nav;
    nav_ = change.nav == Nav::Back ? ui::PageNav::Back : change.nav == Nav::Replace ? ui::PageNav::Replace : ui::PageNav::Push;
    start_ = ui::frame::realNow();
    scale_ = motionScale();

    // The shared cover: the source is read while the outgoing page is still in the tree (window coordinates).
    if (change.nav == Nav::Push && armed && start_ - armed->at < 1000 && flyable(armed->cover)) {
        src_ = std::move(armed->cover);
        target_ = Target::Slot;
        remember(change.to, change.from, src_);
    } else if (change.nav == Nav::Back) {
        if (const Origin* o = findOrigin(change.from, change.to)) {
            SharedCover s;
            if (outgoing->coverSlot(s) && s.ready && flyable(s) && mostlyInside(s.rect, hostWin_)) {
                src_ = std::move(s);
                target_ = Target::Match;
                matchKey_ = o->key;
                matchNear_ = o->rect;
            }
        }
    } else if (change.nav == Nav::Forward) {
        if (const Origin* o = findOrigin(change.to, change.from)) {
            SharedCover s;
            if (findCover(*outgoing, o->key, o->rect, hostWin_, s) && flyable(s)) {
                src_ = std::move(s);
                target_ = Target::Slot;
            }
        }
    }
    const bool shared = target_ != Target::None && nav_ != ui::PageNav::Replace;
    if (!shared) target_ = Target::None;
    if (shared) {
        flight_.start(src_.rect, src_.radius, start_, scale_);
        firstFrame_ = true;
    }
    motion_.start(nav_, shared, start_, scale_);

    auto owned = host.release(outgoing);   // forgets hover / capture / focus inside it, as removeChild() does
    out_ = std::move(owned);
    outAlpha_ = fromAlpha;
    (void)incoming;
}

bool PageTransition::resolve(ui::Widget& host, Page* incoming, SharedCover& out) const {
    if (!incoming) return false;
    const Rect area = host.toWindow(host.rect());
    if (target_ == Target::Slot) return incoming->coverSlot(out) && mostlyInside(out.rect, area);
    if (target_ == Target::Match) return findCover(*incoming, matchKey_, matchNear_, area, out) && !out.rect.empty();
    return false;
}

void PageTransition::paint(gfx::Canvas& c, ui::Widget& host, Page* incoming) {
    const double now = ui::frame::now();
    if (flight_.active()) {
        SharedCover d;
        std::optional<ui::CoverFlight::Target> t;
        if (resolve(host, incoming, d)) {
            t = ui::CoverFlight::Target{d.rect, d.radius, d.ready};
            dst_ = std::move(d);
        } else {
            dst_.owner = nullptr;
        }
        if (firstFrame_ && !t) {
            // Nothing to fly to (no header on that page, the card is gone or off screen): the plain transition, from the
            // same start.
            flight_.cancel();
            target_ = Target::None;
            src_ = dst_ = {};
            motion_.start(nav_, false, start_, scale_);
        } else {
            flight_.update(now, t);
        }
        firstFrame_ = false;
    }

    const bool pages = motion_.running(now);
    if (!pages) dropOutgoing(host);
    if (!pages) {
        host.paintChildren(c);
    } else {
        const Rect hr = host.rect();
        const float s = c.scale();
        auto paintPage = [&](ui::Widget& page, const ui::PageLayer& l, float alpha) {
            if (alpha <= 0.004f) return;
            c.pushSurfaceGroup(alpha);
            c.pushTransform(D2D1::Matrix3x2F::Translation(hr.x + std::round(l.dx * s) / s, hr.y + std::round(l.dy * s) / s));
            page.paint(c);
            c.popTransform();
            c.popLayer();
        };
        c.pushClip(hr);
        if (out_) {
            const ui::PageLayer l = motion_.outgoing(now);
            paintPage(*out_, l, l.alpha * outAlpha_);
        }
        if (incoming) {
            const ui::PageLayer l = motion_.incoming(now);
            if (l.identity()) {
                c.popClip();
                host.paintChildren(c);
                c.pushClip(hr);
            } else {
                paintPage(*incoming, l, l.alpha);
            }
        }
        c.popClip();
    }
    if (flight_.active() && flight_.alpha() > 0.004f) drawFlight(c, host);

    if (motion_.running(now) || flight_.animating()) {
        ui::frame::requestNext();
    } else if (flight_.phase() == ui::CoverFlight::Phase::Hold) {
        // Waiting for the header's image (ImageCache::onLoaded repaints) or the hold's end.
        if (auto* w = host.window()) w->invalidateAfter(std::max(1.0, flight_.wakeAt() - ui::frame::realNow() + 1));
    }
    if (!flight_.active() && target_ != Target::None) {   // landed: the owners paint their covers again
        target_ = Target::None;
        src_ = dst_ = {};
    }
}

void PageTransition::drawFlight(gfx::Canvas& c, ui::Widget& host) {
    // Window DIPs -> the host's paint space (its parent's content space).
    const Rect hr = host.rect(), hw = host.toWindow(hr);
    const float ox = hr.x - hw.x, oy = hr.y - hw.y;
    const Rect r = flight_.rect().offset(ox, oy);
    const float radius = std::min(flight_.radius(), std::min(r.w, r.h) * 0.5f);
    const float a = flight_.alpha();
    // The content area: the page and, left of it, the sidebar (a cover may fly from the icon rail).
    c.pushClip({ox, hw.y + oy, hw.right(), hw.h});
    const auto& painter = src_.paint ? src_.paint : dst_.paint;
    if (painter && (src_.paint || src_.images.empty())) {
        c.pushOpacity(a);
        painter(c, r, radius);
        c.popLayer();
    } else {
        // The sharpest loaded bitmap of the two covers: the smallest one at least as large as drawn, else the largest.
        const int need = pixelsFor(r, c.scale());
        ID2D1Bitmap1* best = nullptr;
        UINT bestW = 0;
        auto consider = [&](const std::vector<catalog::Image>& images, const Rect& at) {
            if (images.empty() || at.empty()) return;
            const int px = pixelsFor(at, c.scale());
            const auto* img = catalog::pickImage(images, px);
            if (!img || img->url.empty()) return;
            ID2D1Bitmap1* bmp = gfx::ImageCache::get().request(img->url, px);
            if (!bmp) return;
            const UINT w = bmp->GetPixelSize().width;
            const bool better = !best || (w >= static_cast<UINT>(need) ? (bestW < static_cast<UINT>(need) || w < bestW) : w > bestW);
            if (better) {
                best = bmp;
                bestW = w;
            }
        };
        consider(src_.images, src_.rect);
        consider(dst_.images, dst_.rect);
        if (best) c.image(best, r, radius, a);
        else if (dst_.paint) {
            c.pushOpacity(a);
            dst_.paint(c, r, radius);
            c.popLayer();
        }
    }
    c.popClip();
}

} // namespace st::app
