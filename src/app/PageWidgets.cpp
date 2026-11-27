#include "app/PageWidgets.h"

#include "app/CollapsingHeader.h"
#include "app/Display.h"
#include "app/Pages.h"
#include "core/I18n.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ---------------------------------------------------------------------------------------------------
// ListRow

ListRow::ListRow(std::wstring title, std::wstring subtitle, std::wstring meta, std::vector<catalog::Image> images,
                 bool circle)
    : title_(std::move(title), type::body),
      subtitle_(std::move(subtitle), type::caption),
      meta_(std::move(meta), type::monoLabel),
      images_(std::move(images)),
      circle_(circle) {
    focusable = true;
}

bool ListRow::onMouseDown(const ui::MouseEvent& e) {
    if (e.button == ui::MouseButton::Right) {
        if (onContext) onContext(e.windowPos);
        return false;
    }
    return e.button == ui::MouseButton::Left;
}

void ListRow::onMouseUp(const ui::MouseEvent& e) {
    if (!rect().contains(e.pos) || !onClick) return;
    pagetx::arm(cover());
    onClick();
}

bool ListRow::coverFor(const std::string& key, SharedCover& out) {
    if (images_.empty()) return false;
    out = cover();
    return out.key == key;
}

void ListRow::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hover_));
    const Rect art = artRect();
    if (pagetx::presence(this) > 0.f)   // 0: the page transition draws this cover
        drawArtwork(c, images_, art, gfx::radii().art, circle_ ? Placeholder::Artist : Placeholder::Album, circle_);
    const float mw = meta_.empty() ? 0 : std::ceil(meta_.measure().w);
    const float tx = art.right() + 14, tw = r.right() - tx - mw - 20;
    c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
    c.text(subtitle_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
    if (mw > 0) c.text(meta_, {r.right() - mw - 8, r.y, mw + 1, r.h}, col.fgTertiary, gfx::VAlign::Center);
    c.hline(tx, r.right(), r.bottom() - 1, col.hairSubtle);
}

// ---------------------------------------------------------------------------------------------------
// Tile

Tile::Tile(std::wstring label, std::wstring title, Color color, std::vector<catalog::Image> images, bool hero)
    : label_(label, type::monoLabel),
      labelShort_(label.substr(0, std::min(label.find(L" · "), label.size())), type::monoLabel),
      title_(std::move(title), hero ? type::displayS.withSize(30) : type::body.withSize(15).withWeight(800)),
      color_(color),
      images_(std::move(images)),
      hero_(hero) {
    focusable = true;
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = hero ? 3 : 2;
    title_.setOptions(wrap);
}

bool Tile::onMouseDown(const ui::MouseEvent& e) {
    if (e.button != ui::MouseButton::Left) return false;
    press_.to(1, ui::motion::microMs(80));
    return true;
}

void Tile::onMouseUp(const ui::MouseEvent& e) {
    press_.to(0, ui::motion::microMs(160));
    if (!rect().contains(e.pos) || !onClick) return;
    if (!images_.empty()) pagetx::arm(cover());
    onClick();
}

Rect Tile::artRect() const {
    const float d = hero_ ? 96.f : 48.f;
    return {rect().right() - d - 16, rect().y + 16, d, d};
}

SharedCover Tile::cover() const { return pagetx::imageCover(*this, artRect(), artRect().w * 0.5f, images_); }

bool Tile::coverFor(const std::string& key, SharedCover& out) {
    if (images_.empty()) return false;
    out = cover();
    return out.key == key;
}

