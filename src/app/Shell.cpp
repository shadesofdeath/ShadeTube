#include "app/Shell.h"

#include "app/Accounts.h"
#include "app/Blacklist.h"
#include "app/Commands.h"
#include "app/ConnectScreen.h"
#include "app/DownloadSync.h"
#include "app/DroppedFiles.h"
#include "app/InternetRadio.h"
#include "app/LyricsFullscreen.h"
#include "app/NowPlaying.h"
#include "app/PlaylistTransfer.h"
#include "app/RuleListsPage.h"
#include "app/PodcastUi.h"
#include "app/SmartShuffle.h"
#include "app/Social.h"
#include "app/Source.h"
#include "app/Together.h"
#include "app/VideoMode.h"
#include "app/WinShell.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;
namespace metrics = gfx::metrics;

// ===================================================================================================
// TitleBar

TitleBar::TitleBar() {
    isDragRegion = true;
    back_ = add<Button>(ButtonKind::Icon, L"", "chevron-left");
    forward_ = add<Button>(ButtonKind::Icon, L"", "chevron-right");
    back_->setIconSize(16);
    forward_->setIconSize(16);
    back_->setTooltip(commands::keyTip(tr(L"Geri"), "go-back"));
    forward_->setTooltip(commands::keyTip(tr(L"İleri"), "go-forward"));
    back_->onClick = [] { ctx().router->back(); };
    forward_->onClick = [] { ctx().router->forward(); };
    collapse_ = add<Button>(ButtonKind::Ghost, tr(L"Küçült"), "chevron-up");
    collapse_->setVisible(false);
    collapse_->onClick = [] { ctx().toggleNowPlaying(false); };
    friends_ = add<Button>(ButtonKind::Icon, L"", "users");
    friends_->setIconSize(16);
    friends_->setTooltip(commands::keyTip(tr(L"Arkadaş etkinliği"), "friend-activity"));
    friends_->onClick = [] { commands::run("friend-activity"); };
    friends_->setVisible(false);
    // "SPOTIFY · <NAME>": a click opens the account menu (switch / add / sign out).
    account_ = add<accounts::AccountChip>();

    min_ = add<Button>(ButtonKind::Caption, L"", "minimize");
    max_ = add<Button>(ButtonKind::Caption, L"", "maximize");
    close_ = add<Button>(ButtonKind::CaptionClose, L"", "close");
    max_->isMaximizeButton = true;
    min_->onClick = [] { ctx().window->minimize(); };
    max_->onClick = [] { ctx().window->toggleMaximize(); };
    close_->onClick = [] { ctx().window->close(); };
}

void TitleBar::setNowPlayingMode(bool on) {
    nowPlaying_ = on;
    collapse_->setVisible(on);
    back_->setVisible(!on);
    forward_->setVisible(!on);
    account_->setVisible(!on);
    requestLayout();
}

void TitleBar::layout() {
    const Rect r = rect();
    close_->setRect({r.w - 8 - 40, 6, 40, 28});
    max_->setRect({r.w - 8 - 80, 6, 40, 28});
    min_->setRect({r.w - 8 - 120, 6, 40, 28});
    friends_->setRect({r.w - 8 - 120 - 12 - 28, 6, 28, 28});
    back_->setRect({130, 8, 24, 24});
    forward_->setRect({156, 8, 24, 24});
    collapse_->setRect({8, 4, collapse_->naturalWidth(), 32});
    // Where the source tag was painted (x 196), up to the friend activity button.
    auto* chip = static_cast<accounts::AccountChip*>(account_);
    const float maxW = std::max(0.f, r.w - 8 - 120 - 12 - 28 - 16 - 188);
    account_->setRect({188, 6, std::min(chip->naturalWidth(), maxW), 28});
}

void TitleBar::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    max_->setIcon(ctx().window && ctx().window->isMaximized() ? "restore" : "maximize");
    back_->setEnabled(ctx().router && ctx().router->canBack());
    forward_->setEnabled(ctx().router && ctx().router->canForward());
    // Friend activity: with a Spotify session, outside Now Playing; lit while its panel is open.
    if (auto* shell = dynamic_cast<Shell*>(parent())) {
        friends_->setVisible(!nowPlaying_ && socialAvailable());
        friends_->setActive(shell->friendsOpen());
    }
    if (!nowPlaying_) {
        // The mark in the accent (it follows the playing song's cover, cross-fading with the rest of the UI).
        gfx::Icons::drawRecolored(c.dc(), "logo/logo-mark", {r.x + 16, r.y + 11, 18, 18}, c.scale(),
                                  Color::rgb(winshell::kLogoAccent), accent().base);
        c.text(L"ShadeTube", type::body.withSize(13).withWeight(700), {r.x + 42, r.y, 90, r.h}, col.fgPrimary,
               gfx::TextAlign::Leading, gfx::VAlign::Center);
        // The source tag ("SPOTIFY · <NAME>") is the account chip, a child (app/Accounts).
    } else {
        std::wstring label = tr(L"ŞİMDİ ÇALIYOR");
        if (auto* p = ctx().player; p && !p->context().name.empty()) label += L" · " + toUpperTr(p->context().name);
        // After "Küçült" (its width follows the language: "Einklappen", "Свернуть"), up to the caption buttons.
        const float lx = std::max(108.f, collapse_->rect().right() + 16);
        c.text(label, type::monoLabel, {r.x + lx, r.y, std::max(0.f, std::min(500.f, r.w - 8 - 120 - 16 - lx)), r.h},
               col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    }
    if (!ctx().window || !ctx().window->isActive()) c.fillRect(r, col.bgBase.withAlpha(0.0f));
    paintChildren(c);
}

// ===================================================================================================
// Sidebar

// A row of the sidebar's playlist list, painted like ButtonKind::SidebarItem: a playlist (name, mono track count, the
// playing indicator) or a Spotify folder (chevron + folder glyph + name; a click opens / closes it). Rows inside
// folders are indented by their depth. While songs are dragged over a row that takes them it shows the drop highlight.
// In the icon rail (compact) the row is a cover tile (Liked Songs: the accent tile with the heart; a folder: the folder
// placeholder with a chevron) named by its tooltip.
class SidebarRow : public ui::Widget {
public:
    static constexpr float kTileH = 48, kTile = 40;   // compact row height / cover size

    SidebarRow(std::wstring label, int depth, bool folder)
        : label_(std::move(label), type::secondary), trailing_({}, type::monoLabel), depth_(depth), folder_(folder) {
        focusable = true;
    }

    std::function<void()> onClick;
    std::function<void(const ui::MouseEvent&)> onContext;
    std::function<void(bool open)> onOpen;   // folders: Right opens, Left closes

    bool folder() const { return folder_; }
    bool expanded() const { return expanded_; }
    void setExpanded(bool on) { expanded_ = on; }
    // Cover of the tile (compact): the collection's images, or the Liked Songs tile.
    void setArt(std::vector<catalog::Image> images, bool liked) {
        images_ = std::move(images);
        liked_ = liked;
    }
    void setCompact(bool on) {
        if (compact_ == on) return;
        compact_ = on;
        setRect({rect().x, rect().y, rect().w, on ? kTileH : metrics::playlistItemH});
        invalidate();
    }
    // The rail names the tile; a row names itself only when its name is cut.
    std::wstring tooltip() const override {
        if (compact_) return label_.text();
        auto& label = const_cast<gfx::Text&>(label_);
        return labelW_ > 0 && label.isTrimmed(labelW_) ? label_.text() : std::wstring();
    }
    void setTrailing(std::wstring t) { trailing_.setText(std::move(t)); }
    void setIcon(std::string icon) {
        if (icon_ == icon) return;
        icon_ = std::move(icon);
        invalidate();
    }
    void setActive(bool on) {
        if (active_ == on) return;
        active_ = on;
        activeA_.to(on ? 1.f : 0.f, ui::motion::microMs(ui::motion::normal));
        invalidate();
    }
    void setDropHot(bool on) {
        if (dropHot_ == on) return;
        dropHot_ = on;
        invalidate();
    }

    void paint(Canvas& c) override {
        if (compact_) {
            paintTile(c);
            return;
        }
        const auto& col = colors();
        const auto& acc = accent();
        const Rect r = pill();
        const float h = hover_, a = activeA_;
        if (dropHot_) {
            c.fillPill(r, acc.tint12);
            c.strokePill(r, acc.base);
        } else {
            c.fillPill(r, col.overlaySelected.mulAlpha(a));
            c.fillPill(r, col.overlayHover.mulAlpha(h * (1 - a)));
        }
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, h), col.fgPrimary, a);
        float right = r.right() - 12;
        if (!trailing_.empty()) {
            const float tw = std::ceil(trailing_.measure().w);
            c.text(trailing_, {right - tw, r.y, tw + 1, r.h}, col.fgTertiary, gfx::VAlign::Center);
            right -= tw + 10;
        }
        float x = r.x + 12;
        if (folder_) {
            c.icon(expanded_ ? "chevron-down" : "chevron-right", {x - 2, r.cy() - 7, 14, 14}, col.fgTertiary);
            x += 16;
            c.icon("folder", {x, r.cy() - 7, 14, 14}, fg);
            x += 20;
        } else if (!icon_.empty()) {   // the playing indicator (live bars, ui/Micro)
            if (icon_ == "equalizer") drawPlayingBars(c, this, {x, r.cy() - 7, 14, 14}, acc.base);
            else c.icon(icon_, {x, r.cy() - 7, 14, 14}, col.fgTertiary);   // a mark (a rule list's filter)
            x += 20;
        }
        label_.setStyle(type::secondary.withWeight(active_ ? 600.f : 400.f));
        labelW_ = right - x;
        c.text(label_, {x, r.y, right - x, r.h}, fg, gfx::VAlign::Center);
    }

    void paintTile(Canvas& c) {
        const auto& col = colors();
        const auto& acc = accent();
        const Rect r = rect();
        const Rect bg{r.cx() - kTileH * 0.5f, r.y, kTileH, kTileH};
        const Rect tile = bg.center(kTile, kTile);
        const float h = hover_, a = activeA_;
        if (dropHot_) {
            c.fillRounded(bg, metrics::radiusSurface, acc.tint12);
            c.strokeRounded(bg, metrics::radiusSurface, acc.base);
        } else {
            c.fillRounded(bg, metrics::radiusSurface, col.overlaySelected.mulAlpha(a));
            c.fillRounded(bg, metrics::radiusSurface, col.overlayHover.mulAlpha(h * (1 - a)));
        }
        if (pagetx::presence(this) <= 0.f) {
            // the page transition draws this cover (it flies into the page the tile opened)
        } else if (liked_ && images_.empty()) {
            c.fillRounded(tile, metrics::radiusSurface, acc.base);
            c.icon("heart-filled", tile.center(16, 16), acc.onAccent);
        } else {
            drawArtwork(c, images_, tile, metrics::radiusSurface, folder_ ? Placeholder::Folder : Placeholder::Playlist,
                        false, Priority::Normal);
        }
        // A corner badge: the folder's open / closed chevron, or the equalizer of the playing collection.
        const gfx::Point corner{tile.right() - 3, tile.bottom() - 3};
        if (folder_) {
            c.fillCircle(corner, 7, col.bgElevated);
            c.icon(expanded_ ? "chevron-down" : "chevron-right", {corner.x - 5, corner.y - 5, 10, 10}, col.fgSecondary);
        } else if (!icon_.empty()) {
            c.fillCircle(corner, 8, acc.base);
            if (icon_ == "equalizer") drawPlayingBars(c, this, {corner.x - 5, corner.y - 5, 10, 10}, acc.onAccent);
            else c.icon(icon_, {corner.x - 5, corner.y - 5, 10, 10}, acc.onAccent);
        }
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            if (onContext) {
                const auto context = onContext;
                context(e);
            }
            return false;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (hovered() && rect().contains(e.pos) && onClick) {
            armCover();
            const auto click = onClick;   // may rebuild the list: the row is deleted after the event
            click();
        }
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

    // Keyboard: Enter / Space = click; a folder opens with Right and closes with Left; the menu key / Shift+F10 opens
    // the context menu under the row.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        armCover();
        const auto click = onClick;
        click();
        return true;
    }
    // A tile of the rail flies its cover into the page it opens (app/PageTransition).
    void armCover() const {
        if (!compact_ || folder_) return;
        const Rect r = rect();
        const Rect tile = Rect{r.cx() - kTileH * 0.5f, r.y, kTileH, kTileH}.center(kTile, kTile);
        pagetx::arm(liked_ && images_.empty() ? pagetx::likedCover(*this, tile, metrics::radiusSurface)
                                              : pagetx::imageCover(*this, tile, metrics::radiusSurface, images_));
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && onContext) {
            ui::MouseEvent me;
            const Rect wr = toWindow(rect());
            me.windowPos = {wr.x + 8, wr.bottom()};
            me.pos = {rect().x + 8, rect().bottom()};
            me.button = ui::MouseButton::Right;
            const auto context = onContext;
            context(me);
            return true;
        }
        if (folder_ && onOpen && !e.ctrl && !e.alt && (e.vk == VK_RIGHT || e.vk == VK_LEFT)) {
            const bool open = e.vk == VK_RIGHT;
            if (open == expanded_) return false;
            const auto toggle = onOpen;
            toggle(open);
            return true;
        }
        return false;
    }
    Rect focusRect() const override { return compact_ ? Rect{rect().cx() - kTileH * 0.5f, rect().y, kTileH, kTileH} : pill(); }
    ui::FocusShape focusShape() const override { return compact_ ? ui::FocusShape::Rect : ui::FocusShape::Pill; }

private:
    // The row minus its indent (16 DIPs per folder level).
    Rect pill() const {
        const Rect r = rect();
        const float indent = std::min(16.f * static_cast<float>(depth_), r.w * 0.5f);
        return {r.x + indent, r.y, r.w - indent, r.h};
    }

    gfx::Text label_, trailing_;
    std::string icon_;
    std::vector<catalog::Image> images_;
    int depth_;
    bool folder_;
    bool liked_ = false;
    bool compact_ = false;
    bool expanded_ = false;
    bool active_ = false;
    bool dropHot_ = false;
    float labelW_ = 0;   // width the name had at the last paint (tooltip when it was cut)
    ui::Anim hover_, activeA_;
};

