// Search, Library and Artist pages. Each reads Spotify when logged in and MusicBrainz otherwise (see Source.h).
#include "app/Blacklist.h"
#include "app/DownloadSync.h"
#include "app/LinkOpener.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SmartListsPage.h"
#include "app/PlaylistTransfer.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;

namespace {

// Row of tab buttons with the accent underline.
class Tabs : public ui::Widget {
public:
    explicit Tabs(std::vector<std::wstring> labels) {
        hitTestVisible = false;
        for (size_t i = 0; i < labels.size(); ++i) {
            auto* b = add<Button>(ButtonKind::Tab, labels[i]);
            b->onClick = [this, i] { select(static_cast<int>(i)); };
            buttons_.push_back(b);
        }
        buttons_[0]->setActive(true);
    }
    std::function<void(int)> onSelect;
    void select(int i, bool notify = true) {
        for (size_t k = 0; k < buttons_.size(); ++k) buttons_[k]->setActive(static_cast<int>(k) == i);
        if (notify && onSelect) onSelect(i);
    }
    float preferredHeight(float) override { return 37; }
    void layout() override {
        float x = 0;
        for (auto* b : buttons_) {
            const float w = b->naturalWidth();
            b->setRect({x, 0, w, 36});
            x += w + 24;
        }
    }
    void paint(Canvas& c) override {
        c.hline(rect().x, rect().right(), rect().bottom() - 1, colors().hairDefault);
        paintChildren(c);
    }

private:
    std::vector<Button*> buttons_;
};

std::wstring albumSubtitle(const Album& a) {
    std::wstring s = toWide(a.year());
    const std::wstring type = a.primaryType == "Single" ? tr(L"Tekli") : a.primaryType == "EP" ? L"EP" : tr(L"Albüm");
    s = s.empty() ? type : s + L" · " + type;
    if (!a.artists.empty()) s += L" · " + toWide(a.artists[0].name);
    return s;
}

MediaCard* addAlbumCard(ui::Grid* grid, const Album& a, bool showArtist = true) {
    std::wstring sub = showArtist ? albumSubtitle(a) : toWide(a.year());
    auto* card = grid->add<MediaCard>(toWide(a.name), sub, a.images);
    const std::string id = a.id;
    card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
    card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    card->onContext = [id, offline = sync::Target{sync::Kind::Album, a.id, a.name, a.images}](gfx::Point wp) {
        ui::Menu::open(ctx().window, wp,
                       {{tr(L"Aç"), "arrow-up-right", L"", [id] { ctx().router->navigate({RouteKind::Album, id}); }},
                        {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); }},
                        sync::menuItem(offline),
                        transfer::exportMenuItem(transfer::What::Album, offline.id, offline.name)});
    };
    return card;
}

// Open / play / keep offline / export, for playlist cards (Liked Songs included).
void setPlaylistCardMenu(MediaCard* card, Route open, sync::Target offline) {
    card->onContext = [open, offline](gfx::Point wp) {
        Route play = open;
        play.id += "#play";
        const auto what = offline.kind == sync::Kind::Liked ? transfer::What::Liked : transfer::What::Playlist;
        const std::string name = offline.kind == sync::Kind::Liked ? toUtf8(tr(L"Beğenilen Şarkılar")) : offline.name;
        ui::Menu::open(ctx().window, wp,
                       {{tr(L"Aç"), "arrow-up-right", L"", [open] { ctx().router->navigate(open); }},
                        {tr(L"Çal"), "play", L"", [play] { ctx().router->navigate(play); }},
                        sync::menuItem(offline),
                        transfer::exportMenuItem(what, offline.id, name)});
    };
}

MediaCard* addArtistCard(ui::Grid* grid, const Artist& a) {
    std::wstring sub = a.type == "Group" ? tr(L"Grup") : tr(L"Sanatçı");
    if (!a.country.empty()) sub += L" · " + toWide(a.country);
    auto* card = grid->add<MediaCard>(toWide(a.name), sub, a.images, MediaCard::Shape::Circle, Placeholder::Artist);
    const std::string id = a.id;
    card->onOpen = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
    return card;
}

// ===================================================================================================
// Son aramalar: the last queries the user really searched for (submitted with Enter, or a result opened from them),
// newest first, max 10, case-insensitively unique. %LOCALAPPDATA%\ShadeTube\recent-searches.json — a few hundred
// bytes, read once on first use and rewritten (write-then-rename) on every change. UI thread only.

class RecentSearches {
public:
    static constexpr size_t kMax = 10;
    static RecentSearches& get() {
        static RecentSearches instance;
        return instance;
    }
    const std::vector<std::wstring>& items() {
        load();
        return items_;
    }
    void record(const std::wstring& query) {
        const std::wstring q = normalize(query);
        if (q.empty()) return;
        load();
        std::erase_if(items_, [&](const std::wstring& s) { return sameQuery(s, q); });
        items_.insert(items_.begin(), q);
        if (items_.size() > kMax) items_.resize(kMax);
        save();
    }
    void remove(const std::wstring& query) {
        load();
        if (std::erase_if(items_, [&](const std::wstring& s) { return sameQuery(s, query); }) > 0) save();
    }
    void clear() {
        load();
        items_.clear();
        save();
    }

private:
    static std::filesystem::path file() { return paths::appData() / L"recent-searches.json"; }
    // Trimmed, inner whitespace runs collapsed, capped (a pasted paragraph must not become a chip).
    static std::wstring normalize(const std::wstring& s) {
        std::wstring out;
        for (wchar_t ch : s) {
            if (std::iswspace(ch)) {
                if (!out.empty() && out.back() != L' ') out += L' ';
            } else {
                out += ch;
            }
        }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        if (out.size() > 120) out.resize(120);
        return out;
    }
    // Case-insensitive, with Turkish İ treated as I/i (the ordinal table leaves U+0130 alone); ı stays its own letter.
    static bool sameQuery(std::wstring a, std::wstring b) {
        std::replace(a.begin(), a.end(), L'\u0130', L'i');
        std::replace(b.begin(), b.end(), L'\u0130', L'i');
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
    }
    void load() {
        if (loaded_) return;
        loaded_ = true;
        std::ifstream f(file(), std::ios::binary);
        if (!f) return;
        const auto j = nlohmann::json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("queries") || !j["queries"].is_array()) return;
        for (const auto& q : j["queries"]) {
            if (!q.is_string()) continue;
            const std::wstring s = normalize(toWide(q.get<std::string>()));
            if (!s.empty() && items_.size() < kMax &&
                std::none_of(items_.begin(), items_.end(), [&](const std::wstring& x) { return sameQuery(x, s); }))
                items_.push_back(s);
        }
    }
    void save() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& q : items_) list.push_back(toUtf8(q));
        const nlohmann::json j{{"v", 1}, {"queries", std::move(list)}};
        auto tmp = file();
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << j.dump();
            if (!f) {
                ST_LOG_WARN("search", "failed to write recent-searches.json");
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file(), ec);
    }

    std::vector<std::wstring> items_;
    bool loaded_ = false;
};