void Tile::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const float h = hover_, p = press_;
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, ui::card::lift(h, p)));
    c.pushScale(ui::card::pressScale(p), {r.cx(), r.cy()});
    ui::card::paintBack(c, r, h);
    const Color fill = tileFill(color_);   // theme-aware (light: pale tint of the tone's hue)
    c.fillRounded(r, gfx::radii().surface, Color::lerp(fill, Color::lerp(fill, col.fgPrimary, 0.06f), h));
    ui::card::paintEdge(c, r, h);
    c.pushClip(r);
    // Signature motif: a thin ring (hi-fi / vinyl cue) drifting on hover.
    const float ringR = hero_ ? r.h * 0.62f : r.h * 0.7f;
    c.strokeCircle({r.right() - ringR * 0.55f - 12 * h, r.bottom() - ringR * 0.25f}, ringR, accent().base.withAlpha(0.35f), 1.f);
    const float artD = hero_ ? 96.f : 48.f;
    if (!images_.empty() && pagetx::presence(this) > 0.f)   // 0: the page transition draws it
        drawArtwork(c, images_, artRect(), 0, Placeholder::Artist, true, Priority::Normal);
    c.popClip();
    // Keep the label off the round artwork in the top-right corner: in a narrow tile only the index is left
    // ("02" instead of "02 · ALBÜM" / "02 · АЛЬБОМ").
    const float labelW = images_.empty() ? r.w - 36 : r.w - 18 - artD - 16 - 6;
    gfx::Text& label = label_.measure().w <= labelW ? label_ : labelShort_;
    c.text(label, {r.x + 18, r.y + 18, labelW, 14}, hero_ ? accent().base : col.fgSecondary);
    const float tw = r.w - 36 - (hero_ ? 0 : 0);
    const float th = title_.measure(tw).h;
    c.text(title_, {r.x + 18, r.bottom() - 18 - th, tw, th}, col.fgPrimary);
    c.popTransform();
    c.popTransform();
}

// ---------------------------------------------------------------------------------------------------
// MessagePanel

namespace {
MessagePanel::Tone toneFor(const std::string& icon) {
    if (icon == "offline") return MessagePanel::Tone::Offline;
    if (icon == "error" || icon == "warning") return MessagePanel::Tone::Error;
    return MessagePanel::Tone::Empty;
}
} // namespace

MessagePanel::MessagePanel(std::string icon, std::wstring title, std::wstring body, std::wstring action,
                           std::function<void()> onAction)
    : icon_(std::move(icon)), title_(std::move(title), type::title), body_(std::move(body), type::bodyRegular) {
    gfx::TextOptions center;
    center.align = gfx::TextAlign::Center;
    center.wrap = true;
    center.maxLines = 2;
    title_.setOptions(center);
    center.maxLines = 4;
    body_.setOptions(center);
    tone_ = toneFor(icon_);
    if (!action.empty()) {
        // A retry carries the refresh glyph; other actions (connect, add a folder, go back) are plain text unless the
        // page gives them an icon (setActionIcon).
        action_ = add<ui::Button>(ui::ButtonKind::Secondary, std::move(action), tone_ == Tone::Empty ? "" : "refresh");
        action_->onClick = std::move(onAction);
    }
}

MessagePanel& MessagePanel::setTone(Tone tone) {
    tone_ = tone;
    invalidate();
    return *this;
}

MessagePanel& MessagePanel::setCompact(bool on) {
    compact_ = on;
    title_.setStyle(on ? type::body.withSize(15).withWeight(700) : type::title);
    body_.setStyle(on ? type::secondary : type::bodyRegular);
    requestLayout();
    return *this;
}

MessagePanel& MessagePanel::setActionIcon(std::string icon) {
    if (action_) action_->setIcon(std::move(icon));
    return *this;
}

// Top pad, the illustration, gap, title, gap, body, gap, action, bottom pad.
float MessagePanel::preferredHeight(float width) {
    const float tw = textW(width);
    const float th = title_.empty() ? 0.f : std::ceil(title_.measure(tw).h);
    const float bh = body_.empty() ? 0.f : std::ceil(body_.measure(tw).h);
    const float pad = compact_ ? 24.f : 40.f;
    float h = pad + illoSize() + (compact_ ? 14.f : 20.f) + th;
    if (bh > 0) h += (compact_ ? 4.f : 8.f) + bh;
    if (action_) h += (compact_ ? 16.f : 24.f) + 40;
    return h + pad;
}

void MessagePanel::layout() {
    if (!action_) return;
    const float w = action_->naturalWidth();
    action_->setRect({rect().w * 0.5f - w * 0.5f, preferredHeight(rect().w) - (compact_ ? 24.f : 40.f) - 40, w, 40});
}

void MessagePanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    // The first paint fades the panel in, rising 6 DIP (no motion with micro-interactions off).
    if (!shown_) {
        shown_ = true;
        in_.snap(0);
        in_.to(1, ui::motion::microMs(ui::motion::medium), ui::Ease::Decelerate);
    }
    const float a = in_.value();
    const bool fading = a < 0.999f;
    if (fading) {
        c.pushOpacity(a);
        c.pushTransform(D2D1::Matrix3x2F::Translation(0, (1 - a) * 6));
    }
    const float pad = compact_ ? 24.f : 40.f;
    const float s = illoSize();
    const gfx::Point o{std::round(r.cx()), r.y + pad + s * 0.5f};
    // The illustration: a hairline ring with the icon, inside a dial of 1 px ticks (the design's hi-fi cue).
    const float ring = s * 0.36f;
    c.strokeCircle(o, ring, col.hairStrong, 1.f);
    if (!compact_) {
        const float r0 = s * 0.44f, r1 = s * 0.5f;
        for (int k = 0; k < 36; ++k) {
            const float ang = static_cast<float>(k) * (6.2831853f / 36.f);
            const float dx = std::sin(ang), dy = -std::cos(ang);
            const float inner = (k % 9 == 0) ? r0 - 3 : r0;   // a longer tick every quarter
            c.line({o.x + dx * inner, o.y + dy * inner}, {o.x + dx * r1, o.y + dy * r1},
                   (k % 9 == 0 ? col.hairStrong : col.hairDefault), 1.f, false);
        }
    }
    const float ic = compact_ ? 20.f : 24.f;
    c.icon(tone_ == Tone::Offline ? std::string("offline") : icon_, {o.x - ic * 0.5f, o.y - ic * 0.5f, ic, ic},
           col.fgSecondary);
    if (tone_ != Tone::Empty)   // a small status dot on the ring: something went wrong
        c.fillCircle({o.x + ring * 0.7071f, o.y - ring * 0.7071f}, compact_ ? 3.f : 4.f,
                     tone_ == Tone::Offline ? col.warning : col.error);
    const float tw = textW(r.w);
    float y = r.y + pad + s + (compact_ ? 14.f : 20.f);
    if (!title_.empty()) {
        const float th = std::ceil(title_.measure(tw).h);
        c.text(title_, {r.cx() - tw * 0.5f, y, tw, th}, col.fgPrimary);
        y += th;
    }
    if (!body_.empty()) {
        y += compact_ ? 4.f : 8.f;
        const float bh = std::ceil(body_.measure(tw).h);
        c.text(body_, {r.cx() - tw * 0.5f, y, tw, bh}, col.fgSecondary);
    }
    paintChildren(c);
    if (fading) {
        c.popTransform();
        c.popLayer();
    }
}

MessagePanel* emptyState(ui::Widget* parent, std::string icon, std::wstring title, std::wstring line, std::wstring action,
                         std::function<void()> onAction, std::string actionIcon) {
    auto* p = parent->add<MessagePanel>(std::move(icon), std::move(title), std::move(line), std::move(action),
                                        std::move(onAction));
    p->setTone(MessagePanel::Tone::Empty);
    if (!actionIcon.empty()) p->setActionIcon(std::move(actionIcon));
    return p;
}

MessagePanel* errorState(ui::Widget* parent, const std::wstring& rawMessage, std::function<void()> retry) {
    bool offline = false;
    std::wstring line = friendlyError(rawMessage, &offline);
    auto* p = parent->add<MessagePanel>(offline ? "offline" : "error",
                                        offline ? tr(L"Çevrimdışısın") : tr(L"Bir şeyler ters gitti"), std::move(line),
                                        retry ? std::wstring(tr(L"Tekrar dene")) : std::wstring(), std::move(retry));
    p->setTone(offline ? MessagePanel::Tone::Offline : MessagePanel::Tone::Error);
    return p;
}

MessagePanel* noResultsState(ui::Widget* parent, std::wstring line) {
    auto* p = parent->add<MessagePanel>("search", std::move(line), std::wstring());
    p->setCompact(true);
    return p;
}

// ---------------------------------------------------------------------------------------------------
// Skeletons

float SkeletonRows::preferredHeight(float) {
    return (header_ ? TrackTable::kHeaderH + 8 : 0.f) + rows_ * TrackTable::rowMetrics().h + 8;
}

void SkeletonRows::paint(Canvas& c) {
    Rect r = rect();
    if (header_) {   // the column header's hairline and two label bars
        ui::paintSkeleton(c, this, {r.x + 12, r.y + 10, 10, 8}, gfx::radii().badge);
        ui::paintSkeleton(c, this, {r.x + 68, r.y + 10, 56, 8}, gfx::radii().badge);
        c.hline(r.x, r.right(), r.y + TrackTable::kHeaderH, colors().hairSubtle);
        r.y += TrackTable::kHeaderH + 8;
    }
    TrackTable::paintSkeletonRows(c, this, {r.x, r.y, r.w, rows_ * TrackTable::rowMetrics().h}, rows_, 0, art_);
}

