// Search (docs/DEVELOPMENT.md, "Artist and Search pages"): type-ahead over the active catalog (Spotify when logged in,
// MusicBrainz or YouTube Music otherwise, see Source.h) with the top result beside the songs and a shelf per result
// kind, a tab per kind, and recent searches + genres (YouTube Music: its moods & genres) while the box is empty. Result kinds are data (resultKinds()): a kind is a tab,
// a section of "Tümü", its cards or rows and what its recent-search entries open.
#include "app/BrowseModel.h"
#include "app/BrowseWidgets.h"
#include "app/Commands.h"
#include "app/HomeWidgets.h"
#include "app/LinkOpener.h"
#include "app/Links.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PodcastsSpotify.h"
#include "app/Recognize.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/SpotifyApi.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"
#include "ytmusic/YtMusic.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using browse::RecentItem;
using namespace catalog;
namespace type = gfx::type;
namespace metrics = gfx::metrics;

namespace {

browse::RecentSearches& recents() {
    static browse::RecentSearches store(paths::appData() / L"recent-searches.json");
    return store;
}

float rowH() { return TrackTable::rowMetrics().h; }

// The tint of a cover once its image is decoded (nullopt before, and for images without a vivid color).
std::optional<Color> artColor(const std::vector<Image>& images, float px) {
    const auto* img = pickImage(images, static_cast<int>(px));
    return img ? gfx::ImageCache::get().accentOf(img->url) : std::nullopt;
}

// =====================================================================================================================
// What a result is to the page

struct ResultEntry {
    std::wstring title, subtitle;             // the card's lines
    std::vector<Image> images;
    bool circle = false;                      // round artwork (artists, profiles)
    Placeholder placeholder = Placeholder::Album;
    std::function<void()> open;               // click / Enter
    std::function<void()> play;               // the play button; null = none
    std::function<void(gfx::Point)> menu;     // context menu (window DIPs); null = none
    std::optional<preview::Target> preview;   // press and hold
    RecentItem recent;                        // what "Son aramalar" keeps once it is opened
};

class SearchPage;

// One kind of result. Most kinds are cards (cardKind()): a shelf in "Tümü", a grid in their own tab, a card in "Son
// aramalar" once opened. A kind with other content (songs: rows) builds it itself.
struct ResultKind {
    std::string id;                           // "artist", "track", "album", ...: recent searches and the top result
    std::wstring title;                       // tab label and section title ("Sanatçılar")
    std::wstring one;                         // one item, the top result's label ("Sanatçı")
    std::function<size_t(const SearchResults&)> count;
    std::function<ResultEntry(const SearchResults&, size_t)> entry;
    void (SearchPage::*custom)(ui::Column*, bool full) = nullptr;   // non-card content; null = cards
    bool besideTop = false;                   // "Tümü" shows it next to the top result instead of as a section
    std::optional<RouteKind> route;           // how its recent items open (null: `reopen`)
    std::function<void(const RecentItem&)> reopen;
};

template <class T>
ResultKind cardKind(std::string id, std::wstring title, std::wstring one, std::vector<T> SearchResults::*list,
                    ResultEntry (*describe)(const T&), std::optional<RouteKind> route) {
    ResultKind k;
    k.id = std::move(id);
    k.title = std::move(title);
    k.one = std::move(one);
    k.route = route;
    k.count = [list](const SearchResults& r) { return (r.*list).size(); };
    k.entry = [list, describe](const SearchResults& r, size_t i) { return describe((r.*list)[i]); };
    return k;
}

ResultEntry artistEntry(const Artist& a) {
    ResultEntry e;
    e.title = toWide(a.name);
    e.subtitle = artistSubtitle(a);
    e.images = a.images;
    e.circle = true;
    e.placeholder = Placeholder::Artist;
    const std::string id = a.id;
    e.open = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
    e.play = [id] { ctx().router->navigate({RouteKind::Artist, id + "#play"}); };
    e.preview = preview::collectionTarget({RouteKind::Artist, id}, e.title, a.images);
    e.recent = {"artist", id, e.title, {}, a.images, std::nullopt};
    return e;
}

ResultEntry albumEntry(const Album& a) {
    ResultEntry e;
    e.title = toWide(a.name);
    e.subtitle = albumSubtitle(a);
    e.images = a.images;
    const std::string id = a.id;
    e.open = [id] { ctx().router->navigate({RouteKind::Album, id}); };
    e.play = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    e.menu = [id, offline = sync::Target{sync::Kind::Album, a.id, a.name, a.images}](gfx::Point wp) {
        showAlbumCardMenu(id, offline, wp);
    };
    e.preview = preview::collectionTarget({RouteKind::Album, id}, e.title, a.images);
    std::wstring artists;
    for (size_t i = 0; i < a.artists.size() && i < 2; ++i) artists += (i ? L", " : L"") + toWide(a.artists[i].name);
    e.recent = {"album", id, e.title, artists, a.images, std::nullopt};
    return e;
}

// Spotify only (MusicBrainz has none of these kinds: their tabs and shelves never show there).
ResultEntry playlistEntry(const Playlist& p) {
    ResultEntry e;
    e.title = toWide(p.name);
    e.subtitle = p.ownerName.empty() ? std::wstring(tr(L"Çalma listesi")) : toWide(p.ownerName);
    e.images = p.images;
    e.placeholder = Placeholder::Playlist;
    const Route open{RouteKind::Playlist, p.id};
    e.open = [open] { ctx().router->navigate(open); };
    e.play = [open] { ctx().router->navigate({open.kind, open.id + "#play"}); };
    e.menu = [open, offline = sync::Target{sync::Kind::Playlist, p.id, p.name, p.images}](gfx::Point wp) {
        showPlaylistCardMenu(open, offline, wp);
    };
    e.preview = preview::collectionTarget(open, e.title, p.images);
    e.recent = {"playlist", p.id, e.title, toWide(p.ownerName), p.images, std::nullopt};
    return e;
}

// A podcast opens in Podcastler, which plays it from its public feed (app/PodcastsSpotify: matched by name and
// publisher) or says why it can't.
void openShow(const PodcastShow& s) {
    spotifyshows::remember(s);   // its name and publisher: what the directory is searched for
    ctx().router->navigate({RouteKind::Podcasts, s.id});
}

ResultEntry showEntry(const PodcastShow& s) {
    ResultEntry e;
    e.title = toWide(s.name);
    e.subtitle = s.publisher.empty() ? std::wstring(tr(L"Podcast")) : toWide(s.publisher);
    e.images = s.images;
    e.open = [s] { openShow(s); };
    e.recent = {"show", s.id, e.title, toWide(s.publisher), s.images, std::nullopt};
    return e;
}

ResultEntry profileEntry(const Profile& p) {
    ResultEntry e;
    e.title = toWide(p.name);
    e.subtitle = tr(L"Profil");
    e.images = p.images;
    e.circle = true;
    e.placeholder = Placeholder::Artist;
    e.open = [id = p.id] { ctx().router->navigate({RouteKind::Profile, id}); };
    e.recent = {"profile", p.id, e.title, {}, p.images, std::nullopt};
    return e;
}

// The kinds Spotify pages one at a time (Api::searchMore): every kind of its results.
std::optional<spotify::SearchKind> pagedKind(const std::string& id) {
    if (id == "track") return spotify::SearchKind::Tracks;
    if (id == "album") return spotify::SearchKind::Albums;
    if (id == "artist") return spotify::SearchKind::Artists;
    if (id == "playlist") return spotify::SearchKind::Playlists;
    if (id == "show") return spotify::SearchKind::Shows;
    if (id == "profile") return spotify::SearchKind::Profiles;
    return std::nullopt;
}

// The kinds YouTube Music pages (ytm::searchMore): songs, albums, artists, playlists.
std::optional<ytm::SearchKind> ytmKind(const std::string& id) {
    if (id == "track") return ytm::SearchKind::Songs;
    if (id == "album") return ytm::SearchKind::Albums;
    if (id == "artist") return ytm::SearchKind::Artists;
    if (id == "playlist") return ytm::SearchKind::Playlists;
    return std::nullopt;
}

// A page of more rows of one kind, from whichever catalog answered.
struct MoreBatch {
    SearchResults results;
    bool hasMore = false;
};

bool roundKind(const std::string& id) { return id == "artist" || id == "profile"; }

RecentItem trackRecent(const Track& t) {
    return {"track", t.id, toWide(t.name), toWide(t.artistLine()), t.album.images, t};
}

void playSearchTracks(std::vector<Track> tracks, int index) {
    if (index >= 0 && index < static_cast<int>(tracks.size())) recents().record(trackRecent(tracks[index]));
    ctx().player->playContext(std::move(tracks), index, {"search", tr(L"Arama sonuçları")});
}

ResultEntry trackEntry(const SearchResults& r, size_t i) {
    const Track& t = r.tracks[i];
    ResultEntry e;
    e.title = toWide(t.name);
    e.subtitle = toWide(t.artistLine());
    e.images = t.album.images;
    e.open = e.play = [tracks = r.tracks, i] { playSearchTracks(tracks, static_cast<int>(i)); };
    e.recent = trackRecent(t);
    return e;
}

const std::vector<ResultKind>& resultKinds();

// =====================================================================================================================
// The top result: a raised card tinted with its artwork's color, the artwork, the name, what it is, and the accent play
// button that rises in on hover / focus. Enter opens it (a song plays), Shift+Enter plays it.

class TopResultCard : public ui::Widget, public CoverSource, public PreviewSource {
public:
    TopResultCard(ResultEntry e, std::wstring kindLabel)
        : e_(std::move(e)), title_(e_.title, type::displayS), kind_(toUpperTr(kindLabel), type::monoLabel),
          // A subtitle that only repeats the kind (YouTube Music's "Sanatçı" under the SANATÇI badge) is left out.
          sub_(toUpperTr(e_.subtitle) == toUpperTr(kindLabel) ? std::wstring() : e_.subtitle, type::secondary) {
        focusable = true;
        gfx::TextOptions two;
        two.wrap = true;
        two.maxLines = 2;
        title_.setOptions(two);
    }
    ResultEntry& entry() { return e_; }
    Lifetime::Ref ref() const { return life_.ref(); }
    // Artwork that arrived later (an open-catalog artist's photo): drawn from now on, its color fades in.
    void setImages(std::vector<Image> images) {
        e_.images = e_.recent.images = std::move(images);
        tint_.reset();
        tintA_.snap(0);
        invalidate();
    }
    float preferredHeight(float) override { return std::max(212.f, 4 * rowH()); }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const bool light = gfx::isLightTheme();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        const float p = press_.value();
        c.pushScale(1 - 0.01f * p, {r.cx(), r.cy()});
        c.fillRounded(r, gfx::radii().surface, col.bgRaised);
        const Rect art = artRect();
        if (!tint_) {
            tint_ = artColor(e_.images, art.w * c.scale());
            if (tint_) tintA_.to(1, ui::motion::slow, ui::Ease::Standard);
        }
        if (tint_) {
            const float a = (light ? 0.24f : 0.36f) * tintA_.value();
            c.fillLinearGradient(r, {r.x, r.y}, {r.x + r.w * 0.85f, r.bottom()}, tint_->withAlpha(a), tint_->withAlpha(0));
        }
        c.strokeRounded(r.inset(0.5f), gfx::radii().surface, Color::lerp(col.hairDefault, col.hairStrong, h));
        if (pagetx::presence(this) > 0.f) {
            if (e_.circle) ui::card::paintBack(c, art, h, true);
            drawArtwork(c, e_.images, art, gfx::radii().art, e_.placeholder, e_.circle);
        }
        const float pad = padding();
        const float tw = r.w - 2 * pad;
        title_.setStyle(type::displayS.withSize(r.w >= 400 ? 32.f : 26.f));
        const float th = std::ceil(title_.measure(tw).h);
        float y = art.bottom() + 16;
        c.text(title_, {r.x + pad - 1, y, tw, th + 4}, col.fgPrimary);
        y += th + 10;
        // What it is (a hairline pill) and its line, beside the play button.
        const float kw = std::ceil(kind_.measure().w) + 16;
        const Rect pill{r.x + pad, y, kw, 20};
        c.strokePill(pill.inset(0.5f), col.hairStrong);
        c.text(kind_, pill.inset(8, 0), col.fgSecondary, gfx::VAlign::Center);
        const float sx = pill.right() + 10, sw = r.right() - pad - 48 - 12 - sx;
        if (!sub_.empty() && sw > 24) c.text(sub_, {sx, y, sw, 20}, col.fgSecondary, gfx::VAlign::Center);
        if (e_.play)
            ui::card::paintPlay(c, playRect(), {h, playHot_.value(), playPress_.value()}, accent().base, accent().hover,
                                accent().onAccent);
        c.popTransform();
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            if (e_.menu) e_.menu(e.windowPos);
            return false;
        }
        if (e.button != ui::MouseButton::Left) return false;
        onPlay_ = e_.play && playRect().contains(e.pos);
        (onPlay_ ? playPress_ : press_).to(1, ui::motion::microMs(80));
        invalidate();
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, ui::motion::microMs(160));
        playPress_.to(0, ui::motion::microMs(160));
        const bool onPlay = onPlay_;
        onPlay_ = false;
        if (!rect().contains(e.pos)) return;
        run(onPlay && playRect().contains(e.pos) ? e_.play : e_.open);
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = e_.play && playRect().contains(e.pos);
        if (hot == playHotOn_) return;
        playHotOn_ = hot;
        playHot_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        playHotOn_ = false;
        playHot_.to(0, ui::motion::microMs(ui::motion::fast));
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        press_.snap(ui::motion::micro() ? 1.f : 0.f);
        press_.to(0, ui::motion::microMs(160));
        run(e_.open);
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk == VK_RETURN && e.shift && !e.ctrl && !e.alt && e_.play) {
            run(e_.play);
            return true;
        }
        if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && e_.menu) {
            const Rect a = toWindow(artRect());
            e_.menu({a.x + 12, a.bottom() - 12});
            return true;
        }
        return false;
    }
    bool coverFor(const std::string& key, SharedCover& out) override {
        if (e_.images.empty()) return false;
        out = cover();
        return out.key == key;
    }
    std::optional<PreviewHit> previewAt(gfx::Point windowPos, bool keyboard) override {
        if (!e_.preview || (!keyboard && !artRect().contains(fromWindow(windowPos)))) return std::nullopt;
        return PreviewHit{*e_.preview, toWindow(artRect())};
    }

