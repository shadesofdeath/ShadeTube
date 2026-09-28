// Radyo: internet radio stations from radio-browser.info (app/InternetRadio) — the page, the station widgets the live
// displays share (drawStationArt, showStationMenu, the "CANLI" badge) and the wiring: the player's stream lookup, the
// click counting radio-browser.info asks for, recently played and the song titles a station announces (ICY).
//
// Routes (Route{RouteKind::Radio, id}): "" = the Radyo page; "tag:<tag>", "country:<CC>", "top" (most listened in the
// last 24 h), "votes" (most voted), "favorites", "recent", "genres" (every genre), "search:<name>" = station lists.
#include "app/InternetRadio.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Shell.h"
#include "app/Updater.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>

namespace st::app {

using gfx::accent;
using gfx::colors;
using radio::CancellationToken;
using radio::Station;
using ui::Button;
using ui::ButtonKind;
using CancelSource = YoutubeExplode::CancellationTokenSource;
namespace type = gfx::type;

// ===================================================================================================
// Shared station widgets (declared in Shell.h)

void drawStationArt(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius, Priority prio) {
    const auto& col = colors();
    const Color surface = gfx::isLightTheme() ? col.bgSunken : col.bgOverlay;
    ID2D1Bitmap1* bmp = nullptr;
    if (!images.empty() && !images.front().url.empty()) {
        const int px = static_cast<int>(std::ceil(std::max(r.w, r.h) * c.scale()));
        bmp = gfx::ImageCache::get().request(images.front().url, px, prio);
    }
    c.fillRounded(r, radius, surface);
    if (!bmp) {
        c.icon("radio", r.center(std::round(r.w * 0.42f), std::round(r.w * 0.42f)), col.fgTertiary);
        return;
    }
    // Logos come in every shape: a square one fills the tile, anything else is letterboxed (never cropped).
    const D2D1_SIZE_F size = bmp->GetSize();
    const float aspect = size.height > 0 ? size.width / size.height : 1.f;
    if (aspect > 0.9f && aspect < 1.1f) {
        c.image(bmp, r, radius);
        return;
    }
    const Rect box = r.inset(std::round(r.w * 0.08f));
    const Rect fit = aspect >= 1 ? box.center(box.w, box.w / aspect) : box.center(box.h * aspect, box.h);
    c.image(bmp, fit, 0);
}

float drawLiveBadge(Canvas& c, gfx::Point leftCenter, bool playing) {
    const auto& acc = accent();
    const std::wstring label = toUpperTr(tr(L"Canlı"));
    auto layout = gfx::makeLayout(label, type::monoLabel, 200);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float w = std::ceil(m.widthIncludingTrailingWhitespace) + 10 + 6 + 8 + 8;
    const Rect pill{leftCenter.x, leftCenter.y - 10, w, 20};
    c.fillPill(pill, acc.tint12);
    c.fillCircle({pill.x + 11, pill.cy()}, 3, playing ? acc.base : colors().fgTertiary);
    c.text(layout.Get(), pill.x + 20, pill.cy() - m.height * 0.5f, acc.base);
    return w;
}

namespace {

// ---------------------------------------------------------------------------------------------------
// Wiring state (UI thread)

struct LiveState {
    std::string trackId;          // the station the heard titles belong to
    std::wstring lastTitle;
    std::vector<HeardTitle> heard;
    std::string startedId;        // station whose start was handled (recent + click), "" after another track
};

LiveState& liveState() {
    static LiveState s;
    return s;
}

bool isHttpUrl(const std::string& u) { return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0; }

// The country of "Popüler": the one picked on the page, else Windows' home location, else the app region.
std::string homeCountry() {
    if (const auto& c = radio::store().country(); c.size() == 2) return c;
    wchar_t geo[16] = {};
    if (GetUserDefaultGeoName(geo, 16) == 3 && iswalpha(geo[0]) && iswalpha(geo[1])) return toUtf8(toUpperTr(geo));
    if (const auto& r = Settings::get().region; r.size() == 2) return r;
    return "TR";
}

std::vector<ui::MenuItem> stationMenuItems(const Station& s) {
    std::vector<ui::MenuItem> items;
    const bool fav = radio::store().isFavorite(s.uuid);
    items.push_back({fav ? tr(L"Favorilerden kaldır") : tr(L"Favorilere ekle"), fav ? "heart-filled" : "heart", L"", [s, fav] {
                         radio::store().setFavorite(s, !fav);
                         toast(fav ? tr(L"Favorilerden kaldırıldı") : tr(L"Favorilere eklendi"));
                     }});
    // Only http(s) links leave the app: the homepage comes from a public, user-edited directory.
    if (isHttpUrl(s.homepage)) {
        const std::wstring url = toWide(s.homepage);
        items.push_back({tr(L"Web sitesini aç"), "external-link", L"",
                         [url] { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }});
    }
    if (const std::string stream = radio::streamUrl(s); !stream.empty()) {
        items.push_back({tr(L"Yayın adresini kopyala"), "link", L"", [stream] {
                             copyText(toWide(stream));
                             toast(tr(L"Yayın adresi kopyalandı"));
                         }});
    }
    return items;
}

// A station started playing: recently played + radio-browser.info's click counter, once per start (a pause / resume
// or a reconnect is not a new start).
void onPlayerChanged() {
    auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    auto& live = liveState();
    if (!t || !radio::isStationId(t->id)) {
        live.startedId.clear();
        return;
    }
    if (t->id != live.trackId) {
        live.trackId = t->id;
        live.heard.clear();
        live.lastTitle.clear();
    }
    if (const std::wstring& title = p->liveTitle(); !title.empty() && title != live.lastTitle) {
        live.lastTitle = title;
        live.heard.insert(live.heard.begin(), {nowUnix(), title});
        if (live.heard.size() > 12) live.heard.pop_back();
    }
    if (p->status() != player::Status::Playing || live.startedId == t->id) return;
    live.startedId = t->id;
    const std::string uuid = radio::uuidOf(t->id);
    ST_LOG_INFO("iradio", "station playing: {}", t->name);
    if (const Station* s = radio::store().find(uuid)) radio::store().recordPlayed(*s);
    background(Priority::Low, [uuid] { radio::Client::shared().countClick(uuid); });
}

// ---------------------------------------------------------------------------------------------------
// Widgets

constexpr float kRowH = 64, kRowMinW = 380, kRowGap = 32;
constexpr float kTileMinW = 200, kTileH = 148;

std::wstring badgeText(const Station& s) { return toWide(radio::codecBadge(s)); }

bool isCurrentStation(const std::string& trackId) {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    return t && t->id == trackId;
}

void paintEqualizer(Canvas& c, const Rect& r) {
    const auto* p = ctx().player;
    const bool playing = p && p->isPlaying();
    const int f = playing ? static_cast<int>(ui::frame::now() / 66.0) % 12 : 0;
    char name[48];
    snprintf(name, sizeof name, "animations/equalizer/frame-%02d", f);
    c.icon(name, r, accent().base);
    if (playing) ui::frame::requestNext();
}

// The small mono codec badge ("MP3 · 128") like the queue's "ENGELLİ" badge. Returns its width (0 = none).
float paintBadge(Canvas& c, gfx::Text& badge, float right, float cy) {
    if (badge.empty()) return 0;
    const float bw = std::ceil(badge.measure().w) + 10;
    const Rect b{right - bw, cy - 7, bw, 14};
    c.fillRounded(b, 2, colors().fgPrimary.withAlpha(0.10f));
    c.text(badge, b.inset(5, 0), colors().fgSecondary, gfx::VAlign::Center);
    return bw;
}

// One station in a list: logo, name, "Country · genres", codec badge, heart. Click plays the list from here.
class StationRow : public ui::Widget {
public:
    StationRow(const Station& s, std::function<void()> onPlay)
        : station_(s), trackId_(std::string(radio::kIdPrefix) + s.uuid), name_(toWide(s.name), type::body),
          sub_(radio::subtitle(s), type::caption), badge_(badgeText(s), type::monoBadge), onPlay_(std::move(onPlay)) {
        focusable = true;
        if (!s.favicon.empty()) images_.push_back({s.favicon, 0, 0});
    }
    float preferredHeight(float) override { return kRowH; }
    LPCWSTR cursor() const override { return IDC_HAND; }
    std::wstring tooltip() const override {
        return heartHot_ ? std::wstring(radio::store().isFavorite(station_.uuid) ? tr(L"Favorilerden kaldır") : tr(L"Favorilere ekle"))
                         : std::wstring();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        heartHot_ = false;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = heartRect().contains(e.pos);
        if (hot != heartHot_) {
            heartHot_ = hot;
            invalidate();
        }
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            openMenu(e.windowPos);
            return false;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (!rect().contains(e.pos)) return;
        if (heartRect().contains(e.pos)) {
            radio::store().toggleFavorite(station_);
            invalidate();
            return;
        }
        play();
    }
    // Keyboard (focusable): Enter / Space play, menu key / Shift+F10 open the station menu (favorites, links).
    bool activatable() const override { return true; }
    bool onActivate() override {
        play();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) return false;
        const Rect r = toWindow(rect());
        openMenu({r.x + 60, r.bottom()});
        return true;
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const bool current = isCurrentStation(trackId_);
        const float h = hover_;
        if (current) c.fillRounded(r, 2, acc.tint06);
        c.fillRounded(r, 2, col.overlayHover.mulAlpha(h));
        const Rect art{r.x + 8, r.cy() - 22, 44, 44};
        drawStationArt(c, images_, art, 2, Priority::Normal);