// The page entries of the sidebar (the content of its nav scroller): the "Daha fazla" group's pages unroll / roll up
// with the group's animation (each slot clipped to its share and faded), and the count pills are painted over them.
class NavBox : public ui::Widget {
public:
    explicit NavBox(Sidebar& sidebar) : sb_(sidebar) { hitTestVisible = false; }
    float contentH = 0;   // set by Sidebar::layout()
    float rowStep = 0;    // row height + gap of the metrics in use
    float preferredHeight(float) override { return contentH; }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const float open = sb_.moreAnim_.value();
        const auto group = sb_.groupButtons();
        const Button* pinned = sb_.pinnedButton();
        c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
        const Rect visible = c.clip();
        for (const auto& child : children()) {
            if (!child->visible() || !child->rect().intersects(visible)) continue;
            auto* b = dynamic_cast<Button*>(child.get());
            const bool folding = b && b != pinned && open < 0.999f && std::find(group.begin(), group.end(), b) != group.end();
            if (!folding) {
                child->paint(c);
                if (b) paintBadge(c, b);
                continue;
            }
            if (open <= 0.001f) continue;
            const Rect cr = child->rect();
            c.pushClip({cr.x - 4, cr.y, cr.w + 8, std::max(0.f, rowStep * open)});
            c.pushOpacity(open);
            child->paint(c);
            paintBadge(c, b);
            c.popLayer();
            c.popClip();
        }
        c.popTransform();
    }

private:
    // A count pill on a nav entry: new episodes of the podcast subscriptions, unseen new releases. In the rail: a dot on
    // the icon's corner (the tooltip names the page).
    void paintBadge(Canvas& c, const Button* b) {
        int n = 0;
        if (b == sb_.podcasts_) n = podcastNewEpisodeCount();
        else if (b == sb_.releases_) n = newReleasesUnseenCount();
        if (n <= 0) return;
        const auto& col = colors();
        const Rect br = b->rect();
        if (sb_.labelOpacity() < 0.5f) {
            const gfx::Point p{br.cx() + metrics::navIcon * 0.5f - 1, br.cy() - metrics::navIcon * 0.5f + 1};
            // Ringed in what is under it (the selected pill of the current page) so it stands off the glyph.
            const Color under = b->active() ? Color::lerp(col.bgBase, col.overlaySelected.withAlpha(1.f), col.overlaySelected.a)
                                            : col.bgBase;
            c.fillCircle(p, 5.5f, under);
            c.fillCircle(p, 3.5f, accent().base);
            return;
        }
        const std::wstring label = n > 99 ? std::wstring(L"99+") : std::to_wstring(n);
        auto layout = gfx::makeLayout(label, type::monoBadge, 100);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float w = std::max(18.f, std::ceil(m.widthIncludingTrailingWhitespace) + 10);
        const Rect pill{br.right() - 10 - w, br.cy() - 9, w, 18};
        c.fillPill(pill, accent().base);
        c.text(label, type::monoBadge, pill, accent().onAccent, gfx::TextAlign::Center, gfx::VAlign::Center);
    }

    Sidebar& sb_;
};

// "DAHA FAZLA ⌄": the header of the folding page group, a section label like "ÇALMA LİSTELERİ" with a chevron that turns
// as the group opens. Click / Enter / Space toggle it, Right opens and Left folds it. Folded, an accent dot after it
// says a hidden page has news (podcast episodes, new releases). In the rail only the chevron is left (centered; the
// tooltip names it).
class NavGroup : public ui::Widget {
public:
    explicit NavGroup(Sidebar& sidebar) : sb_(sidebar), label_(toUpperTr(tr(L"Daha fazla")), type::monoLabel) {
        focusable = true;
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float h = hover_, show = sb_.labelOpacity(), open = sb_.moreAnim_.value();
        if (show < 0.5f) c.fillPill(r.center(std::min(r.w, 48.f), r.h), col.overlayHover.mulAlpha(h));   // the rail's hit pill
        const Color fg = Color::lerp(col.fgTertiary, col.fgPrimary, h * 0.8f);
        const float lw = std::ceil(label_.measure().w);
        const float tx = r.x + 12;
        if (show > 0.01f) c.text(label_, {tx, r.y, lw + 1, r.h}, fg.mulAlpha(show), gfx::VAlign::Center);
        constexpr float cs = 12;
        const float cx = std::round(ui::lerp(r.cx() - cs * 0.5f, tx + lw + 6, show));
        const D2D1_POINT_2F pivot{cx + cs * 0.5f, r.cy()};
        c.pushTransform(D2D1::Matrix3x2F::Rotation(180.f * open, pivot));
        c.icon("chevron-down", {cx, r.cy() - cs * 0.5f, cs, cs}, fg);
        c.popTransform();
        // News on a folded page: the dot fades in as the group folds.
        const float dot = (1 - open) * (hiddenNews() ? 1.f : 0.f);
        if (dot > 0.01f) c.fillCircle({cx + cs + 7, r.cy()}, 3, accent().base.mulAlpha(dot));
    }

    std::wstring tooltip() const override {
        if (sb_.labelOpacity() >= 0.5f) return {};
        return sb_.moreOpen() ? std::wstring(tr(L"Daha az göster")) : std::wstring(tr(L"Daha fazla göster"));
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (hovered() && rect().contains(e.pos)) sb_.setMoreOpen(!sb_.moreOpen());
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        sb_.setMoreOpen(!sb_.moreOpen());
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt || (e.vk != VK_RIGHT && e.vk != VK_LEFT)) return false;
        const bool open = e.vk == VK_RIGHT;
        if (open == sb_.moreOpen()) return false;
        sb_.setMoreOpen(open);
        return true;
    }
    Rect focusRect() const override {
        const Rect r = rect();
        if (sb_.labelOpacity() < 0.5f) return r.center(std::min(r.w, 48.f), r.h);
        return {r.x, r.y + 4, std::min(r.w, std::ceil(label_.measure().w) + 12 + 6 + 12 + 14), r.h - 8};
    }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }

private:
    bool hiddenNews() const {
        const Button* pinned = sb_.pinnedButton();
        if (sb_.podcasts_ != pinned && podcastNewEpisodeCount() > 0) return true;
        return sb_.releases_ != pinned && newReleasesUnseenCount() > 0;   // 0 without a Spotify session
    }

    Sidebar& sb_;
    mutable gfx::Text label_;   // measured by focusRect() too
    ui::Anim hover_;
};

Sidebar::Sidebar() {
    // The page entries live in their own scroller (it only scrolls when even the tight nav would squeeze the playlists).
    nav_ = add<ui::ScrollView>();
    nav_->notchDip = 72;
    navBox_ = nav_->setContent<NavBox>(*this);
    // Page names ("Ara" = the Search page); in the icon rail they are the tooltips.
    auto nav = [this](std::wstring name, const char* icon) {
        auto* b = navBox_->add<Button>(ButtonKind::Nav, name, icon);
        navNames_.emplace_back(b, std::move(name));
        return b;
    };
    home_ = nav(tr(L"Ana Sayfa"), "home");
    search_ = nav(tr(L"Ara"), "search");
    library_ = nav(tr(L"Kitaplık"), "library");
    group_ = navBox_->add<NavGroup>(*this);
    downloads_ = nav(tr(L"İndirilenler"), "download");
    local_ = nav(tr(L"Yerel dosyalar"), "music-note");
    radio_ = nav(tr(L"Radyo"), "radio");
    podcasts_ = nav(tr(L"Podcastler"), "podcast");
    releases_ = nav(tr(L"Yeni çıkanlar"), "sparkle");
    releases_->setVisible(false);
    stats_ = nav(tr(L"İstatistikler"), "stats");
    settings_ = add<Button>(ButtonKind::Nav, tr(L"Ayarlar"), "settings");   // pinned to the bottom, outside the scroller
    navNames_.emplace_back(settings_, tr(L"Ayarlar"));
    // "Daha fazla": the saved choice, else open until the first layout decides by the height.
    moreOpen_ = Settings::get().sidebarMore != 0;
    moreAnim_.snap(moreOpen_ ? 1.f : 0.f);
    // The command palette / a key of the user's choosing toggles the group too (the sidebar lives as long as the app).
    commands::setHandler("sidebar-more", [this] {
        setMoreOpen(!moreOpen_);
        return true;
    });
    home_->onClick = [] { ctx().router->navigate({RouteKind::Home}); };
    search_->onClick = [] { ctx().router->navigate({RouteKind::Search}); };
    library_->onClick = [] { ctx().router->navigate({RouteKind::Library}); };
    downloads_->onClick = [] { ctx().router->navigate({RouteKind::Downloads}); };
    local_->onClick = [] {   // (the page names its view in the route: on it, a click scrolls it to the top)
        auto* r = ctx().router;
        r->navigate(r->current().kind == RouteKind::LocalFiles ? r->current() : Route{RouteKind::LocalFiles});
    };
    radio_->onClick = [] { ctx().router->navigate({RouteKind::Radio}); };
    podcasts_->onClick = [] { ctx().router->navigate({RouteKind::Podcasts}); };
    releases_->onClick = [] { ctx().router->navigate({RouteKind::NewReleases}); };
    stats_->onClick = [] { ctx().router->navigate({RouteKind::Stats}); };
    settings_->onClick = [] { ctx().router->navigate({RouteKind::Settings}); };
    newPlaylist_ = add<Button>(ButtonKind::Icon, L"", "plus");
    newPlaylist_->setIconSize(14);
    newPlaylist_->setTooltip(tr(L"Yeni çalma listesi"));
    newPlaylist_->onClick = [this] {
        const Rect br = newPlaylist_->toWindow(newPlaylist_->rect());
        // Logged in this creates the playlist on Spotify (then opens it); logged out, a local playlist.
        std::vector<ui::MenuItem> items = {
            {tr(L"Yeni çalma listesi"), "plus", L"",
             [] { promptNewPlaylist([](const std::string& id) { ctx().router->navigate({RouteKind::Playlist, id}); }); }},
            rulelists::createMenuItems()[0],   // "Kurallı liste"
            ui::MenuItem::sep()};
        for (auto& m : transfer::importMenuItems()) items.push_back(std::move(m));   // file, YouTube, Deezer, Apple Music, TIDAL
        items.push_back(rulelists::createMenuItems()[1]);   // a rule list's JSON
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    };
    list_ = add<ui::ScrollView>();
    listCol_ = list_->setContent<ui::Column>(0.f);
}