private:
    float padding() const { return rect().w >= 360 ? 24.f : 18.f; }
    Rect artRect() const {
        const Rect r = rect();
        const float pad = padding();
        const float s = std::clamp(r.h - 2 * pad - 80, 64.f, 112.f);
        return {r.x + pad, r.y + pad, s, s};
    }
    Rect playRect() const {
        const Rect r = rect();
        const float pad = padding() - 4;
        return {r.right() - pad - 48, r.bottom() - pad - 48, 48, 48};
    }
    SharedCover cover() const {
        const Rect a = artRect();
        return pagetx::imageCover(*this, a, e_.circle ? a.w * 0.5f : gfx::radii().art, e_.images);
    }
    void run(const std::function<void()>& fn) {
        if (!fn) return;
        recents().record(e_.recent);
        if (!e_.images.empty()) pagetx::arm(cover());
        const auto call = fn;   // may navigate (this card goes away)
        call();
    }

    ResultEntry e_;
    gfx::Text title_, kind_, sub_;
    std::optional<Color> tint_;
    ui::Anim hover_, press_, playHot_, playPress_, tintA_;
    bool onPlay_ = false, playHotOn_ = false;
    Lifetime life_;
};

// =====================================================================================================================
// A recent search: the artwork (a round photo for artists, the search glyph for a query), the name, what it is and a
// "×" that removes it (also Delete). Opening it searches again / opens the item / plays the song.

class RecentCard : public ui::Widget, public CoverSource {
public:
    RecentCard(RecentItem item, std::wstring kindLabel)
        : item_(std::move(item)), title_(item_.title, type::body), sub_({}, type::caption) {
        focusable = true;
        std::wstring s = kindLabel;
        if (!item_.subtitle.empty()) s += L" · " + item_.subtitle;
        sub_.setText(s);
    }
    std::function<void()> onOpen, onRemove;
    const RecentItem& item() const { return item_; }
    static float height() { return Settings::get().compactLists ? 56.f : 64.f; }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        c.pushScale(1 - 0.01f * press_.value(), {r.cx(), r.cy()});
        c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(h));
        c.strokeRounded(r.inset(0.5f), gfx::radii().surface, Color::lerp(col.hairDefault, col.hairStrong, h));
        const Rect art = artRect();
        if (item_.isQuery()) {
            c.fillRounded(art, gfx::radii().art, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
            c.icon("search", art.center(20, 20), col.fgSecondary);
        } else if (pagetx::presence(this) > 0.f) {
            const bool circle = roundKind(item_.kind);
            drawArtwork(c, item_.images, art, gfx::radii().art, circle ? Placeholder::Artist : Placeholder::Album, circle,
                        Priority::Normal);
        }
        const Rect x = closeRect();
        const float tx = art.right() + 12, tw = std::max(0.f, x.x - 8 - tx);
        c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
        c.text(sub_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
        const float xh = closeHot_.value();
        if (xh > 0.01f) c.fillCircle({x.cx(), x.cy()}, x.w * 0.5f, col.overlayHover.mulAlpha(xh * 1.5f));
        c.icon("close", x.center(14, 14), Color::lerp(col.fgTertiary, col.fgPrimary, std::max(xh, h * 0.5f)).mulAlpha(0.6f + 0.4f * h));
        c.popTransform();
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        onClose_ = closeRect().inset(-4).contains(e.pos);
        if (!onClose_) press_.to(1, ui::motion::microMs(80));
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, ui::motion::microMs(160));
        const bool close = onClose_;
        onClose_ = false;
        if (!rect().contains(e.pos)) return;
        if (close && closeRect().inset(-4).contains(e.pos)) return call(onRemove);
        if (!close) open();
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = closeRect().inset(-4).contains(e.pos);
        if (hot == closeOn_) return;
        closeOn_ = hot;
        closeHot_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        closeOn_ = false;
        closeHot_.to(0, ui::motion::microMs(ui::motion::fast));
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        open();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_DELETE || e.ctrl || e.alt) return false;
        call(onRemove);
        return true;
    }
    std::wstring tooltip() const override {
        if (closeOn_) return tr(L"Aramayı kaldır");
        auto& t = const_cast<gfx::Text&>(title_);
        const float tw = closeRect().x - 8 - (artRect().right() + 12);
        return t.isTrimmed(tw) ? item_.title : std::wstring();
    }
    bool coverFor(const std::string& key, SharedCover& out) override {
        if (item_.images.empty() || item_.isQuery()) return false;
        out = cover();
        return out.key == key;
    }