void SkeletonLines::paint(Canvas& c) {
    const Rect r = rect();
    const Rect vis = c.clip();
    for (int k = 0; k < rows_; ++k) {
        const Rect row{r.x, r.y + k * rowH_, r.w, rowH_};
        if (!row.intersects(vis)) continue;
        float x = row.x + 10;
        if (art_ > 0) {
            ui::paintSkeleton(c, this, {x, row.cy() - art_ * 0.5f, art_, art_}, round_ ? art_ * 0.5f : gfx::radii().art);
            x += art_ + 12;
        }
        const float w = std::max(0.f, row.right() - x - 10);
        const int v = k * 7 % 5;
        ui::paintSkeleton(c, this, {x, row.cy() - 10, std::min(w, 140.f + v * 26.f), 9}, gfx::radii().badge);
        ui::paintSkeleton(c, this, {x, row.cy() + 5, std::min(w, 80.f + v * 14.f), 7}, gfx::radii().badge);
    }
}

float SkeletonCards::gridHeight(float width, int rows, float minCell, float tileH) {
    const float gap = tileH > 0 ? gfx::metrics::gridGap : gfx::metrics::cardGap;
    const int cols = std::max(1, static_cast<int>((width + gap) / (minCell + gap)));
    const float cw = (width - gap * (cols - 1)) / cols;
    return rows * (tileH > 0 ? tileH : cw + 10 + 18 + 4 + 16) + (rows - 1) * gap;
}

// MediaCard's geometry: the square (or round) artwork, a title bar and a shorter subtitle bar under it; or editorial
// tiles (ui::Grid of Tile: the grid gap, a fixed height) as plain blocks.
void SkeletonCards::paintGrid(Canvas& c, ui::Widget* owner, const Rect& r, int rows, float minCell, bool round, float tileH) {
    const float gap = tileH > 0 ? gfx::metrics::gridGap : gfx::metrics::cardGap;
    const int cols = std::max(1, static_cast<int>((r.w + gap) / (minCell + gap)));
    const float cw = (r.w - gap * (cols - 1)) / cols, ch = tileH > 0 ? tileH : cw + 10 + 18 + 4 + 16;
    const Rect vis = c.clip();
    for (int row = 0; row < rows; ++row)
        for (int k = 0; k < cols; ++k) {
            const float x = r.x + k * (cw + gap), y = r.y + row * (ch + gap);
            if (!Rect{x, y, cw, ch}.intersects(vis)) continue;
            if (tileH > 0) {
                ui::paintSkeleton(c, owner, {x, y, cw, ch}, gfx::radii().surface);
                continue;
            }
            const int v = (row * cols + k) * 5 % 4;
            ui::paintSkeleton(c, owner, {x, y, cw, cw}, round ? cw * 0.5f : gfx::radii().art);
            const float tw = std::round(cw * (0.72f - 0.1f * v));
            const float sw = std::round(cw * (0.42f + 0.06f * v));
            const float tx = round ? x + (cw - tw) * 0.5f : x, sx = round ? x + (cw - sw) * 0.5f : x;
            ui::paintSkeleton(c, owner, {tx, y + cw + 14, tw, 10}, gfx::radii().badge);
            ui::paintSkeleton(c, owner, {sx, y + cw + 34, sw, 8}, gfx::radii().badge);
        }
}

void SkeletonShelf::paint(Canvas& c) {
    const Rect r = rect();
    ui::paintSkeleton(c, this, {r.x, r.y + 12, 180, 16}, gfx::radii().badge);
    c.hline(r.x, r.right(), r.y + 44, colors().hairSubtle);
    SkeletonCards::paintGrid(c, this, {r.x, r.y + 48, r.w, r.h - 48}, 1, minCell_, round_);
}