void Sidebar::refresh() {
    // (Yeni çıkanlar is Spotify only: logging in / out shows / hides it at the next layout, see groupButtons().)
    listCol_->clearChildren();
    items_.clear();
    dropRow_ = restingFolder_ = nullptr;
    // Plain track count with the UI language's digit grouping ("18", "1.234").
    auto countStr = [](int n) { return i18n::number(std::max(0, n)); };
    // count < 0: unknown (Spotify gave none) -> no trailing count rather than a misleading "0".
    auto addItem = [&](std::wstring name, int count, Route route, int depth = 0) {
        auto* b = listCol_->add<SidebarRow>(std::move(name), depth, false);
        b->setTrailing(count < 0 ? std::wstring() : countStr(count));
        b->setRect({0, 0, 0, metrics::playlistItemH});
        b->onClick = [route] { ctx().router->navigate(route); };
        items_.emplace_back(b, route);
        return b;
    };
    if (source::loggedIn()) {
        // Logged in: the user's Spotify playlists (Liked Songs is the pseudo-playlist first), in their folders.
        const auto& lib = ctx().session->library();
        const auto& open = Settings::get().expandedFolders;
        const auto expanded = [&open](const std::string& id) { return std::find(open.begin(), open.end(), id) != open.end(); };
        for (const auto& row : spotify::treeRows(lib.playlists, lib.tree, expanded)) {
            if (row.folder) {
                const auto& f = lib.tree.folders[row.index];
                const std::wstring name = f.name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(f.name);
                auto* b = listCol_->add<SidebarRow>(name, row.depth, true);
                b->setRect({0, 0, 0, metrics::playlistItemH});
                const bool isOpen = expanded(f.id);
                b->setExpanded(isOpen);
                const std::string id = f.id;
                b->onClick = [this, id, isOpen] { setFolderOpen(id, !isOpen); };
                b->onOpen = [this, id](bool on) { setFolderOpen(id, on); };
                const Route inLibrary{RouteKind::Library, "folder:" + id};
                b->onContext = [this, id, isOpen, inLibrary](const ui::MouseEvent& e) {
                    ui::Menu::open(ctx().window, e.windowPos,
                                   {{isOpen ? tr(L"Klasörü kapat") : tr(L"Klasörü aç"), isOpen ? "chevron-up" : "chevron-down",
                                     L"", [this, id, isOpen] { setFolderOpen(id, !isOpen); }},
                                    {tr(L"Kitaplıkta göster"), "library", L"", [inLibrary] { ctx().router->navigate(inLibrary); }}});
                };
                items_.emplace_back(b, inLibrary);
                continue;
            }
            const auto& p = lib.playlists[row.index];
            const bool liked = p.id == spotify::Api::kLikedSongsUri;
            const Route route = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
            auto* b = addItem(toWide(p.name), p.countKnown ? p.totalTracks : -1, route, row.depth);
            b->setArt(p.images, liked);
            const std::string id = p.id;
            const std::wstring name = toWide(p.name);
            const bool editable = !liked && p.editable, owned = p.owned;
            const sync::Target offline{liked ? sync::Kind::Liked : sync::Kind::Playlist, id, p.name, p.images};
            b->onContext = [id, name, liked, editable, owned, offline](const ui::MouseEvent& e) {
                const Route r = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, id};
                Route rp = r;
                rp.id += "#play";
                std::vector<ui::MenuItem> items{
                    {tr(L"Aç"), "arrow-up-right", L"", [r] { ctx().router->navigate(r); }},
                    {tr(L"Çal"), "play", L"", [rp] { ctx().router->navigate(rp); }},
                    sync::menuItem(offline),
                    transfer::exportMenuItem(liked ? transfer::What::Liked : transfer::What::Playlist, id, toUtf8(name))};
                // Editable Spotify playlists (owned or collaborative): details (owner only) / delete (unfollow).
                if (editable) {
                    items.push_back(ui::MenuItem::sep());
                    if (owned)
                        items.push_back({tr(L"Ayrıntıları düzenle"), "edit", L"", [id] {
                                             const auto* s = ctx().session;
                                             if (const auto* pl = s ? s->findPlaylist(id) : nullptr)
                                                 editPlaylistDetails(id, toWide(pl->name), toWide(pl->description), pl->images);
                                         }});
                    ui::MenuItem del{owned ? tr(L"Sil") : tr(L"Kitaplıktan kaldır"), "trash", L"",
                                     [id, name] { confirmDeletePlaylist(id, name); }};
                    del.destructive = true;
                    items.push_back(std::move(del));
                }
                ui::Menu::open(ctx().window, e.windowPos, std::move(items));
            };
        }
    } else {
        auto& lib = ctx().library;
        auto* liked = addItem(tr(L"Beğenilen Şarkılar"), static_cast<int>(lib.liked().size()), {RouteKind::Liked});
        liked->setArt({}, true);
        liked->onContext = [](const ui::MouseEvent& e) {
            ui::Menu::open(ctx().window, e.windowPos,
                           {{tr(L"Aç"), "arrow-up-right", L"", [] { ctx().router->navigate({RouteKind::Liked}); }},
                            {tr(L"Çal"), "play", L"", [] { ctx().router->navigate({RouteKind::Liked, "#play"}); }},
                            sync::menuItem(sync::likedTarget()),
                            transfer::exportMenuItem(transfer::What::Liked, sync::kLocalLikedId,
                                                     toUtf8(tr(L"Beğenilen Şarkılar")))});
        };
        for (const auto& p : lib.playlists()) {
            auto* b = addItem(toWide(p.name), p.totalTracks, {RouteKind::Playlist, p.id});
            b->setArt(p.images, false);
            const std::string id = p.id;
            const std::wstring name = toWide(p.name);
            const sync::Target offline{sync::Kind::Playlist, id, p.name, p.images};
            b->onContext = [id, name, offline](const ui::MouseEvent& e) {
                // The Spotify playlists' menu: a destructive "Sil" through the shared confirmation (it also leaves the
                // playlist's page when that is open).
                ui::MenuItem del{tr(L"Sil"), "trash", L"", [id, name] { confirmDeletePlaylist(id, name); }};
                del.destructive = true;
                ui::Menu::open(ctx().window, e.windowPos,
                               {{tr(L"Aç"), "arrow-up-right", L"", [id] { ctx().router->navigate({RouteKind::Playlist, id}); }},
                                {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::Playlist, id + "#play"}); }},
                                sync::menuItem(offline),
                                transfer::exportMenuItem(transfer::What::Playlist, id, offline.name),
                                ui::MenuItem::sep(),
                                {tr(L"Ayrıntıları düzenle"), "edit", L"", [id] {
                                     if (const auto* pl = ctx().library.playlist(id))
                                         editPlaylistDetails(id, toWide(pl->name), toWide(pl->description), pl->images);
                                 }},
                                std::move(del)});
            };
        }
    }
    for (const auto& l : rulelists::lists()) {   // Kurallı listeler (app/RuleListsPage)
        auto* b = addItem(toWide(l.def.name), l.evaluated ? static_cast<int>(l.count) : -1, {RouteKind::SmartList, "rule:" + l.id});
        b->setArt(l.covers.empty() ? std::vector<catalog::Image>{} : std::vector<catalog::Image>{l.covers.front()}, false);
        b->setTooltip(rulelists::subtitle(l));
        b->onContext = [id = l.id](const ui::MouseEvent& e) { rulelists::showMenu(id, e.windowPos); };
    }
    for (auto& [b, _] : items_) {
        b->setCompact(compact_);   // the rail's tiles
        b->requestLayout();
    }
    // Rows are fixed height: make Column use it.
    syncActive();
    requestLayout();
}

void Sidebar::setCollapsed(bool on) {
    if (on == collapsed_) return;
    collapsed_ = on;
    for (auto& [b, name] : navNames_) b->setTooltip(on ? name : std::wstring());
    requestLayout();
    invalidate();
}

void Sidebar::setRailProgress(float progress) {
    progress = std::clamp(progress, 0.f, 1.f);
    if (progress == rail_) return;
    rail_ = progress;
    requestLayout();
}

std::vector<Button*> Sidebar::navButtons() const {
    return {home_, search_, library_, downloads_, local_, radio_, podcasts_, releases_, stats_, settings_};
}

std::vector<Button*> Sidebar::groupButtons() const {
    std::vector<Button*> out{downloads_, local_, radio_, podcasts_};
    if (socialAvailable()) out.push_back(releases_);   // Spotify only
    out.push_back(stats_);
    return out;
}

Button* Sidebar::pinnedButton() const {
    if (moreOpen_ && !moreAnim_.running()) return nullptr;   // nothing to keep: the whole group shows
    for (Button* b : groupButtons())
        if (b->active()) return b;
    return nullptr;
}

void Sidebar::setMoreOpen(bool open) {
    auto& s = Settings::get();
    if (s.sidebarMore != (open ? 1 : 0)) {
        s.sidebarMore = open ? 1 : 0;   // from now on the user's choice, no longer by the height
        s.markDirty();
    }
    if (open == moreOpen_) return;
    // Folding with the keyboard focus on a page that goes away: the focus stays on the header.
    if (!open)
        if (auto* w = window())
            for (Button* b : groupButtons())
                if (b->focused() && !b->active()) {
                    if (w->focusVisible()) w->focusByKeyboard(group_);
                    else group_->focus();
                }
    moreOpen_ = open;
    for (Button* b : groupButtons()) b->setVisible(true);   // layoutNav() hides the folded ones when it settles
    moreAnim_.to(open ? 1.f : 0.f, ui::motion::medium, ui::Ease::Standard);
    requestLayout();
    invalidate();
}

float Sidebar::navContentHeight(float rowH, float gap, float groupH, float open) const {
    const float step = rowH + gap;
    float h = 3 * step + groupH;
    // The current page keeps its row while the others unroll / roll up.
    const Button* pinned = pinnedButton();
    for (Button* b : groupButtons()) h += (b == pinned ? 1.f : open) * step;
    return h + 2;   // room for the last entry's focus ring
}

void Sidebar::layoutNav(float rowH, float gap, float groupH) {
    const float t = rail_;
    const float x = std::round(ui::lerp(16.f, 12.f, t)), w = std::max(0.f, rect().w - 2 * x);
    const float open = moreAnim_.value();
    const float step = rowH + gap;
    float y = 0;
    for (Button* b : {home_, search_, library_}) {
        b->setRect({x, y, w, rowH});
        y += step;
    }
    group_->setRect({x, y, w, groupH});
    y += groupH;
    const auto group = groupButtons();
    if (std::find(group.begin(), group.end(), releases_) == group.end()) releases_->setVisible(false);
    const bool settledFolded = !moreOpen_ && !moreAnim_.running();
    const Button* pinned = pinnedButton();
    for (Button* b : group) {
        // Folded (and settled): only the current page stays; the others leave the Tab order.
        const bool shown = !settledFolded || b == pinned;
        b->setVisible(shown);
        if (!shown) continue;
        b->setRect({x, y, w, rowH});
        y += (b == pinned ? 1.f : open) * step;
    }
}

float Sidebar::labelOpacity() const {
    // As ButtonKind::Nav fades its name: by the room the name has next to the icon.
    const float labelW = home_->rect().w - 36 - metrics::navIcon;
    return std::clamp((labelW - 16) / 64.f, 0.f, 1.f);
}

void Sidebar::setFolderOpen(const std::string& folderId, bool open) {
    auto& s = Settings::get();
    auto& list = s.expandedFolders;
    const auto it = std::find(list.begin(), list.end(), folderId);
    if ((it != list.end()) == open) return;
    if (open) list.push_back(folderId);
    else list.erase(it);
    s.markDirty();
    // Rebuilt after the event: the row that asked is one of the rows refresh() replaces.
    Dispatcher::post([this, ref = life_.ref()] {
        if (!ref.expired()) refresh();
    });
}

void Sidebar::syncActive() {
    if (!ctx().router) return;
    Route cur = ctx().router->current();
    if (auto hash = cur.id.find('#'); hash != std::string::npos) cur.id.resize(hash);
    home_->setActive(cur.kind == RouteKind::Home);
    search_->setActive(cur.kind == RouteKind::Search);
    library_->setActive(cur.kind == RouteKind::Library);
    downloads_->setActive(cur.kind == RouteKind::Downloads);
    local_->setActive(cur.kind == RouteKind::LocalFiles);
    radio_->setActive(cur.kind == RouteKind::Radio);
    podcasts_->setActive(cur.kind == RouteKind::Podcasts);
    releases_->setActive(cur.kind == RouteKind::NewReleases);
    stats_->setActive(cur.kind == RouteKind::Stats);
    settings_->setActive(cur.kind == RouteKind::Settings);
    requestLayout();   // a folded group keeps the current page's entry
    // A nav that scrolls (short window) shows the current page's entry.
    if (nav_->maxScroll() > 0)
        for (Button* b : groupButtons())
            if (b->active() && b->visible()) b->scrollIntoView();
    for (auto& [b, route] : items_) b->setActive(route == cur);
    // Show an equalizer next to the playlist that is currently playing.
    const auto* p = ctx().player;
    for (auto& [b, route] : items_) {
        const bool rule = route.kind == RouteKind::SmartList;   // a rule list: its mark, or the bars while it plays
        const bool playing = p && p->isPlaying() &&
                             ((route.kind == RouteKind::Playlist && p->context().uri == "playlist:" + route.id) ||
                              (route.kind == RouteKind::Liked && p->context().uri == "liked") ||
                              (rule && p->context().uri == "smart:" + route.id));
        b->setIcon(playing ? "equalizer" : rule ? "filter" : "");
    }
}

SidebarRow* Sidebar::rowAt(gfx::Point windowPos) const {
    if (!list_->toWindow(list_->rect()).contains(windowPos)) return nullptr;
    for (const auto& [row, _] : items_)
        if (row->toWindow(row->rect()).contains(windowPos)) return row;
    return nullptr;
}

bool Sidebar::dragOver(const DragPayload& payload, gfx::Point windowPos) {
    const double now = ui::frame::realNow();
    // Near the list's top / bottom edge: scroll it, faster the closer the pointer is to the edge.
    const Rect lr = list_->toWindow(list_->rect());
    const float edge = 28;
    if (lr.contains(windowPos) && lastScrollTick_ > 0) {
        const float dt = static_cast<float>(std::min(now - lastScrollTick_, 100.0)) / 1000.f;
        float v = 0;
        if (windowPos.y < lr.y + edge) v = -(1 - (windowPos.y - lr.y) / edge);
        else if (windowPos.y > lr.bottom() - edge) v = 1 - (lr.bottom() - windowPos.y) / edge;
        if (v != 0) list_->scrollTo(std::clamp(list_->scrollY() + v * 900.f * dt, 0.f, list_->maxScroll()), false);
    }
    lastScrollTick_ = now;

    SidebarRow* row = payload.fromExplorer() ? nullptr : rowAt(windowPos);
    // A closed folder opens when the songs rest on it for a moment (their playlist may be inside).
    if (row && row->folder() && !row->expanded()) {
        if (row != restingFolder_) {
            restingFolder_ = row;
            restingSince_ = now;
        } else if (now - restingSince_ > 700) {
            restingFolder_ = nullptr;
            for (const auto& [b, route] : items_)
                if (b == row) setFolderOpen(route.id.substr(std::string("folder:").size()), true);
        }
    } else {
        restingFolder_ = nullptr;
    }
    SidebarRow* hot = nullptr;
    if (row && !row->folder())
        for (const auto& [b, route] : items_)
            if (b == row && !dragdrop::tracksForRow(route, payload.tracks).empty()) hot = row;
    if (hot != dropRow_) {
        if (dropRow_) dropRow_->setDropHot(false);
        dropRow_ = hot;
        if (dropRow_) dropRow_->setDropHot(true);
    }
    return hot != nullptr;
}

void Sidebar::dragLeave() {
    if (dropRow_) dropRow_->setDropHot(false);
    dropRow_ = restingFolder_ = nullptr;
    lastScrollTick_ = 0;
}

void Sidebar::drop(const DragPayload& payload, gfx::Point windowPos) {
    SidebarRow* row = rowAt(windowPos);
    dragLeave();
    if (!row || payload.fromExplorer()) return;
    for (const auto& [b, route] : items_)
        if (b == row) {
            const Route target = route;   // dropOnRow may refresh the list (new counts): copy it first
            dragdrop::dropOnRow(target, payload.tracks);
            return;
        }
}