private:
    Rect artRect() const {
        const Rect r = rect();
        const float s = r.h - 16;
        return {r.x + 8, r.y + 8, s, s};
    }
    Rect closeRect() const {
        const Rect r = rect();
        return {r.right() - 12 - 28, r.cy() - 14, 28, 28};
    }
    SharedCover cover() const {
        const Rect a = artRect();
        return pagetx::imageCover(*this, a, roundKind(item_.kind) ? a.w * 0.5f : gfx::radii().art, item_.images);
    }
    void open() {
        if (!item_.isQuery() && !item_.images.empty()) pagetx::arm(cover());
        call(onOpen);
    }
    static void call(const std::function<void()>& fn) {
        if (!fn) return;
        const auto f = fn;   // rebuilds / navigates (this card goes away)
        f();
    }

    RecentItem item_;
    gfx::Text title_, sub_;
    ui::Anim hover_, press_, closeHot_;
    bool onClose_ = false, closeOn_ = false;
};

// Cards in columns: 4 / 3 / 2 / 1 by width.
class RecentGrid : public ui::Widget {
public:
    RecentGrid() { hitTestVisible = false; }
    static int columns(float w) { return w >= 1080 ? 4 : w >= 720 ? 3 : w >= 420 ? 2 : 1; }
    float preferredHeight(float width) override {
        const int n = static_cast<int>(children().size()), cols = columns(width);
        const int rows = (n + cols - 1) / cols;
        return rows <= 0 ? 0.f : rows * RecentCard::height() + (rows - 1) * metrics::gridGap;
    }
    void layout() override {
        const Rect r = rect();
        const int cols = columns(r.w);
        const float w = std::floor((r.w - metrics::gridGap * (cols - 1)) / cols), h = RecentCard::height();
        int i = 0;
        for (auto& ch : children()) {
            ch->setRect({(i % cols) * (w + metrics::gridGap), (i / cols) * (h + metrics::gridGap), w, h});
            ++i;
        }
    }
};

// =====================================================================================================================
// Loading placeholders: the top result beside four rows, then a shelf.

class TopRowSkeleton : public ui::Widget {
public:
    TopRowSkeleton() { hitTestVisible = false; }
    float preferredHeight(float width) override {
        const float card = std::max(212.f, 4 * rowH());
        return width >= 760 ? 48 + card : 48 + card + 32 + 48 + 4 * rowH();
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const float card = std::max(212.f, 4 * rowH());
        const bool wide = r.w >= 760;
        const float lw = wide ? std::clamp(r.w * 0.4f, 300.f, 460.f) : r.w;
        ui::paintSkeleton(c, this, {r.x, r.y + 10, 140, 22}, gfx::radii().badge);
        ui::paintSkeleton(c, this, {r.x, r.y + 48, lw, card}, gfx::radii().surface);
        const float rx = wide ? r.x + lw + 24 : r.x;
        const float ry = wide ? r.y : r.y + 48 + card + 32;
        const float rw = wide ? r.right() - rx : r.w;
        ui::paintSkeleton(c, this, {rx, ry + 10, 120, 22}, gfx::radii().badge);
        const float h = rowH(), a = TrackTable::rowMetrics().art;
        for (int i = 0; i < 4; ++i) {
            const float y = ry + 48 + i * h;
            ui::paintSkeleton(c, this, {rx + 12, y + (h - a) * 0.5f, a, a}, gfx::radii().art);
            ui::paintSkeleton(c, this, {rx + 24 + a, y + h * 0.5f - 12, std::min(rw * 0.5f, 240.f - (i % 3) * 40.f), 10}, gfx::radii().badge);
            ui::paintSkeleton(c, this, {rx + 24 + a, y + h * 0.5f + 4, std::min(rw * 0.3f, 120.f), 8}, gfx::radii().badge);
        }
    }
};

// =====================================================================================================================
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
        sub_ = gfx::Text(unsupported && link_.service == links::Service::ShadeTube
                             ? tr(L"Birlikte dinleme davetleri shadetube://jam/ ile başlar.")
                         : unsupported ? tr(L"Şarkı, albüm, çalma listesi, sanatçı, podcast ve profil bağlantıları desteklenir.")
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
        c.fillRounded(r, gfx::radii().surface, col.bgRaised);
        if (active) c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hover_));
        c.strokeRounded(r, gfx::radii().surface, col.hairDefault);
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

// =====================================================================================================================
// Genres (the empty view): a search for the genre (Spotify) or a MusicBrainz tag search (logged out).

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

// =====================================================================================================================
// Answers of the last searches (this run), so typing back and Back show results at once. Per catalog.

struct CachedSearch {
    std::string key;
    SearchResults results;
};
std::deque<CachedSearch>& searchCache() {
    static std::deque<CachedSearch> cache;
    return cache;
}
std::string cacheKey(const std::wstring& q) {
    std::wstring folded = browse::RecentSearches::normalizeQuery(q);
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return (source::loggedIn() ? "spotify|" : source::ytmMode() ? "ytm|" : "mb|") + toUtf8(folded);
}

// YouTube Music's "Moods & genres" tiles, fetched once per run (UI language) for the empty view; the pages that wait for
// them are called back when they land.
struct MoodCache {
    std::vector<ytm::Mood> moods;
    bool loading = false;
    bool failed = false;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    Lifetime life;
};
MoodCache& moodCache() {
    static MoodCache c;
    return c;
}

// A card of a mood's shelves: its playlists (albums / artists when YouTube Music puts some there), the subtitle as
// YouTube Music shows it.
ResultEntry shelfEntry(const CardItem& it) {
    ResultEntry e;
    e.title = toWide(it.title);
    e.subtitle = toWide(it.subtitle);
    e.images = it.images;
    const RouteKind kind = it.kind == CardItem::Kind::Album    ? RouteKind::Album
                           : it.kind == CardItem::Kind::Artist ? RouteKind::Artist
                                                               : RouteKind::Playlist;
    e.circle = kind == RouteKind::Artist;
    e.placeholder = kind == RouteKind::Artist ? Placeholder::Artist : kind == RouteKind::Album ? Placeholder::Album : Placeholder::Playlist;
    const Route open{kind, it.id};
    e.open = [open] { ctx().router->navigate(open); };
    if (!e.circle) e.play = [open] { ctx().router->navigate({open.kind, open.id + "#play"}); };
    e.preview = preview::collectionTarget(open, e.title, it.images);
    e.recent = {kind == RouteKind::Album ? "album" : kind == RouteKind::Artist ? "artist" : "playlist", it.id, e.title, e.subtitle,
                it.images, std::nullopt};
    return e;
}
const SearchResults* cachedSearch(const std::wstring& q) {
    const std::string key = cacheKey(q);
    for (const auto& c : searchCache())
        if (c.key == key) return &c.results;
    return nullptr;
}
void cacheSearch(const std::wstring& q, const SearchResults& r) {
    auto& cache = searchCache();
    const std::string key = cacheKey(q);
    std::erase_if(cache, [&](const CachedSearch& c) { return c.key == key; });
    cache.push_front({key, r});
    while (cache.size() > 16) cache.pop_back();
}

// =====================================================================================================================
// The page

class SearchPage : public ScrollPage {
public:
    explicit SearchPage(std::string initial);

    void onShown() override { box_->focus(); }
    bool focusSearch() override {
        if (auto* w = window()) w->focusByKeyboard(box_);
        box_->selectAll();
        return true;
    }
    void layout() override;
    void paint(Canvas& c) override;
    bool onKeyDown(const ui::KeyEvent& e) override;