void SkeletonHeader::paintAt(Canvas& c, ui::Widget* owner, const Rect& r) {
    // A cover flying into this (loading) page lands where its header's cover will be: no block under it.
    if (!pagetx::coverIncoming()) ui::paintSkeleton(c, owner, {r.x, r.y, 232, 232}, gfx::radii().art);
    const float tx = r.x + 264, tw = std::max(0.f, r.w - 264);
    ui::paintSkeleton(c, owner, {tx, r.y + 118, std::min(tw, 150.f), 10}, gfx::radii().badge);
    ui::paintSkeleton(c, owner, {tx, r.y + 140, std::min(tw, 460.f), 48}, gfx::radii().badge);
    ui::paintSkeleton(c, owner, {tx, r.y + 204, std::min(tw, 210.f), 12}, gfx::radii().badge);
    // The action row: the play button, shuffle, a few round buttons (pills and circles).
    const float ay = r.y + 260;
    ui::paintSkeleton(c, owner, {r.x, ay, 56, 56}, 28);
    ui::paintSkeleton(c, owner, {r.x + 72, ay + 8, 112, 40}, 20);
    for (int k = 0; k < 3; ++k) ui::paintSkeleton(c, owner, {r.x + 198 + k * 54.f, ay + 8, 40, 40}, 20);
}

void SkeletonTitle::paint(Canvas& c) {
    const Rect r = rect();
    ui::paintSkeleton(c, this, {r.x, r.y + 8, std::min(r.w, 300.f), 48}, gfx::radii().badge);
    ui::paintSkeleton(c, this, {r.x, r.y + 70, std::min(r.w, 180.f), 10}, gfx::radii().badge);
}

// The collection page's own geometry (DetailHeader 340 + the column's 28 gap + the table's header).
float SkeletonBlock::preferredHeight(float) {
    return SkeletonHeader::kHeight + 24 + 28 + TrackTable::kHeaderH + 8 + rows_ * TrackTable::rowMetrics().h;
}

void SkeletonBlock::paint(Canvas& c) {
    const Rect r = rect();
    SkeletonHeader::paintAt(c, this, r);
    const float y0 = r.y + SkeletonHeader::kHeight + 24 + 28;
    c.hline(r.x, r.right(), y0 + TrackTable::kHeaderH, colors().hairSubtle);
    TrackTable::paintSkeletonRows(c, this, {r.x, y0 + TrackTable::kHeaderH + 8, r.w, rows_ * TrackTable::rowMetrics().h},
                                  rows_, 0, true);
}

ui::Grid* addCardRow(ui::Column* col, float minCell, int maxRows) {
    return col->add<ui::Grid>(minCell, gfx::metrics::cardGap, [](float w) { return w + 10 + 18 + 4 + 16; }, maxRows);
}

float cardMinCell() { return display::compactLists() ? 140.f : 160.f; }

ui::Grid* addCardGrid(ui::Column* col, int maxRows) { return addCardRow(col, cardMinCell(), maxRows); }

// ---------------------------------------------------------------------------------------------------
// PageTitle

PageTitle::PageTitle(std::wstring title, std::wstring meta, std::wstring kicker)
    : title_(std::move(title), type::displayM), meta_(std::move(meta), type::secondary), kicker_(std::move(kicker), type::monoLabel) {
    hitTestVisible = false;
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = 2;
    meta_.setOptions(wrap);
}

ui::Button* PageTitle::addAction(ui::ButtonKind kind, std::wstring label, std::string icon) {
    auto* b = add<ui::Button>(kind, std::move(label), std::move(icon));
    actions_.push_back(b);
    requestLayout();
    return b;
}

ui::TextBox* PageTitle::addSearch(std::wstring placeholder) {
    search_ = add<ui::TextBox>(ui::TextBox::Look::Search, std::move(placeholder));
    requestLayout();
    return search_;
}

void PageTitle::setTitle(std::wstring title) {
    title_.setText(std::move(title));
    widths_.clear();
    requestLayout();
    invalidate();
}

void PageTitle::setMeta(std::wstring meta) {
    if (meta == meta_.text()) return;
    const bool had = !meta_.empty();
    meta_.setText(std::move(meta));
    if (had != !meta_.empty() && parent()) parent()->requestLayout();   // the block's height changes
    requestLayout();
    invalidate();
}

void PageTitle::setKicker(std::wstring kicker) {
    if (kicker == kicker_.text()) return;
    const bool had = !kicker_.empty();
    kicker_.setText(std::move(kicker));
    if (had != !kicker_.empty() && parent()) parent()->requestLayout();
    requestLayout();
    invalidate();
}

float PageTitle::searchWidth(float width) const { return std::clamp(width * 0.4f, 220.f, 380.f); }