void Sidebar::layout() {
    const Rect r = rect();
    const float t = rail_;
    // Half way into the rail the playlist rows turn into (taller) cover tiles: the list re-measures.
    if (const bool compact = t >= 0.5f; compact != compact_) {
        compact_ = compact;
        for (auto& [b, _] : items_) b->setCompact(compact);
        listCol_->requestLayout();
    }
    // Expanded: 16 DIP margins; the rail (72 DIPs): 48 DIP wide icon pills. The list ends 8 DIPs before the edge (its
    // scrollbar); in the rail its tiles center on the rail.
    const float x = std::round(ui::lerp(16.f, 12.f, t)), w = std::max(0.f, r.w - 2 * x);
    const float lx = std::round(ui::lerp(16.f, 8.f, t)), listW = std::max(0.f, r.w - 8 - lx);

    // Vertical metrics: the spec's (nav rows 36 + 2) and a tight set (32 + 0, smaller margins) for a short sidebar or
    // the compact list density. `label` = the "Çalma listeleri" label block above the list (`labelC`: its center above
    // the list's top), `bottom` = Ayarlar with its margins.
    struct Metrics {
        float top, rowH, gap, groupH, label, labelC, bottom, settingsPad;
    };
    static constexpr Metrics kNormal{24, metrics::navItemH, 2, 36, 50, 28, 16 + metrics::navItemH + 8, 16};
    static constexpr Metrics kTight{12, 32, 0, 28, 34, 19, 10 + 32 + 6, 10};
    // The list needs its rows, but no more than `rows` of them for the budget.
    const float listAll = listCol_->preferredHeight(listW);
    const float rowH = compact_ ? SidebarRow::kTileH : metrics::playlistItemH;
    auto listNeed = [&](int rows) { return std::min(listAll, rows * rowH); };
    auto need = [&](const Metrics& m, float open, int rows) {
        return m.top + navContentHeight(m.rowH, m.gap, m.groupH, open) + m.label + listNeed(rows) + m.bottom;
    };
    const bool compactDensity = Settings::get().compactLists;
    // "Daha fazla" left to the height (never toggled): open while that still leaves kAutoListRows playlist rows. Snapped,
    // not animated (it follows window resizes).
    if (const int pref = Settings::get().sidebarMore; pref < 0) {
        const bool open = need(compactDensity ? kTight : kNormal, 1.f, kAutoListRows) <= r.h;
        if (open != moreOpen_) {
            moreOpen_ = open;
            moreAnim_.snap(open ? 1.f : 0.f);
        }
    }
    const float open = moreAnim_.value();
    tight_ = compactDensity || need(kNormal, open, kMinListRows) > r.h;
    const Metrics& m = tight_ ? kTight : kNormal;
    // The nav takes what it needs; when that would leave the list fewer than kMinListRows rows it scrolls instead
    // (keeping at least two of its rows in view).
    layoutNav(m.rowH, m.gap, m.groupH);
    const float navH = navContentHeight(m.rowH, m.gap, m.groupH, open);
    const float navRoom = r.h - m.top - m.label - listNeed(kMinListRows) - m.bottom;
    // (A few DIPs short: the list gives them up rather than the nav scrolling by a hair.)
    const float navViewH = std::round(navH <= navRoom + 6 ? navH : std::max(navRoom, 2 * m.rowH + m.gap));
    navBox_->contentH = navH;
    navBox_->rowStep = m.rowH + m.gap;
    nav_->setRect({0, m.top, r.w, navViewH});
    nav_->layout();   // the content height may change at the same view size (re-measures; no new layout pass)

    const float listTop = m.top + navViewH + m.label;
    listLabelY_ = listTop - m.labelC - 7;
    const float bottom = r.h - m.bottom;
    list_->setRect({lx, listTop, listW, std::max(0.f, bottom - listTop)});
    // "+": at the end of the section label; centered in the rail.
    const float plusX = std::round(ui::lerp(r.w - 16 - 28, r.w * 0.5f - 12, t));
    newPlaylist_->setRect({plusX, listTop - m.labelC - 12, 24, 24});
    settings_->setRect({x, r.h - m.settingsPad - m.rowH, w, m.rowH});
}

void Sidebar::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    // The group folding / unfolding moves everything below it: lay out every frame, and once more when it settles
    // (the folded entries leave the Tab order then).
    if (moreAnim_.running()) {
        requestLayout();
        moreSettling_ = true;
    } else if (moreSettling_) {
        moreSettling_ = false;
        requestLayout();
    }
    c.vline(r.right() - 1, r.y, r.bottom(), col.hairSubtle);
    const float show = labelOpacity();
    if (show > 0.01f) {
        const float labelW = newPlaylist_->rect().x - 28 - 8;   // up to the "+"
        c.text(toUpperTr(tr(L"Çalma listeleri")), type::monoLabel, {r.x + 28, r.y + listLabelY_, std::max(0.f, labelW), 14},
               col.fgTertiary.mulAlpha(show));
    }
    // A nav that scrolls: a hairline under it while the list's label would otherwise touch scrolled-away entries.
    if (nav_->maxScroll() > 0 && nav_->scrollY() < nav_->maxScroll() - 0.5f) {
        const Rect nr = nav_->rect();
        c.hline(r.x + 16, r.right() - 16, r.y + nr.bottom(), col.hairSubtle);
    }
    paintChildren(c);
}

// ===================================================================================================
// PlayerBar

namespace {
// Player bar geometry (DIP): the transport row and the seek row of the center column, the time labels beside the wave.
constexpr float kBarMidY = 44, kTransportY = 33, kSeekY = 69, kTimeW = 46, kTimeGap = 12;
}

PlayerBar::PlayerBar()
    : title_({}, type::body), artist_({}, type::caption), elapsed_({}, type::monoMeta), total_({}, type::monoMeta),
      oldTitle_({}, type::body), oldArtist_({}, type::caption) {
    gfx::TextOptions right;
    right.align = gfx::TextAlign::Trailing;
    elapsed_.setOptions(right);
    seek_ = add<ui::Slider>(ui::Slider::Look::Wave);
    seek_->hitPadding = 4;
    seek_->onCommit = [](float f) { ctx().player->seek(static_cast<int64_t>(f * ctx().player->durationMs())); };
    shuffle_ = add<Button>(ButtonKind::Icon, L"", "shuffle");
    prev_ = add<Button>(ButtonKind::Icon, L"", "prev");
    play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
    next_ = add<Button>(ButtonKind::Icon, L"", "next");
    repeat_ = add<Button>(ButtonKind::Icon, L"", "repeat");
    heart_ = add<Button>(ButtonKind::Icon, L"", "heart");
    lyrics_ = add<Button>(ButtonKind::Icon, L"", "lyrics");
    queue_ = add<Button>(ButtonKind::Icon, L"", "queue");
    knob_ = add<ui::Knob>();
    mini_ = add<Button>(ButtonKind::Icon, L"", "mini-player");
    expand_ = add<Button>(ButtonKind::Icon, L"", "expand");
    // Playback speed: always offered for podcast episodes, for songs only once it isn't 1x (keys / palette set it).
    speed_ = add<Button>(ButtonKind::Ghost, speedLabel(1.f));
    speed_->setTooltip(tr(L"Çalma hızı"));
    speed_->onClick = [this] {
        auto* p = ctx().player;
        if (!p || !p->current()) return;
        std::vector<ui::MenuItem> items;
        for (float s : speedSteps()) {
            ui::MenuItem it{speedLabel(s), "", s == 1.f ? std::wstring(tr(L"Normal")) : std::wstring{}, [s] {
                                if (ctx().player) ctx().player->setSpeed(s);
                            }};
            it.checked = std::fabs(p->speed() - s) < 0.01f;
            items.push_back(std::move(it));
        }
        const Rect br = speed_->toWindow(speed_->rect());
        ui::Menu::open(ctx().window, {br.x, br.y - 6}, std::move(items));
    };
    // "Birlikte dinle" (app/Together): the panel's button; during a session a pill that names it.
    jam_ = add<Button>(ButtonKind::Icon, L"", "users");
    jam_->setTooltip(tr(L"Birlikte dinle"));
    jam_->onClick = [] { commands::run("listen-together"); };
    jamLabel_ = add<Button>(ButtonKind::Ghost, L"", "users");
    jamLabel_->setTooltip(tr(L"Birlikte dinle"));
    jamLabel_->onClick = [] { commands::run("listen-together"); };
    jamLabel_->setVisible(false);
    sleep_ = add<Button>(ButtonKind::Icon, L"", "clock");
    sleep_->setTooltip(tr(L"Uyku zamanlayıcı"));
    sleep_->onClick = [this] {
        const Rect br = sleep_->toWindow(sleep_->rect());
        showSleepTimerMenu({br.x, br.y - 6});   // Menu flips upward/leftward to stay on screen
    };
    for (auto* b : {shuffle_, prev_, next_, repeat_}) b->setIconSize(18);
    // Tooltips name the key of the action where there is one (commands::keyTip), as the menus do.
    shuffle_->setTooltip(commands::keyTip(tr(L"Karıştır"), "shuffle"));
    prev_->setTooltip(commands::keyTip(tr(L"Önceki"), "previous"));
    next_->setTooltip(commands::keyTip(tr(L"Sonraki"), "next"));
    repeat_->setTooltip(commands::keyTip(tr(L"Tekrarla"), "repeat"));
    lyrics_->setTooltip(tr(L"Şarkı sözleri"));
    queue_->setTooltip(commands::keyTip(tr(L"Sıradaki"), "queue"));
    mini_->setTooltip(commands::keyTip(tr(L"Mini oynatıcı"), "mini-player"));
    expand_->setTooltip(commands::keyTip(tr(L"Tam ekran"), "now-playing"));
    heart_->setTooltip(commands::keyTip(tr(L"Beğen"), "like"));
    play_->setTooltip(commands::keyTip(tr(L"Oynat"), "play-pause"));
    heart_->setHeart("heart", "heart-filled");   // the like feedback (ui::HeartPop) when it changes

    shuffle_->onClick = [] { smartshuffle::cycleMode(); };   // off -> shuffle -> smart shuffle
    prev_->onClick = [] { ctx().player->previous(); };
    next_->onClick = [] { ctx().player->next(); };
    play_->onClick = [] { ctx().player->togglePause(); };
    repeat_->onClick = [] { ctx().player->cycleRepeat(); };
    heart_->onClick = [] {
        const auto* t = ctx().player->current();
        if (!t) return;
        if (radio::isStationId(t->id)) {   // a station: the heart is the radio favorite
            if (const auto* s = radio::store().find(radio::uuidOf(t->id))) radio::store().toggleFavorite(*s);
            return;
        }
        ctx().library.toggleLiked(*t);
    };
    lyrics_->onClick = [this] { ctx().toggleNowPlaying(!nowPlaying_); };
    queue_->onClick = [] { ctx().toggleQueue(true); };
    mini_->onClick = [] {
        if (ctx().openMiniPlayer) ctx().openMiniPlayer();
    };
    expand_->onClick = [this] { ctx().toggleNowPlaying(!nowPlaying_); };
    knob_->setValue(Settings::get().volume);
    knob_->onChange = [](float v) { ctx().player->setVolume(v); };
    // "Video modu" (app/VideoMode): the song's music video where the cover is in Now Playing, or in its own window.
    video_ = add<Button>(ButtonKind::Icon, L"", "video");
    video_->onClick = [] { videomode::toggle(); };
    video_->onContext = [](const ui::MouseEvent& e) { videomode::showMenu(ctx().window, e.windowPos); };   // video mode's menu
    // The full-screen visualizer (app/Visualizer), beside video mode: another way to watch the song.
    viz_ = add<Button>(ButtonKind::Icon, L"", "visualizer");
    viz_->setTooltip(commands::keyTip(tr(L"Görselleştirici"), "visualizer"));
    viz_->onClick = [] { commands::run("visualizer"); };
    sync();
}

void PlayerBar::setNowPlayingMode(bool on) {
    nowPlaying_ = on;
    heart_->setVisible(!on);
    expand_->setIcon(on ? "collapse" : "expand");
    lyrics_->setActive(on);
    requestLayout();
}

void PlayerBar::sync() {
    auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    // A new track: what was shown so far becomes the outgoing layer of the transition.
    if (swap_.update(p)) {
        old_ = shown_;
        oldTitle_.setText(title_.text());
        oldArtist_.setText(artist_.text());
        heartFrom_ = heart_->visible() ? heart_->rect().x : -1.f;
    }
    live_ = t && radio::isStationId(t->id);
    shown_.images = t ? t->album.images : std::vector<catalog::Image>{};
    shown_.live = live_;
    shown_.recommended = t && t->recommended;
    title_.setText(t ? toWide(t->name) : L"");
    // A station: the song it announces (ICY), else its country and genres.
    artist_.setText(!t ? std::wstring() : live_ && !p->liveTitle().empty() ? p->liveTitle() : toWide(t->artistLine()));
    // A song preview holding the current item (app/Preview) shows it as paused until it comes back.
    previewHeld_ = p && p->previewHoldsCurrent();
    play_->setPlaying(p && p->isPlaying() && !previewHeld_);
    play_->setLoading(p && p->status() == player::Status::Resolving);
    // The wave flows only while sound plays: paused, stopped, resolving or buffering = a straight line.
    seek_->setFlowing(p && p->status() == player::Status::Playing && !previewHeld_);
    seek_->setLive(live_);
    const auto shuffleMode = smartshuffle::mode();
    shuffle_->setActive(shuffleMode != smartshuffle::Mode::Off);
    shuffle_->setIcon(shuffleMode == smartshuffle::Mode::Smart ? "shuffle-smart" : "shuffle");
    shuffle_->setTooltip(commands::keyTip(smartshuffle::modeLabel(shuffleMode), "shuffle"));
    play_->setTooltip(commands::keyTip(p && p->isPlaying() && !previewHeld_ ? tr(L"Duraklat") : tr(L"Oynat"), "play-pause"));
    const auto rep = p ? p->repeat() : RepeatMode::Off;
    repeat_->setActive(rep != RepeatMode::Off);
    repeat_->setIcon(rep == RepeatMode::One ? "repeat-one" : "repeat");
    const bool liked = t && (live_ ? radio::store().isFavorite(radio::uuidOf(t->id)) : ctx().library.isLiked(t->id));
    heart_->setIcon(liked ? "heart-filled" : "heart");
    heart_->setActive(liked);
    // Liked / unliked while it plays (the heart, a row's heart, Ctrl+L, a menu, the phone): the feedback. Another song
    // just shows its own state.
    if (const std::string id = t ? t->id : std::string(); !id.empty() && id == likedId_ && liked != likedWas_) heart_->pulse();
    likedId_ = t ? t->id : std::string();
    likedWas_ = liked;
    heart_->setTooltip(!live_ ? commands::keyTip(tr(L"Beğen"), "like") : liked ? std::wstring(tr(L"Favorilerden kaldır")) : std::wstring(tr(L"Favorilere ekle")));
    // Live: no seeking (the wave runs end to end without a playhead).
    seek_->setEnabled(!live_);
    {
        const float sp = p ? p->speed() : 1.f;
        const bool episode = t && catalog::isPodcastId(t->id);
        speed_->setVisible(t && !live_ && (episode || std::fabs(sp - 1.f) > 0.01f));
        speed_->setLabel(speedLabel(sp));
        speed_->setActive(std::fabs(sp - 1.f) > 0.01f);
    }
    sleep_->setActive(sleepTimerActive());
    sleep_->setTooltip(sleepTimerActive() ? i18n::format(tr(L"Uyku zamanlayıcı · {}"), {sleepTimerLabel()})
                                          : std::wstring(tr(L"Uyku zamanlayıcı")));
    video_->setVisible(videomode::offered());   // not for podcasts and radio
    video_->setEnabled(videomode::enabled());   // a file / stand-in source has no video to follow
    video_->setActive(videomode::on());
    video_->setTooltip(!videomode::enabled() ? std::wstring(tr(L"Bu şarkı için video yok"))
                       : videomode::on()     ? commands::keyTip(tr(L"Video modunu kapat"), "video-mode")
                                             : commands::keyTip(tr(L"Video modu"), "video-mode"));
    for (auto* b : {prev_, next_}) b->setEnabled(t != nullptr);
    // Listen together: a guest's transport follows the host (requests when the host allows it, else off), shuffle and
    // repeat are the host's.
    {
        const std::wstring session = together::barLabel();
        jamLabel_->setLabel(session);
        jam_->setActive(together::active());
        jamLabel_->setActive(together::active());
        if (together::guest()) {
            const bool control = together::guestCanControl();
            for (auto* b : {prev_, next_}) b->setEnabled(t && control);
            play_->setEnabled(t && control);
            seek_->setEnabled(!live_ && control);
            shuffle_->setEnabled(false);
            repeat_->setEnabled(false);
        } else {
            play_->setEnabled(true);
            shuffle_->setEnabled(true);
            repeat_->setEnabled(true);
        }
    }
    if (p) knob_->setValue(p->volume());   // volume / mute shortcuts
    // Keyboard-focused seek bar: arrows step 5 s like the global shortcuts (0 = the slider's 2 % default).
    const int64_t durMs = p ? p->durationMs() : 0;
    seek_->keyStep = durMs > 0 ? std::clamp(5000.f / static_cast<float>(durMs), 0.001f, 0.25f) : 0.f;
    requestLayout();
    invalidate();
}

