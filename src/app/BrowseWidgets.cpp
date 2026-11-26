#include "app/BrowseWidgets.h"

#include "app/BrowseModel.h"
#include "app/PlaylistTransfer.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
using namespace catalog;
namespace type = gfx::type;

// =====================================================================================================================
// ChipTabs

namespace {
constexpr float kChipPadX = 14, kChipGap = 8, kRowGap = 8;
}

ChipTabs::ChipTabs(std::vector<std::wstring> labels, int selected) {
    focusable = true;
    setLabels(std::move(labels), selected);
}

void ChipTabs::setLabels(std::vector<std::wstring> labels, int selected) {
    chips_.clear();
    for (auto& l : labels) chips_.push_back({gfx::Text(std::move(l), type::body.withSize(13)), {}});
    selected_ = chips_.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(chips_.size()) - 1);
    hot_ = pressed_ = -1;
    hover_.clear();
    placed_ = false;
    requestLayout();
    invalidate();
}

float ChipTabs::place(float width, bool apply) {
    float x = 0, y = 0;
    for (auto& ch : chips_) {
        const float w = std::ceil(ch.label.measure().w) + 2 * kChipPadX;
        if (x > 0 && x + w > width) {
            x = 0;
            y += kH + kRowGap;
        }
        if (apply) ch.r = {x, y, w, kH};
        x += w + kChipGap;
    }
    return chips_.empty() ? 0.f : y + kH;
}

float ChipTabs::preferredHeight(float width) { return place(width, false); }

void ChipTabs::layout() {
    place(rect().w, true);
    if (selected_ < 0) return;
    const Rect s = chips_[selected_].r;
    if (!placed_ || !pillX_.running()) {   // first layout / a reflow: the pill stands on its chip
        pillX_.snap(s.x);
        pillY_.snap(s.y);
        pillW_.snap(s.w);
    }
    placed_ = true;
}

void ChipTabs::select(int i, bool notify) {
    if (chips_.empty()) return;
    i = std::clamp(i, 0, static_cast<int>(chips_.size()) - 1);
    if (i != selected_) {
        selected_ = i;
        const Rect s = chips_[i].r;
        const double ms = ui::motion::microMs(ui::motion::normal);
        pillX_.to(s.x, ms);
        pillY_.to(s.y, ms);
        pillW_.to(s.w, ms);
        invalidate();
    }
    if (notify && onSelect) {
        const auto fn = onSelect;   // may rebuild the page around this widget
        fn(i);
    }
}

int ChipTabs::chipAt(gfx::Point p) const {
    const Rect r = rect();
    for (size_t i = 0; i < chips_.size(); ++i)
        if (chips_[i].r.offset(r.x, r.y).contains(p)) return static_cast<int>(i);
    return -1;
}

void ChipTabs::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const Rect pill = selected_ >= 0 ? Rect{r.x + pillX_.value(), r.y + pillY_.value(), pillW_.value(), kH} : Rect{};
    for (size_t i = 0; i < chips_.size(); ++i) {
        const Rect cr = chips_[i].r.offset(r.x, r.y);
        const float h = hover_.of(static_cast<int>(i));
        const bool sel = static_cast<int>(i) == selected_;
        if (!sel) {
            c.fillPill(cr, col.overlayHover.mulAlpha(h));
            c.strokePill(cr.inset(0.5f), Color::lerp(col.hairControl, col.hairStrong, h));
        }
    }
    if (selected_ >= 0) c.fillPill(pill, col.fgPrimary);
    for (size_t i = 0; i < chips_.size(); ++i) {
        const Rect cr = chips_[i].r.offset(r.x, r.y);
        // How much of the chip the sliding pill covers: its label turns from the outline's color to the inverse one.
        const float covered =
            pill.w > 0 && std::abs(pill.y - cr.y) < 1
                ? std::clamp((std::min(pill.right(), cr.right()) - std::max(pill.x, cr.x)) / std::max(1.f, cr.w), 0.f, 1.f)
                : 0.f;
        const Color idle = Color::lerp(col.fgSecondary, col.fgPrimary, hover_.of(static_cast<int>(i)));
        c.text(chips_[i].label, cr.inset(kChipPadX, 0), Color::lerp(idle, col.fgInverse, covered), gfx::VAlign::Center);
    }
}

bool ChipTabs::onMouseDown(const ui::MouseEvent& e) {
    if (e.button != ui::MouseButton::Left) return false;
    pressed_ = chipAt(e.pos);
    return pressed_ >= 0;
}

void ChipTabs::onMouseUp(const ui::MouseEvent& e) {
    const int i = chipAt(e.pos);
    const int p = pressed_;
    pressed_ = -1;
    if (i >= 0 && i == p && i != selected_) select(i);
}

void ChipTabs::onMouseMove(const ui::MouseEvent& e) {
    const int i = chipAt(e.pos);
    if (i == hot_) return;
    hot_ = i;
    hover_.set(i == selected_ ? -1 : i);
    invalidate();
}

void ChipTabs::onMouseLeave() {
    hot_ = -1;
    hover_.set(-1);
    invalidate();
}

bool ChipTabs::onKeyDown(const ui::KeyEvent& e) {
    if (chips_.empty() || e.ctrl || e.alt) return false;
    const int n = static_cast<int>(chips_.size());
    int i = selected_;
    switch (e.vk) {
    case VK_LEFT: i = std::max(0, i - 1); break;
    case VK_RIGHT: i = std::min(n - 1, i + 1); break;
    case VK_HOME: i = 0; break;
    case VK_END: i = n - 1; break;
    default: return false;
    }
    if (i != selected_) select(i);
    return true;
}