// A recent-search chip (spec Chip: h 30, padX 12, B 12/600, hairline.control, close icon 12 gap 6). Click / Enter
// searches again; the small ✕ is its own Tab stop, and Delete on the focused chip removes it too.
class RecentChip : public ui::Widget {
public:
    static constexpr float kH = 30, kMaxW = 320;
    explicit RecentChip(std::wstring query) : query_(std::move(query)), label_(query_, type::caption.withWeight(600)) {
        focusable = true;
        remove_ = add<Button>(ButtonKind::Icon, L"", "close");
        remove_->setIconSize(12);
        remove_->setTooltip(tr(L"Aramayı kaldır"));
        remove_->onClick = [this] {
            const auto fn = onRemove;   // rebuilds the list (this chip dies)
            if (fn) fn();
        };
    }
    std::function<void()> onOpen, onRemove;
    const std::wstring& query() const { return query_; }
    Button* removeButton() const { return remove_; }

    float naturalWidth() { return std::min(kMaxW, 12 + std::ceil(label_.measure().w) + 6 + 12 + 12); }
    void layout() override {
        const Rect r = rect();
        // 22 px hit circle around the 12 px icon, which sits 12 px from the right edge.
        remove_->setRect({r.w - 12 - 6 - 11, (r.h - 22) * 0.5f, 22, 22});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float h = hover_;
        c.fillPill(r, col.overlayHover.mulAlpha(h));
        c.strokePill(r, Color::lerp(col.hairControl, col.fgSecondary.withAlpha(0.5f), h * 0.6f));
        c.text(label_, {r.x + 12, r.y, r.w - 12 - 6 - 12 - 12, r.h}, Color::lerp(col.fgSecondary, col.fgPrimary, h),
               gfx::VAlign::Center);
        paintChildren(c);
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (hovered() && rect().contains(e.pos)) open();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        open();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_DELETE || !onRemove) return false;
        const auto fn = onRemove;
        fn();
        return true;
    }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }
    std::wstring tooltip() const override { return label_.isTrimmed(rect().w - 42) ? query_ : std::wstring(); }

private:
    void open() {
        const auto fn = onOpen;   // runs the search (this chip dies)
        if (fn) fn();
    }
    std::wstring query_;
    mutable gfx::Text label_;
    Button* remove_;
    ui::Anim hover_;
};

// The row under the search box while its text is a link (app/Links): what the link is and what opening it does.
// Enter in the box or a click opens it; a Spotify link while logged out offers the connect screen instead; an
// unsupported link only explains itself.
class LinkHint : public ui::Widget {
public:
    static constexpr float kH = 72, kMaxW = 640;
    explicit LinkHint(links::Link link) : link_(std::move(link)) {
        const bool unsupported = link_.kind == links::Kind::Unsupported;
        const bool connect = linkNeedsSpotify(link_);
        title_ = gfx::Text(unsupported ? tr(L"Bu bağlantı açılamıyor")
                           : connect   ? tr(L"Spotify'a bağlan")
                                       : linkAction(link_),
                           type::body);
        sub_ = gfx::Text(unsupported ? tr(L"Şarkı, albüm, çalma listesi ve sanatçı bağlantıları desteklenir.")
                         : connect   ? i18n::format(tr(L"{} · Açmak için Spotify hesabınla bağlan"), {linkLabel(link_)})
                                     : linkLabel(link_),
                         type::secondary);
        key_ = gfx::Text(L"ENTER", type::monoMeta);
        focusable = !unsupported;
    }
    float preferredHeight(float) override { return kH; }
    void paint(Canvas& c) override {
        const Rect r = box();
        const auto& col = colors();
        const bool active = focusable;
        c.fillRounded(r, 2, col.bgRaised);
        if (active) c.fillRounded(r, 2, col.overlayHover.mulAlpha(hover_));
        c.strokeRounded(r, 2, col.hairDefault);
        const Rect icon{r.x + 20, r.cy() - 12, 24, 24};
        c.icon(linkIcon(link_), icon, active ? accent().base : col.fgTertiary);
        const float x = icon.right() + 16;
        const float right = r.right() - (active ? 20 + 16 + 12 + key_.measure().w + 12 : 20);
        c.text(title_, {x, r.y + 16, right - x, 20}, active ? col.fgPrimary : col.fgSecondary, gfx::VAlign::Center);
        c.text(sub_, {x, r.y + 38, right - x, 18}, col.fgSecondary, gfx::VAlign::Center);
        if (active) {
            const Rect arrow{r.right() - 20 - 16, r.cy() - 8, 16, 16};
            c.icon("arrow-up-right", arrow, Color::lerp(col.fgTertiary, col.fgPrimary, hover_));
            const float kw = key_.measure().w;
            c.text(key_, {arrow.x - 12 - kw, r.y, kw, r.h}, col.fgTertiary, gfx::VAlign::Center);
        }
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return focusable && e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (focusable && box().contains(e.pos)) open();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return focusable ? IDC_HAND : IDC_ARROW; }
    bool activatable() const override { return focusable; }
    bool onActivate() override {
        open();
        return true;
    }
    Rect focusRect() const override { return box(); }
    // Opens the link, or the Spotify connect screen when the link needs a session.
    static void activate(const links::Link& link) {
        if (linkNeedsSpotify(link) && ctx().showConnect) ctx().showConnect(true);
        else openLink(link);
    }

private:
    Rect box() const {
        const Rect r = rect();
        return {r.x, r.y, std::min(r.w, kMaxW), r.h};
    }
    void open() {
        const links::Link link = link_;   // opening navigates (this row dies)
        activate(link);
    }
    links::Link link_;
    gfx::Text title_, sub_, key_;
    ui::Anim hover_;
};