void PlayerBar::layout() {
    const Rect r = rect();
    const float cy = kBarMidY;
    // Right group.
    float rx = r.w - 20;
    auto place = [&](Widget* w, float size, float gapAfter) {
        rx -= size;
        w->setRect({rx, cy - size * 0.5f, size, size});
        rx -= gapAfter;
    };
    place(expand_, 32, 6);
    place(mini_, 32, 12);
    place(knob_, 36, 12);
    place(queue_, 32, 6);
    place(lyrics_, 32, 6);
    // The visualizer's button only where it costs the center column nothing it needs: shown while the column keeps its
    // 380 DIP with it (wide windows), hidden otherwise (the key, the palette and Now Playing's button still open it).
    const float cx = std::round(r.w * 0.5f);
    {
        const float rest = 38.f + (video_->visible() ? 38.f : 0.f) + 42.f;   // viz, video, sleep (with their gaps)
        const bool fits = !nowPlaying_ && std::min(cx - 230.f, rx - rest - 38 - 16 - cx) >= 190.f;
        viz_->setVisible(fits);
        if (fits) place(viz_, 32, 6);
    }
    if (video_->visible()) place(video_, 32, 6);
    place(sleep_, 32, 10);
    // Center column: the transport row over the seek row (elapsed · wave · total), centered on the bar; 40 % of the
    // width (380..640 DIP) as far as the left info (at least ~140 DIP of text) and the right group (with the jam icon
    // that always follows) leave room.
    const float room = std::min(cx - 230.f, rx - 38 - 16 - cx);
    const float colW = std::round(std::max(240.f, std::min({std::max(380.f, r.w * 0.4f), 640.f, 2 * room})));
    const float colX = cx - std::round(colW * 0.5f);
    {
        constexpr float gap = 10;
        float x = cx - std::round((36 * 4 + 40 + gap * 4) * 0.5f);
        for (Widget* w : std::initializer_list<Widget*>{shuffle_, prev_, play_, next_, repeat_}) {
            const float s = (w == play_) ? 40.f : 36.f;
            w->setRect({x, kTransportY - s * 0.5f, s, s});
            x += s + gap;
        }
    }
    elapsedRect_ = {colX, kSeekY - 10, kTimeW, 20};
    totalRect_ = {colX + colW - kTimeW, kSeekY - 10, kTimeW, 20};
    const float waveX = colX + kTimeW + kTimeGap;
    seek_->setRect({waveX, kSeekY - 8, colX + colW - kTimeW - kTimeGap - waveX, 16});
    // "Birlikte dinle": the pill while a session runs and there is room before the center column, else the icon.
    {
        const float centerRight = colX + colW + 16;
        const float pillW = std::min(260.f, std::ceil(jamLabel_->naturalWidth()));
        const bool pill = together::active() && rx - pillW - (speed_->visible() ? 52 : 0) >= centerRight;
        jamLabel_->setVisible(pill);
        jam_->setVisible(!pill);
        if (pill) {
            rx -= pillW;
            jamLabel_->setRect({rx, cy - 16, pillW, 32});
            rx -= 6;
        } else {
            place(jam_, 32, 6);
        }
    }
    if (speed_->visible()) {
        const float w = std::max(40.f, std::ceil(speed_->naturalWidth()));
        rx -= w;
        speed_->setRect({rx, cy - 16, w, 32});
        rx -= 6;
    }
    // Left: art + title/artist + heart.
    artRect_ = {20, cy - 28, 56, 56};
    const float textX = artRect_.right() + 14;
    const float maxW = std::max(60.f, std::min(320.f, colX - textX - 48));
    const float tw = std::min(maxW, std::ceil(title_.measure().w));
    infoRect_ = {textX, cy - 20, std::max(tw, std::ceil(artist_.measure().w)), 40};
    shown_.titleW = tw + 2;
    // The heart follows the title's end; during a track change it slides from the old title's end to the new one's.
    float heartX = textX + tw + 6;
    heartSliding_ = swap_.running() && heartFrom_ >= 0;
    if (heartSliding_) heartX = ui::lerp(heartFrom_, heartX, swap_.progress());
    heart_->setRect({heartX, cy - 16 - 8, 32, 32});
    // No heart for a track that can't be liked (logged in: only Spotify tracks).
    const auto* cur = ctx().player ? ctx().player->current() : nullptr;
    heart_->setVisible(!nowPlaying_ && cur && (radio::isStationId(cur->id) || ctx().library.canLike(cur->id)));
}

bool PlayerBar::onMouseDown(const ui::MouseEvent& e) {
    const gfx::Point p{e.pos.x - rect().x, e.pos.y - rect().y};
    if (e.button == ui::MouseButton::Left && nowPlaying_ && stripRect_.contains(p)) {   // the spectrum strip
        commands::run("visualizer");
        return true;
    }
    if (e.button == ui::MouseButton::Left && !nowPlaying_ && (artRect_.contains(p) || infoRect_.contains(p)) &&
        ctx().player->current()) {
        const auto* t = ctx().player->current();
        if (artRect_.contains(p)) ctx().toggleNowPlaying(true);
        else if (radio::isStationId(t->id)) ctx().router->navigate({RouteKind::Radio});
        else if (catalog::isPodcastId(t->id)) openEpisodeShow(*t);
        else if (!t->album.id.empty()) ctx().router->navigate({RouteKind::Album, t->album.id});
        return true;
    }
    if (e.button == ui::MouseButton::Right && infoRect_.contains(p)) {
        if (const auto* t = ctx().player->current()) {
            if (radio::isStationId(t->id)) showStationMenu(*t, e.windowPos);
            else showTrackMenu({*t}, e.windowPos);
        }
        return false;
    }
    return false;
}

void PlayerBar::onMouseMove(const ui::MouseEvent& e) {
    const gfx::Point p{e.pos.x - rect().x, e.pos.y - rect().y};
    const bool hot = !nowPlaying_ && (artRect_.contains(p) || infoRect_.contains(p));
    if (hot != infoHover_) {
        infoHover_ = hot;
        invalidate();
    }
    bool locked = false;
    if (!together::lockedReason().empty())
        for (Widget* w : std::initializer_list<Widget*>{shuffle_, prev_, play_, next_, repeat_, seek_})
            if (!w->enabled() && w->visible() && w->rect().contains(p)) locked = true;
    lockedHover_ = locked;
}

void PlayerBar::onMouseLeave() {
    infoHover_ = false;
    lockedHover_ = false;
    invalidate();
}

ui::Widget* PlayerBar::hitTest(gfx::Point pos) {
    Widget* hit = Widget::hitTest(pos);
    if (hit && hit != this && !hit->enabled() && hit->parent() == this && !together::lockedReason().empty()) return this;
    return hit;
}

std::wstring PlayerBar::tooltip() const { return lockedHover_ ? together::lockedReason() : std::wstring(); }

void PlayerBar::paintInfo(Canvas& c, bool outgoing) {
    const auto& col = colors();
    const auto layer = outgoing ? swap_.outgoing() : swap_.incoming();
    if (layer.alpha <= 0.001f) return;
    const Shown& s = outgoing ? old_ : shown_;
    gfx::Text& title = outgoing ? oldTitle_ : title_;
    gfx::Text& artist = outgoing ? oldArtist_ : artist_;
    const bool group = layer.alpha < 0.999f;
    if (group) c.pushOpacity(layer.alpha);
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, layer.dy));
    c.shadow(artRect_, gfx::radii().art, 40, 0, accent().glow);   // playerThumbGlow
    if (s.live) drawStationArt(c, s.images, artRect_, gfx::radii().art);
    else if (outgoing) drawArtwork(c, s.images, artRect_, gfx::radii().art);
    else art_.draw(c, s.images, artRect_, gfx::radii().art);   // a cover still loading fades in over the placeholder
    c.text(title, {infoRect_.x, infoRect_.y + 1, s.titleW, 20}, col.fgPrimary, gfx::VAlign::Center);
    c.text(artist, {infoRect_.x, infoRect_.y + 21, 320, 18}, infoHover_ && !outgoing ? col.fgPrimary : col.fgSecondary,
           gfx::VAlign::Center);
    if (s.recommended)   // smart shuffle / "Geliştir"
        smartshuffle::drawBadge(c, infoRect_.x + std::min(320.f, std::ceil(artist.measure().w)) + 8, infoRect_.y + 30);
    c.popTransform();
    if (group) c.popLayer();
}

void PlayerBar::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    auto* p = ctx().player;
    if (p && p->previewHoldsCurrent() != previewHeld_) {   // a song preview started / ended (it repaints the window)
        previewHeld_ = !previewHeld_;
        play_->setPlaying(p->isPlaying() && !previewHeld_);
        seek_->setFlowing(p->status() == player::Status::Playing && !previewHeld_);
    }
    // The queue button is lit while its panel is open, as the title bar's friend activity button is for its panel.
    if (auto* shell = dynamic_cast<Shell*>(parent())) queue_->setActive(shell->queueOpen() && !nowPlaying_);
    c.fillSurface(r);   // bg.base (Mica: its translucent tint)
    c.hline(r.x, r.right(), r.y, col.hairSubtle);   // the bar's top edge
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));

    const auto* t = p ? p->current() : nullptr;
    const int64_t dur = p ? p->durationMs() : 0;
    const int64_t pos = p ? p->positionMs() : 0;
    if (!seek_->dragging()) seek_->setValue(!live_ && dur > 0 ? static_cast<float>(pos) / static_cast<float>(dur) : 0.f);
    seek_->setBuffered(p && !live_ ? p->bufferedFraction() : 0.f);

    if (nowPlaying_) {
        // Spectrum strip instead of track info (hi-fi cue from the Now Playing mock).
        float bands[48] = {};
        const bool live = p && p->spectrum(bands, 48);
        const float x0 = 28, baseY = kBarMidY + 6;
        stripRect_ = {x0 - 6, baseY - 22, 48 * 2.5f + 12, 28};
        for (int i = 0; i < 48; ++i) {
            const float v = live ? bands[i] : 0.f;
            const float h = 2 + v * 14;
            const bool played = live_ || i < 48 * (dur > 0 ? static_cast<float>(pos) / dur : 0);   // a stream is all "now"
            c.fillRect({x0 + i * 2.5f, baseY - h, 1, h}, (played ? acc.base : col.fgTertiary).mulAlpha(live ? 1.f : 0.5f));
        }
    } else {
        if (swap_.running() || heartSliding_) requestLayout();   // the heart slides along
        if (swap_.running()) paintInfo(c, true);
        if (t) paintInfo(c, false);
    }

    // Times beside the wave: elapsed (fg.secondary) and total (fg.tertiary); a live stream has none: the "CANLI"
    // badge after the wave instead.
    if (live_ && t) {
        drawLiveBadge(c, {totalRect_.x, totalRect_.cy()}, p->isPlaying());
    } else {
        elapsed_.setText(ui::formatDuration(seek_->dragging() ? static_cast<int64_t>(seek_->value() * dur) : pos));
        total_.setText(ui::formatDuration(dur));
        c.text(elapsed_, elapsedRect_, col.fgSecondary, gfx::VAlign::Center);
        c.text(total_, totalRect_, col.fgTertiary, gfx::VAlign::Center);
    }
    c.popTransform();
    paintChildren(c);
    // Hover time preview just above the wave, between the two rows (after the buttons: it may cover their edge).
    if (const float hf = seek_->hoverFraction(); hf >= 0 && dur > 0 && !live_) {
        const Rect sr = seek_->rect();
        const std::wstring s = ui::formatDuration(static_cast<int64_t>(hf * dur));
        const float x = std::clamp(sr.x + hf * sr.w, 24.f, r.w - 24.f);
        const Rect tip{r.x + x - 24, r.y + sr.cy() - 6 - 18, 48, 18};
        c.fillRounded(tip, gfx::radii().tooltip, col.bgElevated);
        c.strokeRounded(tip, gfx::radii().tooltip, col.hairStrong);
        c.text(s, type::monoMeta, tip, col.fgPrimary, gfx::TextAlign::Center, gfx::VAlign::Center);
    }
    // Keep repainting while playing: the wave flows at ~30 fps while it is on screen (not under the full-screen lyrics,
    // not with reduce motion); otherwise only the time label ticks, 4 times a second (Now Playing's spectrum strip:
    // 33 ms). Paused: no wake-ups at all.
    if (p && p->isPlaying()) {
        const bool flowing = seek_->waveMoving() && !LyricsFullscreen::isOpen();
        if (auto* w = window()) {   // the flowing wave: paced by presentation (~30 fps at any refresh rate)
            if (flowing) w->invalidateAfterShown(ui::kWaveFrameMs);
            else w->invalidateAfter(nowPlaying_ ? 33 : 250);
        }
    }
}