        const Rect heart = heartRect();
        const bool fav = radio::store().isFavorite(station_.uuid);
        if (heartHot_) c.fillCircle({heart.cx(), heart.cy()}, 16, col.overlayHover);
        if (fav || h > 0.01f)
            c.icon(fav ? "heart-filled" : "heart", heart.center(16, 16), fav ? acc.base : col.fgSecondary.mulAlpha(heartHot_ ? 1.f : h));
        float right = heart.x - 8;
        if (const float bw = paintBadge(c, badge_, right, r.cy()); bw > 0) right -= bw + 12;
        if (current) {
            paintEqualizer(c, {right - 16, r.cy() - 8, 16, 16});
            right -= 16 + 12;
        }
        const float tx = art.right() + 14, tw = std::max(0.f, right - tx);
        c.text(name_, {tx, r.cy() - 19, tw, 20}, current ? acc.base : col.fgPrimary, gfx::VAlign::Center);
        c.text(sub_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
        c.hline(r.x + 8, r.right() - 8, r.bottom() - 1, col.hairSubtle);
    }

private:
    Rect heartRect() const { return {rect().right() - 8 - 32, rect().cy() - 16, 32, 32}; }
    void play() {
        const auto fn = onPlay_;   // may rebuild the list this row is in
        if (fn) fn();
    }
    void openMenu(gfx::Point windowPos) {
        std::vector<ui::MenuItem> items;
        if (auto fn = onPlay_) {
            items.push_back({tr(L"Çal"), "play", L"", [fn] { fn(); }});
            items.push_back(ui::MenuItem::sep());
        }
        for (auto& it : stationMenuItems(station_)) items.push_back(std::move(it));
        ui::Menu::open(ctx().window, windowPos, std::move(items));
    }

    Station station_;
    std::string trackId_;
    std::vector<catalog::Image> images_;
    gfx::Text name_, sub_, badge_;
    std::function<void()> onPlay_;
    ui::Anim hover_;
    bool heartHot_ = false;
};

// Rows in as many columns as fit (>= kRowMinW each), row-major: reading order = Tab order.
class RowList : public ui::Widget {
public:
    RowList() { hitTestVisible = false; }
    static int columnsFor(float w) { return std::max(1, static_cast<int>((w + kRowGap) / (kRowMinW + kRowGap))); }
    float preferredHeight(float w) override {
        const int n = static_cast<int>(children().size());
        const int cols = std::min(2, columnsFor(w));
        return n == 0 ? 0.f : std::ceil(static_cast<float>(n) / static_cast<float>(cols)) * kRowH;
    }
    void layout() override {
        const Rect r = rect();
        const int cols = std::min(2, columnsFor(r.w));
        const float cw = (r.w - kRowGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        int i = 0;
        for (const auto& child : children()) {
            child->setRect({static_cast<float>(i % cols) * (cw + kRowGap), static_cast<float>(i / cols) * kRowH, cw, kRowH});
            ++i;
        }
    }
};

// Loading placeholder in the RowList geometry.
class SkeletonRows : public ui::Widget {
public:
    explicit SkeletonRows(int n) : n_(n) { hitTestVisible = false; }
    float preferredHeight(float w) override {
        const int cols = std::min(2, RowList::columnsFor(w));
        return std::ceil(static_cast<float>(n_) / static_cast<float>(cols)) * kRowH;
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const double now = ui::frame::now();
        const int cols = std::min(2, RowList::columnsFor(r.w));
        const float cw = (r.w - kRowGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        for (int i = 0; i < n_; ++i) {
            const float x = r.x + static_cast<float>(i % cols) * (cw + kRowGap), y = r.y + static_cast<float>(i / cols) * kRowH;
            c.skeleton({x + 8, y + 10, 44, 44}, 2, now);
            c.skeleton({x + 66, y + 18, std::min(cw - 120, 200.f - static_cast<float>(i % 3) * 36.f), 12}, 2, now);
            c.skeleton({x + 66, y + 38, 110, 10}, 2, now);
        }
        ui::frame::requestNext();
    }

private:
    int n_;
};

// A favorite / recently played station as a tile (the category palette tone of Tile): logo, heart, name, genres, badge.
class StationTile : public ui::Widget {
public:
    StationTile(const Station& s, std::function<void()> onPlay, bool recentList)
        : station_(s), trackId_(std::string(radio::kIdPrefix) + s.uuid), name_(toWide(s.name), type::body.withSize(15).withWeight(800)),
          sub_(radio::subtitle(s), type::caption), badge_(badgeText(s), type::monoBadge), tone_(tileColor(s.uuid)),
          onPlay_(std::move(onPlay)), recentList_(recentList) {
        focusable = true;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        name_.setOptions(wrap);
        if (!s.favicon.empty()) images_.push_back({s.favicon, 0, 0});
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    std::wstring tooltip() const override {
        return heartHot_ ? std::wstring(radio::store().isFavorite(station_.uuid) ? tr(L"Favorilerden kaldır") : tr(L"Favorilere ekle"))
                         : std::wstring();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        heartHot_ = false;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = heartRect().contains(e.pos);
        if (hot != heartHot_) {
            heartHot_ = hot;
            invalidate();
        }
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            openMenu(e.windowPos);
            return false;
        }
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, 80);
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, 160);
        if (!rect().contains(e.pos)) return;
        if (heartRect().contains(e.pos)) {
            radio::store().toggleFavorite(station_);   // may rebuild the section this tile is in (deferred delete)
            return;
        }
        play();
    }
    bool activatable() const override { return true; }
    bool onActivate() override {
        press_.snap(1);
        press_.to(0, 160);
        play();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) return false;
        const Rect r = toWindow(rect());
        openMenu({r.x + 16, r.y + 80});
        return true;
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        const bool current = isCurrentStation(trackId_);
        c.pushScale(1 - 0.01f * press_, {r.cx(), r.cy()});
        const Color fill = tileFill(tone_);
        c.fillRounded(r, 2, Color::lerp(fill, Color::lerp(fill, col.fgPrimary, 0.06f), h));
        if (current) c.strokeRounded(r.inset(0.5f), 2, acc.base, 1.f);
        const Rect art{r.x + 16, r.y + 16, 56, 56};
        drawStationArt(c, images_, art, 2);

