// Komut paleti (Ctrl+K). See CommandPalette.h.
#include "app/CommandPalette.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Router.h"
#include "app/Shortcuts.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "spotify/SpotifyApi.h"
#include "ui/Anim.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace st::app {

namespace {

namespace type = gfx::type;
using gfx::accent;
using gfx::Canvas;
using gfx::colors;
using gfx::Point;
using gfx::Rect;
using json = nlohmann::json;

constexpr float kPanelW = 640, kPad = 12, kRowH = 48, kHeaderH = 30, kFooterH = 34;
constexpr size_t kMaxRecent = 8;

enum class Group { Recent, Commands, Library, Songs, Albums, Artists };

const wchar_t* groupLabel(Group g) {
    switch (g) {
    case Group::Recent: return tr(L"SON KULLANILANLAR");
    case Group::Commands: return tr(L"KOMUTLAR");
    case Group::Library: return tr(L"KİTAPLIĞIN");
    case Group::Songs: return tr(L"ŞARKILAR");
    case Group::Albums: return tr(L"ALBÜMLER");
    case Group::Artists: return tr(L"SANATÇILAR");
    }
    return L"";
}

struct Item {
    Group group = Group::Commands;
    std::string key;              // identity in the recent list: "action:<id>", "settings:<id>", "playlist:<id>", ...
    std::wstring title, subtitle;
    std::wstring foldedTitle, foldedAll;
    std::string icon;             // drawn when there is no art
    std::string imageUrl;
    bool round = false;           // artist pictures
    std::wstring hint;            // right side: the action's key combo
    std::function<void(bool alt)> run;
    json recent;                  // enough to rebuild the item from palette-recent.json
    int score = 0;
};

// ---- Recent picks (palette-recent.json) ----

std::filesystem::path recentFile() { return paths::appData() / L"palette-recent.json"; }

json loadRecent() {
    std::ifstream f(recentFile());
    if (!f) return json::array();
    json j = json::parse(f, nullptr, false);
    return j.is_array() ? j : json::array();
}

void saveRecent(const json& list) {
    const auto path = recentFile();
    const auto tmp = path.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << list.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

void remember(const Item& it) {
    if (it.recent.is_null()) return;
    json list = loadRecent();
    json out = json::array();
    out.push_back(it.recent);
    for (const auto& e : list)
        if (e.is_object() && e.value("key", "") != it.key && out.size() < kMaxRecent) out.push_back(e);
    saveRecent(out);
}

// ---- Items ----

Item makeItem(Group g, std::string key, std::wstring title, std::wstring subtitle) {
    Item it;
    it.group = g;
    it.key = std::move(key);
    it.title = std::move(title);
    it.subtitle = std::move(subtitle);
    it.foldedTitle = shortcuts::fold(it.title);
    it.foldedAll = shortcuts::fold(it.title + L" " + it.subtitle);
    it.recent = json{{"key", it.key}};
    return it;
}

std::string imageOf(const std::vector<catalog::Image>& images) {
    const auto* img = catalog::pickImage(images, 64);
    return img ? img->url : std::string{};
}

Item trackItem(const catalog::Track& t, Group g) {
    Item it = makeItem(g, "track:" + t.id, toWide(t.name), i18n::format(tr(L"Şarkı · {}"), {toWide(t.artistLine())}));
    it.imageUrl = imageOf(t.album.images);
    it.icon = "music-note";
    it.run = [t](bool alt) {
        auto* p = ctx().player;
        if (!p) return;
        if (alt) {
            p->enqueue({t});
            toast(tr(L"Sıraya eklendi"));
        } else {
            p->playContext({t}, 0, {"search", tr(L"Arama sonuçları")});
        }
    };
    it.recent = json{{"key", it.key}, {"kind", "track"}, {"track", toJson(t)}};
    return it;
}

std::wstring artistsOf(const std::vector<catalog::ArtistRef>& artists) {
    std::wstring s;
    for (const auto& a : artists) {
        if (!s.empty()) s += L", ";
        s += toWide(a.name);
    }
    return s;
}

Item albumItem(const std::string& id, const std::wstring& name, const std::wstring& artists, std::string image, Group g) {
    Item it = makeItem(g, "album:" + id, name,
                       artists.empty() ? std::wstring(tr(L"Albüm")) : i18n::format(tr(L"Albüm · {}"), {artists}));
    it.imageUrl = std::move(image);
    it.icon = "album";
    it.run = [id](bool alt) { ctx().router->navigate({RouteKind::Album, alt ? id + "#play" : id}); };
    it.recent = json{{"key", it.key}, {"kind", "album"}, {"id", id}, {"t", toUtf8(name)}, {"s", toUtf8(artists)}, {"img", it.imageUrl}};
    return it;
}

Item artistItem(const std::string& id, const std::wstring& name, std::string image, Group g) {
    Item it = makeItem(g, "artist:" + id, name, tr(L"Sanatçı"));
    it.imageUrl = std::move(image);
    it.icon = "artist";
    it.round = true;
    it.run = [id](bool) { ctx().router->navigate({RouteKind::Artist, id}); };
    it.recent = json{{"key", it.key}, {"kind", "artist"}, {"id", id}, {"t", toUtf8(name)}, {"img", it.imageUrl}};
    return it;
}

Item playlistItem(const catalog::Playlist& p, bool liked) {
    const std::wstring name = liked ? std::wstring(tr(L"Beğenilen Şarkılar")) : toWide(p.name);
    const std::wstring sub = p.countKnown ? i18n::format(tr(L"Çalma listesi · {}"), {i18n::plural(L"{} şarkı", p.totalTracks)})
                                          : std::wstring(tr(L"Çalma listesi"));
    Item it = makeItem(Group::Library, liked ? std::string("liked") : "playlist:" + p.id, name, sub);
    it.imageUrl = liked ? std::string{} : imageOf(p.images);
    it.icon = liked ? "heart" : "playlist";
    const Route route = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
    it.run = [route](bool alt) {
        Route r = route;
        if (alt) r.id += "#play";
        ctx().router->navigate(r);
    };
    it.recent = liked ? json{{"key", it.key}}
                      : json{{"key", it.key}, {"kind", "playlist"}, {"id", p.id}, {"t", p.name}, {"img", it.imageUrl}};
    return it;
}

Item commandItem(std::string key, std::wstring title, std::wstring subtitle, std::string icon, std::function<void()> fn) {
    Item it = makeItem(Group::Commands, std::move(key), std::move(title), std::move(subtitle));
    it.icon = std::move(icon);
    it.run = [fn = std::move(fn)](bool) { fn(); };
    return it;
}

// Everything that doesn't need the network: commands, pages, settings sections, the library.
std::vector<Item> staticItems() {
    std::vector<Item> items;
    const auto& o = Settings::get().shortcuts;
    for (const auto& a : shortcuts::actions()) {
        if ((a.flags & shortcuts::kGlobalOnly) || std::string_view(a.id) == "command-palette") continue;
        const std::string_view id = a.id;
        if ((id == "lyrics-fullscreen" && !ctx().toggleLyricsFullscreen) ||
            ((id == "lyrics-earlier" || id == "lyrics-later") && !ctx().lyricsOffsetBy))
            continue;
        Item it = commandItem("action:" + std::string(id), a.name(), shortcuts::categoryName(a.category), a.icon,
                              [idStr = std::string(id)] { commands::run(idStr); });
        it.hint = shortcuts::display(shortcuts::binding(o, id, false));
        items.push_back(std::move(it));
    }
    // Ayarlar sections (SettingsPage deep links: Route{Settings, <id>}).
    const std::pair<const char*, const wchar_t*> sections[] = {
        {"spotify", tr(L"Spotify hesabı")},
        {"connections", tr(L"Bağlantılar: Last.fm, ListenBrainz, Discord")},
        {"playback", tr(L"Oynatma ayarları")},
        {"audio", tr(L"Ses ayarları")},
        {"blocklist", tr(L"Kara liste")},
        {"appearance", tr(L"Görünüm ve tema")},
        {"window", tr(L"Pencere ve başlangıç")},
        {"keyboard", tr(L"Klavye kısayolları")},
        {"downloads", tr(L"İndirme ayarları")},
        {"local", tr(L"Yerel müzik klasörleri")},
        {"storage", tr(L"Kitaplık ve depolama")},
        {"about", tr(L"Hakkında ve güncellemeler")},
    };
    for (const auto& [id, name] : sections)
        items.push_back(commandItem(std::string("settings:") + id, name, tr(L"Ayarlar"), "settings",
                                    [id = std::string(id)] { ctx().router->navigate({RouteKind::Settings, id}); }));
    items.push_back(commandItem("theme:dark", tr(L"Koyu tema"), tr(L"Görünüm"), "palette", [] { setThemeMode(ThemeMode::Dark); }));
    items.push_back(commandItem("theme:light", tr(L"Açık tema"), tr(L"Görünüm"), "palette", [] { setThemeMode(ThemeMode::Light); }));
    items.push_back(commandItem("theme:system", tr(L"Sistem teması"), tr(L"Görünüm"), "palette",
                                [] { setThemeMode(ThemeMode::System); }));
    items.push_back(commandItem("cmd:new-playlist", tr(L"Yeni çalma listesi"), tr(L"Kitaplık"), "plus", [] { promptNewPlaylist(); }));
    if (ctx().player && !ctx().player->items().empty())
        items.push_back(commandItem("cmd:clear-queue", tr(L"Çalma sırasını temizle"), tr(L"Oynatma"), "trash", [] {
            ctx().player->clearQueue();
            toast(tr(L"Çalma sırası temizlendi"));
        }));
    if (!source::loggedIn() && ctx().startSpotifyLogin)
        items.push_back(commandItem("cmd:spotify-login", tr(L"Spotify ile bağlan"), L"Spotify", "spotify-link",
                                    [] { ctx().startSpotifyLogin(); }));
    if (ctx().quitApp)
        items.push_back(commandItem("cmd:quit", tr(L"ShadeTube'dan çık"), tr(L"Uygulama"), "logout", [] { ctx().quitApp(); }));

    // The library: Spotify's when logged in (Liked Songs is its first pseudo-playlist), else the local one.
    if (source::loggedIn()) {
        const auto& lib = ctx().session->library();
        for (const auto& p : lib.playlists) items.push_back(playlistItem(p, p.id == spotify::Api::kLikedSongsUri));
        for (const auto& a : lib.albums)
            items.push_back(albumItem(a.id, toWide(a.name), artistsOf(a.artists), imageOf(a.images), Group::Library));
        for (const auto& a : lib.artists) items.push_back(artistItem(a.id, toWide(a.name), imageOf(a.images), Group::Library));
    } else {
        auto& lib = ctx().library;
        catalog::Playlist liked;
        liked.totalTracks = static_cast<int>(lib.liked().size());
        items.push_back(playlistItem(liked, true));
        for (const auto& p : lib.playlists()) items.push_back(playlistItem(p, false));
        for (const auto& a : lib.albums())
            items.push_back(albumItem(a.id, toWide(a.name), artistsOf(a.artists), imageOf(a.images), Group::Library));
        for (const auto& a : lib.artists()) items.push_back(artistItem(a.id, toWide(a.name), imageOf(a.images), Group::Library));
    }
    return items;
}

// A recent pick: the live item with the same key, else rebuilt from what was stored.
std::optional<Item> recentItem(const json& e, const std::vector<Item>& statics) {
    const std::string key = e.value("key", "");
    for (const auto& it : statics)
        if (it.key == key) {
            Item copy = it;
            copy.group = Group::Recent;
            return copy;
        }
    const std::string kind = e.value("kind", "");
    try {
        if (kind == "track" && e.contains("track")) return trackItem(trackFromJson(e["track"]), Group::Recent);
        if (kind == "album")
            return albumItem(e.value("id", ""), toWide(e.value("t", "")), toWide(e.value("s", "")), e.value("img", ""), Group::Recent);
        if (kind == "artist") return artistItem(e.value("id", ""), toWide(e.value("t", "")), e.value("img", ""), Group::Recent);
        if (kind == "playlist") {
            catalog::Playlist p;
            p.id = e.value("id", "");
            p.name = e.value("t", "");
            p.countKnown = false;
            if (auto img = e.value("img", ""); !img.empty()) p.images.push_back({img, 64, 64});
            Item it = playlistItem(p, false);
            it.group = Group::Recent;
            return it;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

class Palette;
Palette* g_open = nullptr;

class Palette : public ui::Widget {
public:
    Palette() {
        g_open = this;
        box_ = add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Komut, sayfa, çalma listesi ya da şarkı ara…"));
        box_->onChange = [this](const std::wstring&) { queryChanged(); };
        box_->onEscape = [this] { close(); };
        box_->onKey = [this](const ui::KeyEvent& e) { return navKey(e); };
        statics_ = staticItems();
        open_.to(1, ui::motion::medium, ui::Ease::Decelerate);
        refilter();
        rebuildRows();
    }
    ~Palette() override {
        if (g_open == this) g_open = nullptr;
        if (cts_) cts_->cancel();
    }

    void type(const std::wstring& text) { box_->setText(text, true); }

    void close() {
        if (closing_) return;
        closing_ = true;
        if (cts_) cts_->cancel();
        if (g_open == this) g_open = nullptr;
        if (auto* w = window()) w->removeOverlay(this);
    }

    void layout() override {
        const Rect r = rect();
        const float w = std::min(kPanelW, r.w - 32);
        const float listMax = std::max(kRowH * 3, std::min(480.f, r.h * 0.62f));
        const float listH = std::min(listMax, contentHeight());
        const float top = std::max(40.f, r.h * 0.12f);
        panel_ = {r.cx() - w * 0.5f, top, w, kPad + gfx::metrics::searchH + 8 + listH + kFooterH};
        box_->setRect({panel_.x + kPad, panel_.y + kPad, panel_.w - kPad * 2, gfx::metrics::searchH});
        list_ = {panel_.x, panel_.y + kPad + gfx::metrics::searchH + 8, panel_.w, listH};
        clampScroll();
    }

    void paint(Canvas& c) override {
        // Debounced catalog search (like the Search page: the box's last keystroke + 300 ms).
        if (!pending_.empty() && ui::frame::realNow() >= pendingAt_) {
            const std::wstring q = pending_;
            pending_.clear();
            search(q);
        } else if (!pending_.empty()) {
            if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
        }
        const auto& col = colors();
        const float t = open_;
        c.fillRect(rect(), col.scrim.mulAlpha(t));
        c.pushOpacity(t);
        c.pushScale(0.98f + 0.02f * t, {panel_.cx(), panel_.y});
        c.shadow(panel_, 2, 64, 32, col.shadowDialog);
        c.fillRounded(panel_, 2, col.bgRaised);
        c.strokeRounded(panel_, 2, col.hairStrong);
        paintChildren(c);
        paintList(c);
        paintFooter(c);
        c.popTransform();
        c.popLayer();
    }

    Widget* hitTest(Point p) override {
        if (!visible()) return nullptr;
        if (Widget* w = Widget::hitTest(p); w && w != this) return w;
        return this;   // full-window: a click on the scrim closes
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (!panel_.contains(e.pos)) {
            close();
            return true;
        }
        pressed_ = rowAt(e.pos);
        return true;
    }

    void onMouseUp(const ui::MouseEvent& e) override {
        const int row = rowAt(e.pos);
        if (row >= 0 && row == pressed_) {
            sel_ = row;
            activate(e.shift);
        }
        pressed_ = -1;
    }

    void onMouseMove(const ui::MouseEvent& e) override {
        const int row = rowAt(e.pos);
        if (row >= 0 && row != sel_) {
            sel_ = row;
            invalidate();
        }
    }

    bool onWheel(float dy, float, const ui::MouseEvent& e) override {
        if (list_.contains(e.pos)) {
            scroll_ -= dy * kRowH * 2;
            clampScroll();
            invalidate();
        }
        return true;
    }

    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk == VK_ESCAPE) {
            close();
            return true;
        }
        return navKey(e);
    }

    LPCWSTR cursor() const override { return IDC_ARROW; }

private:
    struct Row {
        bool header = false;
        Group group = Group::Commands;
        int item = -1;                // index into shown_
        float y = 0, h = 0;           // in list content space
    };

    // ---- data ----

    void queryChanged() {
        refilter();
        const std::wstring q = box_->text();
        if (cts_) cts_->cancel();
        searching_ = false;
        catalog_.clear();
        catalogFor_.clear();
        if (q.size() < 2) {
            pending_.clear();
        } else {
            pending_ = q;
            pendingAt_ = ui::frame::realNow() + 300;
            if (auto* w = window()) w->invalidateAfter(301);
        }
        rebuildRows();
    }

    void search(const std::wstring& q) {
        if (cts_) cts_->cancel();
        cts_ = std::make_shared<YoutubeExplode::CancellationTokenSource>();
        const auto token = cts_->token();
        searching_ = true;
        spotify::Api* api = source::activeApi();
        const std::string query = toUtf8(q);
        life_.renew();
        async(Priority::High, life_.ref(), [api, query, token] { return source::search(api, query, 6, token); },
              [this, q](Result<catalog::SearchResults> r) {
                  if (box_->text() != q) return;   // typed on meanwhile: a newer search follows
                  searching_ = false;
                  catalog_.clear();
                  catalogFor_ = q;
                  if (!r) {
                      ST_LOG_WARN("palette", "catalog search failed: {}", r.errorMessage());
                  } else {
                      for (size_t i = 0; i < r->tracks.size() && i < 5; ++i) catalog_.push_back(trackItem(r->tracks[i], Group::Songs));
                      for (size_t i = 0; i < r->albums.size() && i < 3; ++i) {
                          const auto& a = r->albums[i];
                          catalog_.push_back(albumItem(a.id, toWide(a.name), artistsOf(a.artists), imageOf(a.images), Group::Albums));
                      }
                      for (size_t i = 0; i < r->artists.size() && i < 3; ++i) {
                          const auto& a = r->artists[i];
                          catalog_.push_back(artistItem(a.id, toWide(a.name), imageOf(a.images), Group::Artists));
                      }
                  }
                  rebuildRows();
                  requestLayout();
                  invalidate();
              });
    }

    void refilter() {
        const std::wstring q = shortcuts::fold(box_ ? box_->text() : std::wstring());
        filtered_.clear();
        const bool empty = q.find_first_not_of(L' ') == std::wstring::npos;
        if (empty) {
            // Recent picks, then the commands in table order.
            for (const auto& e : loadRecent())
                if (auto it = recentItem(e, statics_)) filtered_.push_back(std::move(*it));
            for (const auto& it : statics_)
                if (it.group == Group::Commands) filtered_.push_back(it);
            return;
        }
        // One or two letters: only word starts (else every item containing "st" matches); longer: anything but
        // scattered-letter noise.
        const int minScore = q.size() <= 2 ? 700 : 200;
        for (const auto& it : statics_) {
            int s = shortcuts::fuzzyScore(q, it.foldedTitle);
            const int s2 = shortcuts::fuzzyScore(q, it.foldedAll);
            if (s2 >= 0) s = std::max(s, s2 - 150);   // a match in the subtitle ("ayarlar") counts less
            if (s < minScore) continue;
            Item copy = it;
            copy.score = s;
            filtered_.push_back(std::move(copy));
        }
    }

    void rebuildRows() {
        shown_.clear();
        rows_.clear();
        const bool empty = box_->text().find_first_not_of(L' ') == std::wstring::npos;
        // Groups: local ones by their best score (commands first on a tie), catalog groups after them in fixed order.
        std::vector<Group> order;
        if (empty) {
            order = {Group::Recent, Group::Commands};
        } else {
            int bestCmd = -1, bestLib = -1;
            for (const auto& it : filtered_) {
                int& best = it.group == Group::Commands ? bestCmd : bestLib;
                best = std::max(best, it.score);
            }
            if (bestLib > bestCmd) order = {Group::Library, Group::Commands};
            else order = {Group::Commands, Group::Library};
            order.insert(order.end(), {Group::Songs, Group::Artists, Group::Albums});
        }
        float y = 0;
        for (Group g : order) {
            std::vector<const Item*> items;
            for (const auto& it : filtered_)
                if (it.group == g) items.push_back(&it);
            if (g == Group::Songs || g == Group::Albums || g == Group::Artists)
                for (const auto& it : catalog_)
                    if (it.group == g) items.push_back(&it);
            if (!empty && g != Group::Songs && g != Group::Albums && g != Group::Artists)
                std::stable_sort(items.begin(), items.end(), [](const Item* a, const Item* b) { return a->score > b->score; });
            const size_t cap = empty ? (g == Group::Recent ? kMaxRecent : 60) : (g == Group::Commands || g == Group::Library ? 6 : 5);
            if (items.size() > cap) items.resize(cap);
            if (items.empty()) continue;
            rows_.push_back({true, g, -1, y, kHeaderH});
            y += kHeaderH;
            for (const Item* it : items) {
                shown_.push_back(*it);
                rows_.push_back({false, g, static_cast<int>(shown_.size()) - 1, y, kRowH});
                y += kRowH;
            }
        }
        titles_.clear();
        subs_.clear();
        for (const auto& it : shown_) {
            titles_.emplace_back(it.title, type::body);
            subs_.emplace_back(it.subtitle, type::caption);
        }
        contentH_ = y;
        // Selection: the first item after every change of the list (the best match is on top).
        sel_ = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (!rows_[i].header) {
                sel_ = i;
                break;
            }
        scroll_ = 0;
        requestLayout();
        invalidate();
    }

    // Height the list wants: the rows, or one line for "searching" / "no results".
    float contentHeight() const {
        const bool status = rows_.empty() || searching_;
        return std::max(kRowH, contentH_ + (status && !rows_.empty() ? kRowH : 0.f));
    }

    // ---- keyboard / selection ----

    bool navKey(const ui::KeyEvent& e) {
        switch (e.vk) {
        case VK_DOWN: move(1); return true;
        case VK_UP: move(-1); return true;
        case VK_NEXT: move(6); return true;
        case VK_PRIOR: move(-6); return true;
        case VK_RETURN:
            if (!e.repeat) activate(e.shift);
            return true;
        default: return false;
        }
    }

    void move(int delta) {
        if (rows_.empty()) return;
        const int n = static_cast<int>(rows_.size());
        int i = sel_ < 0 ? -1 : sel_;
        const int step = delta > 0 ? 1 : -1;
        for (int left = std::abs(delta); left > 0;) {
            int next = i + step;
            while (next >= 0 && next < n && rows_[next].header) next += step;
            if (next < 0 || next >= n) {
                if (std::abs(delta) == 1) {   // single steps wrap around
                    next = step > 0 ? 0 : n - 1;
                    while (next >= 0 && next < n && rows_[next].header) next += step;
                    if (next < 0 || next >= n) return;
                } else {
                    break;
                }
            }
            i = next;
            --left;
        }
        if (i < 0) return;
        sel_ = i;
        const Row& r = rows_[sel_];
        // Keep the selection (and its group header when it is the first row) in view.
        const float top = sel_ > 0 && rows_[sel_ - 1].header ? rows_[sel_ - 1].y : r.y;
        if (top < scroll_) scroll_ = top;
        if (r.y + r.h > scroll_ + list_.h) scroll_ = r.y + r.h - list_.h;
        clampScroll();
        invalidate();
    }

    void activate(bool alt) {
        if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size()) || rows_[sel_].header) return;
        const Item it = shown_[rows_[sel_].item];
        remember(it);
        close();   // first: the action may open a dialog or navigate
        if (it.run) it.run(alt);
    }

    int rowAt(Point p) const {
        if (!list_.contains(p)) return -1;
        const float y = p.y - list_.y + scroll_;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (!rows_[i].header && y >= rows_[i].y && y < rows_[i].y + rows_[i].h) return i;
        return -1;
    }

    void clampScroll() { scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - list_.h)); }

    // ---- painting ----

    void paintList(Canvas& c) {
        const auto& col = colors();
        c.pushClip(list_);
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const Row& row = rows_[i];
            const Rect r{list_.x, list_.y + row.y - scroll_, list_.w, row.h};
            if (r.bottom() < list_.y || r.y > list_.bottom()) continue;
            if (row.header) {
                c.text(groupLabel(row.group), type::monoLabel, {r.x + 20, r.y + 10, r.w - 40, r.h - 10},
                       col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
                continue;
            }
            const Item& it = shown_[row.item];
            const bool selected = i == sel_;
            const Rect bg = r.inset(6, 2);
            if (selected) {
                c.fillRounded(bg, 2, col.overlaySelected);
                c.fillRect({bg.x, bg.y + 8, 2, bg.h - 16}, accent().base);
            }
            const Rect art{r.x + 16, r.cy() - 16, 32, 32};
            ID2D1Bitmap1* bmp = it.imageUrl.empty() ? nullptr
                                                    : gfx::ImageCache::get().request(it.imageUrl, static_cast<int>(32 * c.scale()));
            if (bmp) {
                if (it.round) c.imageCircle(bmp, art);
                else c.image(bmp, art, 2);
            } else {
                if (it.round) c.fillCircle({art.cx(), art.cy()}, 16, col.bgOverlay);
                else c.fillRounded(art, 2, col.bgOverlay);
                c.icon(it.icon.empty() ? "music-note" : it.icon, art.center(18, 18), selected ? accent().base : col.fgSecondary);
            }
            const float x = art.right() + 12;
            float right = r.right() - 20;
            if (!it.hint.empty()) {
                const float hw = 132;
                c.text(it.hint, type::monoMeta, {right - hw, r.y, hw, r.h}, col.fgTertiary, gfx::TextAlign::Trailing,
                       gfx::VAlign::Center);
                right -= hw + 12;
            }
            c.text(titles_[row.item], {x, r.y + 7, right - x, 18}, col.fgPrimary, gfx::VAlign::Center);
            c.text(subs_[row.item], {x, r.y + 26, right - x, 16}, col.fgSecondary, gfx::VAlign::Center);
        }
        // Status line below the rows (or instead of them).
        const float statusY = list_.y + contentH_ - scroll_;
        if (searching_ || rows_.empty()) {
            const std::wstring msg = searching_ ? std::wstring(tr(L"Katalogda aranıyor…")) : std::wstring(tr(L"Sonuç bulunamadı"));
            c.text(msg, type::secondary, {list_.x + 20, statusY, list_.w - 40, kRowH}, col.fgTertiary, gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
        }
        c.popClip();
    }

    void paintFooter(Canvas& c) {
        const auto& col = colors();
        const Rect f{panel_.x, panel_.bottom() - kFooterH, panel_.w, kFooterH};
        c.hline(f.x, f.right(), f.y, col.hairSubtle);
        c.text(tr(L"↑↓ seç · Enter aç · Shift+Enter çal ya da sıraya ekle · Esc kapat"), type::monoMeta,
               f.inset(20, 0), col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    }

    ui::TextBox* box_;
    std::vector<Item> statics_, filtered_, catalog_, shown_;
    std::vector<Row> rows_;
    std::vector<gfx::Text> titles_, subs_;
    std::wstring catalogFor_;
    float contentH_ = 0, scroll_ = 0;
    int sel_ = -1, pressed_ = -1;
    Rect panel_{}, list_{};
    ui::Anim open_;
    bool closing_ = false, searching_ = false;
    std::wstring pending_;
    double pendingAt_ = 0;
    Lifetime life_;
    std::shared_ptr<YoutubeExplode::CancellationTokenSource> cts_;
};

} // namespace

void openCommandPalette(const std::wstring& query) {
    auto* w = ctx().window;
    if (!w || g_open) return;
    auto* p = static_cast<Palette*>(w->pushOverlay(std::make_unique<Palette>(), true));
    if (!query.empty()) p->type(query);
    w->invalidate();
}

void toggleCommandPalette() {
    if (g_open) g_open->close();
    else openCommandPalette();
}

bool commandPaletteOpen() { return g_open != nullptr; }

} // namespace st::app