// ===================================================================================================
// QueuePanel

namespace {
// Upcoming rows: 52 DIP, 40 with "Liste yoğunluğu: Sıkı" (Settings::compactLists, app/Display); "Şimdi çalıyor" is a
// 64 DIP block of its own.
float queueRowH() { return Settings::get().compactLists ? 40.f : 52.f; }
constexpr float kPadX = 20;
constexpr float kNowLabelY = 64, kNowY = 84, kNowH = 64, kSectionY = 168, kListTop = 192;
constexpr float kHeadH = 36;   // a section header inside the list
} // namespace

QueuePanel::QueuePanel()
    : title_(tr(L"Sıradaki"), type::title), meta_({}, type::monoLabel), nowLabel_(toUpperTr(tr(L"Şimdi çalıyor")), type::monoLabel) {
    clipsChildren = true;
    focusable = true;   // one Tab stop: the keyboard cursor over the upcoming rows (onKeyDown)
    gfx::TextOptions right;
    right.align = gfx::TextAlign::Trailing;
    meta_.setOptions(right);
    save_ = add<Button>(ButtonKind::Icon, L"", "playlist");
    save_->setTooltip(tr(L"Kuyruğu kaydet"));
    save_->onClick = [this] { saveAsPlaylist(); };
    save_->setVisible(false);
    clear_ = add<Button>(ButtonKind::Icon, L"", "trash");
    clear_->setTooltip(tr(L"Sırayı temizle"));
    clear_->onClick = [] {
        if (auto* p = ctx().player) p->clearQueue();
    };
    clear_->setVisible(false);
    // The side panels share one header: title, their actions, then "Kapat" at the right end (the friend activity and
    // "Birlikte dinle" panels have it too). Not in Now Playing's tab.
    close_ = add<Button>(ButtonKind::Icon, L"", "close");
    close_->setTooltip(tr(L"Kapat"));
    close_->onClick = [] { commands::run("queue"); };
    empty_ = add<MessagePanel>("queue", tr(L"Sıra boş"), tr(L"Bir şarkı çal ya da şarkıları buraya sürükle."));
    empty_->setCompact(true);
    empty_->hitTestVisible = false;
}

void QueuePanel::saveAsPlaylist() {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return;
    std::vector<catalog::Track> tracks;
    for (int i = p->currentOrderIndex(); i < static_cast<int>(p->order().size()); ++i)
        if (const auto& t = p->items()[p->order()[i]]; !radio::isStationId(t.id) && !catalog::isPodcastId(t.id)) {
            tracks.push_back(t);   // songs only
            tracks.back().queued = tracks.back().recommended = false;
        }
    if (tracks.empty()) return;
    // Name dialog -> a Spotify playlist with the Spotify tracks (logged in) or a local playlist; it reports with a toast.
    promptNewPlaylist({}, std::move(tracks));
}

void QueuePanel::rebuildSections() {
    auto* p = ctx().player;
    const int cur = p ? p->currentOrderIndex() : -1;
    const int n = p ? static_cast<int>(p->order().size()) : 0;
    sections_.rowH = queueRowH();
    sections_.headH = kHeadH;
    if (cur < 0) sections_.build(0, [](int) { return false; });
    else sections_.build(n - cur - 1, [p, cur](int k) { return p->items()[p->order()[cur + 1 + k]].queued; });
    secPos_ = cur;
    secSize_ = n;
    secGen_ = p ? p->queueGeneration() : 0;
}

void QueuePanel::sync() {
    auto* p = ctx().player;
    rebuildSections();
    int64_t remaining = 0;
    int count = 0;
    if (p) {
        for (int i = p->currentOrderIndex() + 1; i < static_cast<int>(p->order().size()); ++i) {
            remaining += p->items()[p->order()[i]].durationMs;
            ++count;
        }
    }
    // "12 · 34 DK": upcoming tracks · their minutes; stations have no length: "12 İSTASYON".
    const auto* cur = p ? p->current() : nullptr;
    const bool stations = cur && radio::isStationId(cur->id);
    meta_.setText(stations ? toUpperTr(i18n::plural(L"{} istasyon", count))
                           : i18n::format(tr(L"{} · {} DK"), {std::to_wstring(count), std::to_wstring(remaining / 60000)}));
    // "Kuyruğu kaydet" makes a playlist of songs: not for a list of stations.
    const bool any = p && p->currentOrderIndex() >= 0 && !p->order().empty();
    save_->setVisible(any && !stations);
    clear_->setVisible(any && count > 0);
    empty_->setVisible(!any);
    requestLayout();
    invalidate();
}

void QueuePanel::layout() {
    const float w = rect().w;
    close_->setVisible(!embedded_);
    if (embedded_) {   // on the "ŞİMDİ ÇALIYOR" row, ending with the row
        const float y = kNowLabelY + oy() - 8;
        clear_->setRect({w - kPadX - 32 + 6, y, 32, 32});
        save_->setRect({w - kPadX - 32 + 6 - (clear_->visible() ? 36 : 0), y, 32, 32});
    } else {           // the side panels' header geometry (FriendPanel, JamPanel): 32 DIP buttons 4 apart, y 22
        float x = w - kPadX - 32;
        close_->setRect({x, 22, 32, 32});
        for (Button* b : {clear_, save_}) {
            if (!b->visible()) continue;
            x -= 36;
            b->setRect({x, 22, 32, 32});
        }
    }
    const float top = embedded_ ? 0.f : kNowLabelY + oy() - 16;
    empty_->setRect({0, top, w, empty_->preferredHeight(w)});
}

float QueuePanel::listTop() const { return kListTop + oy(); }

Rect QueuePanel::rowRect(int orderIndex) const {
    auto* p = ctx().player;
    const int cur = p ? p->currentOrderIndex() : 0;
    if (orderIndex == cur) return {kPadX - 8, kNowY + oy(), rect().w - (kPadX - 8) * 2, kNowH};
    const int k = orderIndex - cur - 1;
    return {0, listTop() + sections_.rowY(k) - scroll_.value(), rect().w, sections_.rowH};
}

Rect QueuePanel::removeRect(const Rect& row) const { return {row.right() - kPadX - 28 + 6, row.cy() - 14, 28, 28}; }

int QueuePanel::rowAt(gfx::Point pt) const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return -1;
    const gfx::Point lp{pt.x - rect().x, pt.y - rect().y};
    if (rowRect(p->currentOrderIndex()).contains(lp)) return p->currentOrderIndex();
    if (lp.y < listTop()) return -1;
    const int k = sections_.rowAt(lp.y - listTop() + scroll_.value());
    return k >= 0 ? p->currentOrderIndex() + 1 + k : -1;
}

float QueuePanel::maxScroll() const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return 0;
    return std::max(0.f, sections_.height() - (rect().h - listTop()) + 16);
}

bool QueuePanel::onWheel(float dy, float, const ui::MouseEvent&) {
    if (!ctx().player || dy == 0) return false;
    scroll_.setBounds(0, maxScroll());
    scroll_.wheel(dy, queueRowH() * 2.f, (rect().h - listTop()) * 0.9f);   // two rows a notch
    invalidate();
    return true;
}

bool QueuePanel::onMouseDown(const ui::MouseEvent& e) {
    const int i = rowAt(e.pos);
    if (i < 0) return false;
    auto* p = ctx().player;
    if (e.button == ui::MouseButton::Right) {
        showRowMenu(i, e.windowPos);
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    // The hovered row's "×": out of the queue at once.
    if (i != p->currentOrderIndex()) {
        const Rect rr = rowRect(i);
        if (removeRect(rr).contains({e.pos.x - rect().x, e.pos.y - rect().y})) {
            p->removeAt(i);
            hover_ = -1;
            hoverFade_.clear();
            return false;
        }
    }
    dragFrom_ = i;
    dragTo_ = i;
    dragStartY_ = dragY_ = e.pos.y - rect().y;
    dragging_ = false;
    return true;
}

void QueuePanel::showRowMenu(int i, gfx::Point windowPos) {
    auto* p = ctx().player;
    if (!p || i < 0 || i >= static_cast<int>(p->order().size())) return;
    const auto track = p->items()[p->order()[i]];
    // The row cursor's own keys as the hints (Enter plays the row, Delete takes it out), as menus show shortcuts.
    auto keyName = [](std::string_view k) { const auto c = shortcuts::parse(k); return c ? shortcuts::display(*c) : std::wstring(); };
    std::vector<ui::MenuItem> items{{tr(L"Şimdi çal"), "play", keyName("Enter"), [i] { ctx().player->jumpTo(i); }},
                                    {tr(L"Sıradan kaldır"), "minus", keyName("Delete"), [i] { ctx().player->removeAt(i); }}};
    if (track.recommended) {   // smart shuffle / "Geliştir": into the list it was recommended for, or stop them
        items.push_back(ui::MenuItem::sep());
        if (smartshuffle::canAdd({}, track))
            items.push_back({tr(L"Bu öneriyi listeye ekle"), "plus", L"", [track] { smartshuffle::add({}, track); }});
        items.push_back({tr(L"Önerileri gösterme"), "close", L"", [] { smartshuffle::stop({}); }});
    }
    items.push_back(ui::MenuItem::sep());
    items.push_back({tr(L"Sırayı dışa aktar…"), "share", L"", [] { transfer::exportList(transfer::What::Queue, {}, {}); }});
    items.push_back({tr(L"Sırayı temizle"), "trash", L"", [] { ctx().player->clearQueue(); }});
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

int QueuePanel::cursorIndex() const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0 || sections_.count == 0) return -1;
    return p->currentOrderIndex() + 1 + std::clamp(cursor_, 0, sections_.count - 1);
}

Rect QueuePanel::focusRect() const {
    const int i = cursorIndex();
    if (i < 0) return rect();
    const Rect rr = rowRect(i);
    const float top = std::max(rr.y, listTop() - 4), bottom = std::min(rr.bottom(), rect().h);   // clipped to the list
    return {rect().x + rr.x + 4, rect().y + top, rr.w - 8, std::max(0.f, bottom - top)};
}

void QueuePanel::revealCursor() {
    if (cursorIndex() < 0) return;
    const float y = sections_.rowY(std::clamp(cursor_, 0, sections_.count - 1)), viewH = rect().h - listTop();
    float s = scroll_.target();
    if (y < s) s = y;
    else if (y + sections_.rowH > s + viewH - 8) s = y + sections_.rowH - viewH + 8;
    scroll_.setBounds(0, maxScroll());
    if (s != scroll_.target()) scroll_.glideTo(s);
}

bool QueuePanel::onKeyDown(const ui::KeyEvent& e) {
    auto* p = ctx().player;
    const int n = sections_.count;
    if (!p || p->currentOrderIndex() < 0 || n == 0 || e.ctrl) return false;
    int k = std::clamp(cursor_, 0, n - 1);
    const int page = std::max(1, static_cast<int>((rect().h - listTop()) / sections_.rowH) - 1);
    const int cur = p->currentOrderIndex();
    if (e.alt && (e.vk == VK_UP || e.vk == VK_DOWN)) {   // move the row
        const int to = std::clamp(k + (e.vk == VK_UP ? -1 : 1), 0, n - 1);
        if (to != k) {
            p->move(cur + 1 + k, cur + 1 + to);
            cursor_ = to;
            revealCursor();
        }
        return true;
    }
    if (e.alt) return false;
    switch (e.vk) {
    case VK_DOWN: k = std::min(n - 1, k + 1); break;
    case VK_UP: k = std::max(0, k - 1); break;
    case VK_NEXT: k = std::min(n - 1, k + page); break;
    case VK_PRIOR: k = std::max(0, k - page); break;
    case VK_HOME: k = 0; break;
    case VK_END: k = n - 1; break;
    case VK_RETURN: p->jumpTo(cur + 1 + k); return true;
    case VK_DELETE:
        if (!e.repeat) p->removeAt(cur + 1 + k);
        cursor_ = std::min(k, n - 2);
        return true;
    case VK_APPS:
    case VK_F10: {
        if (e.vk == VK_F10 && !e.shift) return false;
        const Rect fr = toWindow(focusRect());
        showRowMenu(cur + 1 + k, {fr.x + 48, fr.bottom()});
        return true;
    }
    default: return false;
    }
    cursor_ = k;
    revealCursor();
    invalidate();
    return true;
}

void QueuePanel::onMouseMove(const ui::MouseEvent& e) {
    auto* p = ctx().player;
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    if (pressed() && dragFrom_ >= 0 && p && dragFrom_ != p->currentOrderIndex()) {
        if (std::abs(lp.y - dragStartY_) > 6 && !dragging_) {
            dragging_ = true;
            dragLineY_.snap(rowRect(dragFrom_).y);
        }
        if (dragging_) {
            dragY_ = lp.y;
            // The slot the row would land in front of (sections' headers count as their first row), as Player::move's
            // final index.
            const int slot = sections_.insertAt(lp.y - listTop() + scroll_.value());
            const int ins = p->currentOrderIndex() + 1 + slot;
            const int to = std::clamp(ins > dragFrom_ ? ins - 1 : ins, p->currentOrderIndex() + 1,
                                      static_cast<int>(p->order().size()) - 1);
            if (to != dragTo_) {
                dragTo_ = to;
                const float y = listTop() + sections_.rowY(slot) - scroll_.value();
                dragLineY_.to(y, ui::motion::microMs(ui::motion::fast), ui::Ease::Decelerate);
            }
            invalidate();
        }
        return;
    }
    const int i = rowAt(e.pos);
    const bool rm = i >= 0 && p && i != p->currentOrderIndex() && removeRect(rowRect(i)).contains(lp);
    if (i != hover_ || rm != hoverRemove_) {
        if (i != hover_) hoverFade_.set(i);
        hover_ = i;
        hoverRemove_ = rm;
        invalidate();
    }
}