        const Rect heart = heartRect();
        const bool fav = radio::store().isFavorite(station_.uuid);
        if (heartHot_) c.fillCircle({heart.cx(), heart.cy()}, 16, col.overlayHover);
        if (fav || h > 0.01f)
            c.icon(fav ? "heart-filled" : "heart", heart.center(16, 16), fav ? acc.base : col.fgSecondary.mulAlpha(heartHot_ ? 1.f : h));
        if (current) paintEqualizer(c, {heart.x - 20, heart.cy() - 8, 16, 16});

        const float tw = r.w - 32;
        const float lineY = r.bottom() - 16 - 16;
        const float bw = paintBadge(c, badge_, r.right() - 16, lineY + 8);
        c.text(sub_, {r.x + 16, lineY, tw - (bw > 0 ? bw + 8 : 0), 16}, col.fgSecondary, gfx::VAlign::Center);
        const float nh = name_.measure(tw).h;
        c.text(name_, {r.x + 16, lineY - 6 - nh, tw, nh}, current ? acc.base : col.fgPrimary);
        c.popTransform();
    }

private:
    Rect heartRect() const { return {rect().right() - 12 - 32, rect().y + 12, 32, 32}; }
    void play() {
        const auto fn = onPlay_;
        if (fn) fn();
    }
    void openMenu(gfx::Point windowPos) {
        std::vector<ui::MenuItem> items;
        if (auto fn = onPlay_) {
            items.push_back({tr(L"Çal"), "play", L"", [fn] { fn(); }});
            items.push_back(ui::MenuItem::sep());
        }
        for (auto& it : stationMenuItems(station_)) items.push_back(std::move(it));
        if (recentList_) {
            const std::string uuid = station_.uuid;
            items.push_back(ui::MenuItem::sep());
            items.push_back({tr(L"Son dinlenenlerden kaldır"), "minus", L"", [uuid] { radio::store().removeRecent(uuid); }});
        }
        ui::Menu::open(ctx().window, windowPos, std::move(items));
    }

    Station station_;
    std::string trackId_;
    std::vector<catalog::Image> images_;
    gfx::Text name_, sub_, badge_;
    Color tone_;
    std::function<void()> onPlay_;
    bool recentList_;
    ui::Anim hover_, press_;
    bool heartHot_ = false;
};

// Section title + mono meta + links on the right (SectionHeader with several links).
class SectionBar : public ui::Widget {
public:
    SectionBar(std::wstring title, std::wstring meta) : title_(std::move(title), type::title), meta_(std::move(meta), type::monoLabel) {
        hitTestVisible = false;
    }
    Button* addLink(std::wstring label, std::string icon, std::function<void()> fn) {
        auto* b = add<Button>(ButtonKind::Link, std::move(label), std::move(icon));
        b->onClick = std::move(fn);
        links_.push_back(b);
        return b;
    }
    float preferredHeight(float) override { return 48; }
    void layout() override {
        float x = rect().w;
        for (auto* b : links_) {
            const float w = b->naturalWidth();
            x -= w;
            b->setRect({x, 0, w, 36});
            x -= 24;
        }
        linksW_ = rect().w - x;
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const float room = std::max(0.f, r.w - linksW_ - 16);
        const float tw = std::min(std::ceil(title_.measure().w), room);
        c.text(title_, {r.x, r.y, tw + 1, 36}, colors().fgPrimary, gfx::VAlign::Center);
        if (!meta_.empty() && tw + 14 < room)
            c.text(meta_, {r.x + tw + 14, r.y + 4, room - tw - 14, 36}, colors().fgTertiary, gfx::VAlign::Center);
        c.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
        paintChildren(c);
    }

private:
    gfx::Text title_, meta_;
    std::vector<Button*> links_;
    float linksW_ = 0;
};

// Compact error line inside a section: icon + text + "Tekrar dene".
class InlineError : public ui::Widget {
public:
    InlineError(std::wstring text, std::function<void()> retry) : text_(std::move(text), type::secondary) {
        hitTestVisible = false;
        retry_ = add<Button>(ButtonKind::Link, tr(L"Tekrar dene"), "refresh");
        retry_->onClick = std::move(retry);
    }
    float preferredHeight(float) override { return 56; }
    void layout() override {
        const float tw = std::ceil(text_.measure().w);
        retry_->setRect({8 + 16 + 10 + tw + 16, 10, retry_->naturalWidth(), 36});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        c.icon("offline", {r.x + 8, r.cy() - 8, 16, 16}, colors().fgTertiary);
        c.text(text_, {r.x + 8 + 16 + 10, r.y, r.w - 60, r.h}, colors().fgSecondary, gfx::VAlign::Center);
        paintChildren(c);
    }

private:
    gfx::Text text_;
    Button* retry_;
};

// Wrapping row of buttons (genre chips).
ui::Box* addFlow(ui::Column* col, float itemH) {
    auto* flow = col->add<ui::Box>();
    flow->hitTestVisible = false;
    auto place = [itemH](ui::Box& b, float width, bool apply) {
        float x = 0, y = 0;
        for (const auto& child : b.children()) {
            auto* btn = static_cast<Button*>(child.get());
            const float w = std::min(btn->naturalWidth(), width);
            if (x > 0 && x + w > width) {
                x = 0;
                y += itemH + 8;
            }
            if (apply) btn->setRect({x, y, w, itemH});
            x += w + 8;
        }
        return b.children().empty() ? 0.f : y + itemH;
    };
    flow->onLayout = [place](ui::Box& b) { place(b, b.rect().w, true); };
    flow->onPreferredHeight = [flow, place](float w) { return place(*flow, w, false); };
    return flow;
}

// Country picker (dialog): a filter field over the countries radio-browser.info lists, most stations first.
class CountryList : public ui::Widget {
public:
    struct Item {
        std::string code;
        std::wstring name;   // localized
        std::wstring folded;
        int stations = 0;
    };
    explicit CountryList(std::string current) : current_(std::move(current)) {
        scroll_ = add<ui::ScrollView>();
        col_ = scroll_->setContent<ui::Column>(0.f);
        status_ = add<ui::Label>(tr(L"Ülkeler yükleniyor…"), type::secondary, ui::Tone::Tertiary);
        auto cts = cts_;
        async(
            Priority::High, life_.ref(),
            [cts] {
                std::vector<Item> items;
                for (const auto& c : radio::Client::shared().countries(cts->token())) {
                    Item it{c.code, radio::countryName(c.code, c.name), {}, c.stationCount};
                    it.folded = foldForSearch(it.name + L" " + toWide(c.name) + L" " + toWide(c.code));
                    items.push_back(std::move(it));
                }
                return items;
            },
            [this](Result<std::vector<Item>> r) {
                if (!r || r->empty()) {
                    status_->setText(tr(L"Ülke listesi alınamadı. Bağlantını kontrol et."));
                    return;
                }
                items_ = std::move(*r);
                status_->setVisible(false);
                rebuild();
            });
    }
    ~CountryList() override { cts_->cancel(); }
    std::function<void(const std::string& code)> onPick;
    float preferredHeight(float) override { return 340; }
    void layout() override {
        scroll_->setRect({0, 0, rect().w, rect().h});
        status_->setRect({4, 0, rect().w - 8, 40});
    }
    void paint(Canvas& c) override {
        c.strokeRounded(rect(), 2, colors().hairDefault);
        paintChildren(c);
    }
    void setFilter(const std::wstring& text) {
        filter_ = foldForSearch(text);
        rebuild();
    }
    void pickFirst() {
        for (const auto& it : items_)
            if (filter_.empty() || it.folded.find(filter_) != std::wstring::npos) {
                pick(it.code);
                return;
            }
    }

private:
    void rebuild() {
        col_->clearChildren();
        for (const auto& it : items_) {
            if (!filter_.empty() && it.folded.find(filter_) == std::wstring::npos) continue;
            auto* b = col_->add<Button>(ButtonKind::SidebarItem, it.name);
            b->setTrailing(i18n::number(it.stations));
            b->setTooltip(i18n::plural(L"{} istasyon", it.stations));
            b->setActive(it.code == current_);
            b->setRect({0, 0, 0, 34});
            const std::string code = it.code;
            b->onClick = [this, code] { pick(code); };
        }
        scroll_->contentChanged();
        requestLayout();
        invalidate();
    }
    void pick(const std::string& code) {
        const auto fn = onPick;   // closes the dialog (this widget goes with it)
        if (fn) fn(code);
    }

