#include "app/Shell.h"

#include "app/Blacklist.h"
#include "app/Commands.h"
#include "app/ConnectScreen.h"
#include "app/DownloadSync.h"
#include "app/DroppedFiles.h"
#include "app/InternetRadio.h"
#include "app/LyricsFullscreen.h"
#include "app/NowPlaying.h"
#include "app/PlaylistTransfer.h"
#include "app/PodcastUi.h"
#include "app/SmartShuffle.h"
#include "app/Social.h"
#include "app/Source.h"
#include "app/Together.h"
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
    back_->setTooltip(tr(L"Geri"));
    forward_->setTooltip(tr(L"İleri"));
    back_->onClick = [] { ctx().router->back(); };
    forward_->onClick = [] { ctx().router->forward(); };
    collapse_ = add<Button>(ButtonKind::Ghost, tr(L"Küçült"), "chevron-up");
    collapse_->setVisible(false);
    collapse_->onClick = [] { ctx().toggleNowPlaying(false); };
    friends_ = add<Button>(ButtonKind::Icon, L"", "users");
    friends_->setIconSize(16);
    friends_->setTooltip(tr(L"Arkadaş etkinliği"));
    friends_->onClick = [] { commands::run("friend-activity"); };
    friends_->setVisible(false);

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
        const std::wstring label = source::sourceLabel();
        c.text(label, type::monoLabel, {r.x + 196, r.y, 400, r.h}, col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    } else {
        std::wstring label = tr(L"ŞİMDİ ÇALIYOR");
        if (auto* p = ctx().player; p && !p->context().name.empty()) label += L" · " + toUpperTr(p->context().name);
        c.text(label, type::monoLabel, {r.x + 108, r.y, 500, r.h}, col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
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
        activeA_.to(on ? 1.f : 0.f, ui::motion::normal);
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
        } else if (!icon_.empty()) {   // the playing indicator
            c.icon(icon_, {x, r.cy() - 7, 14, 14}, acc.base);
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
        if (liked_ && images_.empty()) {
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
            c.icon(icon_, {corner.x - 5, corner.y - 5, 10, 10}, acc.onAccent);
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
            const auto click = onClick;   // may rebuild the list: the row is deleted after the event
            click();
        }
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::fast);
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

    // Keyboard: Enter / Space = click; a folder opens with Right and closes with Left; the menu key / Shift+F10 opens
    // the context menu under the row.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        const auto click = onClick;
        click();
        return true;
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

Sidebar::Sidebar() {
    // Page names ("Ara" = the Search page); in the icon rail they are the tooltips.
    auto nav = [this](std::wstring name, const char* icon) {
        auto* b = add<Button>(ButtonKind::Nav, name, icon);
        navNames_.emplace_back(b, std::move(name));
        return b;
    };
    home_ = nav(tr(L"Ana Sayfa"), "home");
    search_ = nav(tr(L"Ara"), "search");
    library_ = nav(tr(L"Kitaplık"), "library");
    downloads_ = nav(tr(L"İndirilenler"), "download");
    local_ = nav(tr(L"Yerel dosyalar"), "music-note");
    radio_ = nav(tr(L"Radyo"), "radio");
    podcasts_ = nav(tr(L"Podcastler"), "podcast");
    releases_ = nav(tr(L"Yeni çıkanlar"), "sparkle");
    releases_->setVisible(false);
    stats_ = nav(tr(L"İstatistikler"), "stats");
    settings_ = nav(tr(L"Ayarlar"), "settings");
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
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6},
                       {// Logged in this creates the playlist on Spotify (then opens it); logged out, a local playlist.
                        {tr(L"Yeni çalma listesi"), "plus", L"",
                         [] { promptNewPlaylist([](const std::string& id) { ctx().router->navigate({RouteKind::Playlist, id}); }); }},
                        ui::MenuItem::sep(),
                        {tr(L"Dosyadan içe aktar…"), "folder", L"", [] { transfer::importFromFile(); }},
                        {tr(L"YouTube listesinden içe aktar…"), "youtube-source", L"", [] { transfer::importFromLink(); }}});
    };
    list_ = add<ui::ScrollView>();
    listCol_ = list_->setContent<ui::Column>(0.f);
}