void QueuePanel::onMouseLeave() {
    hover_ = -1;
    hoverRemove_ = false;
    hoverFade_.set(-1);
    invalidate();
}

void QueuePanel::onMouseUp(const ui::MouseEvent& e) {
    auto* p = ctx().player;
    if (dragging_ && dragFrom_ >= 0 && dragTo_ >= 0 && dragTo_ != dragFrom_) p->move(dragFrom_, dragTo_);
    else if (!dragging_ && dragFrom_ >= 0 && dragFrom_ == rowAt(e.pos) && dragFrom_ != p->currentOrderIndex())
        p->jumpTo(dragFrom_);
    dragging_ = false;
    dragFrom_ = dragTo_ = -1;
    invalidate();
}

std::optional<PreviewHit> QueuePanel::previewAt(gfx::Point windowPos, bool keyboard) {
    auto* p = ctx().player;
    if (keyboard || !p) return std::nullopt;
    const int i = rowAt(fromWindow(windowPos));
    if (i < 0 || i == p->currentOrderIndex()) return std::nullopt;
    const Rect rr = rowRect(i);
    return PreviewHit{preview::songTarget(p->items()[p->order()[i]]), toWindow({rect().x + rr.x, rect().y + rr.y, rr.w, rr.h})};
}

int QueuePanel::dropIndexAt(gfx::Point windowPos) const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return 0;
    const gfx::Point pp = fromWindow(windowPos);
    return p->currentOrderIndex() + 1 + sections_.insertAt(pp.y - rect().y - listTop() + scroll_.value());
}

bool QueuePanel::dragOver(const DragPayload& payload, gfx::Point windowPos) {
    auto* p = ctx().player;
    if (!p || !visible() || (!payload.fromExplorer() && payload.tracks.empty())) return false;
    // Near the list's top / bottom edge: scroll it, faster the closer the pointer is to the edge.
    const double now = ui::frame::realNow();
    if (lastScrollTick_ > 0 && p->currentOrderIndex() >= 0) {
        const gfx::Point pp = fromWindow(windowPos);
        const float y = pp.y - rect().y, top = listTop(), bottom = rect().h, edge = 32;
        const float dt = static_cast<float>(std::min(now - lastScrollTick_, 100.0)) / 1000.f;
        float v = 0;
        if (y >= top && y < top + edge) v = -(1 - (y - top) / edge);
        else if (y > bottom - edge && y <= bottom) v = 1 - (bottom - y) / edge;
        if (v != 0) {
            scroll_.setBounds(0, maxScroll());
            scroll_.snap(scroll_.value() + v * 700.f * dt);   // clamped to the bounds
        }
    }
    lastScrollTick_ = now;
    const int at = dropIndexAt(windowPos);
    if (at != dropAt_) {
        const float y = p->currentOrderIndex() >= 0
                            ? listTop() + sections_.rowY(at - p->currentOrderIndex() - 1) - scroll_.value()
                            : 0.f;
        if (dropAt_ < 0) dragLineY_.snap(y);
        else dragLineY_.to(y, ui::motion::microMs(ui::motion::fast), ui::Ease::Decelerate);
        dropAt_ = at;
        invalidate();
    }
    return true;
}

void QueuePanel::dragLeave() {
    dropAt_ = -1;
    lastScrollTick_ = 0;
    invalidate();
}

void QueuePanel::drop(const DragPayload& payload, gfx::Point windowPos) {
    const int at = dropIndexAt(windowPos);
    dragLeave();
    dragdrop::dropOnQueue(at, payload);
}

std::wstring QueuePanel::sectionLabel(bool queued) const {
    if (queued) return toUpperTr(tr(L"Sırada"));
    std::wstring s = tr(L"SONRAKİ");
    if (const auto* p = ctx().player; p && !p->context().name.empty()) s += L" · " + toUpperTr(p->context().name);
    return s;
}

void QueuePanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    auto* p = ctx().player;
    if (p && (p->currentOrderIndex() != secPos_ || static_cast<int>(p->order().size()) != secSize_ ||
              p->queueGeneration() != secGen_ || sections_.rowH != queueRowH()))
        rebuildSections();   // the queue changed before sync() ran (or the density did)
    scroll_.setBounds(0, maxScroll());   // the queue changed / the panel resized: the offset follows the new end
    if (!embedded_) {
        c.fillRect(r, col.bgBase.withAlpha(0.35f));
        c.vline(r.x, r.y, r.bottom(), col.hairSubtle);
    }
    c.pushClip(r);
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    if (!embedded_) {
        const float tw = std::ceil(title_.measure().w);
        c.text(title_, {kPadX, 20, tw + 1, 36}, col.fgPrimary, gfx::VAlign::Center);
    }
    if (!p || p->currentOrderIndex() < 0) {   // the empty state is the child panel
        if (dropAt_ >= 0) c.strokeRounded({8, 8, r.w - 16, r.h - 16}, gfx::radii().surface, acc.base, 2);   // songs dragged in play here
        c.popTransform();
        c.popClip();
        paintChildren(c);
        return;
    }
    const int cur = p->currentOrderIndex();
    // "Şimdi çalıyor": the label (the actions on its row when embedded), then the playing song's block with its progress.
    c.text(nowLabel_, {kPadX, kNowLabelY + oy(), r.w - kPadX * 2, 16}, acc.base);

    // One upcoming / playing row. Badges (recommended, added by a guest, blocked) sit right, before the duration; the
    // hovered row trades its duration for "×" (out of the queue).
    auto drawRow = [&](int oi, const Rect& rr, bool current, float hv) {
        const auto& t = p->items()[p->order()[oi]];
        float x = current ? rr.x + 8 : kPadX;
        if (!current) c.icon("drag-handle", {x - 14, rr.cy() - 6, 12, 12}, col.fgTertiary.mulAlpha(0.35f + 0.65f * hv));
        // Kara liste: an upcoming blocked track is skipped by the player, so it is dimmed like in the track tables;
        // the badge also marks a blocked track that plays because the user picked it.
        const bool blocked = blacklist::isBlocked(t);
        const float alpha = blocked && !current ? 0.38f : 1.f;
        const float art = current ? 48.f : Settings::get().compactLists ? 28.f : 36.f;
        const Rect artR{x, rr.cy() - art * 0.5f, art, art};
        const bool station = radio::isStationId(t.id);
        if (station) drawStationArt(c, t.album.images, artR, gfx::radii().art, current ? Priority::High : Priority::Normal);
        else drawArtwork(c, t.album.images, artR, gfx::radii().art, Placeholder::Album, false, current ? Priority::High : Priority::Normal);
        if (alpha < 1) c.fillRect(artR, col.bgBase.withAlpha(0.6f));
        x += art + 12;
        const float right = current ? rr.right() - 12 : r.w - kPadX;
        float rightW = current ? 24.f : 52.f;
        if (t.recommended) {   // smart shuffle / "Geliştir"
            const float bw = smartshuffle::badgeWidth();
            smartshuffle::drawBadge(c, right - rightW - 8 - bw, rr.cy());
            rightW += bw + 8;
        }
        if (const std::wstring by = together::addedBy(t); !by.empty()) {   // a listen-together guest queued it
            const float bw = together::addedBadgeWidth(by);
            together::drawAddedBadge(c, right - rightW - 8 - bw, rr.cy(), by);
            rightW += bw + 8;
        }
        if (blocked) {
            const float bw = badge::width(badge::blockedLabel());
            badge::draw(c, right - rightW - 8 - bw, rr.cy(), badge::blockedLabel(), badge::Tone::Neutral);
            rightW += bw + 8;
        }
        const float tw = std::max(0.f, right - rightW - 8 - x);
        // The playing station: the song it announces (ICY) when it sends one.
        const std::wstring second = current && station && !p->liveTitle().empty() ? p->liveTitle() : toWide(t.artistLine());
        const bool alone = second.empty();
        const auto& titleStyle = current ? type::body : type::body.withSize(13);
        const float th = current ? 20.f : 18.f;
        c.text(toWide(t.name), titleStyle, {x, alone ? rr.cy() - th * 0.5f : rr.cy() - th + 1, tw, th},
               (current ? acc.base : col.fgPrimary).mulAlpha(alpha), gfx::TextAlign::Leading, gfx::VAlign::Center);
        if (!alone)
            c.text(second, (current ? type::caption : type::caption.withSize(11)), {x, rr.cy() + 2, tw, 16},
                   col.fgSecondary.mulAlpha(alpha), gfx::TextAlign::Leading, gfx::VAlign::Center);
        if (current) {
            drawPlayingBars(c, this, {right - 16, rr.cy() - 8, 16, 16}, acc.base);   // moving / frozen
            return;
        }
        const float rm = (hover_ == oi && !dragging_) ? hv : 0.f;
        if (!station && rm < 0.999f)   // stations have no duration
            c.text(ui::formatDuration(t.durationMs), type::monoMeta, {right - 52, rr.y, 52, rr.h},
                   col.fgTertiary.mulAlpha(alpha * (1 - rm)), gfx::TextAlign::Trailing, gfx::VAlign::Center);
        if (rm > 0.001f) {
            const Rect xr = removeRect(rr);
            c.icon("close", xr.center(14, 14).offset((1 - rm) * 6, 0), (hoverRemove_ ? col.fgPrimary : col.fgSecondary).mulAlpha(rm));
        }
    };

    // The playing song: a tinted block with a 2 DIP progress line along its foot.
    const Rect now = rowRect(cur);
    c.fillRounded(now, gfx::radii().surface, acc.tint06);
    drawRow(cur, now, true, 0);
    if (const int64_t dur = p->durationMs(); dur > 0 && !p->isLive()) {
        const float f = std::clamp(static_cast<float>(p->positionMs()) / static_cast<float>(dur), 0.f, 1.f);
        const Rect track{now.x + 8, now.bottom() - 6, now.w - 16, 2};
        c.fillRect(track, col.hairSubtle);
        c.fillRect({track.x, track.y, std::round(track.w * f), track.h}, acc.base.mulAlpha(0.85f));
    }

    // Sections: the first one's label above the list (with the count and minutes), the others inside it.
    if (sections_.count > 0) {
        c.text(sectionLabel(sections_.queued.front()), type::monoLabel, {kPadX, kSectionY + oy(), r.w - kPadX * 2 - 90, 16},
               col.fgTertiary);
        c.text(meta_, {r.w - kPadX - 90, kSectionY + oy(), 90, 16}, col.fgTertiary);
    } else {
        c.text(tr(L"Sırada başka şarkı yok."), type::secondary, {kPadX, kSectionY + oy() - 2, r.w - kPadX * 2, 20}, col.fgTertiary);
    }
    c.pushClip({0, listTop() - 4, r.w, r.h - listTop() + 4});
    const float scroll = scroll_.value();
    const float viewH = r.h - listTop();
    for (int run = 1; run < static_cast<int>(sections_.queued.size()); ++run) {
        const float y = listTop() + sections_.headerY(run) - scroll;
        if (y + kHeadH < listTop() - 4 || y > r.h) continue;
        c.hline(kPadX, r.w - kPadX, y + 8, col.hairSubtle);
        c.text(sectionLabel(sections_.queued[run]), type::monoLabel, {kPadX, y + 16, r.w - kPadX * 2, 16}, col.fgTertiary);
    }
    // Visible rows only (5 000 queued songs cost what fits in the panel).
    const int n = sections_.count;
    int k = std::max(0, static_cast<int>(scroll / sections_.rowH) - static_cast<int>(sections_.breaks.size()) - 1);
    while (k < n && sections_.rowY(k) + sections_.rowH < scroll) ++k;
    for (; k < n; ++k) {
        const float y = sections_.rowY(k) - scroll;
        if (y > viewH) break;
        const int oi = cur + 1 + k;
        const Rect rr{0, listTop() + y, r.w, sections_.rowH};
        if (dragging_ && oi == dragFrom_) {   // the slot the dragged row left: a hairline outline
            c.strokeRounded(rr.inset(kPadX - 8, 3).inset(0.5f), gfx::radii().surface, col.hairDefault);
            continue;
        }
        const float hv = hoverFade_.of(oi);
        if (hv > 0.001f && !dragging_) c.fillRect(rr, col.overlayHover.mulAlpha(hv));
        drawRow(oi, rr, false, dragging_ ? 0.f : hv);
    }
    // The insertion line (a row moved here, or songs dragged in from a list / files from Explorer): it glides between
    // slots, with a small ring at its start.
    if ((dragging_ && dragTo_ >= 0 && dragTo_ != dragFrom_) || dropAt_ >= 0) {
        const float y = std::round(dragLineY_.value());
        c.fillRect({kPadX - 4, y - 1, r.w - kPadX * 2 + 4, 2}, acc.base);
        c.strokeCircle({kPadX - 9, y}, 3.5f, acc.base, 1.5f);
    }
    c.popClip();
    // The dragged row follows the pointer, lifted on the elevated surface.
    if (dragging_ && dragFrom_ >= 0 && dragFrom_ < static_cast<int>(p->order().size())) {
        const float h = sections_.rowH;
        const Rect lifted{6, std::round(dragY_ - h * 0.5f), r.w - 12, h};
        c.shadow(lifted, gfx::radii().surface, 18, 6, col.shadowCard);
        c.fillRounded(lifted, gfx::radii().surface, col.bgElevated);
        c.strokeRounded(lifted.inset(0.5f), gfx::radii().surface, col.hairStrong);
        drawRow(dragFrom_, {0, lifted.y, r.w, h}, false, 0);
    }
    c.popTransform();
    c.popClip();
    paintChildren(c);
}