    std::string current_;
    std::wstring filter_;
    std::vector<Item> items_;
    ui::ScrollView* scroll_;
    ui::Column* col_;
    ui::Label* status_;
    std::shared_ptr<CancelSource> cts_ = std::make_shared<CancelSource>();
    Lifetime life_;
};

void pickCountry(const std::string& current, std::function<void(const std::string&)> done) {
    auto* d = ui::Dialog::open(ctx().window, tr(L"Ülke seç"), tr(L"Popüler istasyonlar bu ülkeden gösterilir."), 460);
    if (!d) return;
    auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Ülke ara"));
    auto* list = d->body()->add<CountryList>(current);
    box->onChange = [list](const std::wstring& s) { list->setFilter(s); };
    box->onSubmit = [list](const std::wstring&) { list->pickFirst(); };
    list->onPick = [d, done](const std::string& code) {
        d->close();
        if (done) done(code);
    };
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    box->focus();
}

// ---------------------------------------------------------------------------------------------------
// The page

enum class View { Home, Tag, Country, TopClick, TopVote, Favorites, Recent, Genres, Search };

struct ListState {
    enum class Phase { Idle, Loading, Ready, Error };
    std::vector<Station> stations;
    Phase phase = Phase::Idle;
    std::wstring error;
    bool more = false;
    bool loadingMore = false;
    int nextOffset = 0;
    unsigned gen = 0;   // bumped per new query (country / sort / search change): stale answers are dropped
};

std::wstring errorText(const std::string& raw) {
    if (raw.find("timed out") != std::string::npos || raw.find("WinHTTP") != std::string::npos ||
        raw.find("unreachable") != std::string::npos || raw.find("no servers") != std::string::npos)
        return tr(L"radio-browser.info'ya ulaşılamadı. Bağlantını kontrol edip tekrar dene.");
    return tr(L"İstasyonlar yüklenemedi. Biraz sonra tekrar dene.");
}

class RadioPage : public ScrollPage {
public:
    explicit RadioPage(const std::string& id) {
        parseRoute(id);
        radio::store().subscribe(subs_.ref(), [this] { onStoreChanged(); });
        build();
    }
    ~RadioPage() override {
        cts_->cancel();
        if (searchCts_) searchCts_->cancel();
    }

    void paint(Canvas& c) override {
        ScrollPage::paint(c);
        // Search debounce: 400 ms after the last keystroke (like the Ara page).
        if (!pending_.empty() && ui::frame::realNow() >= pendingAt_) {
            const std::wstring q = pending_;
            pending_.clear();
            startSearch(q);
        } else if (!pending_.empty()) {
            if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
        }
    }

private:
    // ---- route -----------------------------------------------------------------------------------------
    void parseRoute(const std::string& id) {
        routeId_ = id;
        auto starts = [&](std::string_view prefix) {
            if (id.rfind(prefix, 0) != 0) return false;
            arg_ = id.substr(prefix.size());
            return true;
        };
        if (starts("tag:")) view_ = View::Tag;
        else if (starts("country:")) view_ = View::Country;
        else if (starts("search:")) view_ = View::Search;
        else if (id == "top") view_ = View::TopClick;
        else if (id == "votes") view_ = View::TopVote;
        else if (id == "favorites") view_ = View::Favorites;
        else if (id == "recent") view_ = View::Recent;
        else if (id == "genres") view_ = View::Genres;
        else view_ = View::Home;
        if ((view_ == View::Tag || view_ == View::Country || view_ == View::Search) && arg_.empty()) view_ = View::Home;
        if (view_ == View::Country) arg_ = toUtf8(toUpperTr(toWide(arg_)));
    }

    std::wstring viewTitle() const {
        switch (view_) {
        case View::Tag: return radio::genreLabel(arg_);
        case View::Country: return radio::countryName(arg_, arg_);
        case View::TopClick: return tr(L"Şu an çok dinlenen");
        case View::TopVote: return tr(L"Dünyada popüler");
        case View::Favorites: return tr(L"Favorilerin");
        case View::Recent: return tr(L"Son dinlediklerin");
        case View::Genres: return tr(L"Tüm türler");
        case View::Search: return i18n::format(tr(L"“{}” için sonuçlar"), {toWide(arg_)});
        default: return tr(L"Radyo");
        }
    }

    std::wstring viewKicker() const {
        switch (view_) {
        case View::Tag: return toUpperTr(tr(L"Radyo · tür"));
        case View::Country: return toUpperTr(tr(L"Radyo · ülke"));
        case View::TopClick: return toUpperTr(tr(L"Radyo · son 24 saat"));
        case View::TopVote: return toUpperTr(tr(L"Radyo · en çok oy alan"));
        case View::Genres: return toUpperTr(tr(L"Radyo · türler"));
        default: return toUpperTr(tr(L"Radyo"));
        }
    }

    // ---- build -----------------------------------------------------------------------------------------
    void build() {
        auto* c = resetContent(24.f);
        buildHeader(c);
        if (view_ == View::Home) {
            buildGenreChips(c);
            body_ = c->add<ui::Column>(gfx::metrics::sectionGap);
            c->setSpacingBefore(body_, 8);
            favSec_ = body_->add<ui::Column>(16.f);
            recentSec_ = body_->add<ui::Column>(16.f);
            countrySec_ = body_->add<ui::Column>(16.f);
            worldSec_ = body_->add<ui::Column>(16.f);
            trendSec_ = body_->add<ui::Column>(16.f);
            offlineSec_ = body_->add<ui::Column>(0.f);
            searchSec_ = body_->add<ui::Column>(16.f);
            offlineSec_->setVisible(false);   // an empty visible section would still take a section gap
            searchSec_->setVisible(false);
            buildStoreSections();
            loadHome();
            refreshFavorites();
        } else {
            body_ = c->add<ui::Column>(16.f);
            buildView();
        }
        contentReady();
    }