void Sidebar::refresh() {
    if (releases_->visible() != socialAvailable()) {   // Spotify only: logging in / out shows / hides it
        releases_->setVisible(socialAvailable());
        requestLayout();
    }
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
            const sync::Target offline{sync::Kind::Playlist, id, p.name, p.images};
            b->onContext = [id, offline](const ui::MouseEvent& e) {
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
                                {tr(L"Sil"), "trash", L"", [id] {
                                     ui::Dialog::confirm(ctx().window, tr(L"Çalma listesi silinsin mi?"),
                                                         tr(L"Bu işlem geri alınamaz."), tr(L"Sil"),
                                                         [id] { ctx().library.deletePlaylist(id); }, true);
                                 }}});
            };
        }
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
    for (auto& [b, route] : items_) b->setActive(route == cur);
    // Show an equalizer next to the playlist that is currently playing.
    const auto* p = ctx().player;
    for (auto& [b, route] : items_) {
        const bool playing = p && p->isPlaying() &&
                             ((route.kind == RouteKind::Playlist && p->context().uri == "playlist:" + route.id) ||
                              (route.kind == RouteKind::Liked && p->context().uri == "liked"));
        b->setIcon(playing ? "equalizer" : "");
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
    // Expanded: 16 DIP margins; the rail (72 DIPs): 48 DIP wide icon pills.
    const float x = std::round(ui::lerp(16.f, 12.f, t)), w = std::max(0.f, r.w - 2 * x);
    float y = 24;
    for (auto* b : {home_, search_, library_, downloads_, local_, radio_, podcasts_, releases_, stats_}) {
        if (!b->visible()) continue;
        b->setRect({x, y, w, metrics::navItemH});
        y += metrics::navItemH + 2;
    }
    listLabelY_ = y + 28 - 14;
    y += 28 + 14 + 8;   // section label
    const float bottom = r.h - 16 - metrics::navItemH - 8;
    // The list ends 8 DIPs before the edge (its scrollbar); in the rail its tiles center on the rail.
    const float lx = std::round(ui::lerp(16.f, 8.f, t));
    list_->setRect({lx, y, std::max(0.f, r.w - 8 - lx), std::max(0.f, bottom - y)});
    // "+": at the end of the section label; centered in the rail.
    const float plusX = std::round(ui::lerp(r.w - 16 - 28, r.w * 0.5f - 12, t));
    newPlaylist_->setRect({plusX, y - 14 - 8 - 24 + 6, 24, 24});
    settings_->setRect({x, r.h - 16 - metrics::navItemH, w, metrics::navItemH});
}