// The title's natural width at a display size, measured once per size and text (a gfx::Text keeps one layout per width:
// measuring at another width than it is drawn at every frame would lay it out twice per frame).
float PageTitle::titleWidth(float size) {
    if (fontGen_ != gfx::Fonts::generation()) {   // the UI font changed (Ayarlar › Görünüm)
        fontGen_ = gfx::Fonts::generation();
        widths_.clear();
    }
    for (const auto& [s, w] : widths_)
        if (s == size) return w;
    gfx::Text probe(title_.text(), type::displayM.withSize(size));
    const float w = std::ceil(probe.measure().w);
    widths_.emplace_back(size, w);
    return w;
}

float PageTitle::rightWidth() const {
    float w = 0;
    int n = 0;
    for (auto* b : actions_)
        if (b->visible()) {
            w += b->naturalWidth() + (n++ ? kActionGap : 0.f);
        }
    return w;
}

PageTitle::Geometry PageTitle::geometry(float width) {
    Geometry g;
    g.size = width >= 900 ? 56.f : width >= 560 ? 44.f : 36.f;
    g.kickerH = kicker_.empty() ? 0.f : 22.f;
    const float actions = rightWidth();
    const float search = search_ && search_->visible() ? searchWidth(width) : 0.f;
    const float right = actions + (search > 0 ? search + (actions > 0 ? 12.f : 0.f) : 0.f);
    float tw = titleWidth(g.size);
    const bool inlineRow = right <= 0 || tw + 24 + right <= width;
    const float room = inlineRow && right > 0 ? width - right - 24 : width;
    // A long name: smaller steps (down to 36) before the ellipsis.
    while (tw > room && g.size > 36.f) {
        g.size = std::max(36.f, g.size - 8.f);
        tw = titleWidth(g.size);
    }
    title_.setStyle(type::displayM.withSize(g.size));
    g.titleH = std::ceil(g.size * 1.14f);
    float y = g.kickerH + g.titleH;
    if (!meta_.empty()) {
        g.metaY = y + 4;
        g.metaH = std::ceil(meta_.measure(width).h);
        y = g.metaY + g.metaH;
    }
    if (!inlineRow) {
        g.rowY = y + 16;
        y = g.rowY + std::max(actions > 0 ? gfx::metrics::pillH : 0.f, search > 0 ? gfx::metrics::searchH : 0.f);
    }
    g.height = y;
    return g;
}

float PageTitle::preferredHeight(float width) { return geometry(width).height; }

void PageTitle::layout() {
    const float w = rect().w;
    const Geometry g = geometry(w);
    const float search = search_ && search_->visible() ? searchWidth(w) : 0.f;
    if (g.rowY < 0) {
        // Beside the title, centered on its line, right-aligned: actions, then the search pill.
        const float cy = g.kickerH + g.titleH * 0.5f;
        float x = w;
        if (search > 0) {
            search_->setRect({x - search, std::round(cy - gfx::metrics::searchH * 0.5f), search, gfx::metrics::searchH});
            x -= search + 12;
        }
        for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
            if (!(*it)->visible()) continue;
            const float bw = (*it)->naturalWidth();
            (*it)->setRect({x - bw, std::round(cy - gfx::metrics::pillH * 0.5f), bw, gfx::metrics::pillH});
            x -= bw + kActionGap;
        }
    } else {
        // Their own row under the title: the search pill first (it is what the page is for), then the actions.
        float x = 0;
        const float rowH = std::max(actions_.empty() ? 0.f : gfx::metrics::pillH, search > 0 ? gfx::metrics::searchH : 0.f);
        if (search > 0) {
            const float sw = std::min(w, 380.f);
            search_->setRect({0, g.rowY + (rowH - gfx::metrics::searchH) * 0.5f, sw, gfx::metrics::searchH});
            x = sw + 12;
        }
        for (auto* b : actions_) {
            if (!b->visible()) continue;
            const float bw = b->naturalWidth();
            b->setRect({x, g.rowY + (rowH - gfx::metrics::pillH) * 0.5f, bw, gfx::metrics::pillH});
            x += bw + kActionGap;
        }
    }
}

void PageTitle::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const Geometry g = geometry(r.w);
    if (!kicker_.empty()) c.text(kicker_, {r.x, r.y, r.w, 14}, col.fgTertiary, gfx::VAlign::Center);
    float room = r.w;
    if (g.rowY < 0) {
        float right = rightWidth();
        if (search_ && search_->visible()) right += searchWidth(r.w) + (right > 0 ? 12.f : 0.f);
        if (right > 0) room = r.w - right - 24;
    }
    c.text(title_, {r.x - 2, r.y + g.kickerH, std::max(0.f, room + 2), g.titleH}, col.fgPrimary, gfx::VAlign::Top);
    if (!meta_.empty()) c.text(meta_, {r.x, r.y + g.metaY, r.w, g.metaH}, col.fgTertiary);
    paintChildren(c);
}