// ===================================================================================================
// Search

struct Genre {
    const wchar_t* title;   // already translated (tr() pointers live for the whole process)
    const char* tag;
    uint32_t color;
};
// A function, not a constant table: tr() only works after i18n::init (main), i.e. after static initialization.
std::array<Genre, 16> genreList() {
    return {{
        {tr(L"Türkçe Pop"), "turkish pop", 0x3B2A1E},   {tr(L"Anadolu Rock"), "anatolian rock", 0x1F2A22},
        {tr(L"Arabesk"), "arabesque", 0x2E1D1D},       {tr(L"Türk Halk Müziği"), "turkish folk", 0x33261B},
        {tr(L"Rock"), "rock", 0x1C2433},               {tr(L"Hip Hop"), "hip hop", 0x2A1E2A},
        {tr(L"Elektronik"), "electronic", 0x1E2B2B},   {tr(L"Caz"), "jazz", 0x26241C},
        {tr(L"Klasik"), "classical", 0x2B2433},        {tr(L"Metal"), "metal", 0x1B1B1B},
        {tr(L"Indie"), "indie", 0x23301F},             {tr(L"R&B"), "rnb", 0x331E27},
        {tr(L"Lo-fi"), "lo-fi", 0x1F2630},             {tr(L"Ambient"), "ambient", 0x1E2A30},
        {tr(L"Soul"), "soul", 0x30241C},               {tr(L"Blues"), "blues", 0x1C2233},
    }};
}

class SearchPage : public ScrollPage {
public:
    explicit SearchPage(std::string initial) {
        box_ = add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Ne dinlemek istersin?  Sanatçı, albüm, şarkı"));
        adopt(release(scroll_));   // Tab order: the search box first, then "Son aramalar" / the results below it
        box_->onChange = [this](const std::wstring& s) {
            // A pasted / typed link: offer to open it instead of searching for a URL.
            if (auto link = links::parse(std::wstring_view(s))) return showLink(std::move(link));
            if (linkShown_ && s.size() < 2) {   // too short to search: back to the browse view
                linkShown_ = false;
                if (!s.empty()) showBrowse();
            }
            scheduleSearch(s);
        };
        box_->onSubmit = [this](const std::wstring& s) {
            pending_.clear();
            if (auto link = links::parse(std::wstring_view(s))) {
                LinkHint::activate(link);   // may navigate away (this page dies)
                return;
            }
            RecentSearches::get().record(s);   // Enter = a real search (typing alone never records)
            runSearch(s);
        };
        box_->setText(toWide(initial));
        showBrowse();
        if (auto link = links::parse(std::string_view(initial))) showLink(std::move(link));
        else if (!initial.empty()) runSearch(toWide(initial));
    }

    void onShown() override { box_->focus(); }

    void layout() override {
        const Rect r = rect();
        const float w = std::min(640.f, r.w - gfx::metrics::pageX * 2);
        box_->setRect({gfx::metrics::pageX, 28, w, gfx::metrics::searchH});
        scroll_->setRect({0, 28 + gfx::metrics::searchH + 12, r.w, r.h - (28 + gfx::metrics::searchH + 12)});
    }

    void paint(Canvas& c) override {
        ScrollPage::paint(c);
        // Debounce: search 450 ms after the last keystroke (MusicBrainz allows 1 request/second).
        if (!pending_.empty() && ui::frame::realNow() >= pendingAt_) {
            auto q = pending_;
            pending_.clear();
            runSearch(q);
        } else if (!pending_.empty()) {
            if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
        }
    }

private:
    void showLink(links::Link link) {
        pending_.clear();
        query_.clear();
        life_.renew();
        linkShown_ = true;
        auto* c = resetContent(20.f);
        c->setPadding(gfx::metrics::pageX, 20, 48);
        c->add<LinkHint>(std::move(link));
        contentReady();
    }

    void scheduleSearch(const std::wstring& q) {
        if (q.empty()) {
            pending_.clear();
            query_.clear();
            life_.renew();
            showBrowse();
            return;
        }
        if (q.size() < 2) return;
        pending_ = q;
        pendingAt_ = ui::frame::realNow() + 450;
        if (auto* w = window()) w->invalidateAfter(451);
    }