// ===================================================================================================
// PageHost

void PageHost::show(std::unique_ptr<Page> page) {
    Page* old = page_;
    page_ = static_cast<Page*>(adopt(std::move(page)));
    // Takes the old page out of the tree (and keeps it while it fades out), or drops it at once without transitions.
    transition_.begin(*this, old, page_, ctx().router ? ctx().router->lastChange() : Router::Change{});
    requestLayout();
    invalidate();
}

void PageHost::layout() {
    if (page_) page_->setRect({0, 0, rect().w, rect().h});
}

void PageHost::paint(Canvas& c) {
    if (page_) transition_.paint(c, *this, page_);
}

// ===================================================================================================
// Sidebar width

namespace {
constexpr float kSideMin = 200, kSideMax = 420, kSideDefault = metrics::sidebarW, kRailW = 72;
// A drag narrower than kCollapseBelow snaps into the rail; from the rail, wider than kExpandFrom expands again (the gap
// keeps it from flickering at the line).
constexpr float kCollapseBelow = 150, kExpandFrom = 160;
constexpr float kPageMin = 400;   // the page keeps at least this much next to an expanded sidebar
} // namespace

// 6 DIPs centered on the sidebar's hairline, above the sidebar and the page. Hover / drag light the hairline in the
// accent; a double click goes back to the default width. No tooltip: anchored to this full-height strip it would show
// far from the pointer. Not a drop target: songs dragged across it just pass (the playlist rows end 8 DIPs before it).
class SidebarEdge : public ui::Widget {
public:
    explicit SidebarEdge(Shell& shell) : shell_(shell) {}

    void paint(Canvas& c) override {
        const float a = dragging_ ? 1.f : hover_.value() * 0.7f;
        if (a <= 0.01f) return;
        const Rect r = rect();
        // The hairline is the sidebar's last DIP (r.cx() - 1): light it and the DIP before it.
        c.fillRect({r.cx() - 2, r.y, 2, r.h}, accent().base.mulAlpha(a));
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::normal));
        invalidate();
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        if (e.clicks == 2) {   // back to the default width (and out of the rail)
            dragging_ = false;
            shell_.resetSidebar();
            return true;
        }
        dragging_ = true;
        startX_ = e.windowPos.x;
        startW_ = shell_.sidebarWidth();
        shell_.beginSidebarDrag();
        invalidate();
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        if (dragging_ && pressed()) shell_.dragSidebar(startW_ + e.windowPos.x - startX_);
    }
    void onMouseUp(const ui::MouseEvent&) override {
        if (!dragging_) return;
        dragging_ = false;
        shell_.saveSidebar();
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_SIZEWE; }

private:
    Shell& shell_;
    ui::Anim hover_;
    bool dragging_ = false;
    float startX_ = 0, startW_ = 0;
};

float Shell::sidebarWidth() const { return sidebar_->rect().w; }

void Shell::setSidebarCollapsed(bool on) {
    if (on == sideCollapsed_) return;
    sideCollapsed_ = on;
    railAnim_.to(on ? 1.f : 0.f, ui::motion::medium, ui::Ease::Standard);
    sidebar_->setCollapsed(on);
    saveSidebar();
    requestLayout();
    invalidate();
}

void Shell::beginSidebarDrag() { sideDragFromW_ = sideW_; }

void Shell::dragSidebar(float width) {
    if (!sideCollapsed_ && width < kCollapseBelow) {
        // On its way down the drag pinned the width at the minimum: expanding again brings back the one it started with.
        sideW_ = sideDragFromW_;
        setSidebarCollapsed(true);
        return;
    }
    if (sideCollapsed_) {
        if (width < kExpandFrom) return;
        setSidebarCollapsed(false);
    }
    // Never wider than the window allows right now (what the user sees is what is kept).
    const float w = std::round(std::clamp(width, kSideMin, std::max(kSideMin, sideMaxW_)));
    if (w == sideW_) return;
    sideW_ = w;
    requestLayout();
    invalidate();
}

void Shell::resetSidebar() {
    sideW_ = kSideDefault;
    setSidebarCollapsed(false);
    saveSidebar();
    requestLayout();
    invalidate();
}

void Shell::saveSidebar() {
    if (sideDevOverride_) return;
    auto& s = Settings::get();
    if (s.sidebarWidth == sideW_ && s.sidebarCollapsed == sideCollapsed_) return;
    s.sidebarWidth = sideW_;
    s.sidebarCollapsed = sideCollapsed_;
    s.markDirty();
}

// ===================================================================================================
// Shell

Shell::Shell() {
    titleBar_ = add<TitleBar>();
    sidebar_ = add<Sidebar>();
    pageHost_ = add<PageHost>();
    edge_ = add<SidebarEdge>(*this);
    queue_ = add<QueuePanel>();
    queue_->setVisible(false);
    friends_ = add<FriendPanel>();
    friends_->setVisible(false);
    jam_ = add<JamPanel>();
    jam_->setVisible(false);
    playerBar_ = add<PlayerBar>();
    // Now Playing covers the player bar too (it has its own controls): above it in paint and hit-test order.
    nowPlayingView_ = add<NowPlayingView>();
    nowPlayingView_->setVisible(false);
    connect_ = add<ConnectScreen>();
    connect_->setVisible(false);

    const auto& s = Settings::get();
    sideW_ = std::clamp(s.sidebarWidth, kSideMin, kSideMax);
    sideCollapsed_ = s.sidebarCollapsed;
    // Dev: SHADETUBE_SIDEBAR_W=<DIPs> | rail for this run (screenshots); nothing is saved.
    wchar_t dev[16] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_SIDEBAR_W", dev, 16) > 0) {
        sideDevOverride_ = true;
        sideCollapsed_ = std::wstring_view(dev) == L"rail";
        if (!sideCollapsed_) sideW_ = std::clamp(static_cast<float>(_wtof(dev)), kSideMin, kSideMax);
    }
    railAnim_.snap(sideCollapsed_ ? 1.f : 0.f);
    sidebar_->setCollapsed(sideCollapsed_);
}

void Shell::setConnect(bool on) {
    if (on == connectMode_) return;
    connectMode_ = on;
    connect_->setVisible(on);
    requestLayout();
    invalidate();
}

void Shell::setNowPlaying(bool on) {
    if (on == nowPlaying_) return;
    nowPlaying_ = on;
    npAnim_.to(on ? 1.f : 0.f, ui::motion::slow, ui::Ease::Decelerate);
    nowPlayingView_->setVisible(true);
    titleBar_->setNowPlayingMode(on);
    playerBar_->setNowPlayingMode(on);
    if (on && !npActive_) {
        npActive_ = true;
        nowPlayingView_->activate();
    }
    requestLayout();
    invalidate();
}

void Shell::setQueueOpen(bool on) {
    queueOpen_ = on;
    if (on) friendsOpen_ = jamOpen_ = false;
    requestLayout();
    invalidate();
}

void Shell::setFriendsOpen(bool on) {
    friendsOpen_ = on;
    if (on) queueOpen_ = jamOpen_ = false;
    requestLayout();
    invalidate();
}

void Shell::setJamOpen(bool on) {
    jamOpen_ = on;
    if (on) queueOpen_ = friendsOpen_ = false;
    requestLayout();
    invalidate();
}

void Shell::paintSidePanel(Canvas& c, ui::Widget* panel, float t, bool leaving) {
    const float a = std::clamp(t, 0.f, 1.f);
    if (a <= 0.001f) return;
    // In: from 16 DIP right of its place; out: drifting 8 DIP right. The value is already eased (Anim).
    const float dx = leaving ? (1 - a) * 8 : (1 - a) * 16;
    c.pushOpacity(a);
    c.pushTransform(D2D1::Matrix3x2F::Translation(dx, 0));
    if (leaving) c.fillSurface(panel->rect());   // the page took its place: the panel covers it while it fades
    panel->paint(c);
    c.popTransform();
    c.popLayer();
}

void Shell::layout() {
    const Rect r = rect();
    const float top = metrics::titleBarH, bottom = r.h - metrics::playerBarH;
    titleBar_->setRect({0, 0, r.w, top});
    if (connectMode_) {
        connect_->setRect({0, top, r.w, r.h - top});
        connect_->setVisible(true);
        for (Widget* w : std::initializer_list<Widget*>{sidebar_, pageHost_, edge_, playerBar_, queue_, friends_, jam_, nowPlayingView_})
            w->setVisible(false);
        return;
    }
    connect_->setVisible(false);
    // Connect mode hid it (logout -> login again); Now Playing covers it once it is fully open.
    playerBar_->setVisible(!nowPlaying_ || npAnim_.running());
    playerBar_->setRect({0, bottom, r.w, metrics::playerBarH});
    // Right side: the queue, the friend activity (Spotify only) or "Birlikte dinle", one at a time.
    const bool jam = jamOpen_ && !nowPlaying_;
    const bool friends = friendsOpen_ && !nowPlaying_ && socialAvailable() && !jam;
    const bool queue = queueOpen_ && !nowPlaying_ && !friends && !jam;
    const float q = queue || friends || jam ? metrics::queueW : 0;
    // Sidebar: the chosen width (no wider than the page allows), or the rail; in between while it collapses / expands.
    sideMaxW_ = std::max(kSideMin, std::min(kSideMax, r.w - q - kPageMin));
    const float rail = railAnim_;
    const float sw = std::round(ui::lerp(std::min(sideW_, sideMaxW_), kRailW, rail));
    sidebar_->setRailProgress(rail);
    sidebar_->setRect({0, top, sw, bottom - top});
    pageHost_->setRect({sw, top, r.w - sw - q, bottom - top});
    edge_->setRect({sw - 3, top, 6, bottom - top});
    queue_->setRect({r.w - metrics::queueW, top, metrics::queueW, bottom - top});
    queue_->setVisible(queue);
    friends_->setRect({r.w - metrics::queueW, top, metrics::queueW, bottom - top});
    if (friends && !friends_->visible()) {   // just opened / back from Now Playing: catch up once it is laid out
        Dispatcher::post([this, ref = life_.ref()] {
            if (!ref.expired() && friends_->visible()) friends_->refreshIfStale();
        });
    }
    friends_->setVisible(friends);
    jam_->setRect({r.w - metrics::queueW, top, metrics::queueW, bottom - top});
    jam_->setVisible(jam);
    // Open / close motion of the side panels (see Shell.h). Not when Now Playing takes the window (its cross-fade covers
    // them) or comes back from it.
    if (ui::Widget* shown = jam ? static_cast<ui::Widget*>(jam_) : friends ? static_cast<ui::Widget*>(friends_) : queue ? static_cast<ui::Widget*>(queue_) : nullptr;
        shown != sideShown_) {
        const bool np = nowPlaying_ || npAnim_.running();
        if (shown) {
            sideLeaving_ = nullptr;   // a panel replacing another one covers it
            sideIn_.snap(np ? 1.f : 0.f);
            sideIn_.to(1, ui::overlay::duration(ui::motion::medium), ui::Ease::Decelerate);
        } else if (sideShown_ && !np && ui::overlay::duration(140) > 0) {
            sideLeaving_ = sideShown_;
            sideOut_.snap(1);
            sideOut_.to(0, ui::overlay::duration(140), ui::Ease::Accelerate);
        }
        sideShown_ = shown;
        invalidate();
    }
    nowPlayingView_->setTopInset(top);   // its backdrop runs behind the title bar (painted over it, see paint())
    nowPlayingView_->setRect({0, 0, r.w, r.h});
    nowPlayingView_->setVisible(nowPlaying_ || npAnim_.running());
    sidebar_->setVisible(!nowPlaying_);
    pageHost_->setVisible(!nowPlaying_);
    edge_->setVisible(!nowPlaying_);
}

void Shell::paint(Canvas& c) {
    if (connectMode_) {
        c.fillSurface(rect());
        connect_->paint(c);
        titleBar_->paint(c);
        return;
    }
    if (railAnim_.running()) requestLayout();   // the sidebar collapsing / expanding: lay out the next frame too
    // Now Playing cross-fade: the regular layout fades out underneath while it opens.
    const float t = npAnim_;
    if (npAnim_.running()) {
        requestLayout();
        sidebar_->setVisible(true);
        pageHost_->setVisible(true);
    }
    // Now Playing spans the window: the title bar goes over it (after it), not under it.
    const bool npShown = nowPlayingView_->visible() && t > 0.001f;
    if (sideLeaving_ && (!sideOut_.running() || sideLeaving_->visible())) sideLeaving_ = nullptr;
    for (auto& child : children()) {
        if (!child->visible()) {
            if (child.get() == sideLeaving_) paintSidePanel(c, sideLeaving_, sideOut_, true);   // closed, fading out
            continue;
        }
        if (child.get() == titleBar_ && npShown) continue;
        if (child.get() == sideShown_ && sideIn_.running() && t <= 0.999f) {
            paintSidePanel(c, sideShown_, sideIn_, false);
            continue;
        }
        if (child.get() == nowPlayingView_) {
            if (t <= 0.001f) continue;
            // Group opacity only while it fades: a layer is a window-sized intermediate texture every frame.
            if (t >= 0.999f) {
                child->paint(c);
            } else {
                c.pushOpacity(t);
                child->paint(c);
                c.popLayer();
            }
            titleBar_->paint(c);
            continue;
        }
        if ((child.get() == sidebar_ || child.get() == pageHost_ || child.get() == edge_ || child.get() == queue_ ||
             child.get() == friends_ || child.get() == jam_) &&
            t > 0.999f)
            continue;
        child->paint(c);
    }
    // Closed and faded out: release the view's bitmaps, lyrics and layouts. Not keyed on visible(): layout() already
    // hid the view earlier in this frame. Paint returns early in connect mode, so an interrupted fade-out is cleaned
    // up on the first normal frame after it.
    if (npActive_ && !nowPlaying_ && !npAnim_.running()) {
        npActive_ = false;
        nowPlayingView_->deactivate();
    }
}

} // namespace st::app