// ---------------------------------------------------------------------------------------------------
// PageTabs

PageTabs::PageTabs(std::vector<std::wstring> labels, int selected) {
    hitTestVisible = false;
    for (size_t i = 0; i < labels.size(); ++i) {
        auto* b = add<ui::Button>(ui::ButtonKind::Tab, labels[i]);
        b->onClick = [this, i] { select(static_cast<int>(i)); };
        buttons_.push_back(b);
    }
    select(std::clamp(selected, 0, std::max(0, static_cast<int>(buttons_.size()) - 1)), false);
}

void PageTabs::select(int i, bool notify) {
    if (i < 0 || i >= static_cast<int>(buttons_.size())) return;
    selected_ = i;
    for (size_t k = 0; k < buttons_.size(); ++k) {
        buttons_[k]->setActive(static_cast<int>(k) == i);
        buttons_[k]->focusable = static_cast<int>(k) == i;   // one Tab stop: the selected tab
    }
    if (notify && onSelect) {
        const auto fn = onSelect;   // may rebuild the page (and this widget with it)
        fn(i);
    }
}

bool PageTabs::onKeyDown(const ui::KeyEvent& e) {
    if (e.ctrl || e.alt || buttons_.empty()) return false;
    const int n = static_cast<int>(buttons_.size());
    int next = -1;
    if (e.vk == VK_LEFT) next = std::max(0, selected_ - 1);
    else if (e.vk == VK_RIGHT) next = std::min(n - 1, selected_ + 1);
    else if (e.vk == VK_HOME) next = 0;
    else if (e.vk == VK_END) next = n - 1;
    else return false;
    if (next != selected_) {
        buttons_[next]->focusable = true;
        buttons_[next]->focus();
        select(next);
    }
    return true;
}

void PageTabs::layout() {
    float x = 0;
    for (auto* b : buttons_) {
        const float w = b->naturalWidth();
        b->setRect({x, 0, w, 36});
        x += w + 24;
    }
}

void PageTabs::paint(Canvas& c) {
    c.hline(rect().x, rect().right(), rect().bottom() - 1, colors().hairDefault);
    paintChildren(c);
}

// ---------------------------------------------------------------------------------------------------
// ScrollPage

ScrollPage::ScrollPage() {
    scroll_ = add<ui::ScrollView>();
    col_ = scroll_->setContent<ui::Column>(gfx::metrics::sectionGap, gfx::metrics::pageX, gfx::metrics::pageTop, 48.f);
}

ui::Column* ScrollPage::resetContent(float gap) {
    if (collapse_) collapse_->detach();   // the header goes with the old content
    col_ = scroll_->setContent<ui::Column>(gap, gfx::metrics::pageX, gfx::metrics::pageTop, 48.f);
    return col_;
}

void ScrollPage::layout() {
    scroll_->setRect({0, 0, rect().w, rect().h});
    if (collapse_) collapse_->setRect({0, 0, rect().w, collapse::kBarH + TrackTable::kHeaderH});
}

void ScrollPage::enableCollapse() {
    if (!collapse_) collapse_ = add<CollapseBar>(scroll_);   // after the ScrollView: painted and hit-tested above it
}

void ScrollPage::attachHeader(HeroHeader* header, ui::Widget* dock, TrackTable* columns) {
    if (collapse_) collapse_->attach(header, dock, columns);
}

// The collapsing header's backdrop goes behind the content; the bar itself is a child painted after the ScrollView.
void ScrollPage::paint(Canvas& c) {
    if (collapse_) {
        const Rect r = rect();
        c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
        collapse_->paintBackdrop(c);
        c.popTransform();
    }
    Widget::paint(c);
}

float ScrollPage::scrollOffset() const { return scroll_->scrollTarget(); }   // where a glide in flight settles

// The router calls restoreScroll() right after creating the page. A page that built its content synchronously (in
// its constructor) has already called contentReady(), so the offset is applied now; otherwise it waits for the
// content (a later unrelated rebuild must not make the page jump).
void ScrollPage::restoreScroll(float y) {
    if (y <= 0) {
        pendingScroll_ = -1;
        scroll_->scrollTo(0);
        return;
    }
    pendingScroll_ = y;
    if (built_) applyPendingScroll();
}