    void buildHeader(ui::Column* c) {
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(viewTitle(), view_ == View::Home ? type::displayM : type::displayS);
        title->setVAlign(gfx::VAlign::Center);
        ui::TextBox* box = nullptr;
        Button* back = nullptr;
        if (view_ == View::Home) {
            box = top->add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"İstasyon ara"));
            box->setText(query_);
            box->onChange = [this](const std::wstring& s) { scheduleSearch(s); };
            box->onSubmit = [this](const std::wstring& s) {
                pending_.clear();
                startSearch(s);
            };
            box->onEscape = [box] { box->setText({}, true); };
        } else {
            back = top->add<Button>(ButtonKind::Ghost, tr(L"Radyo"), "arrow-back");
            back->onClick = [] { ctx().router->navigate({RouteKind::Radio}); };
        }
        const bool home = view_ == View::Home;
        top->onPreferredHeight = [home](float) { return home ? 64.f : 56.f; };
        top->onLayout = [title, box, back](ui::Box& b) {
            const float w = b.rect().w, h = b.rect().h;
            float right = w;
            if (box) {
                const float bw = std::clamp(w * 0.4f, 220.f, 380.f);
                box->setRect({w - bw, h * 0.5f - gfx::metrics::searchH * 0.5f, bw, gfx::metrics::searchH});
                right = w - bw - 24;
            }
            if (back) {
                const float bw = back->naturalWidth();
                back->setRect({w - bw, h * 0.5f - 18, bw, 36});
                right = w - bw - 24;
            }
            title->setRect({0, 0, std::max(0.f, right), h});
        };
        auto* kicker = c->add<ui::Label>(home ? toUpperTr(tr(L"Canlı radyo")) + L" · RADIO-BROWSER.INFO" : viewKicker(), type::monoLabel,
                                         ui::Tone::Tertiary);
        c->setSpacingBefore(kicker, 2);
    }

    void buildGenreChips(ui::Column* c) {
        auto* flow = addFlow(c, 32);
        for (const auto& tag : radio::featuredGenres()) {
            auto* chip = flow->add<Button>(ButtonKind::Chip, radio::genreLabel(tag));
            chip->onClick = [tag] { ctx().router->navigate({RouteKind::Radio, "tag:" + tag}); };
        }
        auto* all = flow->add<Button>(ButtonKind::Chip, tr(L"Tüm türler"));
        all->onClick = [] { ctx().router->navigate({RouteKind::Radio, "genres"}); };
    }

    // ---- Home: favorites + recently played (store) ------------------------------------------------------
    void buildStoreSections() {
        auto addTiles = [this](ui::Column* sec, const std::vector<Station>& list, const std::wstring& title, const char* route,
                               bool recent) {
            sec->clearChildren();
            sec->setVisible(!list.empty() && !searching());
            if (list.empty()) return;
            auto* bar = sec->add<SectionBar>(title, std::to_wstring(list.size()));
            const std::string r = route;
            bar->addLink(tr(L"Tümünü gör"), "arrow-forward", [r] { ctx().router->navigate({RouteKind::Radio, r}); });
            auto* grid = sec->add<ui::Grid>(kTileMinW, 12.f, [](float) { return kTileH; }, 1);
            const std::wstring context = title;
            for (size_t i = 0; i < list.size() && i < 12; ++i) {
                const int index = static_cast<int>(i);
                grid->add<StationTile>(list[i], [recent, index, context] {
                    playStations(recent ? radio::store().recent() : radio::store().favorites(), index, context,
                                 recent ? "radio:recent" : "radio:favorites");
                }, recent);
            }
        };
        addTiles(favSec_, radio::store().favorites(), tr(L"Favorilerin"), "favorites", false);
        addTiles(recentSec_, radio::store().recent(), tr(L"Son dinlediklerin"), "recent", true);
        scroll_->contentChanged();
    }

    void onStoreChanged() {
        if (view_ == View::Home) {
            if (radio::store().country() != countryShown_ && !radio::store().country().empty()) {
                loadCountry();
                buildCountrySection();
            }
            buildStoreSections();
        } else if (view_ == View::Favorites || view_ == View::Recent) {
            buildView();
        }
        invalidate();
    }

    // ---- Home: network sections -------------------------------------------------------------------------
    void loadHome() {
        loadCountry();
        fetch(world_, [](const CancellationToken& ct) { return radio::Client::shared().topVote(24, 0, ct); }, [this] { buildWorldSection(); });
        fetch(trend_, [](const CancellationToken& ct) { return radio::Client::shared().topClick(24, 0, ct); }, [this] { buildTrendSection(); });
        buildCountrySection();
        buildWorldSection();
        buildTrendSection();
    }

    void loadCountry() {
        countryShown_ = homeCountry();
        radio::Query q;
        q.countryCode = countryShown_;
        q.limit = 24;
        fetch(country_, [q](const CancellationToken& ct) { return radio::Client::shared().search(q, ct); },
              [this] { buildCountrySection(); });
    }

    // Starts (or restarts) a first-page request for `ls`; `done` rebuilds its section on the UI thread.
    template <class Work>
    void fetch(ListState& ls, Work work, std::function<void()> done) {
        const unsigned gen = ++ls.gen;
        ls.phase = ListState::Phase::Loading;
        ls.stations.clear();
        ls.more = false;
        auto cts = cts_;
        ListState* target = &ls;
        async(
            Priority::High, life_.ref(), [work, cts] { return work(cts->token()); },
            [this, target, gen, done](Result<radio::StationPage> r) {
                if (target->gen != gen) return;
                if (!r) {
                    target->phase = ListState::Phase::Error;
                    target->error = errorText(r.errorMessage());
                    ST_LOG_WARN("iradio", "list failed: {}", r.errorMessage());
                } else {
                    target->phase = ListState::Phase::Ready;
                    target->stations = std::move(r->stations);
                    target->more = r->more;
                    target->nextOffset = r->nextOffset;
                    radio::store().remember(target->stations);
                }
                if (done) done();
                syncOffline();
            });
    }

    void fillSection(ui::Column* sec, ListState& ls, int maxRows, const std::wstring& context, const std::string& uri,
                     std::function<void()> retry) {
        switch (ls.phase) {
        case ListState::Phase::Idle:
        case ListState::Phase::Loading: sec->add<SkeletonRows>(maxRows); break;
        case ListState::Phase::Error: sec->add<InlineError>(ls.error, std::move(retry)); break;
        case ListState::Phase::Ready:
            if (ls.stations.empty()) {
                sec->add<ui::Label>(tr(L"Burada henüz çalınabilir istasyon yok."), type::secondary, ui::Tone::Tertiary);
                break;
            }
            auto* rows = sec->add<RowList>();
            ListState* src = &ls;
            for (int i = 0; i < static_cast<int>(ls.stations.size()) && i < maxRows; ++i)
                rows->add<StationRow>(ls.stations[i], [this, src, i, context, uri] { playStations(src->stations, i, context, uri); });
            break;
        }
        scroll_->contentChanged();
    }

    void buildCountrySection() {
        countrySec_->clearChildren();
        countrySec_->setVisible(!searching() && !allOffline());
        const std::wstring name = radio::countryName(countryShown_, countryShown_);
        auto* bar = countrySec_->add<SectionBar>(i18n::format(tr(L"Popüler · {}"), {name}), toUpperTr(tr(L"En çok dinlenen")));
        const std::string code = countryShown_;
        bar->addLink(tr(L"Tümünü gör"), "arrow-forward", [code] { ctx().router->navigate({RouteKind::Radio, "country:" + code}); });
        bar->addLink(tr(L"Ülke değiştir"), "chevron-down", [this] {
            pickCountry(countryShown_, [](const std::string& picked) {
                radio::store().setCountry(picked);   // the page's onStoreChanged reloads the section
            });
        });
        fillSection(countrySec_, country_, 10, i18n::format(tr(L"Popüler · {}"), {name}), "radio:country:" + code, [this] {
            loadCountry();
            buildCountrySection();
        });
    }

    void buildWorldSection() {
        worldSec_->clearChildren();
        worldSec_->setVisible(!searching() && !allOffline());
        auto* bar = worldSec_->add<SectionBar>(tr(L"Dünyada popüler"), toUpperTr(tr(L"En çok oy alan")));
        bar->addLink(tr(L"Tümünü gör"), "arrow-forward", [] { ctx().router->navigate({RouteKind::Radio, "votes"}); });
        fillSection(worldSec_, world_, 10, tr(L"Dünyada popüler"), "radio:votes", [this] {
            fetch(world_, [](const CancellationToken& ct) { return radio::Client::shared().topVote(24, 0, ct); }, [this] { buildWorldSection(); });
            buildWorldSection();
        });
    }

    void buildTrendSection() {
        trendSec_->clearChildren();
        trendSec_->setVisible(!searching() && !allOffline());
        auto* bar = trendSec_->add<SectionBar>(tr(L"Şu an çok dinlenen"), toUpperTr(tr(L"Son 24 saat")));
        bar->addLink(tr(L"Tümünü gör"), "arrow-forward", [] { ctx().router->navigate({RouteKind::Radio, "top"}); });
        fillSection(trendSec_, trend_, 10, tr(L"Şu an çok dinlenen"), "radio:top", [this] {
            fetch(trend_, [](const CancellationToken& ct) { return radio::Client::shared().topClick(24, 0, ct); }, [this] { buildTrendSection(); });
            buildTrendSection();
        });
    }

    bool allOffline() const {
        return country_.phase == ListState::Phase::Error && world_.phase == ListState::Phase::Error &&
               trend_.phase == ListState::Phase::Error;
    }

    // Nothing from radio-browser.info at all: one offline panel instead of three errors (favorites stay above).
    void syncOffline() {
        if (view_ != View::Home || !offlineSec_) return;
        const bool off = allOffline() && !searching();
        if (off == (offlineSec_->visible() && !offlineSec_->children().empty())) return;
        offlineSec_->clearChildren();
        offlineSec_->setVisible(off);
        for (auto* sec : {countrySec_, worldSec_, trendSec_}) sec->setVisible(!off && !searching());
        if (off)
            offlineSec_->add<MessagePanel>("offline", tr(L"Radyo istasyonlarına ulaşılamadı"),
                                           tr(L"İnternet bağlantını kontrol edip tekrar dene. Favorilerin burada kalır."),
                                           tr(L"Tekrar dene"), [this] {
                                               Dispatcher::post([this, ref = subs_.ref()] {
                                                   if (ref.expired()) return;
                                                   radio::Client::shared().clearCache();
                                                   loadHome();
                                                   syncOffline();
                                               });
                                           });
        scroll_->contentChanged();
    }

    // Favorites carry the data they had when saved: refresh them from the server once per app session.
    void refreshFavorites() {
        static bool done = false;
        if (done || radio::store().favorites().empty()) return;
        done = true;
        std::vector<std::string> uuids;
        for (const auto& s : radio::store().favorites()) uuids.push_back(s.uuid);
        auto cts = cts_;
        async(
            Priority::Low, life_.ref(), [uuids, cts] { return radio::Client::shared().byUuids(uuids, cts->token()); },
            [](Result<std::vector<Station>> r) {
                if (r) radio::store().refresh(*r);
            });
    }

    // ---- search (Home) -----------------------------------------------------------------------------------
    bool searching() const { return !query_.empty(); }

    void scheduleSearch(const std::wstring& q) {
        std::wstring t = q;
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        if (t.empty()) {
            pending_.clear();
            startSearch(L"");
            return;
        }
        if (t.size() < 2) return;
        pending_ = t;
        pendingAt_ = ui::frame::realNow() + 400;
        if (auto* w = window()) w->invalidateAfter(401);
    }

    void startSearch(const std::wstring& raw) {
        std::wstring q = raw;
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        while (!q.empty() && iswspace(q.front())) q.erase(q.begin());
        if (q == query_) return;
        query_ = q;
        if (searchCts_) searchCts_->cancel();
        searchCts_.reset();
        const bool on = searching();
        favSec_->setVisible(on ? false : !radio::store().favorites().empty());
        recentSec_->setVisible(on ? false : !radio::store().recent().empty());
        for (auto* sec : {countrySec_, worldSec_, trendSec_}) sec->setVisible(!on && !allOffline());
        offlineSec_->setVisible(!on && allOffline());
        searchSec_->setVisible(on);
        searchSec_->clearChildren();
        if (!on) {
            ++search_.gen;
            syncOffline();   // the offline panel is not built while a search shows (the sections may have failed meanwhile)
            scroll_->contentChanged();
            return;
        }
        searchCts_ = std::make_shared<CancelSource>();
        radio::Query rq;
        rq.name = toUtf8(q);
        rq.limit = 40;
        const unsigned gen = ++search_.gen;
        search_.phase = ListState::Phase::Loading;
        search_.stations.clear();
        search_.more = false;
        buildSearchSection();
        auto cts = searchCts_;
        async(
            Priority::High, life_.ref(), [rq, cts] { return radio::Client::shared().search(rq, cts->token()); },
            [this, gen](Result<radio::StationPage> r) {
                if (search_.gen != gen) return;
                if (!r) {
                    search_.phase = ListState::Phase::Error;
                    search_.error = errorText(r.errorMessage());
                } else {
                    search_.phase = ListState::Phase::Ready;
                    search_.stations = std::move(r->stations);
                    search_.more = r->more;
                    search_.nextOffset = r->nextOffset;
                    radio::store().remember(search_.stations);
                }
                buildSearchSection();
            });
    }

    void buildSearchSection() {
        searchSec_->clearChildren();
        auto* bar = searchSec_->add<SectionBar>(i18n::format(tr(L"“{}” için sonuçlar"), {query_}),
                                                search_.phase == ListState::Phase::Ready ? std::to_wstring(search_.stations.size()) : L"");
        if (search_.more) {   // the full list pages on ("Daha fazla yükle") and sorts by votes too
            const std::string q = toUtf8(query_);
            bar->addLink(tr(L"Tümünü gör"), "arrow-forward", [q] { ctx().router->navigate({RouteKind::Radio, "search:" + q}); });
        }
        if (search_.phase == ListState::Phase::Ready && search_.stations.empty()) {
            searchSec_->add<MessagePanel>("search", tr(L"İstasyon bulunamadı"), tr(L"Başka bir ad dene ya da bir türe göz at."));
            scroll_->contentChanged();
            return;
        }
        const std::wstring context = i18n::format(tr(L"“{}” için sonuçlar"), {query_});
        fillSection(searchSec_, search_, 40, context, "radio:search", [this] {
            const std::wstring q = query_;
            query_.clear();
            startSearch(q);
        });
    }

    // ---- list views ------------------------------------------------------------------------------------------
    void buildView() {
        body_->clearChildren();
        loadMore_ = nullptr;
        rows_ = nullptr;
        switch (view_) {
        case View::Favorites:
        case View::Recent: {
            const bool recent = view_ == View::Recent;
            const auto& list = recent ? radio::store().recent() : radio::store().favorites();
            if (list.empty()) {
                if (recent)
                    body_->add<MessagePanel>("radio", tr(L"Henüz radyo dinlemedin"), tr(L"Dinlediğin istasyonlar burada görünür."));
                else
                    body_->add<MessagePanel>("heart", tr(L"Henüz favori istasyonun yok"),
                                             tr(L"Bir istasyonun kalbine tıkla; favorilerin burada toplanır."));
                break;
            }
            body_->add<ui::Label>(i18n::plural(L"{} istasyon", static_cast<long long>(list.size())), type::secondary, ui::Tone::Tertiary);
            auto* grid = body_->add<ui::Grid>(kTileMinW, 12.f, [](float) { return kTileH; });
            const std::wstring context = viewTitle();
            for (size_t i = 0; i < list.size(); ++i) {
                const int index = static_cast<int>(i);
                grid->add<StationTile>(list[i], [recent, index, context] {
                    playStations(recent ? radio::store().recent() : radio::store().favorites(), index, context,
                                 recent ? "radio:recent" : "radio:favorites");
                }, recent);
            }
            break;
        }
        case View::Genres: buildGenres(); break;
        default: buildList(); break;
        }
        scroll_->contentChanged();
    }

    void buildGenres() {
        if (genres_.empty() && !genresFailed_) {
            body_->add<SkeletonRows>(6);
            async(
                Priority::High, life_.ref(), [cts = cts_] { return radio::Client::shared().tags(120, cts->token()); },
                [this](Result<std::vector<radio::Tag>> r) {
                    if (r) {
                        // Only tags that carry a real list (a handful of stations under a typo is noise).
                        for (auto& t : *r)
                            if (t.stationCount >= 25) genres_.push_back(std::move(t));
                    }
                    genresFailed_ = !r || genres_.empty();
                    buildView();
                });
            return;
        }
        if (genresFailed_) {
            body_->add<MessagePanel>("offline", tr(L"Türler yüklenemedi"), tr(L"İnternet bağlantını kontrol edip tekrar dene."),
                                     tr(L"Tekrar dene"), [this] {
                                         Dispatcher::post([this, ref = subs_.ref()] {
                                             if (ref.expired()) return;
                                             genresFailed_ = false;
                                             buildView();
                                         });
                                     });
            return;
        }
        body_->add<ui::Label>(tr(L"radio-browser.info'daki en yaygın etiketler, istasyon sayısına göre."), type::secondary,
                              ui::Tone::Tertiary);
        // Genres we know (translated) first, then the other popular tags minus the ones that say nothing ("radio").
        static const std::string_view kNoise[] = {"music",  "radio",   "fm",      "am",     "estación", "música",
                                                  "musica", "webradio", "local",  "local radio", "internet radio",
                                                  "online radio", "entretenimiento", "entertainment", "information",
                                                  "variety", "full service", "regional"};
        auto addChips = [this](ui::Box* flow, bool genres) {
            for (const auto& t : genres_) {
                if (radio::isGenre(t.name) != genres) continue;
                if (!genres && std::find(std::begin(kNoise), std::end(kNoise), t.name) != std::end(kNoise)) continue;
                auto* chip = flow->add<Button>(ButtonKind::Chip, radio::genreLabel(t.name));
                chip->setTooltip(i18n::plural(L"{} istasyon", t.stationCount));
                const std::string tag = t.name;
                chip->onClick = [tag] { ctx().router->navigate({RouteKind::Radio, "tag:" + tag}); };
            }
        };
        auto* known = body_->add<SectionBar>(tr(L"Türler"), L"");
        body_->setSpacingBefore(known, 24);
        addChips(addFlow(body_, 32), true);
        auto* other = body_->add<SectionBar>(tr(L"Diğer etiketler"), L"");
        body_->setSpacingBefore(other, 32);
        addChips(addFlow(body_, 32), false);
    }

    radio::Query listQuery(int offset) const {
        radio::Query q;
        q.limit = 40;
        q.offset = offset;
        q.order = byVotes_ ? radio::Query::Order::Votes : radio::Query::Order::ClickCount;
        if (view_ == View::Tag) q.tag = arg_;
        if (view_ == View::Country) q.countryCode = arg_;
        if (view_ == View::Search) q.name = arg_;
        return q;
    }

    // Worker thread: everything it needs is passed by value (the page may be gone by the time it runs).
    static radio::StationPage fetchPage(View view, const radio::Query& q, const CancellationToken& ct) {
        switch (view) {
        case View::TopClick: return radio::Client::shared().topClick(q.limit, q.offset, ct);
        case View::TopVote: return radio::Client::shared().topVote(q.limit, q.offset, ct);
        default: return radio::Client::shared().search(q, ct);
        }
    }

    void buildList() {
        const bool sortable = view_ == View::Tag || view_ == View::Country || view_ == View::Search;
        if (sortable) {
            auto* tabs = body_->add<ui::Box>();
            auto* clicks = tabs->add<Button>(ButtonKind::Tab, tr(L"En çok dinlenen"));
            auto* votes = tabs->add<Button>(ButtonKind::Tab, tr(L"En çok oy alan"));
            clicks->setActive(!byVotes_);
            votes->setActive(byVotes_);
            clicks->onClick = [this] { setSort(false); };
            votes->onClick = [this] { setSort(true); };
            tabs->onPreferredHeight = [](float) { return 37.f; };
            tabs->onLayout = [clicks, votes](ui::Box&) {
                clicks->setRect({0, 0, clicks->naturalWidth(), 36});
                votes->setRect({clicks->naturalWidth() + 24, 0, votes->naturalWidth(), 36});
            };
            tabs->onPaint = [](ui::Box& b, Canvas& cv) { cv.hline(b.rect().x, b.rect().right(), b.rect().bottom() - 1, colors().hairDefault); };
        }
        if (list_.phase == ListState::Phase::Idle) {
            const unsigned gen = ++list_.gen;
            list_.phase = ListState::Phase::Loading;
            auto cts = cts_;
            async(
                Priority::High, life_.ref(), [view = view_, q = listQuery(0), cts] { return fetchPage(view, q, cts->token()); },
                [this, gen](Result<radio::StationPage> r) {
                    if (list_.gen != gen) return;
                    if (!r) {
                        list_.phase = ListState::Phase::Error;
                        list_.error = errorText(r.errorMessage());
                    } else {
                        list_.phase = ListState::Phase::Ready;
                        list_.stations = std::move(r->stations);
                        list_.more = r->more;
                        list_.nextOffset = r->nextOffset;
                        radio::store().remember(list_.stations);
                    }
                    buildView();
                });
        }
        switch (list_.phase) {
        case ListState::Phase::Idle:
        case ListState::Phase::Loading: body_->add<SkeletonRows>(12); return;
        case ListState::Phase::Error:
            body_->add<MessagePanel>("offline", tr(L"İstasyonlar yüklenemedi"), list_.error, tr(L"Tekrar dene"), [this] {
                Dispatcher::post([this, ref = subs_.ref()] {
                    if (ref.expired()) return;
                    list_.phase = ListState::Phase::Idle;
                    buildView();
                });
            });
            return;
        case ListState::Phase::Ready: break;
        }
        if (list_.stations.empty()) {
            body_->add<MessagePanel>("radio", tr(L"İstasyon bulunamadı"), tr(L"Bu listede şu an çalınabilir istasyon yok."));
            return;
        }
        rows_ = body_->add<RowList>();
        for (int i = 0; i < static_cast<int>(list_.stations.size()); ++i) addListRow(i);
        auto* foot = body_->add<ui::Box>();
        loadMore_ = foot->add<Button>(ButtonKind::Secondary, tr(L"Daha fazla yükle"), "chevron-down");
        loadMore_->onClick = [this] { loadMore(); };
        foot->hitTestVisible = false;
        foot->onPreferredHeight = [](float) { return 56.f; };
        foot->onLayout = [b = loadMore_](ui::Box& box) {
            const float w = b->naturalWidth();
            b->setRect({box.rect().w * 0.5f - w * 0.5f, 8, w, 40});
        };
        foot->setVisible(list_.more);
    }

    void addListRow(int i) {
        rows_->add<StationRow>(list_.stations[i], [this, i] { playStations(list_.stations, i, viewTitle(), "radio:" + routeId_); });
    }

    void setSort(bool votes) {
        if (votes == byVotes_) return;
        byVotes_ = votes;
        const unsigned gen = list_.gen + 1;   // drops a load-more still in flight
        list_ = ListState{};
        list_.gen = gen;
        Dispatcher::post([this, ref = subs_.ref()] {   // the clicked tab is rebuilt with the list
            if (!ref.expired()) buildView();
        });
    }

    void loadMore() {
        if (list_.loadingMore || !list_.more) return;
        list_.loadingMore = true;
        if (loadMore_) {
            loadMore_->setLabel(tr(L"Yükleniyor…"));
            loadMore_->setEnabled(false);
        }
        const unsigned gen = list_.gen;
        const int offset = list_.nextOffset;
        auto cts = cts_;
        async(
            Priority::High, life_.ref(), [view = view_, q = listQuery(offset), cts] { return fetchPage(view, q, cts->token()); },
            [this, gen](Result<radio::StationPage> r) {
                if (list_.gen != gen) return;
                list_.loadingMore = false;
                if (!r) {
                    toast(errorText(r.errorMessage()), true);
                } else {
                    // Pages can overlap when the ranking moved in between: skip stations already listed.
                    const int before = static_cast<int>(list_.stations.size());
                    for (auto& s : r->stations)
                        if (std::none_of(list_.stations.begin(), list_.stations.end(), [&](const Station& o) { return o.uuid == s.uuid; }))
                            list_.stations.push_back(std::move(s));
                    list_.more = r->more;
                    list_.nextOffset = r->nextOffset;
                    radio::store().remember(list_.stations);
                    if (rows_)
                        for (int i = before; i < static_cast<int>(list_.stations.size()); ++i) addListRow(i);
                }
                if (loadMore_) {
                    loadMore_->setLabel(tr(L"Daha fazla yükle"));
                    loadMore_->setEnabled(true);
                    if (auto* foot = loadMore_->parent()) foot->setVisible(list_.more);
                }
                scroll_->contentChanged();
            });
    }

    // ---- playback --------------------------------------------------------------------------------------------
    // The list is the context: next / previous switch stations.
    static void playStations(const std::vector<Station>& list, int index, const std::wstring& name, const std::string& uri) {
        if (index < 0 || index >= static_cast<int>(list.size()) || !ctx().player) return;
        const std::vector<Station> copy = list;   // `list` may be the store's, which playing changes
        radio::store().rememberQueue(copy, static_cast<size_t>(index));
        ctx().player->playContext(radio::toTracks(copy), index, {uri, name});
    }

    View view_ = View::Home;
    std::string routeId_, arg_;
    ui::Column* body_ = nullptr;
    ui::Column *favSec_ = nullptr, *recentSec_ = nullptr, *countrySec_ = nullptr, *worldSec_ = nullptr, *trendSec_ = nullptr;
    ui::Column *offlineSec_ = nullptr, *searchSec_ = nullptr;
    RowList* rows_ = nullptr;
    Button* loadMore_ = nullptr;
    ListState country_, world_, trend_, search_, list_;
    std::vector<radio::Tag> genres_;
    bool genresFailed_ = false;
    bool byVotes_ = false;
    std::string countryShown_;
    std::wstring query_, pending_;
    double pendingAt_ = 0;
    std::shared_ptr<CancelSource> cts_ = std::make_shared<CancelSource>();
    std::shared_ptr<CancelSource> searchCts_;
    Lifetime subs_;   // store subscription + posted rebuilds (life_ guards the requests)
};

} // namespace