void Sidebar::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.vline(r.right() - 1, r.y, r.bottom(), col.hairSubtle);
    const float show = labelOpacity();
    if (show > 0.01f) {
        const float labelW = newPlaylist_->rect().x - 28 - 8;   // up to the "+"
        c.text(toUpperTr(tr(L"Çalma listeleri")), type::monoLabel, {r.x + 28, r.y + listLabelY_, std::max(0.f, labelW), 14},
               col.fgTertiary.mulAlpha(show));
    }
    paintChildren(c);
    // A count pill on a nav entry: new episodes of the podcast subscriptions, unseen new releases. In the rail: a dot on
    // the icon's corner (the tooltip names the page).
    auto badge = [&](const Button* b, int n) {
        if (n <= 0 || !b->visible()) return;
        const Rect br = b->rect();
        if (show < 0.5f) {
            const gfx::Point p{r.x + br.cx() + metrics::navIcon * 0.5f - 1, r.y + br.cy() - metrics::navIcon * 0.5f + 1};
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
        const Rect pill{r.x + br.right() - 10 - w, r.y + br.cy() - 9, w, 18};
        c.fillPill(pill, accent().base);
        c.text(label, type::monoBadge, pill, accent().onAccent, gfx::TextAlign::Center, gfx::VAlign::Center);
    };
    badge(podcasts_, podcastNewEpisodeCount());
    badge(releases_, newReleasesUnseenCount());
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
    shuffle_ = add<Button>(ButtonKind::Icon, L"", "shuffle-thin");
    prev_ = add<Button>(ButtonKind::Icon, L"", "prev-thin");
    play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
    next_ = add<Button>(ButtonKind::Icon, L"", "next-thin");
    repeat_ = add<Button>(ButtonKind::Icon, L"", "repeat-thin");
    heart_ = add<Button>(ButtonKind::Icon, L"", "heart-thin");
    lyrics_ = add<Button>(ButtonKind::Icon, L"", "lyrics-thin");
    queue_ = add<Button>(ButtonKind::Icon, L"", "queue-thin");
    knob_ = add<ui::Knob>();
    mini_ = add<Button>(ButtonKind::Icon, L"", "mini-player-thin");
    expand_ = add<Button>(ButtonKind::Icon, L"", "expand-thin");
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
    jam_ = add<Button>(ButtonKind::Icon, L"", "users-thin");
    jam_->setTooltip(tr(L"Birlikte dinle"));
    jam_->onClick = [] { commands::run("listen-together"); };
    jamLabel_ = add<Button>(ButtonKind::Ghost, L"", "users-thin");
    jamLabel_->setTooltip(tr(L"Birlikte dinle"));
    jamLabel_->onClick = [] { commands::run("listen-together"); };
    jamLabel_->setVisible(false);
    sleep_ = add<Button>(ButtonKind::Icon, L"", "clock-thin");
    sleep_->setTooltip(tr(L"Uyku zamanlayıcı"));
    sleep_->onClick = [this] {
        const Rect br = sleep_->toWindow(sleep_->rect());
        showSleepTimerMenu({br.x, br.y - 6});   // Menu flips upward/leftward to stay on screen
    };
    for (auto* b : {shuffle_, prev_, next_, repeat_}) b->setIconSize(18);
    shuffle_->setTooltip(tr(L"Karıştır"));
    prev_->setTooltip(tr(L"Önceki"));
    next_->setTooltip(tr(L"Sonraki"));
    repeat_->setTooltip(tr(L"Tekrarla"));
    lyrics_->setTooltip(tr(L"Şarkı sözleri"));
    queue_->setTooltip(tr(L"Sıradaki"));
    mini_->setTooltip(tr(L"Mini oynatıcı"));
    expand_->setTooltip(tr(L"Tam ekran"));
    heart_->setTooltip(tr(L"Beğen"));

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
    sync();
}

void PlayerBar::setNowPlayingMode(bool on) {
    nowPlaying_ = on;
    heart_->setVisible(!on);
    expand_->setIcon(on ? "collapse-thin" : "expand-thin");
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
    play_->setPlaying(p && p->isPlaying());
    play_->setLoading(p && p->status() == player::Status::Resolving);
    // The wave flows only while sound plays: paused, stopped, resolving or buffering = a straight line.
    seek_->setFlowing(p && p->status() == player::Status::Playing);
    seek_->setLive(live_);
    const auto shuffleMode = smartshuffle::mode();
    shuffle_->setActive(shuffleMode != smartshuffle::Mode::Off);
    shuffle_->setIcon(shuffleMode == smartshuffle::Mode::Smart ? "shuffle-smart-thin" : "shuffle-thin");
    shuffle_->setTooltip(smartshuffle::modeLabel(shuffleMode));
    const auto rep = p ? p->repeat() : RepeatMode::Off;
    repeat_->setActive(rep != RepeatMode::Off);
    repeat_->setIcon(rep == RepeatMode::One ? "repeat-one-thin" : "repeat-thin");
    const bool liked = t && (live_ ? radio::store().isFavorite(radio::uuidOf(t->id)) : ctx().library.isLiked(t->id));
    heart_->setIcon(liked ? "heart-liked-thin" : "heart-thin");
    heart_->setActive(liked);
    heart_->setTooltip(!live_ ? std::wstring(tr(L"Beğen")) : liked ? std::wstring(tr(L"Favorilerden kaldır")) : std::wstring(tr(L"Favorilere ekle")));
    // Live: no seeking (the wave runs end to end without a playhead).
    seek_->setEnabled(!live_);
    queue_->setActive(false);
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
    place(sleep_, 32, 10);
    // Center column: the transport row over the seek row (elapsed · wave · total), centered on the bar; 40 % of the
    // width (380..640 DIP) as far as the left info (at least ~140 DIP of text) and the right group (with the jam icon
    // that always follows) leave room.
    const float cx = std::round(r.w * 0.5f);
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
    c.shadow(artRect_, 2, 40, 0, accent().glow);   // playerThumbGlow
    if (s.live) drawStationArt(c, s.images, artRect_, 2);
    else if (outgoing) drawArtwork(c, s.images, artRect_, 2);
    else art_.draw(c, s.images, artRect_, 2);   // a cover still loading fades in over the placeholder
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
    c.fillRect(r, col.bgBase);
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
        c.fillRounded(tip, 2, col.bgElevated);
        c.strokeRounded(tip, 2, col.hairStrong);
        c.text(s, type::monoMeta, tip, col.fgPrimary, gfx::TextAlign::Center, gfx::VAlign::Center);
    }
    // Keep repainting while playing: the wave flows at ~30 fps while it is on screen (not under the full-screen lyrics,
    // not with reduce motion); otherwise only the time label ticks, 4 times a second (Now Playing's spectrum strip:
    // 33 ms). Paused: no wake-ups at all.
    if (p && p->isPlaying()) {
        const bool flowing = seek_->waveMoving() && !LyricsFullscreen::isOpen();
        if (auto* w = window()) w->invalidateAfter(flowing ? ui::kWaveFrameMs : nowPlaying_ ? 33 : 250);
    }
}