    // Empty query: "Son aramalar" (when there are any) above the genre tiles. `focusChip` >= 0: a chip was removed
    // from the keyboard -> keep the focus ring on the chip now at that index (else on the search box).
    void showBrowse(int focusChip = -1) {
        linkShown_ = false;
        auto* c = resetContent(20.f);
        c->setPadding(gfx::metrics::pageX, 20, 48);
        std::vector<RecentChip*> chips;
        if (const auto& recent = RecentSearches::get().items(); !recent.empty()) {
            // Header like SectionHeader, with a plain "Temizle" link (no arrow: it is not a navigation).
            auto* head = c->add<ui::Box>();
            auto* clear = head->add<Button>(ButtonKind::Link, tr(L"Temizle"));
            clear->setTooltip(tr(L"Son aramaları temizle"));
            clear->onClick = [this, clear] {
                const bool keyboard = clear->keyboardFocused();
                RecentSearches::get().clear();
                rebuildBrowse(-1, keyboard);
            };
            head->hitTestVisible = false;
            head->onPreferredHeight = [](float) { return 48.f; };
            head->onLayout = [clear](ui::Box& b) {
                const float w = clear->naturalWidth();
                clear->setRect({b.rect().w - w, 0, w, 36});
            };
            head->onPaint = [](ui::Box& b, Canvas& cv) {
                const Rect r = b.rect();
                cv.text(tr(L"Son aramalar"), type::title, {r.x, r.y, r.w * 0.7f, 36}, colors().fgPrimary,
                        gfx::TextAlign::Leading, gfx::VAlign::Center);
                cv.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
            };
            // Wrapping row of chips (8 px gaps).
            auto* flow = c->add<ui::Box>();
            flow->hitTestVisible = false;
            for (size_t i = 0; i < recent.size(); ++i) {
                auto* chip = flow->add<RecentChip>(recent[i]);
                const std::wstring q = recent[i];
                const int index = static_cast<int>(i);
                chip->onOpen = [this, q] {
                    Dispatcher::post([this, q, ref = life_.ref()] {
                        if (ref.expired()) return;
                        box_->setText(q);
                        pending_.clear();
                        RecentSearches::get().record(q);   // searched again: move it to the front
                        runSearch(q);
                    });
                };
                chip->onRemove = [this, q, index, chip] {
                    const bool keyboard = chip->keyboardFocused() || chip->removeButton()->keyboardFocused();
                    RecentSearches::get().remove(q);
                    rebuildBrowse(keyboard ? index : -1, keyboard);
                };
                chips.push_back(chip);
            }
            auto flowLayout = [](ui::Box& b, float width, bool place) {
                float x = 0, y = 0;
                for (const auto& child : b.children()) {
                    auto* chip = static_cast<RecentChip*>(child.get());
                    const float w = std::min(chip->naturalWidth(), width);
                    if (x > 0 && x + w > width) {
                        x = 0;
                        y += RecentChip::kH + 8;
                    }
                    if (place) chip->setRect({x, y, w, RecentChip::kH});
                    x += w + 8;
                }
                return b.children().empty() ? 0.f : y + RecentChip::kH;
            };
            flow->onLayout = [flowLayout](ui::Box& b) { flowLayout(b, b.rect().w, true); };
            flow->onPreferredHeight = [flow, flowLayout](float w) { return flowLayout(*flow, w, false); };
            c->setSpacingBefore(flow, 12);
        }
        auto* genres = c->add<SectionHeader>(tr(L"Türlere göz at"), tr(L"MUSICBRAINZ ETİKETLERİ"));
        if (!chips.empty()) c->setSpacingBefore(genres, gfx::metrics::sectionGap);
        auto* grid = c->add<ui::Grid>(200.f, 12.f, [](float) { return 120.f; });
        int i = 0;
        for (const auto& g : genreList()) {
            wchar_t idx[8];
            swprintf(idx, 8, L"%02d", ++i);
            auto* t = grid->add<Tile>(idx, g.title, Color::rgb(g.color));
            const std::wstring title = g.title;
            const std::string tag = g.tag;
            t->onClick = [this, title, tag] { runTagSearch(title, tag); };
        }
        scroll_->contentChanged();
        if (focusChip >= 0) {
            if (auto* w = window()) {
                if (chips.empty()) w->focusByKeyboard(box_);
                else w->focusByKeyboard(chips[std::min<size_t>(focusChip, chips.size() - 1)]);
            }
        }
    }

    // Rebuilds the empty-query view after a recent search was removed / cleared (posted: the clicked chip or link dies
    // in the rebuild). Skipped if the user has started typing meanwhile.
    void rebuildBrowse(int focusChip, bool keyboard) {
        Dispatcher::post([this, focusChip, keyboard, ref = life_.ref()] {
            if (ref.expired() || !box_->text().empty()) return;
            showBrowse(focusChip);
            if (keyboard && focusChip < 0) {
                if (auto* w = window()) w->focusByKeyboard(box_);
            }
        });
    }

    // The current text query, remembered once the user acts on its results (opens a card / plays a track).
    std::function<void()> remembering(std::function<void()> fn) const {
        return [q = query_, fn = std::move(fn)] {
            RecentSearches::get().record(q);
            fn();
        };
    }
    void rememberCard(MediaCard* card) const {
        if (card->onOpen) card->onOpen = remembering(card->onOpen);
        if (card->onPlay) card->onPlay = remembering(card->onPlay);
    }

    void runTagSearch(const std::wstring& title, const std::string& tag) {
        life_.renew();
        query_ = title;
        auto* c = resetContent(20.f);
        c->setPadding(gfx::metrics::pageX, 20, 48);
        c->add<SkeletonBlock>(4);
        scroll_->contentChanged();
        const std::string q = "tag:\"" + tag + "\"";
        struct Out {
            std::vector<Artist> artists;
            std::vector<Album> albums;
        };
        async(Priority::High, life_.ref(),
              [q] {
                  Out o;
                  o.artists = mb::searchArtists(q, 18);
                  o.albums = mb::searchAlbums(q + " AND primarytype:album", 24);
                  return o;
              },
              [this, title](Result<Out> r) {
                  if (!r) return showError(toWide(r.errorMessage()), [this] { showBrowse(); });
                  auto* c = resetContent(24.f);
                  c->setPadding(gfx::metrics::pageX, 20, 48);
                  auto* heading = c->add<ui::Label>(title, type::displayM);
                  heading->setVAlign(gfx::VAlign::Top);
                  if (!r->artists.empty()) {
                      c->add<SectionHeader>(tr(L"Sanatçılar"), std::to_wstring(r->artists.size()));
                      auto* g = addCardRow(c, 150, 0);
                      for (const auto& a : r->artists) addArtistCard(g, a);
                  }
                  if (!r->albums.empty()) {
                      c->add<SectionHeader>(tr(L"Albümler"), std::to_wstring(r->albums.size()));
                      auto* g = addCardRow(c, 150, 0);
                      for (const auto& a : r->albums) addAlbumCard(g, a);
                  }
                  contentReady();
              });
    }

