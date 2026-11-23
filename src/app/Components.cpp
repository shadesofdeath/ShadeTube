#include "app/Components.h"

#include "app/Blacklist.h"
#include "app/Display.h"
#include "app/DragDrop.h"
#include "app/LyricsFullscreen.h"
#include "app/SmartShuffle.h"
#include "app/Router.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ---------------------------------------------------------------------------------------------------

namespace {
// placeholders/*.svg redrawn from the palette for every theme but the default dark one (the SVGs bake its bg.overlay
// #211D19 and fg.tertiary #6B645B): same 256-unit geometry, bg.sunken paper (bg.overlay on dark) + fg.tertiary strokes.
void drawLightPlaceholder(Canvas& c, Placeholder ph, const Rect& r, float radius) {
    const auto& col = colors();
    c.fillRounded(r, radius, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
    const float s = std::min(r.w, r.h) / 256.f;
    const gfx::Point o{r.cx() - 128 * s, r.cy() - 128 * s};
    auto P = [&](float x, float y) { return gfx::Point{o.x + x * s, o.y + y * s}; };
    const Color ink = col.fgTertiary;
    const float w2 = std::max(1.f, 2 * s), w1 = std::max(0.75f, s);
    switch (ph) {
    case Placeholder::Album:
        c.strokeCircle(P(128, 128), 72 * s, ink, w2);
        c.strokeCircle(P(128, 128), 48 * s, ink, w1);
        c.fillCircle(P(128, 128), 16 * s, ink);
        break;
    case Placeholder::Artist:
        c.strokeCircle(P(128, 100), 36 * s, ink, w2);
        c.arc(P(128, 212), 72 * s, 270.f, 180.f, ink, w2);   // shoulders: (56,212) over the top to (200,212)
        break;
    case Placeholder::Playlist:
        c.line(P(64, 88), P(160, 88), ink, w2);
        c.line(P(64, 128), P(160, 128), ink, w2);
        c.line(P(64, 168), P(120, 168), ink, w2);
        c.icon("play", {o.x + 166 * s, o.y + 146 * s, 44 * s, 44 * s}, ink);   // the SVG's small play triangle
        break;
    case Placeholder::Folder: break;   // drawn by drawArtwork (no SVG)
    }
}
} // namespace

void drawArtwork(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius, Placeholder ph,
                 bool circle, Priority prio) {
    const int px = static_cast<int>(std::ceil(std::max(r.w, r.h) * c.scale()));
    ID2D1Bitmap1* bmp = nullptr;
    if (const auto* img = catalog::pickImage(images, px)) bmp = gfx::ImageCache::get().request(img->url, px, prio);
    if (bmp) {
        if (circle) c.imageCircle(bmp, r);
        else c.image(bmp, r, radius);
        return;
    }
    if (ph == Placeholder::Folder && !circle) {   // no placeholder SVG: the folder glyph on the placeholder surface
        c.fillRounded(r, radius, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
        const float g = std::round(std::min(r.w, r.h) * 0.3f);
        c.icon("folder", r.center(g, g), colors().fgTertiary);
        return;
    }
    // Placeholder (placeholders/*.svg are full-bleed artwork in their own colors).
    const char* name = ph == Placeholder::Artist     ? "placeholders/artist-placeholder"
                       : ph == Placeholder::Playlist ? "placeholders/playlist-placeholder"
                                                     : "placeholders/album-placeholder";
    if (circle) {
        c.fillCircle({r.cx(), r.cy()}, r.w * 0.5f, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
        c.icon("user", r.center(r.w * 0.35f, r.w * 0.35f), colors().fgTertiary);
    } else if (gfx::isLightTheme() || gfx::Theme::get().themeId() != "dark") {   // the SVGs are the default dark's colors
        drawLightPlaceholder(c, ph, r, radius);
    } else {
        c.fillRounded(r, radius, colors().bgOverlay);
        c.iconColored(name, r.center(r.w, r.h));
    }
}

void drawPlayingBars(Canvas& c, ui::Widget* owner, const Rect& box, Color color) {
    const auto* p = ctx().player;
    ui::paintEqualizer(c, owner, box, color, p && p->isPlaying() && !LyricsFullscreen::isOpen());
}

Color tileColor(std::string_view seed) {
    static const uint32_t palette[] = {0x3B2A1E, 0x1F2A22, 0x2A1E2A, 0x1C2433, 0x33261B, 0x1E2B2B, 0x2E1D1D, 0x26241C};
    uint32_t h = 2166136261u;
    for (char ch : seed) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return Color::rgb(palette[h % std::size(palette)]);
}

Color tileFill(Color tone) {
    if (!gfx::isLightTheme()) return tone;
    const gfx::Hsl h = gfx::toHsl(tone);
    if (h.s < 0.05f) return colors().bgSunken;   // neutral tones (Metal): plain sunken paper
    // Same hue as the dark tone, pale and softly saturated: a tinted paper card under fg.primary text.
    return gfx::fromHsl({h.h, std::clamp(h.s * 1.1f + 0.08f, 0.18f, 0.42f), 0.86f}, tone.a);
}

// ---------------------------------------------------------------------------------------------------
// TrackSwap / FadingArt

namespace {
constexpr float kSwapShift = 10;   // DIP
}

bool TrackSwap::update(const player::Player* p) {
    const auto* t = p ? p->current() : nullptr;
    const std::string id = t ? t->id : std::string();
    const int index = p ? p->currentOrderIndex() : -1;
    const uint64_t gen = p ? p->queueGeneration() : 0;
    const bool animate = started_ && id != id_ && !id.empty() && !id_.empty();
    if (animate) {
        // Forward unless an earlier slot of the same queue plays (previous, a queue row above); repeat-all wrapping
        // from the last slot to the first still counts as forward, and back from the first to the last as previous.
        const int last = p ? static_cast<int>(p->order().size()) - 1 : -1;
        bool back = gen == gen_ && index >= 0 && index_ >= 0 && index < index_;
        if (gen == gen_ && index == 0 && index_ == last && last > 1) back = false;
        if (gen == gen_ && index_ == 0 && index == last && last > 1) back = true;
        dir_ = back ? -1.f : 1.f;
        t_.snap(0);
        t_.to(1, ui::motion::slow, ui::Ease::Linear);
    }
    started_ = true;
    id_ = id;
    index_ = index;
    gen_ = gen;
    return animate && t_.running();
}

TrackSwap::Layer TrackSwap::outgoing() const {
    const float t = t_.value();
    if (t >= 1.f) return {0, 0};
    const float u = std::clamp(t / 0.55f, 0.f, 1.f);
    return {1 - u, -dir_ * kSwapShift * ui::ease(ui::Ease::Accelerate, u)};
}

TrackSwap::Layer TrackSwap::incoming() const {
    const float u = std::clamp((t_.value() - 0.3f) / 0.7f, 0.f, 1.f);
    const float e = ui::ease(ui::Ease::Decelerate, u);
    return {e, dir_ * kSwapShift * (1 - e)};
}

float TrackSwap::progress() const { return ui::ease(ui::Ease::Standard, std::clamp(t_.value(), 0.f, 1.f)); }

void FadingArt::draw(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius) {
    const int px = static_cast<int>(std::ceil(std::max(r.w, r.h) * c.scale()));
    const auto* img = catalog::pickImage(images, px);
    const std::string url = img ? img->url : std::string();
    if (url != url_) {   // another cover: shown at once if it is cached, else faded in when it arrives
        url_ = url;
        waiting_ = false;
        in_.snap(1);
    }
    ID2D1Bitmap1* bmp = img ? gfx::ImageCache::get().request(img->url, px, Priority::High) : nullptr;
    if (!bmp) {
        waiting_ = !url.empty();
        drawArtwork(c, {}, r, radius);
        return;
    }
    if (waiting_) {
        waiting_ = false;
        in_.snap(0);
        in_.to(1, ui::motion::slow, ui::Ease::Standard);
    }
    const float a = in_.value();
    if (a < 0.999f) drawArtwork(c, {}, r, radius);
    c.image(bmp, r, radius, a);
}

// ---------------------------------------------------------------------------------------------------
// MediaCard

MediaCard::MediaCard(std::wstring title, std::wstring subtitle, std::vector<catalog::Image> images, Shape shape,
                     Placeholder ph)
    : title_(std::move(title), type::body.withSize(13)),
      subtitle_(std::move(subtitle), type::caption),
      images_(std::move(images)),
      shape_(shape),
      ph_(ph) {
    focusable = true;
    if (shape == Shape::Circle) {
        gfx::TextOptions o;
        o.align = gfx::TextAlign::Center;
        title_.setOptions(o);
        subtitle_.setOptions(o);
        title_.setStyle(type::body);
    }
}

float MediaCard::preferredHeight(float width) { return width + 10 + 18 + 4 + 16; }

Rect MediaCard::artRect() const {
    const Rect r = rect();
    return {r.x, r.y, r.w, r.w};
}

Rect MediaCard::playRect() const {
    const Rect a = artRect();
    return {a.right() - 12 - 48, a.bottom() - 12 - 48, 48, 48};
}

bool MediaCard::onMouseDown(const ui::MouseEvent& e) {
    if (e.button == ui::MouseButton::Right) {
        if (onContext) onContext(e.windowPos);
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    playPressed_ = onPlay && playRect().contains(e.pos);
    (playPressed_ ? playPressA_ : press_).to(1, ui::motion::microMs(80));
    invalidate();
    return true;
}

void MediaCard::onMouseUp(const ui::MouseEvent& e) {
    press_.to(0, ui::motion::microMs(160));
    playPressA_.to(0, ui::motion::microMs(160));
    playPressed_ = false;
    invalidate();
    if (!rect().contains(e.pos)) return;
    if (onPlay && playRect().contains(e.pos)) {
        pagetx::arm(cover());   // "#play" opens the page too
        onPlay();
    } else if (onOpen) {
        pagetx::arm(cover());
        onOpen();
    }
}

SharedCover MediaCard::cover() const {
    const Rect art = artRect();
    return pagetx::imageCover(*this, art, shape_ == Shape::Circle ? art.w * 0.5f : 2.f, images_);
}

bool MediaCard::coverFor(const std::string& key, SharedCover& out) {
    if (images_.empty()) return false;
    out = cover();
    return out.key == key;
}

void MediaCard::onMouseMove(const ui::MouseEvent& e) {
    const bool hot = onPlay && playRect().contains(e.pos);
    if (hot != playHot_) {
        playHot_ = hot;
        playHotA_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
}

void MediaCard::onMouseLeave() {
    hover_.to(0, ui::motion::microMs(ui::motion::fast));
    playHot_ = false;
    playHotA_.to(0, ui::motion::microMs(ui::motion::fast));
}

bool MediaCard::onActivate() {
    if (!onOpen) return false;
    press_.snap(ui::motion::micro() ? 1.f : 0.f);
    press_.to(0, ui::motion::microMs(160));
    pagetx::arm(cover());
    const auto open = onOpen;   // navigating destroys this card (deferred), call a copy
    open();
    return true;
}

bool MediaCard::onKeyDown(const ui::KeyEvent& e) {
    if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && onContext) {
        const Rect a = toWindow(artRect());
        const auto context = onContext;
        context({a.x + 12, a.bottom() - 12});
        return true;
    }
    return false;
}

// Square cards: the whole card (art + title); artist cards: the round artwork.
Rect MediaCard::focusRect() const { return shape_ == Shape::Circle ? artRect() : rect(); }

void MediaCard::paint(Canvas& c) {
    const auto& col = colors();
    // Keyboard focus shows a square card's hover state (shadow + play button, no rise: the ring stays on the card);
    // round cards keep just the ring.
    const float pointer = hover_.value(), p = press_.value();
    const float h = std::max(pointer, shape_ == Shape::Square && keyboardFocused() ? 1.f : 0.f);
    const Rect art = artRect();
    const float lift = ui::card::lift(pointer, p);
    // Page transition: while this cover flies, the flight draws it (presence 0), then it lands here (0 -> 1).
    const float shown = pagetx::presence(this);
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, lift));
    c.pushScale(ui::card::pressScale(p), {art.cx(), art.cy()});
    if (shown <= 0.f) {
        // nothing: the flight draws this cover
    } else if (shape_ == Shape::Circle) {
        const float d = std::min(art.w, 160.f + (art.w - 160.f));
        const Rect circle = art.center(d, d);
        ui::card::paintBack(c, circle, pointer, true);
        drawArtwork(c, images_, circle, 0, Placeholder::Artist, true);
        ui::card::paintEdge(c, circle, h, true);
    } else {
        ui::card::paintBack(c, art, h * shown);
        drawArtwork(c, images_, art, gfx::radii().art, ph_);
        ui::card::paintEdge(c, art, h);
        if (!badge_.empty()) {
            const float bw = std::ceil(badge_.measure().w) + 12;
            const Rect pill{art.x + 8, art.y + 8, bw, 16};
            c.fillPill(pill, accent().base);
            c.text(badge_, pill.inset(6, 0), accent().onAccent, gfx::VAlign::Center);
        }
        if (onPlay)
            ui::card::paintPlay(c, playRect(), {h, playHotA_.value(), playPressA_.value()}, accent().base, accent().hover,
                                accent().onAccent);
    }
    c.popTransform();
    const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
    c.text(title_, tr, col.fgPrimary, gfx::VAlign::Center);
    c.text(subtitle_, {rect().x, tr.bottom() + 4 - 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
    c.popTransform();
}

// ---------------------------------------------------------------------------------------------------
// SectionHeader

SectionHeader::SectionHeader(std::wstring title, std::wstring meta, std::wstring link)
    : title_(std::move(title), type::title), meta_(std::move(meta), type::monoLabel) {
    hitTestVisible = false;
    if (!link.empty()) {
        link_ = add<ui::Button>(ui::ButtonKind::Link, std::move(link), "arrow-forward");
        link_->onClick = [this] {
            if (onLink) onLink();
        };
    }
}

void SectionHeader::layout() {
    if (!link_) return;
    const float w = link_->naturalWidth();
    link_->setRect({rect().w - w, 0, w, 36});
}

void SectionHeader::paint(Canvas& c) {
    const Rect r = rect();
    const float tw = std::ceil(title_.measure().w);
    c.text(title_, {r.x, r.y, tw + 1, 36}, colors().fgPrimary, gfx::VAlign::Center);
    if (!meta_.empty()) c.text(meta_, {r.x + tw + 14, r.y + 4, 200, 36}, colors().fgTertiary, gfx::VAlign::Center);
    c.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
    paintChildren(c);
}

// ---------------------------------------------------------------------------------------------------
// TrackTable

// "Engelli" = blocked (on the kara liste, the user's block list), not "disabled"; also the badge in the queue panel
// and the artist banner.
TrackTable::TrackTable(Options opts) : opts_(opts), blockedBadge_(toUpperTr(tr(L"Engelli")), type::monoBadge) {
    focusable = true;
}

TrackTable::~TrackTable() {
    if (dragging_) dragdrop::cancel();   // the page went away mid-drag
}

void TrackTable::setTracks(std::vector<catalog::Track> tracks) {
    tracks_ = std::move(tracks);
    selected_.clear();
    anchor_ = -1;
    nearEndSignaled_ = false;
    reorderRows_.clear();   // a drag under way can't move rows that were replaced (it still drops elsewhere)
    dropAt_ = -1;
    rebuildView();
}

void TrackTable::selectRows(const std::vector<int>& rows) {
    selected_.clear();
    for (int i : rows)
        if (i >= 0 && i < static_cast<int>(tracks_.size())) selected_.insert(i);
    if (!rows.empty())
        for (int d = 0; d < static_cast<int>(view_.size()); ++d)
            if (view_[d] == rows.front()) anchor_ = d;
    invalidate();
}

bool TrackTable::canReorder() const {
    return reorderable_ && onReorder && sortKey_ == SortKey::None && filter_.empty() &&
           std::none_of(tracks_.begin(), tracks_.end(), [](const catalog::Track& t) { return t.recommended; });
}

void TrackTable::appendTracks(std::vector<catalog::Track> tracks) {
    for (auto& t : tracks) tracks_.push_back(std::move(t));
    nearEndSignaled_ = false;
    rebuildView();
}

void TrackTable::setFilter(const std::wstring& text) {
    filter_ = foldForSearch(text);
    rebuildView();
}

void TrackTable::setSort(SortKey key, bool descending) {
    sortKey_ = key;
    sortDesc_ = descending;
    rebuildView();
}

void TrackTable::rebuildView() {
    view_.clear();
    view_.reserve(tracks_.size());
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (!filter_.empty()) {
            const auto& t = tracks_[i];
            const std::wstring hay = foldForSearch(toWide(t.name + " " + t.artistLine() + " " + t.album.name));
            if (hay.find(filter_) == std::wstring::npos) continue;
        }
        view_.push_back(i);
    }
    if (sortKey_ != SortKey::None) {
        auto key = sortKey_;
        std::stable_sort(view_.begin(), view_.end(), [&](int a, int b) {
            const auto& x = tracks_[a];
            const auto& y = tracks_[b];
            switch (key) {
            case SortKey::Title: return foldForSearch(toWide(x.name)) < foldForSearch(toWide(y.name));
            case SortKey::Album: return foldForSearch(toWide(x.album.name)) < foldForSearch(toWide(y.album.name));
            case SortKey::Added: return x.addedAt < y.addedAt;
            case SortKey::Duration: return x.durationMs < y.durationMs;
            default: return false;
            }
        });
        if (sortDesc_) std::reverse(view_.begin(), view_.end());
    }
    texts_.clear();
    hover_ = -1;
    hoverFade_.clear();
    requestLayout();
    invalidate();
}

std::vector<catalog::Track> TrackTable::displayedTracks() const {
    std::vector<catalog::Track> out;
    out.reserve(view_.size());
    for (int i : view_) out.push_back(tracks_[i]);
    return out;
}

TrackTable::RowMetrics TrackTable::rowMetrics() {
    // Sıkı: the two text lines (16.9 + 14.3 DIP line boxes) sit 4 DIP from the row's edges, the art 6.
    if (display::compactLists()) return {true, 40, 28, 10, 4, 17, 21, 15};
    return {false, gfx::metrics::trackRowH, gfx::metrics::trackArt, 12, 9, 20, 29, 16};
}

float TrackTable::preferredHeight(float) {
    return headerHeight() + (static_cast<float>(view_.size()) + static_cast<float>(loadingRows_)) * rowMetrics().h + 8;
}

TrackTable::Columns TrackTable::columns() const {
    const Rect r = rect();
    const float padX = gfx::metrics::rowPadX, gap = gfx::metrics::colGap;
    Columns c{};
    c.num = r.x + padX;
    const float right = r.right() - padX;
    c.dur = right - gfx::metrics::durCol;
    c.heart = c.dur - 8 - 16;
    // Collapse optional columns on narrow widths.
    const float avail = right - (c.num + gfx::metrics::numCol + gap);
    const bool album = opts_.showAlbum && avail > 700;
    const bool added = opts_.showAdded && avail > 560;
    c.addedW = added ? gfx::metrics::dateCol : 0;
    c.albumW = album ? std::min(gfx::metrics::albumCol, avail * 0.3f) : 0;
    c.added = added ? c.dur - gap - c.addedW : c.dur;
    c.album = album ? (added ? c.added : c.dur) - gap - c.albumW : c.added;
    c.title = c.num + gfx::metrics::numCol + gap;
    c.titleW = (album ? c.album : added ? c.added : c.heart) - gap - c.title;
    return c;
}

Rect TrackTable::rowRect(int i) const {
    const Rect r = rect();
    const float h = rowMetrics().h;
    return {r.x, r.y + headerHeight() + i * h, r.w, h};
}

int TrackTable::rowAt(float y) const {
    const float top = rect().y + headerHeight();
    if (y < top) return -1;
    const int i = static_cast<int>((y - top) / rowMetrics().h);
    return i < static_cast<int>(view_.size()) ? i : -1;
}

TrackTable::RowText& TrackTable::rowText(int di) {
    if (const bool compact = display::compactLists(); compact != textsCompact_) {   // density changed: new styles
        texts_.clear();
        textsCompact_ = compact;
    }
    auto it = texts_.find(di);
    if (it != texts_.end()) return it->second;
    // Bound the cache: drop everything when it grows past a few screens of rows.
    if (texts_.size() > 160) texts_.clear();
    const auto& t = tracks_[view_[di]];
    RowText rt;
    rt.title = gfx::Text(toWide(t.name), textsCompact_ ? type::body.withSize(13) : type::body);
    rt.artist = gfx::Text(toWide(t.artistLine()), textsCompact_ ? type::tiny : type::caption);
    rt.album = gfx::Text(toWide(t.album.name), type::secondary);
    rt.added = gfx::Text(relativeTime(t.addedAt), type::caption);
    rt.dur = gfx::Text(ui::formatDuration(t.durationMs), type::monoDuration);
    gfx::TextOptions right;
    right.align = gfx::TextAlign::Trailing;
    rt.dur.setOptions(right);
    wchar_t num[16];
    swprintf(num, 16, L"%02d", opts_.albumNumbering ? t.trackNumber : di + 1);
    rt.num = gfx::Text(num, type::monoDuration);
    rt.blocked = blacklist::isBlocked(t);
    return texts_.emplace(di, std::move(rt)).first->second;
}

void TrackTable::paintHeader(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const Columns cl = columns();
    const float y = r.y;
    auto label = [&](std::wstring s, float x, float w, SortKey key, bool rightAlign = false) {
        if (sortKey_ == key && key != SortKey::None) s += sortDesc_ ? L" ↓" : L" ↑";
        c.text(s, type::monoLabel, {x, y, w, kHeaderH}, sortKey_ == key && key != SortKey::None ? col.fgSecondary : col.fgTertiary,
               rightAlign ? gfx::TextAlign::Trailing : gfx::TextAlign::Leading, gfx::VAlign::Center);
    };
    label(L"#", cl.num, gfx::metrics::numCol, SortKey::None);
    label(toUpperTr(tr(L"Başlık")), cl.title, cl.titleW, SortKey::Title);
    if (cl.albumW > 0)
        label(opts_.albumNumbering ? std::wstring() : toUpperTr(tr(L"Albüm")), cl.album, cl.albumW, SortKey::Album);
    if (cl.addedW > 0) label(toUpperTr(tr(L"Eklendi")), cl.added, cl.addedW, SortKey::Added);
    label(toUpperTr(tr(L"Süre")), cl.dur, gfx::metrics::durCol, SortKey::Duration, true);
    c.hline(r.x, r.right(), y + kHeaderH, col.hairDefault);
}

void TrackTable::paintRow(Canvas& c, int i, const Rect& r, const Columns& cl) {
    const auto& col = colors();
    const auto& acc = accent();
    const int ti = view_[i];
    const auto& t = tracks_[ti];
    const bool selected = selected_.contains(ti);
    const auto* cur = ctx().player ? ctx().player->current() : nullptr;
    const bool isCurrent = cur && !t.id.empty() && cur->id == t.id;
    const bool playing = isCurrent && ctx().player->isPlaying();
    const bool hot = i == hover_;
    const float hv = hoverFade_.of(i);   // the hover look fades in / out (ui::HoverFade)
    auto& tx = rowText(i);
    const RowMetrics m = rowMetrics();
    const float titleCy = r.y + m.titleY + m.titleH * 0.5f;   // badges are centered on the title line
    // Unplayable and blocked (kara liste) rows are dimmed; a blocked row still plays when picked directly.
    const float alpha = t.playable && !tx.blocked ? 1.f : 0.38f;

    if (selected) {
        c.fillRounded(r, gfx::radii().surface, acc.tint12);
        c.fillRect({r.x, r.y, 2, r.h}, acc.base);
    } else {
        if (t.recommended) c.fillRounded(r, gfx::radii().surface, acc.tint06);   // "Geliştir": a recommendation, not the list's own song
        if (hv > 0.001f) c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hv));
    }
    // Index / play glyph / equalizer: the current track's row shows the playing bars (moving while it plays, frozen
    // while paused); hovering crossfades the cell to the play / pause glyph (motion-spec: 120 ms).
    const Rect numR{cl.num, r.y, gfx::metrics::numCol, r.h};
    const float glyph = t.playable ? hv : 0.f;
    if (glyph < 0.999f) {
        if (isCurrent) drawPlayingBars(c, this, {numR.x, numR.cy() - 7, 14, 14}, acc.base.mulAlpha(1 - glyph));
        else c.text(tx.num, {numR.x, r.y, numR.w, r.h}, col.fgTertiary.mulAlpha(alpha * (1 - glyph)), gfx::VAlign::Center);
    }
    if (glyph > 0.001f) c.icon(playing ? "pause" : "play", {numR.x, numR.cy() - 6, 12, 12}, col.fgPrimary.mulAlpha(glyph));
    // Title (+ art) and artist.
    float tx0 = cl.title;
    if (opts_.showArt) {
        const Rect art{cl.title, r.cy() - m.art * 0.5f, m.art, m.art};
        drawArtwork(c, t.album.images, art, gfx::radii().art, Placeholder::Album, false, Priority::High);
        if (alpha < 1) c.fillRect(art, col.bgBase.withAlpha(0.6f));
        tx0 += m.art + m.artGap;
    }
    const float titleW = cl.title + cl.titleW - tx0;
    float titleTextW = titleW;
    const float blockedW = tx.blocked ? std::ceil(blockedBadge_.measure().w) + 10 : 0.f;
    if (t.explicitContent) titleTextW -= 22;
    if (tx.blocked) titleTextW -= blockedW + 8;
    if (t.recommended) titleTextW -= smartshuffle::badgeWidth() + 8;
    c.text(tx.title, {tx0, r.y + m.titleY, titleTextW, m.titleH}, (isCurrent ? acc.base : col.fgPrimary).mulAlpha(alpha),
           gfx::VAlign::Center);
    float badgeX = tx0 + std::min(titleTextW, std::ceil(tx.title.measure().w)) + 8;
    if (t.explicitContent) {
        const Rect badge{badgeX, titleCy - 7, 14, 14};
        c.fillRounded(badge, gfx::radii().badge, col.fgPrimary.withAlpha(0.14f));
        c.text(L"E", type::monoBadge, badge, col.fgSecondary, gfx::TextAlign::Center, gfx::VAlign::Center);
        badgeX += 14 + 6;
    }
    if (tx.blocked) {   // full strength on the dimmed row, like the explicit badge
        const Rect badge{badgeX, titleCy - 7, blockedW, 14};
        c.fillRounded(badge, gfx::radii().badge, col.fgPrimary.withAlpha(0.14f));
        c.text(blockedBadge_, badge.inset(5, 0), col.fgSecondary, gfx::VAlign::Center);
        badgeX += blockedW + 6;
    }
    if (t.recommended) smartshuffle::drawBadge(c, badgeX, titleCy);
    const Color artistCol = (hot && hoverArtist_) ? col.fgPrimary : col.fgSecondary;
    c.text(tx.artist, {tx0, r.y + m.artistY, titleW, m.artistH}, artistCol.mulAlpha(alpha), gfx::VAlign::Center);
    if (!t.playable)
        c.text(toUpperTr(tr(L"Kullanılamıyor")), type::monoBadge,
               {tx0 + std::min(titleW - 90, std::ceil(tx.artist.measure().w) + 10), r.y + m.artistY, 100, m.artistH},
               col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    if (cl.albumW > 0) c.text(tx.album, {cl.album, r.y, cl.albumW, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
    if (cl.addedW > 0) c.text(tx.added, {cl.added, r.y, cl.addedW, r.h}, col.fgTertiary.mulAlpha(alpha), gfx::VAlign::Center);
    // A recommendation of a "Geliştir" list: "+" (always) and "×" (on hover) instead of the heart.
    if (t.recommended && onAddRecommended) {
        c.icon("plus", {cl.heart, r.cy() - 8, 16, 16}, hot && hoverRecAdd_ ? acc.base : col.fgPrimary);
        if (hv > 0.001f && onHideRecommended)
            c.icon("close", {cl.heart - 28, r.cy() - 7, 14, 14},
                   (hot && hoverRecHide_ ? col.fgPrimary : col.fgSecondary).mulAlpha(hv));
        c.text(tx.dur, {cl.dur, r.y, gfx::metrics::durCol, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
        return;
    }
    // Heart (visible when liked, or fading in on hover for a track that can be hearted; a click plays the like /
    // unlike feedback) + duration.
    const bool liked = ctx().library.isLiked(t.id);
    const bool popping = !popId_.empty() && popId_ == t.id && pop_.running();
    const float heartA = liked || popping ? 1.f : ctx().library.canLike(t.id) ? hv : 0.f;
    if (heartA > 0.001f) {
        const Rect hr{cl.heart, r.cy() - 8, 16, 16};
        const Color outline = (hot && hoverHeart_ ? col.fgPrimary : col.fgSecondary).mulAlpha(heartA);
        if (popping) pop_.draw(c, hr, liked, "heart", "heart-filled", outline, acc.base);
        else c.icon(liked ? "heart-filled" : "heart", hr, liked ? acc.base : outline);
    }
    c.text(tx.dur, {cl.dur, r.y, gfx::metrics::durCol, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
}

void TrackTable::paint(Canvas& c) {
    const Rect r = rect();
    if (const uint64_t rev = blacklist::revision(); rev != blockRev_) {   // blocked / unblocked: re-evaluate the rows
        blockRev_ = rev;
        texts_.clear();
    }
    if (opts_.showHeader) paintHeader(c, {r.x, r.y, r.w, kHeaderH});
    // The playing song liked / unliked elsewhere (the player bar's heart, Ctrl+L, a menu): its row's heart answers too.
    if (const auto* cur = ctx().player ? ctx().player->current() : nullptr) {
        const bool liked = ctx().library.isLiked(cur->id);
        if (cur->id == curId_ && liked != curLiked_ && !(popId_ == cur->id && pop_.running())) {
            popId_ = cur->id;
            pop_.start(liked);
        }
        curId_ = cur->id;
        curLiked_ = liked;
    }
    const Columns cl = columns();
    // Only rows intersecting the visible clip are painted (virtualization).
    const Rect vis = c.clip();
    const float top = r.y + headerHeight();
    const RowMetrics m = rowMetrics();
    const int first = std::max(0, static_cast<int>((vis.y - top) / m.h));
    const int last = std::min(static_cast<int>(view_.size()) - 1, static_cast<int>((vis.bottom() - top) / m.h));
    for (int i = first; i <= last; ++i) paintRow(c, i, rowRect(i), cl);
    if (dropAt_ >= 0) {   // own rows dragged over the list: they go in front of this row
        const float y = rowRect(dropAt_).y;
        c.fillRect({r.x, y - 1, r.w, 2}, accent().base);
        c.fillCircle({r.x + 3, y}, 3, accent().base);
    }
    // Skeleton rows while the next page loads.
    for (int k = 0; k < loadingRows_; ++k) {
        const Rect rr = rowRect(static_cast<int>(view_.size()) + k);
        if (!rr.intersects(vis)) continue;
        const double now = ui::frame::now();
        if (opts_.showArt) c.skeleton({cl.title, rr.cy() - m.art * 0.5f, m.art, m.art}, gfx::radii().art, now);
        const float x = cl.title + (opts_.showArt ? m.art + m.artGap : 0);
        c.skeleton({x, rr.y + m.titleY + m.titleH * 0.5f - 6, 180, 12}, gfx::radii().badge, now);   // the title and artist lines
        c.skeleton({x, rr.y + m.artistY + m.artistH * 0.5f - 5, 110, 10}, gfx::radii().badge, now);
        ui::frame::requestNext();
    }
    // Ask for more when the bottom 1.5 screens become visible.
    if (onNearEnd && !nearEndSignaled_ && last >= static_cast<int>(view_.size()) - 30 && !view_.empty()) {
        nearEndSignaled_ = true;
        onNearEnd();
    }
}

bool TrackTable::recAddHit(int i, gfx::Point p) const {
    if (i < 0 || !onAddRecommended || !tracks_[view_[i]].recommended) return false;
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    return Rect{cl.heart - 6, rr.cy() - 14, 28, 28}.contains(p);
}

bool TrackTable::recHideHit(int i, gfx::Point p) const {
    if (i < 0 || !onHideRecommended || !tracks_[view_[i]].recommended) return false;
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    return Rect{cl.heart - 34, rr.cy() - 13, 26, 26}.contains(p);
}

bool TrackTable::heartHit(int i, gfx::Point p) const {
    if (i < 0 || !ctx().library.canLike(tracks_[view_[i]].id)) return false;
    if (tracks_[view_[i]].recommended && onAddRecommended) return false;   // "+" takes its place
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    return Rect{cl.heart - 6, rr.cy() - 14, 28, 28}.contains(p);
}

bool TrackTable::artistHit(int i, gfx::Point p) {
    if (i < 0) return false;
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    const RowMetrics m = rowMetrics();
    const float x0 = cl.title + (opts_.showArt ? m.art + m.artGap : 0);
    const float w = std::ceil(rowText(i).artist.measure().w);
    return Rect{x0, rr.y + m.artistY, w, m.artistH}.contains(p);
}

void TrackTable::onMouseMove(const ui::MouseEvent& e) {
    // A pressed row that travels past the threshold becomes a drag of its songs.
    if (pressRow_ >= 0 && pressed() && !dragging_ &&
        std::hypot(e.windowPos.x - pressPos_.x, e.windowPos.y - pressPos_.y) > dragdrop::kThreshold) {
        auto tracks = dragTracks();
        if (!tracks.empty()) {
            dragging_ = true;
            collapseOnUp_ = -1;
            reorderRows_ = canReorder() ? dragRows() : std::vector<int>{};
            dragdrop::begin(std::move(tracks), e.windowPos);
        }
    }
    if (dragging_) dragdrop::move(e.windowPos);
    const int i = rowAt(e.pos.y);
    const bool hh = heartHit(i, e.pos), ha = artistHit(i, e.pos), ra = recAddHit(i, e.pos), rh = recHideHit(i, e.pos);
    if (i != hover_ || hh != hoverHeart_ || ha != hoverArtist_ || ra != hoverRecAdd_ || rh != hoverRecHide_) {
        hover_ = i;
        hoverFade_.set(i);
        hoverHeart_ = hh;
        hoverArtist_ = ha;
        hoverRecAdd_ = ra;
        hoverRecHide_ = rh;
        invalidate();
    }
}

void TrackTable::onMouseLeave() {
    hover_ = -1;
    hoverFade_.set(-1);
    invalidate();
}

LPCWSTR TrackTable::cursor() const {
    return (hoverHeart_ || hoverArtist_ || hoverRecAdd_ || hoverRecHide_) ? IDC_HAND : IDC_ARROW;
}

void TrackTable::headerClick(float x) {
    const Columns cl = columns();
    SortKey key = SortKey::None;
    if (x >= cl.title && x < cl.title + cl.titleW) key = SortKey::Title;
    else if (cl.albumW > 0 && x >= cl.album && x < cl.album + cl.albumW) key = SortKey::Album;
    else if (cl.addedW > 0 && x >= cl.added && x < cl.added + cl.addedW) key = SortKey::Added;
    else if (x >= cl.dur) key = SortKey::Duration;
    if (key == SortKey::None) return;
    // Cycle: asc -> desc -> none
    if (sortKey_ != key) setSort(key, false);
    else if (!sortDesc_) setSort(key, true);
    else setSort(SortKey::None, false);
}

bool TrackTable::onMouseDown(const ui::MouseEvent& e) {
    if (opts_.showHeader && e.pos.y < rect().y + kHeaderH && e.button == ui::MouseButton::Left) {
        headerClick(e.pos.x);
        return true;
    }
    const int i = rowAt(e.pos.y);
    if (i < 0) return false;
    const int ti = view_[i];
    const auto& t = tracks_[ti];
    if (e.button == ui::MouseButton::Right) {
        if (!selected_.contains(ti)) {
            selected_ = {ti};
            anchor_ = i;
        }
        std::vector<catalog::Track> sel;
        for (int k : view_)
            if (selected_.contains(k)) sel.push_back(tracks_[k]);
        showTrackMenu(sel, e.windowPos, playlistId_, recommendKey_, menuRemove());
        invalidate();
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    if (recAddHit(i, e.pos)) {
        const auto add = onAddRecommended;   // the page may rebuild the table
        add(t);
        return true;
    }
    if (recHideHit(i, e.pos)) {
        const auto hide = onHideRecommended;
        hide(t);
        return true;
    }
    if (heartHit(i, e.pos)) {
        const std::string id = t.id;   // the library's notify may rebuild the rows
        ctx().library.toggleLiked(t);
        popId_ = id;
        pop_.start(ctx().library.isLiked(id));
        invalidate();
        return true;
    }
    if (artistHit(i, e.pos) && !t.artists.empty() && !t.artists[0].id.empty()) {
        ctx().router->navigate({RouteKind::Artist, t.artists[0].id});
        return true;
    }
    const Columns cl = columns();
    const bool onNumber = e.pos.x >= cl.num && e.pos.x < cl.num + gfx::metrics::numCol;
    if (e.clicks == 2 || onNumber) {
        const auto* cur = ctx().player->current();
        if (onNumber && cur && cur->id == t.id) ctx().player->togglePause();
        else if (onPlay && t.playable) onPlay(i);
        return true;
    }
    collapseOnUp_ = -1;
    if (e.ctrl) {
        if (selected_.contains(ti)) selected_.erase(ti);
        else selected_.insert(ti);
        anchor_ = i;
    } else if (e.shift && anchor_ >= 0) {
        selected_.clear();
        for (int k = std::min(anchor_, i); k <= std::max(anchor_, i); ++k) selected_.insert(view_[k]);
    } else if (selected_.contains(ti) && selected_.size() > 1) {
        collapseOnUp_ = ti;   // keep the selection: this press may drag all of it
        anchor_ = i;
    } else {
        selected_ = {ti};
        anchor_ = i;
    }
    pressRow_ = i;
    pressPos_ = e.windowPos;
    invalidate();
    return true;
}

void TrackTable::onMouseUp(const ui::MouseEvent& e) {
    if (dragging_) {
        dragging_ = false;
        dragdrop::finish(e.windowPos);
    } else if (collapseOnUp_ >= 0) {
        selected_ = {collapseOnUp_};
        invalidate();
    }
    reorderRows_.clear();
    dropAt_ = -1;
    collapseOnUp_ = -1;
    pressRow_ = -1;
}

// ---- Reordering (the table as a drop target for its own rows) ------------------------------------------------------

bool TrackTable::dragOver(const DragPayload& payload, gfx::Point windowPos) {
    if (reorderRows_.empty() || payload.fromExplorer() || !canReorder()) return false;
    autoScroll(windowPos);
    const int at = dropIndexAt(windowPos);
    if (at != dropAt_) {
        dropAt_ = at;
        invalidate();
    }
    return true;
}

void TrackTable::dragLeave() {
    // reorderRows_ stays until the release: drop() comes right after this.
    lastScrollTick_ = 0;
    if (dropAt_ >= 0) {
        dropAt_ = -1;
        invalidate();
    }
}

void TrackTable::drop(const DragPayload&, gfx::Point windowPos) {
    if (reorderRows_.empty() || !canReorder()) return;
    const int at = dropIndexAt(windowPos);
    // Custom order, unfiltered: display index == tracks_ index.
    const int before = at < static_cast<int>(view_.size()) ? view_[at] : static_cast<int>(tracks_.size());
    std::vector<int> rows = std::move(reorderRows_);
    reorderRows_.clear();
    dropAt_ = -1;
    // After the event: the page reorders (or rebuilds) the table, and the release is still being handled here.
    Dispatcher::post([ref = life_.ref(), this, rows = std::move(rows), before] {
        if (!ref.expired() && onReorder) onReorder(rows, before);
    });
}

int TrackTable::dropIndexAt(gfx::Point windowPos) const {
    const gfx::Point p = fromWindow(windowPos);
    const float top = rect().y + headerHeight();
    const int i = static_cast<int>(std::floor((p.y - top) / rowMetrics().h + 0.5f));
    return std::clamp(i, 0, static_cast<int>(view_.size()));
}

void TrackTable::autoScroll(gfx::Point windowPos) {
    const double now = ui::frame::realNow(), last = lastScrollTick_;
    lastScrollTick_ = now;
    if (last <= 0) return;
    ui::ScrollView* sv = nullptr;
    for (ui::Widget* w = parent(); w && !sv; w = w->parent()) sv = dynamic_cast<ui::ScrollView*>(w);
    if (!sv) return;
    // Near the visible top (below a sticky header) / bottom edge: faster the closer the pointer is to the edge.
    const Rect vr = sv->toWindow(sv->rect());
    const float top = vr.y + sv->topInset, bottom = vr.bottom(), edge = 48, y = windowPos.y;
    float v = 0;
    if (y >= top && y < top + edge) v = -(1 - (y - top) / edge);
    else if (y > bottom - edge && y <= bottom) v = 1 - (bottom - y) / edge;
    if (v == 0) return;
    const float dt = static_cast<float>(std::min(now - last, 100.0)) / 1000.f;
    sv->scrollTo(std::clamp(sv->scrollY() + v * 900.f * dt, 0.f, sv->maxScroll()), false);
}

std::function<void(const std::vector<catalog::Track>&)> TrackTable::menuRemove() {
    if (!onRemove) return {};
    return [ref = life_.ref(), fn = onRemove](const std::vector<catalog::Track>& rows) {
        if (!ref.expired()) fn(rows);
    };
}

std::vector<catalog::Track> TrackTable::dragTracks() const {
    std::vector<catalog::Track> out;
    if (pressRow_ < 0 || pressRow_ >= static_cast<int>(view_.size())) return out;
    const int pressed = view_[pressRow_];
    if (!selected_.contains(pressed)) {   // a ctrl-click just took it out of the selection: drag it alone
        if (!tracks_[pressed].id.empty()) out.push_back(tracks_[pressed]);
    } else {
        for (int k : view_)   // display order
            if (selected_.contains(k) && !tracks_[k].id.empty()) out.push_back(tracks_[k]);
    }
    for (auto& t : out) t.recommended = false;   // dropped somewhere, a recommendation becomes the user's song
    return out;
}

std::vector<int> TrackTable::dragRows() const {
    std::vector<int> out;
    if (pressRow_ < 0 || pressRow_ >= static_cast<int>(view_.size())) return out;
    const int pressed = view_[pressRow_];
    if (!selected_.contains(pressed)) return {pressed};
    for (int k : view_)
        if (selected_.contains(k)) out.push_back(k);
    return out;
}

// ---- Song preview (app/Preview) ----------------------------------------------------------------------------------------

std::optional<PreviewHit> TrackTable::previewAt(gfx::Point windowPos, bool keyboard) {
    if (view_.empty()) return std::nullopt;
    int i = -1;
    if (keyboard) {
        i = std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1);
    } else {
        const gfx::Point p = fromWindow(windowPos);
        if (opts_.showHeader && p.y < rect().y + kHeaderH) return std::nullopt;
        i = rowAt(p.y);
        if (i < 0) return std::nullopt;
        const Columns cl = columns();
        const bool onNumber = p.x >= cl.num && p.x < cl.num + gfx::metrics::numCol;
        if (onNumber || heartHit(i, p) || artistHit(i, p) || recAddHit(i, p) || recHideHit(i, p)) return std::nullopt;
    }
    const auto& t = tracks_[view_[i]];
    if (!t.playable) return std::nullopt;
    return PreviewHit{preview::songTarget(t), toWindow(rowRect(i))};
}

std::optional<PreviewHit> MediaCard::previewAt(gfx::Point windowPos, bool keyboard) {
    if (!previewTarget) return std::nullopt;
    if (!keyboard) {
        const gfx::Point p = fromWindow(windowPos);
        if (!artRect().contains(p) || (onPlay && playRect().contains(p))) return std::nullopt;
    }
    return PreviewHit{*previewTarget, toWindow(artRect())};
}

std::vector<catalog::Track> TrackTable::selectedOwnRows() const {
    std::vector<catalog::Track> out;
    for (int k : view_)
        if (selected_.contains(k) && !tracks_[k].recommended) out.push_back(tracks_[k]);
    return out;
}

// Keyboard cursor = anchor_ (display index). The Window draws the focus ring around it and scrolls it into view
// after every arrow key; before the first move (Tab into the table) the cursor is row 0.
Rect TrackTable::focusRect() const {
    if (view_.empty()) return rect();
    return rowRect(std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1));
}

bool TrackTable::onKeyDown(const ui::KeyEvent& e) {
    if (dragging_ && e.vk == VK_ESCAPE) {
        dragging_ = false;
        pressRow_ = -1;
        reorderRows_.clear();
        dragdrop::cancel();
        return true;
    }
    if (view_.empty()) return false;
    if (e.vk == VK_DELETE && !e.ctrl && !e.alt) {   // remove the selected songs from this list
        if (!onRemove || dragging_) return false;
        auto rows = selectedOwnRows();
        if (rows.empty() || e.repeat) return true;
        Dispatcher::post([ref = life_.ref(), this, rows = std::move(rows)] {
            if (!ref.expired() && onRemove) onRemove(rows);
        });
        return true;
    }
    const int n = static_cast<int>(view_.size());
    int cur = std::min(anchor_, n - 1);   // the view may have shrunk (filter) since the anchor was set
    switch (e.vk) {
    case VK_DOWN: cur = std::min(n - 1, cur + 1); break;
    case VK_UP: cur = std::max(0, cur - 1); break;
    case VK_NEXT: cur = std::min(n - 1, std::max(cur, 0) + 10); break;   // PageDown
    case VK_PRIOR: cur = std::max(0, cur - 10); break;                   // PageUp
    case VK_HOME: cur = 0; break;
    case VK_END: cur = n - 1; break;
    case VK_RETURN: {
        const int i = std::max(cur, 0);
        if (onPlay && tracks_[view_[i]].playable) onPlay(i);
        return true;
    }
    case VK_APPS:
    case VK_F10: {
        if (e.vk == VK_F10 && !e.shift) return false;
        const int i = std::max(cur, 0);
        if (!selected_.contains(view_[i])) {
            selected_ = {view_[i]};
            anchor_ = i;
        }
        std::vector<catalog::Track> sel;
        for (int k : view_)
            if (selected_.contains(k)) sel.push_back(tracks_[k]);
        const Rect rr = toWindow(rowRect(i));
        showTrackMenu(sel, {rr.x + 48, rr.bottom()}, playlistId_, recommendKey_, menuRemove());
        invalidate();
        return true;
    }
    case 'A':
        if (e.ctrl) {
            for (int i : view_) selected_.insert(i);
            invalidate();
            return true;
        }
        return false;
    default: return false;
    }
    anchor_ = cur;
    selected_ = {view_[cur]};
    invalidate();
    return true;
}

} // namespace st::app
