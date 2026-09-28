#include "app/Components.h"

#include "app/Blacklist.h"
#include "app/Router.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ---------------------------------------------------------------------------------------------------

namespace {
// placeholders/*.svg redrawn from tokens for the light theme (the SVGs bake the dark bg.overlay #211D19 and
// fg.tertiary #6B645B): same 256-unit geometry, bg.sunken paper + fg.tertiary strokes.
void drawLightPlaceholder(Canvas& c, Placeholder ph, const Rect& r, float radius) {
    const auto& col = colors();
    c.fillRounded(r, radius, col.bgSunken);
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
    // Placeholder (placeholders/*.svg are full-bleed artwork in their own colors).
    const char* name = ph == Placeholder::Artist     ? "placeholders/artist-placeholder"
                       : ph == Placeholder::Playlist ? "placeholders/playlist-placeholder"
                                                     : "placeholders/album-placeholder";
    if (circle) {
        c.fillCircle({r.cx(), r.cy()}, r.w * 0.5f, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
        c.icon("user", r.center(r.w * 0.35f, r.w * 0.35f), colors().fgTertiary);
    } else if (gfx::isLightTheme()) {
        drawLightPlaceholder(c, ph, r, radius);
    } else {
        c.fillRounded(r, radius, colors().bgOverlay);
        c.iconColored(name, r.center(r.w, r.h));
    }
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
    press_.to(1, 80);
    return true;
}

void MediaCard::onMouseUp(const ui::MouseEvent& e) {
    press_.to(0, 160);
    if (!rect().contains(e.pos)) return;
    if (onPlay && playRect().contains(e.pos)) onPlay();
    else if (onOpen) onOpen();
}

void MediaCard::onMouseMove(const ui::MouseEvent& e) {
    const bool hot = onPlay && playRect().contains(e.pos);
    if (hot != playHot_) {
        playHot_ = hot;
        invalidate();
    }
}

bool MediaCard::onActivate() {
    if (!onOpen) return false;
    press_.snap(1);
    press_.to(0, 160);
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
    // Keyboard focus shows a square card's hover state (lift shadow + play button); round cards keep just the ring.
    const float h = std::max(hover_.value(), shape_ == Shape::Square && keyboardFocused() ? 1.f : 0.f);
    const Rect art = artRect();
    const float s = 1 - 0.01f * press_;
    c.pushScale(s, {art.cx(), art.cy()});
    if (shape_ == Shape::Circle) {
        const float d = std::min(art.w, 160.f + (art.w - 160.f));
        const Rect circle = art.center(d, d);
        drawArtwork(c, images_, circle, 0, Placeholder::Artist, true);
        if (h > 0.01f) c.strokeCircle({circle.cx(), circle.cy()}, d * 0.5f, accent().base.mulAlpha(h * 0.8f), 1.5f);
    } else {
        if (h > 0.01f) c.shadow(art, 2, 24, 12, col.shadowCard.mulAlpha(h));   // cardHoverShadow
        drawArtwork(c, images_, art, 2, ph_);
        if (onPlay && h > 0.01f) {
            const Rect pr = playRect().offset(0, 8 * (1 - ui::ease(ui::Ease::Decelerate, h)));
            c.pushOpacity(h);
            c.fillCircle({pr.cx(), pr.cy()}, 24, playHot_ ? accent().hover : accent().base);
            c.icon("play", pr.center(16, 16).offset(1, 0), accent().onAccent);
            c.popLayer();
        }
    }
    c.popTransform();
    const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
    c.text(title_, tr, Color::lerp(col.fgPrimary, col.fgPrimary, h), gfx::VAlign::Center);
    c.text(subtitle_, {rect().x, tr.bottom() + 4 - 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
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

void TrackTable::setTracks(std::vector<catalog::Track> tracks) {
    tracks_ = std::move(tracks);
    selected_.clear();
    anchor_ = -1;
    nearEndSignaled_ = false;
    rebuildView();
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
    requestLayout();
    invalidate();
}

std::vector<catalog::Track> TrackTable::displayedTracks() const {
    std::vector<catalog::Track> out;
    out.reserve(view_.size());
    for (int i : view_) out.push_back(tracks_[i]);
    return out;
}

float TrackTable::preferredHeight(float) {
    return headerHeight() + (static_cast<float>(view_.size()) + static_cast<float>(loadingRows_)) * gfx::metrics::trackRowH + 8;
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
    return {r.x, r.y + headerHeight() + i * gfx::metrics::trackRowH, r.w, gfx::metrics::trackRowH};
}

int TrackTable::rowAt(float y) const {
    const float top = rect().y + headerHeight();
    if (y < top) return -1;
    const int i = static_cast<int>((y - top) / gfx::metrics::trackRowH);
    return i < static_cast<int>(view_.size()) ? i : -1;
}

TrackTable::RowText& TrackTable::rowText(int di) {
    auto it = texts_.find(di);
    if (it != texts_.end()) return it->second;
    // Bound the cache: drop everything when it grows past a few screens of rows.
    if (texts_.size() > 160) texts_.clear();
    const auto& t = tracks_[view_[di]];
    RowText rt;
    rt.title = gfx::Text(toWide(t.name), type::body);
    rt.artist = gfx::Text(toWide(t.artistLine()), type::caption);
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
    auto& tx = rowText(i);
    // Unplayable and blocked (kara liste) rows are dimmed; a blocked row still plays when picked directly.
    const float alpha = t.playable && !tx.blocked ? 1.f : 0.38f;

    if (selected) {
        c.fillRounded(r, 2, acc.tint12);
        c.fillRect({r.x, r.y, 2, r.h}, acc.base);
    } else if (hot) {
        c.fillRounded(r, 2, col.overlayHover);
    }
    // Index / play glyph / equalizer.
    const Rect numR{cl.num, r.y, gfx::metrics::numCol, r.h};
    if (playing && !hot) {
        const int frameIdx = static_cast<int>(ui::frame::now() / 66.0) % 12;
        char name[48];
        snprintf(name, sizeof name, "animations/equalizer/frame-%02d", frameIdx);
        c.icon(name, {numR.x, numR.cy() - 7, 14, 14}, acc.base);
        ui::frame::requestNext();
    } else if (hot && t.playable) {
        c.icon(playing ? "pause" : "play", {numR.x, numR.cy() - 6, 12, 12}, col.fgPrimary);
    } else {
        c.text(tx.num, {numR.x, r.y, numR.w, r.h}, (isCurrent ? acc.base : col.fgTertiary).mulAlpha(alpha), gfx::VAlign::Center);
    }
    // Title (+ art) and artist.
    float tx0 = cl.title;
    if (opts_.showArt) {
        const Rect art{cl.title, r.cy() - 20, 40, 40};
        drawArtwork(c, t.album.images, art, 2, Placeholder::Album, false, Priority::High);
        if (alpha < 1) c.fillRect(art, col.bgBase.withAlpha(0.6f));
        tx0 += 40 + 12;
    }
    const float titleW = cl.title + cl.titleW - tx0;
    float titleTextW = titleW;
    const float blockedW = tx.blocked ? std::ceil(blockedBadge_.measure().w) + 10 : 0.f;
    if (t.explicitContent) titleTextW -= 22;
    if (tx.blocked) titleTextW -= blockedW + 8;
    c.text(tx.title, {tx0, r.y + 9, titleTextW, 20}, (isCurrent ? acc.base : col.fgPrimary).mulAlpha(alpha), gfx::VAlign::Center);
    float badgeX = tx0 + std::min(titleTextW, std::ceil(tx.title.measure().w)) + 8;
    if (t.explicitContent) {
        const Rect badge{badgeX, r.y + 12, 14, 14};
        c.fillRounded(badge, 2, col.fgPrimary.withAlpha(0.14f));
        c.text(L"E", type::monoBadge, badge, col.fgSecondary, gfx::TextAlign::Center, gfx::VAlign::Center);
        badgeX += 14 + 6;
    }
    if (tx.blocked) {   // full strength on the dimmed row, like the explicit badge
        const Rect badge{badgeX, r.y + 12, blockedW, 14};
        c.fillRounded(badge, 2, col.fgPrimary.withAlpha(0.14f));
        c.text(blockedBadge_, badge.inset(5, 0), col.fgSecondary, gfx::VAlign::Center);
    }
    const Color artistCol = (hot && hoverArtist_) ? col.fgPrimary : col.fgSecondary;
    c.text(tx.artist, {tx0, r.y + 29, titleW, 16}, artistCol.mulAlpha(alpha), gfx::VAlign::Center);
    if (!t.playable)
        c.text(toUpperTr(tr(L"Kullanılamıyor")), type::monoBadge,
               {tx0 + std::min(titleW - 90, std::ceil(tx.artist.measure().w) + 10), r.y + 29, 100, 16}, col.fgTertiary,
               gfx::TextAlign::Leading, gfx::VAlign::Center);
    if (cl.albumW > 0) c.text(tx.album, {cl.album, r.y, cl.albumW, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
    if (cl.addedW > 0) c.text(tx.added, {cl.added, r.y, cl.addedW, r.h}, col.fgTertiary.mulAlpha(alpha), gfx::VAlign::Center);
    // Heart (visible when liked, or on hover for a track that can be hearted) + duration.
    const bool liked = ctx().library.isLiked(t.id);
    if (liked || (hot && ctx().library.canLike(t.id))) {
        const Color hc = liked ? acc.base : (hoverHeart_ ? col.fgPrimary : col.fgSecondary);
        c.icon(liked ? "heart-filled" : "heart", {cl.heart, r.cy() - 8, 16, 16}, hc);
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
    const Columns cl = columns();
    // Only rows intersecting the visible clip are painted (virtualization).
    const Rect vis = c.clip();
    const float top = r.y + headerHeight();
    const int first = std::max(0, static_cast<int>((vis.y - top) / gfx::metrics::trackRowH));
    const int last = std::min(static_cast<int>(view_.size()) - 1, static_cast<int>((vis.bottom() - top) / gfx::metrics::trackRowH));
    for (int i = first; i <= last; ++i) paintRow(c, i, rowRect(i), cl);
    // Skeleton rows while the next page loads.
    for (int k = 0; k < loadingRows_; ++k) {
        const Rect rr = rowRect(static_cast<int>(view_.size()) + k);
        if (!rr.intersects(vis)) continue;
        const double now = ui::frame::now();
        if (opts_.showArt) c.skeleton({cl.title, rr.cy() - 20, 40, 40}, 2, now);
        const float x = cl.title + (opts_.showArt ? 52 : 0);
        c.skeleton({x, rr.y + 14, 180, 12}, 2, now);
        c.skeleton({x, rr.y + 32, 110, 10}, 2, now);
        ui::frame::requestNext();
    }
    // Ask for more when the bottom 1.5 screens become visible.
    if (onNearEnd && !nearEndSignaled_ && last >= static_cast<int>(view_.size()) - 30 && !view_.empty()) {
        nearEndSignaled_ = true;
        onNearEnd();
    }
}

bool TrackTable::heartHit(int i, gfx::Point p) const {
    if (i < 0 || !ctx().library.canLike(tracks_[view_[i]].id)) return false;
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    return Rect{cl.heart - 6, rr.cy() - 14, 28, 28}.contains(p);
}

bool TrackTable::artistHit(int i, gfx::Point p) {
    if (i < 0) return false;
    const Columns cl = columns();
    const Rect rr = rowRect(i);
    const float x0 = cl.title + (opts_.showArt ? 52 : 0);
    const float w = std::ceil(rowText(i).artist.measure().w);
    return Rect{x0, rr.y + 29, w, 16}.contains(p);
}

void TrackTable::onMouseMove(const ui::MouseEvent& e) {
    const int i = rowAt(e.pos.y);
    const bool hh = heartHit(i, e.pos), ha = artistHit(i, e.pos);
    if (i != hover_ || hh != hoverHeart_ || ha != hoverArtist_) {
        hover_ = i;
        hoverHeart_ = hh;
        hoverArtist_ = ha;
        invalidate();
    }
}

void TrackTable::onMouseLeave() {
    hover_ = -1;
    invalidate();
}

LPCWSTR TrackTable::cursor() const { return (hoverHeart_ || hoverArtist_) ? IDC_HAND : IDC_ARROW; }

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
        showTrackMenu(sel, e.windowPos, playlistId_);
        invalidate();
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    if (heartHit(i, e.pos)) {
        ctx().library.toggleLiked(t);
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
    if (e.ctrl) {
        if (selected_.contains(ti)) selected_.erase(ti);
        else selected_.insert(ti);
        anchor_ = i;
    } else if (e.shift && anchor_ >= 0) {
        selected_.clear();
        for (int k = std::min(anchor_, i); k <= std::max(anchor_, i); ++k) selected_.insert(view_[k]);
    } else {
        selected_ = {ti};
        anchor_ = i;
    }
    pressRow_ = i;
    invalidate();
    return true;
}

void TrackTable::onMouseUp(const ui::MouseEvent&) { pressRow_ = -1; }

// Keyboard cursor = anchor_ (display index). The Window draws the focus ring around it and scrolls it into view
// after every arrow key; before the first move (Tab into the table) the cursor is row 0.
Rect TrackTable::focusRect() const {
    if (view_.empty()) return rect();
    return rowRect(std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1));
}

bool TrackTable::onKeyDown(const ui::KeyEvent& e) {
    if (view_.empty()) return false;
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
        showTrackMenu(sel, {rr.x + 48, rr.bottom()}, playlistId_);
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