    void runSearch(const std::wstring& q) {
        if (q.empty()) return;
        query_ = q;
        linkShown_ = false;
        life_.renew();
        auto* c = resetContent(20.f);
        c->setPadding(gfx::metrics::pageX, 20, 48);
        c->add<SkeletonBlock>(6);
        scroll_->contentChanged();
        const std::string query = toUtf8(q);
        spotify::Api* api = source::activeApi();
        async(Priority::High, life_.ref(), [query, api] { return source::search(api, query, 12); },
              [this](Result<SearchResults> r) {
                  if (!r) {
                      showError(toWide(r.errorMessage()), [this] { runSearch(query_); });
                      return;
                  }
                  results_ = std::move(*r);
                  showResults(tab_);
              });
    }

    void showResults(int tab) {
        tab_ = tab;
        auto* c = resetContent(24.f);
        c->setPadding(gfx::metrics::pageX, 16, 48);
        auto* tabs =
            c->add<Tabs>(std::vector<std::wstring>{tr(L"Tümü"), tr(L"Şarkılar"), tr(L"Albümler"), tr(L"Sanatçılar")});
        tabs->select(tab, false);
        tabs->onSelect = [this](int i) {
            Dispatcher::post([this, i, ref = life_.ref()] {
                if (!ref.expired()) showResults(i);
            });
        };
        const auto& R = results_;
        if (R.tracks.empty() && R.albums.empty() && R.artists.empty()) {
            c->add<MessagePanel>(
                "search", tr(L"Sonuç bulunamadı"),
                i18n::format(tr(L"\"{}\" için bir şey bulamadık. Yazımı kontrol et ya da başka bir şey dene."),
                             {query_}));
            contentReady();
            return;
        }
        auto playTracks = [q = query_](std::vector<Track> tracks, int i) {
            RecentSearches::get().record(q);
            ctx().player->playContext(std::move(tracks), i, {"search", tr(L"Arama sonuçları")});
        };
        auto addTracks = [&](size_t limit, bool header) {
            TrackTable::Options o;
            o.showAdded = false;
            o.showHeader = header;
            auto* table = c->add<TrackTable>(o);
            std::vector<Track> shown(R.tracks.begin(), R.tracks.begin() + std::min(limit, R.tracks.size()));
            table->setTracks(shown);
            table->onPlay = [shown, playTracks](int i) { playTracks(shown, i); };
        };
        if (tab == 0) {
            if (!R.artists.empty()) {
                c->add<SectionHeader>(tr(L"Sanatçılar"), L"", tr(L"Tümünü gör"))->onLink = [this] { showResults(3); };
                auto* g = addCardRow(c, 150, 1);
                for (const auto& a : R.artists) rememberCard(addArtistCard(g, a));
            }
            if (!R.tracks.empty()) {
                c->add<SectionHeader>(tr(L"Şarkılar"), L"", tr(L"Tümünü gör"))->onLink = [this] { showResults(1); };
                addTracks(5, false);
            }
            if (!R.albums.empty()) {
                c->add<SectionHeader>(tr(L"Albümler"), L"", tr(L"Tümünü gör"))->onLink = [this] { showResults(2); };
                auto* g = addCardRow(c, 150, 1);
                for (const auto& a : R.albums) rememberCard(addAlbumCard(g, a));
            }
        } else if (tab == 1) {
            addTracks(R.tracks.size(), true);
        } else if (tab == 2) {
            auto* g = addCardRow(c, 150, 0);
            for (const auto& a : R.albums) rememberCard(addAlbumCard(g, a));
        } else {
            auto* g = addCardRow(c, 150, 0);
            for (const auto& a : R.artists) rememberCard(addArtistCard(g, a));
        }
        contentReady();
    }

    ui::TextBox* box_;
    std::wstring query_, pending_;
    double pendingAt_ = 0;
    SearchResults results_;
    int tab_ = 0;
    bool linkShown_ = false;   // the content is the LinkHint row
};

// ===================================================================================================
// Library (local)