// ===================================================================================================
// QueuePanel

namespace {
// Upcoming rows: 52 DIP, 40 with "Liste yoğunluğu: Sıkı" (Settings::compactLists, app/Display); the playing row keeps 56.
float queueRowH() { return Settings::get().compactLists ? 40.f : 52.f; }
} // namespace

QueuePanel::QueuePanel()
    : title_(tr(L"Sıradaki"), type::title), meta_({}, type::monoLabel), blockedBadge_(toUpperTr(tr(L"Engelli")), type::monoBadge) {
    clipsChildren = true;
    gfx::TextOptions right;
    right.align = gfx::TextAlign::Trailing;
    meta_.setOptions(right);
    save_ = add<Button>(ButtonKind::Icon, L"", "playlist");
    save_->setTooltip(tr(L"Kuyruğu kaydet"));
    save_->onClick = [this] { saveAsPlaylist(); };
    save_->setVisible(false);
}

void QueuePanel::saveAsPlaylist() {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return;
    std::vector<catalog::Track> tracks;
    for (int i = p->currentOrderIndex(); i < static_cast<int>(p->order().size()); ++i)
        if (const auto& t = p->items()[p->order()[i]]; !radio::isStationId(t.id) && !catalog::isPodcastId(t.id))
            tracks.push_back(t);   // songs only
    if (tracks.empty()) return;
    // Name dialog -> a Spotify playlist with the Spotify tracks (logged in) or a local playlist; it reports with a toast.
    promptNewPlaylist({}, std::move(tracks));
}

void QueuePanel::sync() {
    auto* p = ctx().player;
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
    save_->setVisible(p && p->currentOrderIndex() >= 0 && !p->order().empty() && !stations);
    invalidate();
}

void QueuePanel::layout() { save_->setRect({rect().w - 20 - 32, 20 + 2, 32, 32}); }

float QueuePanel::listTop() const { return 20 + 36 + 8 + 20 + 56 + 24 + 20; }

Rect QueuePanel::rowRect(int orderIndex) const {
    auto* p = ctx().player;
    const int cur = p ? p->currentOrderIndex() : 0;
    if (orderIndex == cur) return {0, 20 + 36 + 8 + 20, rect().w, 56};
    const int k = orderIndex - cur - 1;
    return {0, listTop() + k * queueRowH() - scroll_.value(), rect().w, queueRowH()};
}