void ScrollPage::contentReady() {
    built_ = true;
    scroll_->contentChanged();
    // Dev (docs/DEVELOPMENT.md 5.4): SHADETUBE_HEADER_SCROLL=<dips> opens pages with a collapsing header scrolled that
    // far once their content is first built (screenshots of the collapse).
    if (!devScrolled_ && collapse_ && collapse_->header()) {
        devScrolled_ = true;
        wchar_t buf[32]{};
        if (GetEnvironmentVariableW(L"SHADETUBE_HEADER_SCROLL", buf, 32) > 0) {
            const float y = static_cast<float>(_wtof(buf));
            if (y > 0) pendingScroll_ = y;
        }
    }
    applyPendingScroll();
}

void ScrollPage::applyPendingScroll() {
    if (pendingScroll_ <= 0) return;
    // Layout must run first so maxScroll() is known: defer to the next frame.
    const float y = pendingScroll_;
    pendingScroll_ = -1;
    auto ref = life_.ref();
    Dispatcher::post([this, y, ref] {
        if (ref.expired()) return;
        scroll_->layout();
        scroll_->scrollTo(y, false);
    });
}

// Maps service exceptions to short explanations (UI language) for the error states.
std::wstring friendlyError(const std::wstring& raw, bool* offline) {
    auto has = [&](const wchar_t* s) { return raw.find(s) != std::wstring::npos; };
    if (offline) *offline = false;
    if (has(L"Not signed in") || has(L"refresh token") || has(L"invalid_grant"))
        return tr(L"Spotify oturumu yok ya da süresi doldu. Ayarlar'dan yeniden bağlan.");
    if (has(L"WinHTTP") || has(L"HTTP request") || has(L"12007") || has(L"12029") || has(L"12002")) {
        if (offline) *offline = true;   // no route / name / timeout: the offline state
        return tr(L"İnternet bağlantısı kurulamadı. Bağlantını kontrol edip tekrar dene.");
    }
    if (has(L"429")) return tr(L"Spotify çok fazla istek aldı. Birkaç saniye sonra tekrar dene.");
    if (has(L"403")) return tr(L"Spotify bu içeriğe erişime izin vermiyor (geliştirici modu kısıtlaması olabilir).");
    if (has(L"404") || has(L"Invalid mbid") || has(L"HTTP 400")) return tr(L"İçerik bulunamadı. Silinmiş ya da gizli olabilir.");
    // Anything else technical (a URL, a status line): one plain sentence instead of the raw text.
    if (has(L"://") || has(L"HTTP ")) return tr(L"Sunucu beklenmeyen bir yanıt verdi. Birazdan tekrar dene.");
    return raw;
}

void ScrollPage::showError(const std::wstring& message, std::function<void()> retry) {
    built_ = false;
    auto* c = resetContent();
    errorState(c, message, std::move(retry));
    scroll_->contentChanged();
}

bool ScrollPage::loadingCoverSlot(SharedCover& out) const {
    if (!col_ || col_->children().empty() || !dynamic_cast<SkeletonBlock*>(col_->children().front().get())) return false;
    // The header is the content's first row: its 232 DIP cover at the column's padding.
    const Rect cw = col_->toWindow(col_->rect());
    out.rect = {cw.x + gfx::metrics::pageX, cw.y + gfx::metrics::pageTop, 232, 232};
    out.radius = 2;
    out.ready = false;
    out.owner = nullptr;
    return true;
}

bool ScrollPage::headerCoverSlot(SharedCover& out) { return collapse_ && collapse_->coverSlot(out); }

void ScrollPage::showSkeleton(int rows) {
    built_ = false;
    auto* c = resetContent();
    c->add<SkeletonBlock>(rows);
    scroll_->contentChanged();
}

void ScrollPage::showLoading(Loading shape) {
    built_ = false;
    auto* c = resetContent();
    c->add<SkeletonTitle>();
    switch (shape) {
    case Loading::Cards: c->add<SkeletonCards>(3, cardMinCell()); break;
    case Loading::Shelves:
        for (int k = 0; k < 3; ++k) c->add<SkeletonShelf>(cardMinCell(), k == 1);
        break;
    case Loading::Rows: c->add<SkeletonRows>(10, true, true); break;
    }
    scroll_->contentChanged();
}

} // namespace st::app