class LibraryPage : public ScrollPage {
public:
    // `route` = the Library route's id: "folder:<id>" shows the playlists of one Spotify library folder.
    explicit LibraryPage(const std::string& route) {
        if (route.rfind("folder:", 0) == 0) folder_ = route.substr(7);
        rebuild();
        ctx().library.subscribe(libLife_.ref(), [this] { rebuild(); });
        if (ctx().session)
            ctx().session->subscribe(libLife_.ref(), [this] { rebuild(); });
    }

private:
    void rebuild() {
        const float y = scroll_->scrollY();
        // A folder that is gone (moved / deleted on Spotify, logged out): the whole library instead.
        const spotify::PlaylistFolder* folder =
            !folder_.empty() && source::loggedIn() ? ctx().session->library().tree.folder(folder_) : nullptr;
        if (folder) {
            buildFolder(*folder);
            contentReady();
            if (y > 0) {
                scroll_->layout();
                scroll_->scrollTo(y, false);
            }
            return;
        }
        auto* c = resetContent(24.f);
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Kitaplık"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        // Logged in this creates the playlist on the user's Spotify account; logged out, a local one.
        Button* create = top->add<Button>(ButtonKind::Secondary, tr(L"Yeni çalma listesi"), "plus");
        create->onClick = [] {
            promptNewPlaylist([](const std::string& id) { ctx().router->navigate({RouteKind::Playlist, id}); });
        };
        // A playlist file or a YouTube playlist -> a local playlist (app/PlaylistTransfer).
        Button* import = top->add<Button>(ButtonKind::Ghost, tr(L"İçe aktar"), "list");
        import->onClick = [import] {
            const Rect br = import->toWindow(import->rect());
            transfer::showImportMenu({br.x, br.bottom() + 6});
        };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, create, import](ui::Box& b) {
            const float w = b.rect().w;
            title->setRect({0, 0, w * 0.6f, 64});
            const float cw = create->naturalWidth(), iw = import->naturalWidth();
            create->setRect({w - cw, 12, cw, 40});
            import->setRect({w - cw - 10 - iw, 12, iw, 40});
        };
        auto* tabs = c->add<Tabs>(
            std::vector<std::wstring>{tr(L"Çalma listeleri"), tr(L"Albümler"), tr(L"Sanatçılar"), tr(L"Geçmiş")});
        tabs->select(tab_, false);
        tabs->onSelect = [this](int i) {
            tab_ = i;
            Dispatcher::post([this, ref = life_.ref()] {
                if (!ref.expired()) rebuild();
            });
        };
        auto& lib = ctx().library;
        const bool sp = source::loggedIn();
        if (tab_ == 0) {
            smart::addSection(c, 1);   // "Senin için listeler" (hidden while there is none)
            auto* grid = addCardRow(c, 170, 0);
            if (sp) {
                // Top level of the library: folders sit where their most recent playlist would be.
                addSpotifyCards(grid, {});
            } else {
                auto* liked = grid->add<MediaCard>(tr(L"Beğenilen Şarkılar"),
                                                   i18n::plural(L"{} şarkı", lib.liked().size()), std::vector<Image>{},
                                                   MediaCard::Shape::Square, Placeholder::Playlist);
                liked->onOpen = [] { ctx().router->navigate({RouteKind::Liked}); };
                liked->onPlay = [] { ctx().router->navigate({RouteKind::Liked, "#play"}); };
                setPlaylistCardMenu(liked, {RouteKind::Liked}, sync::likedTarget());
                for (const auto& p : lib.playlists()) {
                    auto* card = grid->add<MediaCard>(toWide(p.name), i18n::plural(L"{} şarkı", p.totalTracks),
                                                      p.images, MediaCard::Shape::Square, Placeholder::Playlist);
                    const std::string id = p.id;
                    card->onOpen = [id] { ctx().router->navigate({RouteKind::Playlist, id}); };
                    card->onPlay = [id] { ctx().router->navigate({RouteKind::Playlist, id + "#play"}); };
                    setPlaylistCardMenu(card, {RouteKind::Playlist, id}, {sync::Kind::Playlist, id, p.name, p.images});
                }
            }
        } else if (tab_ == 1) {
            const auto& albums = sp ? ctx().session->library().albums : lib.albums();
            if (albums.empty())
                c->add<MessagePanel>("album", tr(L"Kayıtlı albüm yok"),
                                     tr(L"Albüm sayfasındaki kalp ile albümleri buraya kaydet."));
            auto* grid = addCardRow(c, 170, 0);
            for (const auto& a : albums) addAlbumCard(grid, a);
        } else if (tab_ == 2) {
            const auto& artists = sp ? ctx().session->library().artists : lib.artists();
            if (artists.empty())
                c->add<MessagePanel>("artist", tr(L"Takip ettiğin sanatçı yok"),
                                     tr(L"Sanatçı sayfalarından takip etmeye başla."));
            auto* grid = addCardRow(c, 160, 0);
            for (const auto& a : artists) addArtistCard(grid, a);
        } else {
            if (lib.history().empty()) {
                c->add<MessagePanel>("clock", tr(L"Henüz geçmiş yok"), tr(L"Dinlediğin şarkılar burada listelenir."));
            } else {
                TrackTable::Options o;
                o.showAdded = false;
                auto* table = c->add<TrackTable>(o);
                std::vector<Track> tracks;
                for (const auto& h : lib.history()) tracks.push_back(h.track);
                table->setTracks(tracks);
                table->onPlay = [tracks](int i) { ctx().player->playContext(tracks, i, {"history", tr(L"Geçmiş")}); };
            }
        }
        contentReady();
        if (y > 0) {
            scroll_->layout();
            scroll_->scrollTo(y, false);
        }
    }

    // Spotify playlists and folders directly under `folderId` ("" = the top level), as cards in library order.
    void addSpotifyCards(ui::Widget* grid, const std::string& folderId) {
        const auto& snap = ctx().session->library();
        const auto closed = [](const std::string&) { return false; };
        for (const auto& row : spotify::treeRows(snap.playlists, snap.tree, closed, folderId)) {
            if (row.folder) {
                const auto& f = snap.tree.folders[row.index];
                auto* card = grid->add<MediaCard>(f.name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(f.name),
                                                  i18n::plural(L"{} çalma listesi", f.playlistCount),
                                                  std::vector<Image>{}, MediaCard::Shape::Square, Placeholder::Folder);
                const Route open{RouteKind::Library, "folder:" + f.id};
                card->onOpen = [open] { ctx().router->navigate(open); };
                continue;
            }
            const auto& p = snap.playlists[row.index];
            const bool liked = p.id == spotify::Api::kLikedSongsUri;
            // No count from Spotify (e.g. "Your Episodes"): show the owner instead of a misleading "0 şarkı".
            const std::wstring sub = p.countKnown      ? i18n::plural(L"{} şarkı", p.totalTracks)
                                     : !p.ownerName.empty() ? toWide(p.ownerName)
                                                            : std::wstring(tr(L"Çalma listesi"));
            auto* card = grid->add<MediaCard>(toWide(p.name), sub, p.images, MediaCard::Shape::Square, Placeholder::Playlist);
            const Route open = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
            Route play = open;
            play.id += "#play";
            card->onOpen = [open] { ctx().router->navigate(open); };
            card->onPlay = [play] { ctx().router->navigate(play); };
            setPlaylistCardMenu(card, open, {liked ? sync::Kind::Liked : sync::Kind::Playlist, p.id, p.name, p.images});
        }
    }

    // One Spotify library folder: its playlists and subfolders, with the way back up.
    void buildFolder(const spotify::PlaylistFolder& f) {
        auto* c = resetContent(24.f);
        const auto& tree = ctx().session->library().tree;
        const auto* parent = f.parentId.empty() ? nullptr : tree.folder(f.parentId);
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(f.name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(f.name), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        // Up one level: the parent folder, else the whole library.
        const std::wstring upLabel = parent ? (parent->name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(parent->name))
                                            : std::wstring(tr(L"Kitaplık"));
        Button* up = top->add<Button>(ButtonKind::Secondary, upLabel, "arrow-back");
        const Route upRoute = parent ? Route{RouteKind::Library, "folder:" + parent->id} : Route{RouteKind::Library};
        up->onClick = [upRoute] { ctx().router->navigate(upRoute); };
        auto* meta = top->add<ui::Label>(toUpperTr(i18n::plural(L"{} çalma listesi", f.playlistCount)), type::monoLabel,
                                         ui::Tone::Tertiary);
        top->onPreferredHeight = [](float) { return 84.f; };
        top->onLayout = [title, up, meta](ui::Box& b) {
            const float w = b.rect().w;
            const float uw = up->naturalWidth();
            meta->setRect({0, 0, w * 0.6f, 16});
            title->setRect({0, 20, w - uw - 24, 64});
            up->setRect({w - uw, 32, uw, 40});
        };
        auto* grid = addCardRow(c, 170, 0);
        addSpotifyCards(grid, f.id);
        if (f.playlistCount == 0)
            c->add<MessagePanel>("folder", tr(L"Bu klasör boş"), tr(L"Spotify'da bu klasöre çalma listesi ekleyebilirsin."));
    }

    int tab_ = 0;
    std::string folder_;   // Spotify library folder shown ("" = the whole library)
    Lifetime libLife_;
};