// ===================================================================================================

void showStationMenu(const catalog::Track& track, gfx::Point windowPos) {
    const Station* s = radio::store().find(radio::uuidOf(track.id));
    if (!s || !ctx().window) return;
    ui::Menu::open(ctx().window, windowPos, stationMenuItems(*s));
}

const std::vector<HeardTitle>& heardTitles() { return liveState().heard; }

void initInternetRadio() {
    radio::Client::shared().setUserAgent("ShadeTube/" + updater::currentVersion() + " (+https://github.com/shadesofdeath/ShadeTube)");
    // Dev: SHADETUBE_RADIO_SERVERS=host[,host...] replaces the server discovery (an unreachable host shows the
    // offline states).
    if (wchar_t env[512]; GetEnvironmentVariableW(L"SHADETUBE_RADIO_SERVERS", env, 512) > 0) {
        std::vector<std::string> hosts;
        std::wstring list = env;
        for (size_t start = 0; start <= list.size();) {
            const size_t comma = std::min(list.find(L',', start), list.size());
            if (comma > start) hosts.push_back(toUtf8(list.substr(start, comma - start)));
            start = comma + 1;
        }
        ST_LOG_INFO("iradio", "servers from SHADETUBE_RADIO_SERVERS: {}", hosts.size());
        radio::Client::shared().setServers(std::move(hosts));
    }
    radio::store().load(paths::appData() / L"radio.json");
    auto* p = ctx().player;
    if (!p) return;
    // Stations play as live streams. The registry knows every station the UI listed and the stations of the last
    // played list (radio.json), so a restored queue resolves too. An unknown one gets an empty URL: it fails like a
    // dead stream instead of being matched on YouTube by its name.
    p->liveStreamFor = [](const catalog::Track& t) -> std::optional<player::Player::LiveStream> {
        if (!radio::isStationId(t.id)) return std::nullopt;
        if (const Station* s = radio::store().find(radio::uuidOf(t.id))) return player::Player::LiveStream{radio::streamUrl(*s), radio::mimeType(*s)};
        ST_LOG_WARN("iradio", "unknown station {}", t.id);
        return player::Player::LiveStream{};
    };
    ctx().playerChangedHooks.push_back([] { onPlayerChanged(); });
    ctx().housekeepingHooks.push_back([] { radio::store().saveIfDirty(); });
    ctx().persistHooks.push_back([] { radio::store().saveIfDirty(); });
    // Hearts in the player bar / mini player follow favorites changed on the page.
    static auto owner = std::make_shared<char>();
    radio::store().subscribe(owner, [] {
        Dispatcher::post([] {
            if (auto* p2 = ctx().player; p2 && p2->onChanged) p2->onChanged();
        });
    });
}

std::unique_ptr<Page> makeRadioPage(const std::string& id) { return std::make_unique<RadioPage>(id); }

} // namespace st::app