    // Content builders of the kinds (ResultKind::custom).
    void songs(ui::Column* c, bool full);

private:
    // ---- views
    void showLink(links::Link link);
    void showBrowse(int focusIndex = -1);
    void rebuildBrowse(int focusIndex, bool keyboard);
    void showResults(int tab, bool scrollTop);
    void buildAll(ui::Column* c);
    void buildKind(ui::Column* c, size_t kind);
    void addTopRow(ui::Column* c);
    void showEmpty(ui::Column* c);
    void runTagSearch(const std::wstring& title, const std::string& tag);
    void addMoodTiles(ui::Column* c, bool afterCards);   // YouTube Music's moods & genres (the empty view)
    void runMoodSearch(const std::wstring& title, const std::string& params);
    void setTabs(std::vector<int> kinds, int selected);
    void selectTab(int chip);

    // ---- searching
    void scheduleSearch(const std::wstring& q);
    void runSearch(const std::wstring& q);
    void startRequest(const std::wstring& q);
    void stopLoading();
    void openRecent(const RecentItem& item);

    // ---- keyboard
    void nav(ui::Widget* w) { nav_.push_back(w); }
    ui::Widget* neighbor(const Rect& from, ui::Widget* self, int dx, int dy) const;
    void focusNav(ui::Widget* w, int dy);
    bool moveFrom(ui::Widget* from, int dx, int dy);
    void focusTop();   // the tabs (with results) or the box

    MediaCard* addCard(ui::Widget* parent, const ResultEntry& e);
    float headerBottom() const;

    // ---- more of one kind (Spotify, YouTube Music)
    static constexpr size_t kOverview = 12;   // rows of each kind in the overview search
    static constexpr int kPage = 30;
    void setResults(SearchResults r) {
        results_ = std::move(r);
        spotifyResults_ = source::loggedIn();   // the catalog the answer (or the cached one) came from
        ytmResults_ = !spotifyResults_ && source::ytmMode();
        exhausted_.clear();
    }
    bool pagedHere(const std::string& kindId) const {
        return (spotifyResults_ && pagedKind(kindId)) || (ytmResults_ && ytmKind(kindId));
    }
    bool canLoadMore(size_t kind) const;
    void loadMore(size_t kind);

    ui::TextBox* box_;
    ui::Button* recognize_ = nullptr;  // "Şarkıyı tanı" next to the box (app/Recognize)
    ChipTabs* tabs_ = nullptr;
    std::vector<int> tabKinds_;        // chip -> kind index (-1 = "Tümü")
    int tab_ = 0;                      // selected chip
    FadeColumn* layer_ = nullptr;      // the results shown (dims while the next ones load)
    std::vector<ui::Widget*> nav_;     // arrow-key stops of the content, in reading order
    std::wstring query_;               // the query of the results shown
    SearchResults results_;
    bool hasResults_ = false;
    std::wstring pending_;             // typed, waiting for the debounce
    double pendingAt_ = 0;
    std::wstring requested_;           // in flight
    std::shared_ptr<YoutubeExplode::CancellationTokenSource> cts_;
    Lifetime searchLife_;
    bool loading_ = false;
    double loadingSince_ = 0;
    bool linkShown_ = false;
    // "Daha fazla yükle" of the kind tab shown (Spotify, YouTube Music).
    bool spotifyResults_ = false;
    bool ytmResults_ = false;
    std::set<std::string> exhausted_;   // kinds of this query with no more pages
    size_t kindShown_ = SIZE_MAX;       // the kind whose tab is shown
    ui::Widget* kindGrid_ = nullptr;    // its cards
    TrackTable* kindTable_ = nullptr;   // or its songs
    ui::Button* moreButton_ = nullptr;
    bool moreLoading_ = false;
    Lifetime moreLife_;
};

const std::vector<ResultKind>& resultKinds() {
    static const std::vector<ResultKind> kinds = [] {
        std::vector<ResultKind> k;
        ResultKind songs;
        songs.id = "track";
        songs.title = tr(L"Şarkılar");
        songs.one = tr(L"Şarkı");
        songs.count = [](const SearchResults& r) { return r.tracks.size(); };
        songs.entry = trackEntry;
        songs.custom = &SearchPage::songs;
        songs.besideTop = true;
        songs.reopen = [](const RecentItem& it) {
            if (it.track) playSearchTracks({*it.track}, 0);
        };
        k.push_back(std::move(songs));
        k.push_back(cardKind<Artist>("artist", tr(L"Sanatçılar"), tr(L"Sanatçı"), &SearchResults::artists, artistEntry,
                                     RouteKind::Artist));
        k.push_back(cardKind<Album>("album", tr(L"Albümler"), tr(L"Albüm"), &SearchResults::albums, albumEntry,
                                    RouteKind::Album));
        // More kinds go here: cardKind(id, tab, one, &SearchResults::list, entryFn, route). The order is the tab order
        // after "Tümü" and the order of the sections.
        k.push_back(cardKind<Playlist>("playlist", tr(L"Çalma listeleri"), tr(L"Çalma listesi"), &SearchResults::playlists,
                                       playlistEntry, RouteKind::Playlist));
        auto shows = cardKind<PodcastShow>("show", tr(L"Podcastler"), tr(L"Podcast"), &SearchResults::shows, showEntry,
                                           RouteKind::Podcasts);
        shows.reopen = [](const RecentItem& it) {   // the name and publisher the page looks the feed up with
            PodcastShow s;
            s.id = it.id;
            s.name = toUtf8(it.title);
            s.publisher = toUtf8(it.subtitle);
            s.images = it.images;
            openShow(s);
        };
        k.push_back(std::move(shows));
        k.push_back(cardKind<Profile>("profile", tr(L"Profiller"), tr(L"Profil"), &SearchResults::profiles, profileEntry,
                                      RouteKind::Profile));
        return k;
    }();
    return kinds;
}

const ResultKind* kindById(const std::string& id) {
    for (const auto& k : resultKinds())
        if (k.id == id) return &k;
    return nullptr;
}

SearchPage::SearchPage(std::string initial) {
    box_ = add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Ne dinlemek istersin?  Sanatçı, albüm, şarkı"));
    // What plays on this PC or in the room, recognized (app/Recognize).
    recognize_ = add<ui::Button>(ui::ButtonKind::IconOutline, std::wstring(), "recognize");
    recognize_->setTooltip(commands::keyTip(tr(L"Şarkıyı tanı"), "recognize-song"));
    recognize_->onClick = [] { openSongRecognition(); };
    tabs_ = add<ChipTabs>();
    tabs_->setVisible(false);
    tabs_->onSelect = [this](int i) {
        Dispatcher::post([this, i, ref = searchLife_.ref()] {
            if (!ref.expired()) selectTab(i);
        });
    };
    adopt(release(scroll_));   // Tab order: the search box first, then the tabs and the results below it
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
        recents().recordQuery(s);   // Enter = a real search (typing alone never records)
        if (s != query_ || !hasResults_) runSearch(s);
    };
    // Down from the box: into the results (or the recent searches).
    box_->onKey = [this](const ui::KeyEvent& e) {
        if (e.vk != VK_DOWN || e.ctrl || e.alt || e.shift || nav_.empty()) return false;
        focusNav(nav_.front(), 1);
        return true;
    };
    box_->setText(toWide(initial));
    showBrowse();
    if (auto link = links::parse(std::string_view(initial))) showLink(std::move(link));
    else if (!initial.empty()) runSearch(toWide(initial));
}

float SearchPage::headerBottom() const {
    const float boxBottom = 28 + metrics::searchH;
    return tabs_ && tabs_->visible() ? tabs_->rect().bottom() + 12 : boxBottom + 12;
}

void SearchPage::layout() {
    const Rect r = rect();
    const float x = metrics::pageX, w = std::max(0.f, r.w - metrics::pageX * 2);
    const float bw = metrics::searchH;   // the recognize button: a circle as tall as the box, 12 to its right
    const float boxW = std::max(0.f, std::min(720.f, w - bw - 12));
    box_->setRect({x, 28, boxW, metrics::searchH});
    recognize_->setRect({x + boxW + 12, 28, bw, metrics::searchH});
    if (tabs_) tabs_->setRect({x, 28 + metrics::searchH + 16, w, tabs_->preferredHeight(w)});
    const float top = headerBottom();
    scroll_->setRect({0, top, r.w, std::max(0.f, r.h - top)});
}

void SearchPage::paint(Canvas& c) {
    ScrollPage::paint(c);
    const Rect r = rect();
    const float y = r.y + headerBottom() - 1;
    // A hairline under the header once results scroll under it; the loading line runs along it.
    if (scroll_->scrollY() > 0.5f) c.hline(r.x, r.right(), y, colors().hairDefault);
    if (loading_) {
        const double now = ui::frame::now();
        const float x0 = r.x + metrics::pageX, w = std::max(0.f, r.w - 2 * metrics::pageX);
        if (ui::motion::micro() && !ui::motion::reduced()) {
            // Indeterminate: a 30 % segment sweeping across in 1.2 s, fading in after a short wait (fast answers
            // never flash it).
            const float in = std::clamp(static_cast<float>((now - loadingSince_ - 150) / 200.0), 0.f, 1.f);
            const float t = static_cast<float>(std::fmod(now - loadingSince_, 1200.0) / 1200.0);
            const float sx = x0 - w * 0.3f + w * 1.3f * t;
            const float a = std::max(sx, x0), b = std::min(sx + w * 0.3f, x0 + w);
            if (b > a && in > 0) c.fillRect({a, y - 0.5f, b - a, 2}, accent().base.mulAlpha(in));
            ui::frame::requestNext();
        } else {
            c.fillRect({x0, y - 0.5f, w, 2}, accent().tint25);
        }
    }
    // Debounce: the search starts a moment after the last keystroke.
    if (!pending_.empty() && ui::frame::realNow() >= pendingAt_) {
        Dispatcher::post([this, q = pending_, ref = life_.ref()] {   // not while painting: it rebuilds the content
            if (!ref.expired() && q == box_->text()) runSearch(q);
        });
        pending_.clear();
    } else if (!pending_.empty()) {
        if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
    }
}