int QueuePanel::rowAt(gfx::Point pt) const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return -1;
    const gfx::Point lp{pt.x - rect().x, pt.y - rect().y};
    if (rowRect(p->currentOrderIndex()).contains(lp)) return p->currentOrderIndex();
    if (lp.y < listTop()) return -1;
    const int k = static_cast<int>((lp.y - listTop() + scroll_.value()) / queueRowH());
    const int idx = p->currentOrderIndex() + 1 + k;
    return idx < static_cast<int>(p->order().size()) ? idx : -1;
}

float QueuePanel::maxScroll() const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return 0;
    const int upcoming = static_cast<int>(p->order().size()) - p->currentOrderIndex() - 1;
    return std::max(0.f, upcoming * queueRowH() - (rect().h - listTop()) + 16);
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
        const auto track = p->items()[p->order()[i]];
        const int idx = i;
        std::vector<ui::MenuItem> items{{tr(L"Şimdi çal"), "play", L"", [idx] { ctx().player->jumpTo(idx); }},
                                        {tr(L"Sıradan kaldır"), "minus", L"", [idx] { ctx().player->removeAt(idx); }}};
        if (track.recommended) {   // smart shuffle / "Geliştir": into the list it was recommended for, or stop them
            items.push_back(ui::MenuItem::sep());
            if (smartshuffle::canAdd({}, track))
                items.push_back({tr(L"Bu öneriyi listeye ekle"), "plus", L"", [track] { smartshuffle::add({}, track); }});
            items.push_back({tr(L"Önerileri gösterme"), "close", L"", [] { smartshuffle::stop({}); }});
        }
        items.push_back(ui::MenuItem::sep());
        items.push_back({tr(L"Sırayı dışa aktar…"), "share", L"", [] { transfer::exportList(transfer::What::Queue, {}, {}); }});
        items.push_back({tr(L"Sırayı temizle"), "trash", L"", [] { ctx().player->clearQueue(); }});
        ui::Menu::open(ctx().window, e.windowPos, std::move(items));
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    dragFrom_ = i;
    dragTo_ = i;
    dragStartY_ = e.pos.y;
    dragging_ = false;
    return true;
}