// ===================================================================================================
// Artist

class ArtistHero : public ui::Widget {
public:
    explicit ArtistHero(const Artist& a) : artist_(a), name_(toWide(a.name), type::hero) {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        follow_ = add<Button>(ButtonKind::Secondary, tr(L"Takip et"));
        // Spotify's artist radio (needs a Spotify session and a Spotify artist; MusicBrainz artists have none).
        radio_ = add<Button>(ButtonKind::Secondary, tr(L"Radyo"), "wifi");
        radio_->setTooltip(tr(L"Bu sanatçıya benzer şarkılarla radyo başlat"));
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        more_->setTooltip(tr(L"Diğer seçenekler"));
        sync();
    }
    void sync() {
        const bool f = ctx().library.isFollowing(artist_.id);
        follow_->setLabel(f ? tr(L"Takip ediliyor") : tr(L"Takip et"));
        follow_->setActive(f);
        radio_->setVisible(radioAvailable() && artist_.id.rfind("spotify:artist:", 0) == 0);
        requestLayout();
        invalidate();
    }
    // Kara liste state, re-read whenever the list changed (the track menu on this page, Ayarlar, …).
    bool blocked() {
        if (const uint64_t rev = blacklist::revision(); rev != blockRev_) {
            blockRev_ = rev;
            blocked_ = blacklist::artistMatches({artist_.id, artist_.name});
        }
        return blocked_;
    }
    float preferredHeight(float) override { return 340; }
    void layout() override {
        const float y = 340 - 56;
        play_->setRect({0, y, 56, 56});
        float x = 72;
        for (Button* b : {follow_, radio_}) {
            if (!b->visible()) continue;
            const float w = b->naturalWidth();
            b->setRect({x, y + 8, w, 40});
            x += w + 14;
        }
        more_->setRect({x, y + 8, 40, 40});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const Rect banner{r.x - 48, r.y - 36, r.w + 96, 340 + 36 - 80};
        if (!artist_.images.empty()) {
            c.pushClip(banner);
            drawArtwork(c, artist_.images, {banner.x + banner.w * 0.4f, banner.y - banner.w * 0.08f, banner.w * 0.6f, banner.w * 0.6f}, 0,
                        Placeholder::Artist);
            c.popClip();
            // The fade starts exactly at the photo's left edge (0.4 w): starting earlier left that edge ~9 % visible,
            // a hard vertical step wherever the photo differs from bg.base (every dark photo on the light theme).
            c.fillLinearGradient(banner, {banner.x + banner.w * 0.4f, 0}, {banner.x + banner.w * 0.8f, 0}, col.bgBase,
                                 col.bgBase.withAlpha(0.15f));
        } else {
            // No photo: editorial ring motif in the accent.
            c.pushClip(banner);
            for (int i = 0; i < 5; ++i)
                c.strokeCircle({banner.right() - 160, banner.cy() + 40}, 220.f - i * 34.f, accent().base.withAlpha(0.10f + i * 0.04f), 1.f);
            c.popClip();
        }
        c.fillLinearGradient(banner, {0, banner.y + banner.h * 0.4f}, {0, banner.bottom()}, col.bgBase.withAlpha(0), col.bgBase);
        std::wstring label = artist_.type == "Group" ? toUpperTr(tr(L"Grup")) : toUpperTr(tr(L"Sanatçı"));
        if (!artist_.country.empty()) label += L" · " + toWide(artist_.country);
        if (!artist_.beginYear.empty()) label += L" · " + toWide(artist_.beginYear);
        if (!artist_.tags.empty()) label += L" · " + toUpperTr(toWide(artist_.tags[0]));
        if (blocked()) label.append(L" · ").append(toUpperTr(tr(L"Engelli")));   // kara liste: its songs never play by themselves
        c.text(label, type::monoLabel, {r.x, r.y + 70, r.w, 14}, accent().base);
        float size = 96;
        for (float s : {96.f, 80.f, 64.f, 52.f}) {
            size = s;
            name_.setStyle(type::hero.withSize(s));
            if (name_.measure().w <= r.w * 0.8f) break;
        }
        c.text(name_, {r.x - size * 0.04f, r.y + 92, r.w, size}, col.fgPrimary);
        std::wstring meta;
        if (artist_.listenCount > 0)
            meta = i18n::plural(L"{} DİNLENME · LISTENBRAINZ", artist_.listenCount, {compactCount(artist_.listenCount)});
        else if (!artist_.disambiguation.empty()) meta = toUpperTr(toWide(artist_.disambiguation));
        if (!meta.empty()) c.text(meta, type::monoMeta, {r.x, r.y + 100 + size, r.w, 16}, col.fgSecondary);
        paintChildren(c);
    }
    ui::PlayButton* play_;
    Button *follow_, *radio_, *more_;

private:
    Artist artist_;
    gfx::Text name_;
    bool blocked_ = false;
    uint64_t blockRev_ = ~0ull;
};