// ---- Views ----------------------------------------------------------------------------------------------------------

void SearchPage::showLink(links::Link link) {
    pending_.clear();
    query_.clear();
    hasResults_ = false;
    stopLoading();
    searchLife_.renew();
    linkShown_ = true;
    setTabs({}, 0);
    nav_.clear();
    layer_ = nullptr;
    auto* c = resetContent(20.f);
    c->setPadding(metrics::pageX, 20, 48);
    nav(c->add<LinkHint>(std::move(link)));
    contentReady();
}

void SearchPage::setTabs(std::vector<int> kinds, int selected) {
    if (kinds.empty()) {
        if (tabs_->visible()) {
            if (auto* w = window(); w && tabs_->focused()) w->focusByKeyboard(box_);
            tabs_->setVisible(false);
        }
        tabKinds_.clear();
        tab_ = 0;
        requestLayout();
        return;
    }
    if (kinds != tabKinds_) {
        std::vector<std::wstring> labels;
        for (int k : kinds) labels.push_back(k < 0 ? std::wstring(tr(L"Tümü")) : resultKinds()[static_cast<size_t>(k)].title);
        tabs_->setLabels(std::move(labels), selected);
        tabKinds_ = std::move(kinds);
    } else {
        tabs_->select(selected, false);
    }
    tabs_->setVisible(true);
    tab_ = selected;
    requestLayout();
}

void SearchPage::selectTab(int chip) {
    if (!hasResults_ || chip < 0 || chip >= static_cast<int>(tabKinds_.size())) return;
    if (tabs_) tabs_->select(chip, false);
    showResults(chip, true);
}