void QueuePanel::onMouseMove(const ui::MouseEvent& e) {
    if (pressed() && dragFrom_ >= 0 && dragFrom_ != ctx().player->currentOrderIndex()) {
        if (std::abs(e.pos.y - dragStartY_) > 6) dragging_ = true;
        if (dragging_) {
            const int i = rowAt(e.pos);
            if (i > ctx().player->currentOrderIndex()) dragTo_ = i;
            invalidate();
        }
        return;
    }
    const int i = rowAt(e.pos);
    if (i != hover_) {
        hover_ = i;
        invalidate();
    }
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

int QueuePanel::dropIndexAt(gfx::Point windowPos) const {
    auto* p = ctx().player;
    if (!p || p->currentOrderIndex() < 0) return 0;
    const gfx::Point pp = fromWindow(windowPos);
    return dropped::queueDropIndex(pp.y - rect().y - listTop() + scroll_.value(), queueRowH(), p->currentOrderIndex(),
                                   static_cast<int>(p->order().size()));
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

void QueuePanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    auto* p = ctx().player;
    scroll_.setBounds(0, maxScroll());   // the queue changed / the panel resized: the offset follows the new end
    c.fillRect(r, col.bgBase.withAlpha(0.35f));
    c.vline(r.x, r.y, r.bottom(), col.hairSubtle);
    c.pushClip(r);
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    const float padX = 20;
    const float tw = std::ceil(title_.measure().w);
    c.text(title_, {padX, 20, tw + 1, 36}, col.fgPrimary, gfx::VAlign::Center);
    // meta right-aligned, left of the "Kuyruğu kaydet" button when it is shown
    const float metaRight = save_->visible() ? save_->rect().x - 8 : r.w - padX;
    c.text(meta_, {padX, 20, metaRight - padX, 36}, col.fgTertiary, gfx::VAlign::Center);
    if (!p || p->currentOrderIndex() < 0) {
        c.text(tr(L"Sıra boş. Bir şarkı çal, burada görünsün."), type::secondary, {padX, 80, r.w - padX * 2, 20},
               col.fgTertiary);
        if (dropAt_ >= 0) c.strokeRounded({8, 8, r.w - 16, r.h - 16}, 2, acc.base, 2);   // songs dragged in play here
        c.popTransform();
        c.popClip();
        paintChildren(c);
        return;
    }
    c.text(tr(L"ŞİMDİ"), type::monoLabel, {padX, 20 + 36 + 8 - 2, 100, 16}, acc.base);
    auto drawRow = [&](int oi, const Rect& rr, bool current) {
        const auto& t = p->items()[p->order()[oi]];
        if (current) c.fillRect(rr, acc.tint06);
        else if (oi == hover_) c.fillRect(rr, col.overlayHover);
        float x = padX;
        if (!current) {
            c.icon("drag-handle", {x - 12, rr.cy() - 6, 12, 12}, col.fgTertiary.mulAlpha(oi == hover_ ? 1.f : 0.5f));
            x += 4;
        }
        // Kara liste: an upcoming blocked track is skipped by the player, so it is dimmed like in the track tables;
        // the badge also marks a blocked track that plays because the user picked it.
        const bool blocked = blacklist::isBlocked(t);
        const float alpha = blocked && !current ? 0.38f : 1.f;
        const float art = current ? 40.f : Settings::get().compactLists ? 28.f : 36.f;
        const Rect artR{x, rr.cy() - art * 0.5f, art, art};
        const bool station = radio::isStationId(t.id);
        if (station) drawStationArt(c, t.album.images, artR, 2, current ? Priority::High : Priority::Normal);
        else drawArtwork(c, t.album.images, artR, 2, Placeholder::Album, false, current ? Priority::High : Priority::Normal);
        if (alpha < 1) c.fillRect(artR, col.bgBase.withAlpha(0.6f));
        x += art + 12;
        float rightW = current ? 24.f : 52.f;
        if (t.recommended) {   // smart shuffle / "Geliştir"
            const float bw = smartshuffle::badgeWidth();
            smartshuffle::drawBadge(c, r.w - padX - rightW - 8 - bw, rr.cy());
            rightW += bw + 8;
        }
        if (const std::wstring by = together::addedBy(t); !by.empty()) {   // a listen-together guest queued it
            const float bw = together::addedBadgeWidth(by);
            together::drawAddedBadge(c, r.w - padX - rightW - 8 - bw, rr.cy(), by);
            rightW += bw + 8;
        }
        if (blocked) {
            const float bw = std::ceil(blockedBadge_.measure().w) + 10;
            const Rect badge{r.w - padX - rightW - 8 - bw, rr.cy() - 7, bw, 14};
            c.fillRounded(badge, 2, col.fgPrimary.withAlpha(0.14f));
            c.text(blockedBadge_, badge.inset(5, 0), col.fgSecondary, gfx::VAlign::Center);
            rightW += bw + 8;
        }
        c.text(toWide(t.name), type::body.withSize(13), {x, rr.cy() - 17, r.w - x - rightW - padX, 18}, col.fgPrimary.mulAlpha(alpha));
        // The playing station: the song it announces (ICY) when it sends one.
        const std::wstring second = current && station && !p->liveTitle().empty() ? p->liveTitle() : toWide(t.artistLine());
        c.text(second, type::caption.withSize(11), {x, rr.cy() + 1, r.w - x - rightW - padX, 16}, col.fgSecondary.mulAlpha(alpha));
        if (current) {
            const int f = p->isPlaying() ? static_cast<int>(ui::frame::now() / 66.0) % 12 : 0;
            char name[48];
            snprintf(name, sizeof name, "animations/equalizer/frame-%02d", f);
            c.icon(name, {r.w - padX - 16, rr.cy() - 8, 16, 16}, acc.base);
            if (p->isPlaying()) ui::frame::requestNext();
        } else if (!station) {   // stations have no duration
            c.text(ui::formatDuration(t.durationMs), type::monoMeta, {r.w - padX - 52, rr.y, 52, rr.h}, col.fgTertiary.mulAlpha(alpha),
                   gfx::TextAlign::Trailing, gfx::VAlign::Center);
        }
    };
    drawRow(p->currentOrderIndex(), rowRect(p->currentOrderIndex()), true);
    std::wstring nextLabel = tr(L"SONRAKİ");
    if (!p->context().name.empty()) nextLabel += L" · " + toUpperTr(p->context().name);
    c.text(nextLabel, type::monoLabel, {padX, listTop() - 24, r.w - padX * 2, 16}, col.fgTertiary);
    c.pushClip({0, listTop() - 4, r.w, r.h - listTop() + 4});
    const int n = static_cast<int>(p->order().size());
    const int firstVisible = p->currentOrderIndex() + 1 + std::max(0, static_cast<int>(scroll_.value() / queueRowH()));
    for (int i = firstVisible; i < n; ++i) {
        const Rect rr = rowRect(i);
        if (rr.y > r.h) break;
        if (dragging_ && i == dragFrom_) {
            c.fillRect(rr, col.overlayPressed);
            continue;
        }
        drawRow(i, rr, false);
    }
    if (dragging_ && dragTo_ >= 0) {
        const Rect tr = rowRect(dragTo_);
        c.fillRect({padX, dragTo_ > dragFrom_ ? tr.bottom() - 1 : tr.y - 1, r.w - padX * 2, 2}, acc.base);
    }
    if (dropAt_ >= 0) {   // songs dragged in from a list / files from Explorer go in front of this slot
        const float y = listTop() + static_cast<float>(dropAt_ - p->currentOrderIndex() - 1) * queueRowH() - scroll_.value();
        c.fillRect({padX, y - 1, r.w - padX * 2, 2}, acc.base);
    }
    c.popClip();
    c.popTransform();
    c.popClip();
    paintChildren(c);
}

// ===================================================================================================
// PageHost

void PageHost::show(std::unique_ptr<Page> page) {
    clearChildren();
    page_ = static_cast<Page*>(adopt(std::move(page)));
    enter_.snap(0);
    enter_.to(1, ui::motion::page, ui::Ease::Decelerate);
    requestLayout();
    invalidate();
}

void PageHost::layout() {
    if (page_) page_->setRect({0, 0, rect().w, rect().h});
}

void PageHost::paint(Canvas& c) {
    if (!page_) return;
    const float t = enter_;
    if (t >= 0.999f) {
        paintChildren(c);
        return;
    }
    c.pushClip(rect());
    c.pushOpacity(t);
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, 16 * (1 - t)));
    paintChildren(c);
    c.popTransform();
    c.popLayer();
    c.popClip();
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
        hover_.to(1, ui::motion::fast);
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::normal);
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
    nowPlayingView_ = add<NowPlayingView>();
    nowPlayingView_->setVisible(false);
    queue_ = add<QueuePanel>();
    queue_->setVisible(false);
    friends_ = add<FriendPanel>();
    friends_->setVisible(false);
    jam_ = add<JamPanel>();
    jam_->setVisible(false);
    playerBar_ = add<PlayerBar>();
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
    queueAnim_.to(on ? 1.f : 0.f, ui::motion::medium, ui::Ease::Decelerate);
    queue_->setVisible(true);
    requestLayout();
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
    playerBar_->setVisible(true);   // connect mode hid it (logout -> login again)
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
    nowPlayingView_->setRect({0, top, r.w, bottom - top});
    nowPlayingView_->setVisible(nowPlaying_ || npAnim_.running());
    sidebar_->setVisible(!nowPlaying_);
    pageHost_->setVisible(!nowPlaying_);
    edge_->setVisible(!nowPlaying_);
}

void Shell::paint(Canvas& c) {
    if (connectMode_) {
        c.fillRect(rect(), colors().bgBase);
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
    for (auto& child : children()) {
        if (!child->visible()) continue;
        if (child.get() == nowPlayingView_) {
            if (t <= 0.001f) continue;
            // Group opacity only while it fades: a layer is a window-sized intermediate texture every frame.
            if (t >= 0.999f) {
                child->paint(c);
                continue;
            }
            c.pushOpacity(t);
            child->paint(c);
            c.popLayer();
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