Rect ChipTabs::focusRect() const {
    if (selected_ < 0) return rect();
    return chips_[selected_].r.offset(rect().x, rect().y);
}

// =====================================================================================================================
// FadeColumn

void FadeColumn::paint(Canvas& c) {
    const float a = alpha_.value();
    if (a >= 0.999f) return ui::Column::paint(c);
    if (a <= 0.001f) return;
    c.pushOpacity(a);
    ui::Column::paint(c);
    c.popLayer();
}

// =====================================================================================================================
// Cards

std::wstring releaseKindLabel(const std::string& primaryType) {
    const std::string k = browse::releaseKind(primaryType);
    if (k == "Single") return tr(L"Tekli");
    if (k == "EP") return L"EP";
    if (k == "Compilation") return tr(L"Derleme");
    return tr(L"Albüm");
}

std::wstring albumSubtitle(const Album& a, bool withArtist) {
    std::wstring s = toWide(a.year());
    const std::wstring kind = releaseKindLabel(a.primaryType);
    s = s.empty() ? kind : s + L" · " + kind;
    if (withArtist && !a.artists.empty()) s += L" · " + toWide(a.artists[0].name);
    return s;
}

std::wstring artistSubtitle(const Artist& a) {
    std::wstring sub = a.type == "Group" ? tr(L"Grup") : tr(L"Sanatçı");
    if (!a.country.empty()) sub += L" · " + toWide(a.country);
    return sub;
}

void showAlbumCardMenu(const std::string& id, const sync::Target& offline, gfx::Point wp) {
    ui::Menu::open(ctx().window, wp,
                   {{tr(L"Aç"), "arrow-up-right", L"", [id] { ctx().router->navigate({RouteKind::Album, id}); }},
                    {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); }},
                    sync::menuItem(offline),
                    transfer::exportMenuItem(transfer::What::Album, offline.id, offline.name)});
}

void showPlaylistCardMenu(const Route& open, const sync::Target& offline, gfx::Point wp) {
    Route play = open;
    play.id += "#play";
    const auto what = offline.kind == sync::Kind::Liked ? transfer::What::Liked : transfer::What::Playlist;
    const std::string name = offline.kind == sync::Kind::Liked ? toUtf8(tr(L"Beğenilen Şarkılar")) : offline.name;
    ui::Menu::open(ctx().window, wp,
                   {{tr(L"Aç"), "arrow-up-right", L"", [open] { ctx().router->navigate(open); }},
                    {tr(L"Çal"), "play", L"", [play] { ctx().router->navigate(play); }},
                    sync::menuItem(offline),
                    transfer::exportMenuItem(what, offline.id, name)});
}

MediaCard* addAlbumCard(ui::Widget* parent, const Album& a, std::wstring subtitle) {
    if (subtitle.empty()) subtitle = albumSubtitle(a);
    auto* card = parent->add<MediaCard>(toWide(a.name), std::move(subtitle), a.images);
    const std::string id = a.id;
    card->previewTarget = preview::collectionTarget({RouteKind::Album, id}, toWide(a.name), a.images);
    card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
    card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    card->onContext = [id, offline = sync::Target{sync::Kind::Album, a.id, a.name, a.images}](gfx::Point wp) {
        showAlbumCardMenu(id, offline, wp);
    };
    return card;
}

MediaCard* addArtistCard(ui::Widget* parent, const Artist& a) {
    auto* card = parent->add<MediaCard>(toWide(a.name), artistSubtitle(a), a.images, MediaCard::Shape::Circle, Placeholder::Artist);
    const std::string id = a.id;
    card->previewTarget = preview::collectionTarget({RouteKind::Artist, id}, toWide(a.name), a.images);
    card->onOpen = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
    return card;
}

MediaCard* addPlaylistCard(ui::Widget* parent, const Playlist& p) {
    std::wstring sub = p.ownerName.empty() ? std::wstring(tr(L"Çalma listesi")) : toWide(p.ownerName);
    auto* card = parent->add<MediaCard>(toWide(p.name), std::move(sub), p.images, MediaCard::Shape::Square, Placeholder::Playlist);
    const Route open{RouteKind::Playlist, p.id};
    card->previewTarget = preview::collectionTarget(open, toWide(p.name), p.images);
    card->onOpen = [open] { ctx().router->navigate(open); };
    card->onPlay = [open] { ctx().router->navigate({open.kind, open.id + "#play"}); };
    card->onContext = [open, offline = sync::Target{sync::Kind::Playlist, p.id, p.name, p.images}](gfx::Point wp) {
        showPlaylistCardMenu(open, offline, wp);
    };
    return card;
}

ui::Button* addLoadMore(ui::Column* parent, std::function<void()> onClick) {
    auto* foot = parent->add<ui::Box>();
    auto* b = foot->add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Daha fazla yükle"), "chevron-down");
    b->onClick = std::move(onClick);
    foot->hitTestVisible = false;
    foot->onPreferredHeight = [](float) { return 56.f; };
    foot->onLayout = [b](ui::Box& box) {
        const float w = b->naturalWidth();
        b->setRect({std::floor(box.rect().w * 0.5f - w * 0.5f), 8, w, 40});
    };
    return b;
}

void setLoadMore(ui::Button* button, bool more, bool loading) {
    if (!button) return;
    button->setLabel(loading ? tr(L"Yükleniyor…") : tr(L"Daha fazla yükle"));
    button->setEnabled(!loading);
    if (auto* foot = button->parent()) foot->setVisible(more);
}

} // namespace st::app