// Empty query: "Son aramalar" (when there are any) above the genre tiles. `focusIndex` >= 0: a recent search was
// removed from the keyboard -> the focus goes to the card now at that index (else to the search box).
void SearchPage::showBrowse(int focusIndex) {
    linkShown_ = false;
    hasResults_ = false;
    setTabs({}, 0);
    nav_.clear();
    layer_ = nullptr;
    auto* c = resetContent(0.f);
    c->setPadding(metrics::pageX, 20, 48);
    std::vector<RecentCard*> cards;
    std::vector<RecentItem> recent;
    for (const auto& it : recents().items())   // the kinds this build can open
        if (it.isQuery() || kindById(it.kind)) recent.push_back(it);
    if (!recent.empty()) {
        // Header like SectionHeader, with a plain "Temizle" link (no arrow: it is not a navigation).
        auto* head = c->add<ui::Box>();
        auto* clear = head->add<Button>(ButtonKind::Link, tr(L"Temizle"));
        clear->setTooltip(tr(L"Son aramaları temizle"));
        clear->onClick = [this, clear] {
            const bool keyboard = clear->keyboardFocused();
            recents().clear();
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
        auto* grid = c->add<RecentGrid>();
        c->setSpacingBefore(grid, 16);
        for (size_t i = 0; i < recent.size(); ++i) {
            const RecentItem& it = recent[i];
            const ResultKind* k = kindById(it.kind);
            auto* card = grid->add<RecentCard>(it, it.isQuery() ? std::wstring(tr(L"Arama")) : k->one);
            const int index = static_cast<int>(i);
            card->onOpen = [this, it] {
                Dispatcher::post([this, it, ref = life_.ref()] {
                    if (!ref.expired()) openRecent(it);
                });
            };
            card->onRemove = [this, it, index, card] {
                const bool keyboard = card->keyboardFocused();
                recents().remove(it);
                rebuildBrowse(keyboard ? index : -1, keyboard);
            };
            cards.push_back(card);
            nav(card);
        }
    }
    // Genres: Spotify's search for the genre, MusicBrainz' tags, or YouTube Music's own moods & genres.
    if (source::ytmMode()) {
        addMoodTiles(c, !cards.empty());
        scroll_->contentChanged();
        if (focusIndex >= 0)
            if (auto* w = window()) {
                if (cards.empty()) w->focusByKeyboard(box_);
                else w->focusByKeyboard(cards[std::min<size_t>(static_cast<size_t>(focusIndex), cards.size() - 1)]);
            }
        return;
    }
    const bool spotify = source::loggedIn();
    auto* genres = c->add<SectionHeader>(tr(L"Türlere göz at"), spotify ? std::wstring() : std::wstring(tr(L"MUSICBRAINZ ETİKETLERİ")));
    if (!cards.empty()) c->setSpacingBefore(genres, metrics::sectionGap + 8);
    auto* grid = c->add<ui::Grid>(180.f, metrics::gridGap, [](float) { return 104.f; });
    c->setSpacingBefore(grid, 16);
    int i = 0;
    for (const auto& g : genreList()) {
        wchar_t idx[8];
        swprintf(idx, 8, L"%02d", ++i);
        auto* t = grid->add<Tile>(idx, g.title, Color::rgb(g.color));
        const std::wstring title = g.title;
        const std::string tag = g.tag;
        t->onClick = [this, title, tag, spotify] {
            Dispatcher::post([this, title, tag, spotify, ref = life_.ref()] {
                if (ref.expired()) return;
                if (spotify) {   // Spotify has no tags: a search for the genre's name
                    box_->setText(title);
                    runSearch(title);
                } else {
                    runTagSearch(title, tag);
                }
            });
        };
        nav(t);
    }
    scroll_->contentChanged();
    if (focusIndex >= 0)
        if (auto* w = window()) {
            if (cards.empty()) w->focusByKeyboard(box_);
            else w->focusByKeyboard(cards[std::min<size_t>(static_cast<size_t>(focusIndex), cards.size() - 1)]);
        }
}

// After a recent search was removed / cleared (posted: the clicked card or link dies in the rebuild). Skipped if the
// user has started typing meanwhile.
void SearchPage::rebuildBrowse(int focusIndex, bool keyboard) {
    Dispatcher::post([this, focusIndex, keyboard, ref = life_.ref()] {
        if (ref.expired() || !box_->text().empty()) return;
        showBrowse(focusIndex);
        if (keyboard && focusIndex < 0)
            if (auto* w = window()) w->focusByKeyboard(box_);
    });
}

void SearchPage::openRecent(const RecentItem& item) {
    recents().record(item);   // to the front
    if (item.isQuery()) {
        box_->setText(item.title);
        runSearch(item.title);
        return;
    }
    const ResultKind* k = kindById(item.kind);
    if (!k) return;
    if (k->reopen) k->reopen(item);   // may navigate
    else if (k->route) ctx().router->navigate({*k->route, item.id});
}

void SearchPage::runTagSearch(const std::wstring& title, const std::string& tag) {
    searchLife_.renew();
    query_ = title;
    hasResults_ = false;
    setTabs({}, 0);
    nav_.clear();
    layer_ = nullptr;
    auto* c = resetContent(20.f);
    c->setPadding(metrics::pageX, 20, 48);
    c->add<home::ShelfSkeleton>(true);
    c->add<home::ShelfSkeleton>();
    scroll_->contentChanged();
    const std::string q = "tag:\"" + tag + "\"";
    struct Out {
        std::vector<Artist> artists;
        std::vector<Album> albums;
    };
    async(Priority::High, searchLife_.ref(),
          [q] {
              Out o;
              o.artists = mb::searchArtists(q, 18);
              o.albums = mb::searchAlbums(q + " AND primarytype:album", 24);
              return o;
          },
          [this, title](Result<Out> r) {
              if (!r) return showError(toWide(r.errorMessage()), [this] { showBrowse(); });
              nav_.clear();
              auto* c = resetContent(0.f);
              c->setPadding(metrics::pageX, 20, 48);
              c->add<PageTitle>(title);   // the content pages' title (fits narrow pages)
              if (!r->artists.empty()) {
                  auto* head = c->add<SectionHeader>(tr(L"Sanatçılar"), std::to_wstring(r->artists.size()));
                  c->setSpacingBefore(head, 24);
                  auto* row = home::shelfRow(c);
                  for (const auto& a : r->artists) nav(addArtistCard(row, a));
              }
              if (!r->albums.empty()) {
                  auto* head = c->add<SectionHeader>(tr(L"Albümler"), std::to_wstring(r->albums.size()));
                  c->setSpacingBefore(head, metrics::sectionGap);
                  auto* g = addCardGrid(c);
                  c->setSpacingBefore(g, 16);
                  for (const auto& a : r->albums) nav(addAlbumCard(g, a));
              }
              contentReady();
          });
}

// YouTube Music's "Moods & genres" (UI language): a section of tiles per group ("Moods & moments", "Genres"),
// placeholders until the first answer of this run lands; a tile opens that mood's playlists.
void SearchPage::addMoodTiles(ui::Column* c, bool afterCards) {
    auto& mc = moodCache();
    if (mc.moods.empty()) {
        auto* head = c->add<SectionHeader>(tr(L"Ruh hâli ve türler"), L"YOUTUBE MUSIC");
        if (afterCards) c->setSpacingBefore(head, metrics::sectionGap + 8);
        if (mc.failed) {
            c->add<MessagePanel>("offline", tr(L"Türler yüklenemedi"), tr(L"Bağlantını kontrol edip tekrar dene."), tr(L"Tekrar dene"),
                                 [this] {
                                     moodCache().failed = false;
                                     rebuildBrowse(-1, false);
                                 });
            return;
        }
        auto* placeholder = c->add<SkeletonCards>(2, 180.f, false, 104.f);   // the mood tiles' grid while they load
        c->setSpacingBefore(placeholder, 16);
        // This view comes back with the tiles when they land (unless the page shows something else by then).
        mc.listeners.emplace_back(searchLife_.ref(), [this] {
            if (box_->text().empty() && !linkShown_) showBrowse();
        });
        if (!mc.loading) {
            mc.loading = true;
            async(Priority::High, mc.life.ref(), [] { return ytm::moods(); },
                  [](Result<std::vector<ytm::Mood>> r) {
                      auto& m = moodCache();
                      m.loading = false;
                      if (r && !r->empty()) {
                          m.moods = std::move(*r);
                      } else {
                          m.failed = true;
                          ST_LOG_WARN("search", "YouTube Music moods: {}", r ? std::string("none") : r.errorMessage());
                      }
                      auto listeners = std::move(m.listeners);
                      m.listeners.clear();
                      for (auto& [ref, fn] : listeners)
                          if (!ref.expired()) fn();
                  });
        }
        return;
    }
    std::string section;
    bool first = true;
    ui::Grid* grid = nullptr;
    int i = 0;
    for (const auto& m : mc.moods) {
        if (!grid || m.section != section) {
            section = m.section;
            auto* head = c->add<SectionHeader>(section.empty() ? std::wstring(tr(L"Ruh hâli ve türler")) : toWide(section),
                                               first ? std::wstring(L"YOUTUBE MUSIC") : std::wstring());
            if (afterCards || !first) c->setSpacingBefore(head, metrics::sectionGap + (first ? 8.f : 0.f));
            first = false;
            grid = c->add<ui::Grid>(180.f, metrics::gridGap, [](float) { return 104.f; });
            c->setSpacingBefore(grid, 16);
            i = 0;
        }
        wchar_t idx[8];
        swprintf(idx, 8, L"%02d", ++i);
        auto* t = grid->add<Tile>(idx, toWide(m.title), tileColor(m.title));
        const std::wstring title = toWide(m.title);
        const std::string params = m.params;
        t->onClick = [this, title, params] {
            Dispatcher::post([this, title, params, ref = life_.ref()] {
                if (!ref.expired()) runMoodSearch(title, params);
            });
        };
        nav(t);
    }
}

// One mood of YouTube Music: its shelves of playlists, a row each.
void SearchPage::runMoodSearch(const std::wstring& title, const std::string& params) {
    searchLife_.renew();
    query_ = title;
    hasResults_ = false;
    setTabs({}, 0);
    nav_.clear();
    layer_ = nullptr;
    auto* c = resetContent(20.f);
    c->setPadding(metrics::pageX, 20, 48);
    c->add<home::ShelfSkeleton>();
    c->add<home::ShelfSkeleton>();
    scroll_->contentChanged();
    async(Priority::High, searchLife_.ref(), [params] { return ytm::moodShelves(params); },
          [this, title, params](Result<std::vector<Shelf>> r) {
              if (!r) return showError(toWide(r.errorMessage()), [this, title, params] { runMoodSearch(title, params); });
              nav_.clear();
              auto* c = resetContent(metrics::sectionGap);
              c->setPadding(metrics::pageX, 20, 48);
              c->add<PageTitle>(title);   // the content pages' title (fits narrow pages)
              for (const auto& shelf : *r) {
                  auto* row = home::addShelf(c, toWide(shelf.title), {});
                  for (const auto& it : shelf.items)
                      if (it.kind != CardItem::Kind::Track) addCard(row, shelfEntry(it));
              }
              if (r->empty())
                  c->add<MessagePanel>("search", tr(L"Sonuç bulunamadı"), tr(L"YouTube Music bu tür için liste göstermedi."));
              contentReady();
          });
}

MediaCard* SearchPage::addCard(ui::Widget* parent, const ResultEntry& e) {
    auto* card = parent->add<MediaCard>(e.title, e.subtitle, e.images,
                                        e.circle ? MediaCard::Shape::Circle : MediaCard::Shape::Square, e.placeholder);
    auto remember = [recent = e.recent](std::function<void()> fn) {
        return [recent, fn = std::move(fn)] {
            recents().record(recent);
            fn();
        };
    };
    if (e.open) card->onOpen = remember(e.open);
    if (e.play && !e.circle) card->onPlay = remember(e.play);   // round cards have no play button
    if (e.menu) card->onContext = e.menu;
    card->previewTarget = e.preview;
    nav(card);
    return card;
}

// The results: "Tümü" (chip 0) or one kind. `scrollTop`: a new query or another tab starts at the top.
void SearchPage::showResults(int tab, bool scrollTop) {
    const auto& kinds = resultKinds();
    std::vector<int> visible{-1};
    for (size_t k = 0; k < kinds.size(); ++k)
        if (kinds[k].count(results_) > 0) visible.push_back(static_cast<int>(k));
    // The tab shown so far, if this query has it too.
    const int keepKind = tab > 0 && tab < static_cast<int>(tabKinds_.size()) ? tabKinds_[static_cast<size_t>(tab)] : -1;
    int chip = 0;
    for (size_t i = 0; i < visible.size(); ++i)
        if (visible[i] == keepKind) chip = static_cast<int>(i);
    hasResults_ = true;
    setTabs(visible.size() > 1 ? visible : std::vector<int>{}, chip);
    tab_ = chip;
    nav_.clear();
    // A page of more results still on its way belongs to the view being replaced.
    moreLife_.renew();
    moreLoading_ = false;
    moreButton_ = nullptr;
    kindGrid_ = nullptr;
    kindTable_ = nullptr;
    kindShown_ = SIZE_MAX;
    const float was = layer_ ? layer_->opacity() : 1.f;
    auto* c = resetContent(0.f);
    c->setPadding(metrics::pageX, 8, 48);
    layer_ = c->add<FadeColumn>(metrics::sectionGap);
    if (was < 0.99f) {   // the dimmed results give way to the new ones: they come up from where those were
        layer_->snapOpacity(was);
        layer_->fadeTo(1.f, ui::motion::microMs(ui::motion::fast));
    }
    if (visible.size() <= 1) showEmpty(layer_);
    else if (chip == 0) buildAll(layer_);
    else buildKind(layer_, static_cast<size_t>(visible[static_cast<size_t>(chip)]));
    if (scrollTop) scroll_->scrollTo(0, false);
    contentReady();
    // Back returns to these results (only while this page is the one shown: a late answer to a page being left
    // must not rename the next page's history entry).
    if (auto* router = ctx().router; router && router->currentPage && router->currentPage() == this)
        router->retarget({RouteKind::Search, toUtf8(query_)});
}

void SearchPage::showEmpty(ui::Column* c) {
    c->add<MessagePanel>("search", tr(L"Sonuç bulunamadı"),
                         i18n::format(tr(L"\"{}\" için bir şey bulamadık. Yazımı kontrol et ya da başka bir şey dene."),
                                      {query_}));
}

// "Tümü": the top result beside the first songs, then a shelf per kind of cards.
void SearchPage::buildAll(ui::Column* c) {
    addTopRow(c);
    const auto& kinds = resultKinds();
    for (size_t k = 0; k < kinds.size(); ++k) {
        const auto& kind = kinds[k];
        const size_t n = kind.count(results_);
        if (n == 0 || kind.besideTop) continue;
        if (kind.custom) {
            (this->*kind.custom)(c, false);
            continue;
        }
        const int chip = static_cast<int>(std::find(tabKinds_.begin(), tabKinds_.end(), static_cast<int>(k)) - tabKinds_.begin());
        auto* row = home::addShelf(c, kind.title, {}, tr(L"Tümünü gör"), [this, chip] {
            Dispatcher::post([this, chip, ref = searchLife_.ref()] {
                if (!ref.expired()) selectTab(chip);
            });
        });
        for (size_t i = 0; i < n; ++i) addCard(row, kind.entry(results_, i));
    }
}

// One kind's own tab: everything (songs as a full list, cards as a grid), and with Spotify "Daha fazla yükle" while
// the kind has more.
void SearchPage::buildKind(ui::Column* c, size_t k) {
    const auto& kind = resultKinds()[k];
    kindShown_ = k;
    if (kind.custom) {
        (this->*kind.custom)(c, true);
    } else {
        kindGrid_ = addCardGrid(c);
        const size_t n = kind.count(results_);
        for (size_t i = 0; i < n; ++i) addCard(kindGrid_, kind.entry(results_, i));
    }
    if (pagedHere(kind.id)) {
        moreButton_ = addLoadMore(c, [this, k] { loadMore(k); });
        setLoadMore(moreButton_, canLoadMore(k), false);
        nav(moreButton_);
    }
}

// The catalog gave a full overview row (12) of the kind, and its pages have not run out.
bool SearchPage::canLoadMore(size_t k) const {
    const auto& kind = resultKinds()[k];
    return pagedHere(kind.id) && !exhausted_.contains(kind.id) && kind.count(results_) >= kOverview;
}

// The kind's next page (Api::searchMore / ytm::searchMore from the rows already here): new rows go to results_ (and the
// cache) and, while the tab is still on screen, to its grid / list. A page that adds nothing ends the kind.
void SearchPage::loadMore(size_t k) {
    const auto& kind = resultKinds()[k];
    const auto paged = pagedKind(kind.id);
    const auto ytmPaged = ytmKind(kind.id);
    spotify::Api* api = spotifyResults_ ? source::activeApi() : nullptr;
    if (moreLoading_ || !canLoadMore(k) || (spotifyResults_ && (!paged || !api)) || (ytmResults_ && !ytmPaged)) return;
    moreLoading_ = true;
    setLoadMore(moreButton_, true, true);
    const int offset = static_cast<int>(kind.count(results_));
    const bool ytmMore = ytmResults_;
    async(Priority::High, moreLife_.ref(),
          [api, paged, ytmPaged, ytmMore, q = toUtf8(query_), offset] {
              MoreBatch b;
              if (ytmMore) {
                  auto r = ytm::searchMore(q, *ytmPaged, offset, kPage);
                  b.results = std::move(r.results);
                  b.hasMore = r.hasMore;
              } else {
                  auto r = api->searchMore(*paged, q, offset, kPage);
                  b.results = std::move(r.results);
                  b.hasMore = r.hasMore();
              }
              return b;
          },
          [this, k, search = searchLife_.ref()](Result<MoreBatch> r) {
              if (search.expired()) return;   // another query / view meanwhile
              moreLoading_ = false;
              const auto& kind = resultKinds()[k];
              if (!r) {
                  ST_LOG_WARN("search", "more {} failed: {}", kind.id, r.errorMessage());
                  toast(tr(L"Daha fazla sonuç yüklenemedi"), true);
                  setLoadMore(moreButton_, true, false);
                  return;
              }
              const size_t before = kind.count(results_);
              auto merge = [](auto& into, auto& more) {
                  for (auto& v : more)
                      if (std::none_of(into.begin(), into.end(), [&](const auto& o) { return o.id == v.id; }))
                          into.push_back(std::move(v));
              };
              auto& R = results_;
              merge(R.tracks, r->results.tracks);
              merge(R.albums, r->results.albums);
              merge(R.artists, r->results.artists);
              merge(R.playlists, r->results.playlists);
              merge(R.shows, r->results.shows);
              merge(R.profiles, r->results.profiles);
              if (kind.count(results_) == before || !r->hasMore) exhausted_.insert(kind.id);
              cacheSearch(query_, results_);   // typing back shows them too
              if (kindShown_ == k) {
                  if (kind.custom) {
                      if (kindTable_) kindTable_->setTracks(results_.tracks);
                  } else if (kindGrid_) {
                      for (size_t i = before, n = kind.count(results_); i < n; ++i) addCard(kindGrid_, kind.entry(results_, i));
                  }
              }
              setLoadMore(moreButton_, canLoadMore(k), false);
              scroll_->contentChanged();
              invalidate();
          });
}

// The top result (the best match among every kind's results) and, beside it, the first songs ("Tümünü gör" -> their
// tab). Narrow pages stack them.
void SearchPage::addTopRow(ui::Column* c) {
    const auto& kinds = resultKinds();
    std::vector<browse::TopCandidate> cands;
    std::vector<size_t> candKind;
    for (const std::string id : {"artist", "track", "album"})   // priority order; other kinds follow
        for (size_t k = 0; k < kinds.size(); ++k)
            if (kinds[k].id == id)
                for (size_t i = 0, n = std::min<size_t>(kinds[k].count(results_), 8); i < n; ++i) {
                    const ResultEntry e = kinds[k].entry(results_, i);
                    cands.push_back({kinds[k].id, i, e.title, e.recent.id});
                    candKind.push_back(k);
                }
    for (size_t k = 0; k < kinds.size(); ++k)
        if (kinds[k].id != "artist" && kinds[k].id != "track" && kinds[k].id != "album")
            for (size_t i = 0, n = std::min<size_t>(kinds[k].count(results_), 4); i < n; ++i) {
                const ResultEntry e = kinds[k].entry(results_, i);
                cands.push_back({kinds[k].id, i, e.title, e.recent.id});
                candKind.push_back(k);
            }
    // Spotify names its own top hit: that one, wherever it is in its list. Otherwise (MusicBrainz, or a top hit of a
    // kind not shown) the best match by name.
    int top = -1;
    if (const std::string& hit = results_.topResult; !hit.empty()) {
        for (size_t i = 0; i < cands.size() && top < 0; ++i)
            if (cands[i].id == hit) top = static_cast<int>(i);
        for (size_t k = 0; k < kinds.size() && top < 0; ++k)
            for (size_t i = 0, n = kinds[k].count(results_); i < n; ++i)
                if (const ResultEntry e = kinds[k].entry(results_, i); e.recent.id == hit) {
                    cands.push_back({kinds[k].id, i, e.title, e.recent.id});
                    candKind.push_back(k);
                    top = static_cast<int>(cands.size()) - 1;
                    break;
                }
    }
    std::vector<std::string> songArtists;
    for (size_t i = 0; i < results_.tracks.size() && i < 3; ++i)
        for (const auto& a : results_.tracks[i].artists) songArtists.push_back(a.id);
    if (top < 0) top = browse::pickTopResult(query_, cands, songArtists);
    const bool hasSongs = !results_.tracks.empty();
    if (top < 0 && !hasSongs) return;

    auto* row = c->add<ui::Box>();
    row->hitTestVisible = false;
    ui::Column* left = nullptr;
    if (top >= 0) {
        const auto& kind = kinds[candKind[static_cast<size_t>(top)]];
        left = row->add<ui::Column>(0.f);
        left->add<SectionHeader>(tr(L"En iyi sonuç"));
        auto* card = left->add<TopResultCard>(kind.entry(results_, cands[static_cast<size_t>(top)].index), kind.one);
        left->setSpacingBefore(card, 16);
        nav(card);
        // MusicBrainz' search hits carry no photos: the top artist's comes from its lookup (cached after the first).
        if (const auto& e = card->entry(); e.circle && e.images.empty() && browse::isMbid(e.recent.id))
            async(Priority::Normal, card->ref(), [id = e.recent.id] { return mb::artist(id).images; },
                  [card](Result<std::vector<Image>> r) {
                      if (r && !r->empty()) card->setImages(std::move(*r));
                  });
    }
    ui::Column* right = nullptr;
    if (hasSongs) {
        right = row->add<ui::Column>(0.f);
        songs(right, false);
    }
    // Side by side from 760 DIP (the card 40 % of the width, 300..460), else stacked.
    auto place = [left, right](float w, bool apply) {
        const bool wide = w >= 760 && left && right;
        const float lw = !left ? 0.f : wide ? std::clamp(w * 0.4f, 300.f, 460.f) : std::min(w, 520.f);
        const float lh = left ? left->preferredHeight(lw) : 0.f;
        const float rx = wide ? lw + 24 : 0.f, rw = wide ? w - rx : w;
        const float ry = wide || !left ? 0.f : lh + metrics::sectionGap;
        const float rh = right ? right->preferredHeight(rw) : 0.f;
        if (apply) {
            if (left) left->setRect({0, 0, lw, lh});
            if (right) right->setRect({rx, ry, rw, rh});
        }
        return wide ? std::max(lh, rh) : ry + rh;
    };
    row->onPreferredHeight = [place](float w) { return place(w, false); };
    row->onLayout = [place](ui::Box& b) { place(b.rect().w, true); };
}

// The songs: four rows next to the top result ("Tümü"), every one in their tab.
void SearchPage::songs(ui::Column* c, bool full) {
    const auto& R = results_;
    if (!full) {
        const auto& kinds = resultKinds();
        int chip = 0;
        for (size_t i = 0; i < tabKinds_.size(); ++i)
            if (tabKinds_[i] >= 0 && kinds[static_cast<size_t>(tabKinds_[i])].id == "track") chip = static_cast<int>(i);
        auto* head = c->add<SectionHeader>(tr(L"Şarkılar"), L"", R.tracks.size() > 4 ? std::wstring(tr(L"Tümünü gör")) : std::wstring());
        head->onLink = [this, chip] {
            Dispatcher::post([this, chip, ref = searchLife_.ref()] {
                if (!ref.expired()) selectTab(chip);
            });
        };
    }
    TrackTable::Options o;
    o.showAdded = false;
    o.showHeader = full;
    auto* table = c->add<TrackTable>(o);
    if (!full) c->setSpacingBefore(table, 16 - 8);
    std::vector<Track> shown(R.tracks.begin(), R.tracks.begin() + std::min<size_t>(full ? R.tracks.size() : 4, R.tracks.size()));
    table->setTracks(shown);
    table->onPlay = [this](int i) { playSearchTracks(results_.tracks, i); };   // the rows are results_'s first ones
    if (full) kindTable_ = table;   // "Daha fazla yükle" adds to it
    // Up on the first row / Down on the last: on to what is above / below the list.
    table->onEdge = [this, table](int dir) { return moveFrom(table, 0, dir); };
    nav(table);
}

// ---- Searching ------------------------------------------------------------------------------------------------------

void SearchPage::scheduleSearch(const std::wstring& q) {
    if (browse::RecentSearches::normalizeQuery(q).empty()) {
        pending_.clear();
        query_.clear();
        requested_.clear();
        if (cts_) cts_->cancel();
        searchLife_.renew();
        stopLoading();
        if (auto* router = ctx().router; router && router->currentPage && router->currentPage() == this)
            router->retarget({RouteKind::Search, ""});
        showBrowse();
        return;
    }
    if (q.size() < 2) return;
    if (const SearchResults* hit = cachedSearch(q)) {   // typed back to an earlier query: at once
        pending_.clear();
        if (cts_) cts_->cancel();
        searchLife_.renew();
        stopLoading();
        const bool sameQuery = q == query_;
        query_ = q;
        requested_.clear();
        setResults(*hit);
        showResults(tab_, !sameQuery);
        return;
    }
    pending_ = q;
    // Spotify answers in a few hundred ms; MusicBrainz allows one request a second (a search is three).
    pendingAt_ = ui::frame::realNow() + (source::loggedIn() ? 220 : 380);
    if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
}

void SearchPage::runSearch(const std::wstring& q) {
    if (browse::RecentSearches::normalizeQuery(q).empty()) return;
    linkShown_ = false;
    if (const SearchResults* hit = cachedSearch(q)) {
        if (cts_) cts_->cancel();
        searchLife_.renew();
        stopLoading();
        const bool sameQuery = q == query_;
        query_ = q;
        setResults(*hit);
        showResults(tab_, !sameQuery);
        return;
    }
    startRequest(q);
}

void SearchPage::startRequest(const std::wstring& q) {
    if (q == requested_ && loading_) return;
    if (cts_) cts_->cancel();   // the previous search stops where it is (MusicBrainz: before its next request)
    cts_ = std::make_shared<YoutubeExplode::CancellationTokenSource>();
    searchLife_.renew();
    requested_ = q;
    if (!loading_) loadingSince_ = ui::frame::realNow();
    loading_ = true;
    invalidate();
    if (hasResults_ && layer_) {
        // The results stay while the next ones load, dimmed (after a moment: a fast answer replaces them first).
        layer_->fadeTo(0.55f, ui::motion::microMs(ui::motion::medium));
    } else {
        // The first results: placeholders in their shape.
        setTabs({}, 0);
        nav_.clear();
        layer_ = nullptr;
        auto* c = resetContent(metrics::sectionGap);
        c->setPadding(metrics::pageX, 8, 48);
        c->add<TopRowSkeleton>();
        c->add<home::ShelfSkeleton>(true);
        scroll_->contentChanged();
    }
    const std::string query = toUtf8(q);
    spotify::Api* api = source::activeApi();
    const auto token = cts_->token();
    async(Priority::High, searchLife_.ref(), [query, api, token] { return source::search(api, query, 12, token); },
          [this, q](Result<SearchResults> r) {
              if (q != requested_) return;   // a newer search follows
              requested_.clear();
              stopLoading();
              if (!r) {
                  ST_LOG_WARN("search", "search failed: {}", r.errorMessage());
                  hasResults_ = false;
                  setTabs({}, 0);
                  layer_ = nullptr;
                  showError(toWide(r.errorMessage()), [this, q] { runSearch(q); });
                  return;
              }
              cacheSearch(q, *r);
              const bool sameQuery = q == query_;
              query_ = q;
              setResults(std::move(*r));
              showResults(tab_, !sameQuery);
          });
}

void SearchPage::stopLoading() {
    if (!loading_) return;
    loading_ = false;
    invalidate();
}

// ---- Keyboard -------------------------------------------------------------------------------------------------------

// The nearest stop from `from` (window DIPs) in a direction: the next row's item closest to its center (Up / Down),
// the next item on the same row (Left / Right).
ui::Widget* SearchPage::neighbor(const Rect& from, ui::Widget* self, int dx, int dy) const {
    struct Hit {
        ui::Widget* w;
        float primary, secondary;
    };
    std::vector<Hit> hits;
    for (ui::Widget* w : nav_) {
        if (w == self || !w->visible()) continue;
        const Rect b = w->toWindow(w->rect());
        if (dy != 0) {
            const float gap = dy > 0 ? b.y - from.bottom() : from.y - b.bottom();
            if (gap < -4) continue;
            hits.push_back({w, std::max(0.f, gap), std::abs(b.cx() - from.cx())});
        } else {
            const float overlap = std::min(from.bottom(), b.bottom()) - std::max(from.y, b.y);
            if (overlap < std::min(from.h, b.h) * 0.3f) continue;
            const float gap = dx > 0 ? b.x - from.right() : from.x - b.right();
            if (gap < -4) continue;
            hits.push_back({w, std::max(0.f, gap), std::abs(b.cy() - from.cy())});
        }
    }
    if (hits.empty()) return nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (const auto& h : hits) nearest = std::min(nearest, h.primary);
    const Hit* best = nullptr;
    for (const auto& h : hits)
        if (h.primary <= nearest + 8 && (!best || h.secondary < best->secondary)) best = &h;
    return best->w;
}

void SearchPage::focusNav(ui::Widget* w, int dy) {
    auto* win = window();
    if (!w || !win) return;
    if (auto* table = dynamic_cast<TrackTable*>(w); table && !table->tracks().empty())
        table->selectRows({dy < 0 ? static_cast<int>(table->tracks().size()) - 1 : 0});
    win->focusByKeyboard(w);
}

void SearchPage::focusTop() {
    auto* w = window();
    if (!w) return;
    if (tabs_ && tabs_->visible()) w->focusByKeyboard(tabs_);
    else w->focusByKeyboard(box_);
}

bool SearchPage::moveFrom(ui::Widget* from, int dx, int dy) {
    if (ui::Widget* next = neighbor(from->toWindow(from->rect()), from, dx, dy)) {
        focusNav(next, dy);
        return true;
    }
    if (dy < 0) {   // above the first row: the tabs / the box
        focusTop();
        return true;
    }
    return false;
}

bool SearchPage::onKeyDown(const ui::KeyEvent& e) {
    auto* w = window();
    if (!w) return false;
    // Ctrl+Tab / Ctrl+Shift+Tab: the next / previous tab, wherever the focus is on the page.
    if (e.vk == VK_TAB && e.ctrl && !e.alt && tabs_ && tabs_->visible() && tabs_->count() > 1) {
        const int n = static_cast<int>(tabs_->count());
        const int next = ((tabs_->selected() + (e.shift ? -1 : 1)) % n + n) % n;
        tabs_->select(next, false);
        selectTab(next);
        return true;
    }
    if (e.ctrl || e.alt) return false;
    ui::Widget* f = w->focusedWidget();
    if (f == tabs_ && e.vk == VK_DOWN) {
        if (!nav_.empty()) focusNav(nav_.front(), 1);
        return true;
    }
    // Arrow keys move between the results; Escape goes back to the box.
    const auto it = std::find(nav_.begin(), nav_.end(), f);
    if (it == nav_.end()) return false;
    switch (e.vk) {
    case VK_LEFT: return moveFrom(f, -1, 0);
    case VK_RIGHT: return moveFrom(f, 1, 0);
    case VK_UP: return moveFrom(f, 0, -1);
    case VK_DOWN: return moveFrom(f, 0, 1);
    case VK_ESCAPE:
        w->focusByKeyboard(box_);
        box_->selectAll();
        return true;
    default: return false;
    }
}

} // namespace

std::unique_ptr<Page> makeSearchPage(const std::string& q) { return std::make_unique<SearchPage>(q); }

} // namespace st::app