class ArtistView : public ScrollPage {
public:
    explicit ArtistView(std::string id) : id_(std::move(id)) {
        showSkeleton(5);
        load();
    }

private:
    void load() {
        hero_ = nullptr;   // the skeleton / error panel replaced it
        const std::string id = id_;
        spotify::Api* api = source::activeApi();
        async(Priority::High, life_.ref(), [id, api] { return source::artistPage(api, id); },
              [this](Result<catalog::ArtistPage> r) {
                  if (!r) {
                      showError(toWide(r.errorMessage()), [this] {
                          showSkeleton(5);
                          load();
                      });
                      return;
                  }
                  build(*r);
              });
    }

    void build(const catalog::ArtistPage& d) {
        auto* c = resetContent(28.f);
        auto* hero = c->add<ArtistHero>(d.artist);
        hero_ = hero;
        const std::string ctxUri = "artist:" + id_;
        const std::wstring ctxName = toWide(d.artist.name);
        const auto top = d.topTracks;
        const Artist artist = d.artist;
        hero->play_->onClick = [top, ctxUri, ctxName] {
            auto* p = ctx().player;
            if (p->context().uri == ctxUri) return p->togglePause();
            if (top.empty()) return toast(tr(L"Bu sanatçı için popüler şarkı verisi yok"));
            p->playContext(top, -1, {ctxUri, ctxName});   // no row picked: the player skips kara liste tracks
        };
        hero->follow_->onClick = [artist, hero] {
            ctx().library.setFollow(artist, !ctx().library.isFollowing(artist.id));
            hero->sync();
        };
        hero->radio_->onClick = [artist] { startRadio(artist.id, toWide(artist.name)); };
        hero->more_->onClick = [this, hero, top, artist] {
            const Rect br = hero->more_->toWindow(hero->more_->rect());
            std::vector<ui::MenuItem> items{
                {tr(L"Popüler şarkıları sıraya ekle"), "queue", L"", [top] {
                     ctx().player->enqueue(top);
                     toast(tr(L"Sıraya eklendi"));
                 }},
                {source::isSpotifyId(artist.id) ? tr(L"Spotify bağlantısını kopyala")
                                                : tr(L"MusicBrainz bağlantısını kopyala"),
                 "link", L"", [artist] {
                     copyText(source::isSpotifyId(artist.id)
                                  ? L"https://open.spotify.com/artist/" + toWide(artist.id.substr(artist.id.rfind(':') + 1))
                                  : L"https://musicbrainz.org/artist/" + toWide(artist.id));
                     toast(tr(L"Bağlantı kopyalandı"));
                 }}};
            // Kara liste (same wording as the track menu): its songs are skipped wherever the player picks by itself.
            const bool blocked = hero->blocked();
            const std::wstring name = toWide(artist.name);
            items.push_back(ui::MenuItem::sep());
            items.push_back({blocked ? tr(L"Sanatçının engelini kaldır") : tr(L"Sanatçıyı engelle"),
                             blocked ? "check" : "error", L"",
                             [this, artist, blocked, name, ref = life_.ref()] {
                                 blacklist::setArtistBlocked(artist.id, artist.name, !blocked);
                                 toast(i18n::format(
                                     blocked ? tr(L"\"{}\" engeli kaldırıldı")
                                             : tr(L"\"{}\" engellendi. Şarkıları artık kendiliğinden çalmaz."),
                                     {name}));
                                 if (!ref.expired() && hero_) hero_->sync();
                             }});
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        if (!d.topTracks.empty()) {
            c->add<SectionHeader>(tr(L"Popüler"), L"LISTENBRAINZ");
            TrackTable::Options o;
            o.showAdded = false;
            o.showHeader = false;
            auto* table = c->add<TrackTable>(o);
            std::vector<Track> shown(d.topTracks.begin(), d.topTracks.begin() + std::min<size_t>(10, d.topTracks.size()));
            table->setTracks(shown);
            table->onPlay = [top, ctxUri, ctxName](int i) { ctx().player->playContext(top, i, {ctxUri, ctxName}); };
            c->setSpacingBefore(table, 8);
        }
        auto addAlbums = [&](const std::wstring& title, const std::vector<Album>& list) {
            if (list.empty()) return;
            c->add<SectionHeader>(title, std::to_wstring(list.size()));
            auto* grid = addCardRow(c, 160, 0);
            c->setSpacingBefore(grid, 16);
            for (const auto& a : list) addAlbumCard(grid, a, false);
        };
        addAlbums(tr(L"Albümler"), d.albums);
        addAlbums(tr(L"Tekliler ve EP'ler"), d.singles);
        addAlbums(tr(L"Derlemeler ve canlı kayıtlar"), d.other);
        if (!d.similar.empty()) {
            c->add<SectionHeader>(tr(L"Benzer sanatçılar"), L"LISTENBRAINZ");
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            for (const auto& a : d.similar) addArtistCard(grid, a);
        }
        contentReady();
    }

    std::string id_;
    ArtistHero* hero_ = nullptr;   // the current content's header (content is rebuilt only by build())
};

} // namespace

std::unique_ptr<Page> makeSearchPage(const std::string& q) { return std::make_unique<SearchPage>(q); }
std::unique_ptr<Page> makeLibraryPage(const std::string& id) { return std::make_unique<LibraryPage>(id); }
std::unique_ptr<Page> makeArtistPage(const std::string& id) { return std::make_unique<ArtistView>(id); }

} // namespace st::app
