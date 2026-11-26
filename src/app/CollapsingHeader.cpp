#include "app/CollapsingHeader.h"

#include "core/I18n.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
using collapse::kBarCover;
using collapse::kBarCoverY;
using collapse::kBarH;
using collapse::kBarTitleGap;
namespace type = gfx::type;

namespace {

void showIf(ui::Widget* w, bool on) {
    if (w->visible() != on) w->setVisible(on);
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// HeroHeader

HeroHeader::HeroHeader() {
    setTitleStyle(type::displayXL, true);
}

gfx::Text& HeroHeader::compactTitle() {
    if (!(compact_.style() == title_.style())) compact_.setStyle(title_.style());
    return compact_;
}

void HeroHeader::setTitle(const std::wstring& text) {
    title_.setText(text);
    compact_.setText(text);
}

void HeroHeader::setTitleStyle(const gfx::TextStyle& style, bool wrap) {
    gfx::TextOptions o;
    o.wrap = wrap;
    o.maxLines = wrap ? 2 : 0;
    title_.setOptions(o);
    title_.setStyle(style);
}

float HeroHeader::fitTitle(float width, std::initializer_list<float> sizes, float oneLineAbove, const gfx::TextStyle& base) {
    float size = sizes.size() ? *sizes.begin() : base.size;
    for (float s : sizes) {
        size = s;
        title_.setStyle(base.withSize(s));
        const float h = title_.measure(width).h;
        if (s > oneLineAbove ? h <= s * 1.05f : h <= s * 2.1f) break;
    }
    titleSize_ = size;
    return title_.measure(width).h;
}

void HeroHeader::drawCover(Canvas& c, const Rect& r, float radius) { drawArtwork(c, images_, r, radius, placeholder_); }

SharedCover HeroHeader::sharedCover(const ui::Widget& owner, const Rect& local) {
    return pagetx::imageCover(owner, local, 2, images_);
}

void HeroHeader::setTintImages(const std::vector<catalog::Image>& images) {
    const auto* img = catalog::pickImage(images, 300);
    const std::string url = img ? img->url : std::string();
    if (url == tintUrl_ && !url.empty()) return;
    tintUrl_ = url;
    tintLife_.renew();
    if (url.empty()) return tintArrived(std::nullopt);
    if (auto known = gfx::ImageCache::get().accentOf(url)) return tintArrived(known);
    gfx::ImageCache::get().fetchAccent(url, [this, ref = tintLife_.ref()](std::optional<Color> c) {
        if (ref.expired()) return;
        tintArrived(c);
        invalidate();
    });
}

void HeroHeader::setTintColor(std::optional<Color> color) {
    tintUrl_.clear();
    tintLife_.renew();
    tintArrived(color);
}

// A new source: cross-faded from what is shown (600 ms, like the app's accent), at once before the first paint.
void HeroHeader::tintArrived(std::optional<Color> c) {
    if (c == tintTo_) return;
    if (!painted_) {
        tintFrom_ = tintTo_ = c;
        tintFade_.snap(1);
        return;
    }
    tintFrom_ = tintTo_;
    tintTo_ = c;
    tintFade_.snap(0);
    tintFade_.to(1, 600, ui::Ease::Standard);
}

Color HeroHeader::tint() {
    const bool light = gfx::isLightTheme();
    const auto& col = colors();
    const collapse::Ink ink{col.bgBase, col.fgPrimary, col.fgSecondary};
    // The light theme darkens the cover's color like the accent (it is drawn on paper).
    const float strength = light ? 0.14f : 0.16f;
    auto washOf = [&](const std::optional<Color>& src) {
        const Color base = src ? (light ? gfx::Theme::adaptAccent(*src, true) : *src) : accent().base;
        return collapse::wash(base.withAlpha(1), ink, strength);
    };
    const Color to = washOf(tintTo_);
    const float t = tintFade_;
    return t >= 0.999f ? to : Color::lerp(washOf(tintFrom_), to, t);
}

void HeroHeader::paintTint(Canvas& c, const Rect& page, const collapse::Frame& f, float amount) {
    painted_ = true;
    if (amount <= 0.001f) return;
    const Color w = tint().mulAlpha(amount);
    if (w.a <= 0.001f) return;
    const float h = std::max(1.f, f.backdropBottom);
    c.fillLinearGradient({page.x, page.y, page.w, h}, {0, page.y}, {0, page.y + h}, w, w.withAlpha(0));
}

void HeroHeader::paintBackdrop(Canvas& c, const Rect& page, const collapse::Frame& f) { paintTint(c, page, f, 1.f); }

// The header's own content dissolves into the bar: transparent above the frame's maskTop, opaque below maskBottom.
void HeroHeader::paint(Canvas& c) {
    const Rect r = rect();
    if (!bar_ || bar_->header() != this || !bar_->active()) {
        paintContent(c);
        paintChildren(c);
        return;
    }
    const auto& f = bar_->frame();
    const float y0 = bar_->pageToLocalY(*this, f.maskTop), y1 = bar_->pageToLocalY(*this, f.maskBottom);
    if (y0 >= r.bottom() + 8) return;   // all of it is under the bar
    const bool fade = y1 > r.y - 8;
    if (fade) c.pushFade({r.x - 8, r.y - 8, r.w + 16, r.h + 16}, {0, y0}, {0, y1}, 0.f, 1.f);
    paintContent(c);
    paintChildren(c);
    if (fade) c.popLayer();
}

// ---------------------------------------------------------------------------------------------------
// HeaderCover

bool HeaderCover::editable() const {
    const HeroHeader* h = bar_.header();
    return h && h->onEditDetails && !h->coverSlot().empty();
}

void HeaderCover::paint(Canvas& c) {
    HeroHeader* h = bar_.header();
    if (!h || !bar_.active()) return;
    const Rect r = rect();
    const auto& f = bar_.frame();
    const Rect slot = h->coverSlot();
    if (slot.empty()) {   // the artist's avatar, fading in with the bar
        if (f.avatar <= 0.01f) return;
        c.pushOpacity(f.avatar);
        h->drawAvatar(c, r);
        c.popLayer();
        return;
    }
    // Page transition: 0 while a cover flies here / from here (the flight draws it), rising as it lands.
    const float shown = pagetx::presence(this);
    if (shown <= 0.f) return;
    const float k = r.w / std::max(1.f, slot.w);
    // The expanded cover's shadow (dark: black 35 %) goes as it shrinks; the bar's thumbnail has none.
    if (const float shadow = (1.f - f.coverProgress) * shown; shadow > 0.01f)
        c.shadow(r, 2, 40 * k, 18 * k, colors().shadowCard.mulAlpha(0.35f / 0.45f * shadow));
    // Drawn at the slot's size and scaled: the image keeps the bitmap it was loaded at (no reload while it shrinks).
    c.pushTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(r.x, r.y));
    h->drawCover(c, {0, 0, slot.w, slot.h}, 2.f / k);
    c.popTransform();
    if (hover_ && editable()) {   // "edit": the dialog that changes the picture
        c.fillRounded(r, 2, Color{0, 0, 0, 0.5f});
        const float s = r.w >= 120 ? 48.f : 20.f;
        c.icon("edit", r.center(s, s), Color{1, 1, 1, 1});
    }
}

// Opens on release: the dialog is modal, and the window still touches the pressed widget after onMouseDown.
bool HeaderCover::onMouseDown(const ui::MouseEvent& e) { return e.button == ui::MouseButton::Left && editable(); }

void HeaderCover::onMouseUp(const ui::MouseEvent& e) {
    if (!rect().contains(e.pos) || !editable()) return;
    const auto edit = bar_.header()->onEditDetails;
    edit();
}

void HeaderCover::onMouseEnter() {
    hover_ = true;
    if (editable()) invalidate();
}

void HeaderCover::onMouseLeave() {
    hover_ = false;
    invalidate();
}

LPCWSTR HeaderCover::cursor() const { return editable() ? IDC_HAND : IDC_ARROW; }

// ---------------------------------------------------------------------------------------------------
// CollapseBar

CollapseBar::CollapseBar(ui::ScrollView* scroll) : scroll_(scroll) {
    cover_ = add<HeaderCover>(*this);
    // Thin outline controls (the player bar's style): the bar is the restrained form of the header.
    shuffle_ = add<ui::Button>(ui::ButtonKind::IconOutline, L"", "shuffle");
    shuffle_->setTooltip(tr(L"Karıştır"));
    play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
    cover_->setVisible(false);
    shuffle_->setVisible(false);
    play_->setVisible(false);
    // The header's own handlers (they may navigate or rebuild the page: called through a copy).
    play_->onClick = [this] {
        if (!header_ || !header_->play_ || !header_->play_->onClick) return;
        const auto click = header_->play_->onClick;
        click();
    };
    shuffle_->onClick = [this] {
        if (!header_ || !header_->shuffle_ || !header_->shuffle_->onClick) return;
        const auto click = header_->shuffle_->onClick;
        click();
    };
}

void CollapseBar::attach(HeroHeader* header, ui::Widget* dock, TrackTable* columns) {
    header_ = header;
    dock_ = dock;
    columns_ = columns;
    if (columns_) columns_->setHeaderDocked(true);   // the bar paints its column header; the table doesn't stick one
    if (header_) header_->bar_ = this;
    valid_ = false;
    titleHover_ = false;
    titleHit_ = {};
    showIf(cover_, false);
    showIf(play_, false);
    showIf(shuffle_, false);
    if (scroll_) scroll_->topInset = 0;
    invalidate();
}

float CollapseBar::pageToLocalY(const ui::Widget& w, float pageY) const {
    return pageY + toWindow({0, 0, 0, 0}).y - w.toWindow({0, 0, 0, 0}).y;
}

Rect CollapseBar::pageRect() const { return scroll_ ? scroll_->rect() : Rect{}; }

void CollapseBar::update() {
    valid_ = false;
    if (!header_ || !scroll_ || !header_->visible()) {
        showIf(cover_, false);
        showIf(play_, false);
        showIf(shuffle_, false);
        return;
    }
    const float s = scroll_->scrollY();   // this frame's offset (the smooth-scroll engine advances to the frame time)
    const Rect page = pageRect();
    const Rect origin = toWindow({0, 0, 0, 0});   // the page's content origin, window DIPs
    // A widget's rect in page coordinates at scroll offset 0.
    auto atRest = [&](const ui::Widget& w) {
        const Rect win = w.toWindow(w.rect());
        return Rect{win.x - origin.x, win.y - origin.y + s, win.w, win.h};
    };
    const Rect hr = atRest(*header_);
    collapse::Spec sp;
    sp.width = page.w;
    sp.left = hr.x;
    const Rect slot = header_->coverSlot();
    if (!slot.empty()) sp.cover = slot.offset(hr.x, hr.y);
    sp.title = header_->titleBox().offset(hr.x, hr.y);
    sp.titleScale = std::min(1.f, type::title.size / std::max(1.f, header_->titleSize()));
    sp.contentTop = hr.y + header_->contentTop();
    sp.dockY = dock_ ? atRest(*dock_).y : hr.bottom() + 28;
    sp.columnHeaderH = columns_ && columns_->headerHeight() > 0 ? TrackTable::kHeaderH : 0.f;
    sp.backdropH = header_->backdropHeight();
    spec_ = sp;
    frame_ = collapse::compute(sp, s);
    valid_ = true;
    scroll_->topInset = frame_.inset;

    // The cover (or the artist's avatar slot).
    cover_->setRect(frame_.cover);
    showIf(cover_, !slot.empty() || frame_.avatar > 0.01f);
    // Play / shuffle at the bar's right end; Tab stops and clickable once they are more than half in.
    const Rect playR{hr.right() - kBarCover, kBarCoverY, kBarCover, kBarCover};
    const Rect shuffleR{playR.x - 12 - kBarCover, kBarCoverY, kBarCover, kBarCover};
    play_->setRect(playR);
    shuffle_->setRect(shuffleR);
    const bool hasShuffle = header_->shuffle_ && header_->shuffle_->visible();
    const bool in = frame_.buttons > 0.5f;
    showIf(play_, in && header_->play_);
    showIf(shuffle_, in && hasShuffle);
    if (header_->play_) {
        const bool playing = header_->isPlaying ? header_->isPlaying() : header_->play_->playing();
        if (header_->isPlaying) header_->play_->setPlaying(playing);
        play_->setPlaying(playing);
    }
    const float compactX = hr.x + kBarCover + kBarTitleGap;
    const float buttonsLeft = (header_->play_ ? (hasShuffle ? shuffleR.x : playR.x) : hr.right()) - 16;
    compactW_ = std::max(40.f, buttonsLeft - compactX);
}

bool CollapseBar::coverSlot(SharedCover& out) {
    if (!header_ || header_->coverSlot().empty()) return false;
    update();   // where the cover is at this offset (also when asked before this frame's paint)
    if (!valid_ || cover_->rect().empty()) return false;
    out = header_->sharedCover(*cover_, cover_->rect());
    return !out.rect.empty();
}

void CollapseBar::paintBackdrop(Canvas& c) {
    update();
    if (!valid_) return;
    header_->paintBackdrop(c, pageRect(), frame_);
}

void CollapseBar::paintTitle(Canvas& c) {
    const auto& f = frame_;
    titleHit_ = {};
    auto& big = header_->titleText();
    if (big.empty()) return;
    const Color fg = colors().fgPrimary;
    const Rect tb = spec_.title;
    const float k = f.titleScale;
    const gfx::Point a = f.titleAnchor;
    // Both layouts are drawn with their left edge / vertical center moved onto the anchor and scaled around it.
    const float cy = tb.y + tb.h * 0.5f;
    auto& compact = header_->compactTitle();
    const float w = compactW_ / std::max(0.01f, spec_.titleScale);   // the bar's room, at the expanded size
    const gfx::Size sz = compact.measure(w);
    const bool oneLine = sz.h >= tb.h - 1.f;
    // A one-line title that fits the bar: the same layout either way, no cross-fade. One line that gets an ellipsis:
    // the one-line layout comes in under the expanded one. Two lines: the expanded layout fades out before the
    // one-line layout fades in (never a double outline).
    const float mix = f.titleMix;
    float bigA = 1.f, compactA = 0.f;
    if (oneLine && compact.isTrimmed(w)) {
        bigA = 1.f - mix;
        compactA = std::min(1.f, mix * 2.f);
    } else if (!oneLine) {
        bigA = 1.f - collapse::smoothstep(mix * 2.f);
        compactA = collapse::smoothstep(mix * 2.f - 1.f);
    }
    c.pushTransform(D2D1::Matrix3x2F::Translation(-tb.x, -cy) * D2D1::Matrix3x2F::Scale(k, k) *
                    D2D1::Matrix3x2F::Translation(a.x, a.y));
    if (compactA > 0.001f) c.text(compact, {tb.x, cy - sz.h * 0.5f, w, sz.h}, fg.mulAlpha(compactA));
    if (bigA > 0.001f) c.text(big, tb, fg.mulAlpha(bigA));
    c.popTransform();
    if (bigA >= compactA) {
        const float inkW = std::min(tb.w, std::ceil(big.measure(tb.w).w));
        titleHit_ = {a.x, a.y - tb.h * k * 0.5f, inkW * k, tb.h * k};
    } else {
        titleHit_ = {a.x, a.y - sz.h * k * 0.5f, std::min(w, std::ceil(sz.w)) * k, sz.h * k};
    }
}

void CollapseBar::paint(Canvas& c) {
    if (!valid_ || !header_) return;
    const auto& f = frame_;
    const Rect page = pageRect();
    if (f.docked) {
        // The list scrolls under the bar: its surface (replacing the pixels under Mica) with the backdrop's tint.
        const Rect bar{page.x, page.y, page.w, f.barBottom};
        c.fillSurface(bar);
        c.pushClip(bar);
        header_->paintBarTint(c, page, f);
        c.popClip();
        if (columns_ && spec_.columnHeaderH > 0)
            columns_->paintHeader(c, {page.x + spec_.left, page.y + kBarH, page.w - spec_.left * 2, TrackTable::kHeaderH});
        else if (f.hairline > 0.001f)
            c.hline(page.x, page.right(), page.y + kBarH - 1, colors().hairDefault.mulAlpha(f.hairline));
    }
    const Rect r = rect();
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    if (cover_->visible()) cover_->paint(c);
    c.popTransform();
    paintTitle(c);
    if (f.buttons > 0.01f && header_->play_) {
        c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
        c.pushOpacity(f.buttons);
        if (header_->shuffle_ && header_->shuffle_->visible()) shuffle_->paint(c);
        play_->paint(c);
        c.popLayer();
        c.popTransform();
    }
}

ui::Widget* CollapseBar::hitTest(gfx::Point p) {
    if (!visible() || !valid_ || !header_) return nullptr;
    const Rect page = pageRect();
    if (!page.contains(p) || p.x >= page.right() - 12) return nullptr;   // the scrollbar strip stays the ScrollView's
    const gfx::Point local{p.x - rect().x, p.y - rect().y};
    for (auto it = children().rbegin(); it != children().rend(); ++it)
        if (Widget* w = (*it)->hitTest(local)) return w;
    if (header_->onEditDetails && titleHit_.contains(p)) return this;
    // What the bar covers: the docked bar, else the band where the header's content has dissolved (invisible
    // controls there must not take clicks).
    const float covered = frame_.docked ? frame_.barBottom : std::max(0.f, frame_.maskTop);
    return p.y - page.y < covered ? this : nullptr;
}

// The bar sits above the ScrollView (not inside it): wheel input over it scrolls the page.
bool CollapseBar::onWheel(float dy, float dx, const ui::MouseEvent& e) { return scroll_ && scroll_->onWheel(dy, dx, e); }

bool CollapseBar::onMouseDown(const ui::MouseEvent& e) {
    // The docked column header sorts the table (left) or opens its columns menu (right), as the table's own header.
    if (inColumns(e.pos) && (e.button == ui::MouseButton::Left || e.button == ui::MouseButton::Right)) {
        columns_->headerPress(e.windowPos, e.button == ui::MouseButton::Right);
        return false;
    }
    return e.button == ui::MouseButton::Left && header_ && header_->onEditDetails && titleHit_.contains(e.pos);
}

void CollapseBar::onMouseUp(const ui::MouseEvent& e) {
    if (!header_ || !header_->onEditDetails || !titleHit_.contains(e.pos)) return;
    const auto edit = header_->onEditDetails;
    edit();
}

void CollapseBar::onMouseMove(const ui::MouseEvent& e) {
    const bool hot = header_ && header_->onEditDetails && titleHit_.contains(e.pos);
    if (hot != titleHover_) {
        titleHover_ = hot;
        invalidate();
    }
    if (columns_) {   // the docked column header: its sortable labels light up like the table's own
        if (inColumns(e.pos)) columns_->headerHover(e.windowPos);
        else columns_->headerLeave();
    }
}

void CollapseBar::onMouseLeave() {
    titleHover_ = false;
    if (columns_) columns_->headerLeave();
}

LPCWSTR CollapseBar::cursor() const { return titleHover_ || (columns_ && columns_->headerHot()) ? IDC_HAND : IDC_ARROW; }

bool CollapseBar::inColumns(gfx::Point p) const {
    if (!columns_ || !valid_ || !frame_.docked || spec_.columnHeaderH <= 0) return false;
    const float y = p.y - pageRect().y;
    return y >= kBarH && y < kBarH + TrackTable::kHeaderH;
}

HeaderCover* findHeaderCover(ui::Widget* page) {
    if (!page) return nullptr;
    for (const auto& child : page->children())
        if (auto* bar = dynamic_cast<CollapseBar*>(child.get()))
            return bar->header() && !bar->header()->coverSlot().empty() ? bar->cover() : nullptr;
    return nullptr;
}

} // namespace st::app
