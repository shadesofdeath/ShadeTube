// Ana Sayfa. Logged in: Spotify's personalized shelves ("home" feed: Made For You, Jump back in, Daily Mixes...)
// followed by the user's library. Logged out: editorial greeting, recently played (local history), trending on
// ListenBrainz, fresh releases, your playlists, artists similar to what you play most.
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SmartListsPage.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>

namespace st::app {

using gfx::accent;
using gfx::colors;
using namespace catalog;
namespace type = gfx::type;

namespace {

// The headline's two lines: before / after the "\n" of greeting() (a translation without one is all bold).
std::pair<std::wstring, std::wstring> greetingLines() {
    const std::wstring g = greeting();
    const size_t nl = g.find(L'\n');
    if (nl == std::wstring::npos) return {g, std::wstring()};
    return {g.substr(0, nl), g.substr(nl + 1)};
}

class Greeting : public ui::Widget {
public:
    Greeting(std::wstring meta1, std::wstring meta2) : Greeting(greetingLines(), std::move(meta1), std::move(meta2)) {}
    Greeting(std::pair<std::wstring, std::wstring> lines, std::wstring meta1, std::wstring meta2)
        : line1_(std::move(lines.first), type::displayL), line2_(std::move(lines.second), type::displayL.withWeight(300)),
          meta1_(std::move(meta1), type::monoLabel), meta2_(std::move(meta2), type::monoLabel) {
        hitTestVisible = false;
        gfx::TextOptions right;
        right.align = gfx::TextAlign::Trailing;
        meta1_.setOptions(right);
        meta2_.setOptions(right);
    }
    float preferredHeight(float) override { return 132; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.text(line1_, {r.x - 3, r.y, r.w * 0.7f, 64}, col.fgPrimary);
        c.text(line2_, {r.x - 3, r.y + 62, r.w * 0.7f, 64}, col.fgPrimary);
        c.text(meta1_, {r.right() - 360, r.bottom() - 44, 360, 14}, col.fgTertiary);
        c.text(meta2_, {r.right() - 360, r.bottom() - 26, 360, 14}, col.fgTertiary);
    }

private:
    gfx::Text line1_, line2_, meta1_, meta2_;
};

// "PAZARTESİ · 27 EYL": the weekday name comes from Windows for the UI language.
std::wstring todayLabel() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    return toUpperTr(i18n::weekdayName(tm.tm_wday)) + L" · " + trDate(t);
}

std::wstring albumSubtitle(const Album& a) {
    std::wstring s = a.artists.empty() ? L"" : toWide(a.artists[0].name);
    if (!a.year().empty()) s += (s.empty() ? L"" : L" · ") + toWide(a.year());
    return s;
}

// Spotify's personalized home shelves (Api::home), kept for the whole app session. Pages are rebuilt on every
// navigation and the session notifies several times while the library loads, so the feed is fetched once per
// user, refreshed when stale, and never retried in a tight loop after a failure (Spotify 429s hard).
// UI thread only.
class ShelfCache {
public:
    static ShelfCache& instance() {
        static ShelfCache cache;
        return cache;
    }

    // Shelves for this user (possibly stale), or nullptr until the first fetch lands.
    const std::vector<Shelf>* shelvesFor(const std::string& userId) const {
        return hasData_ && userId_ == userId ? &shelves_ : nullptr;
    }

    // Starts a background fetch when nothing fresh is cached for this user and no retry back-off is pending.
    // The Api* is captured here, on the UI thread; the request runs on a worker. `owner` = the asking page.
    void ensure(const std::string& userId, Lifetime::Ref owner) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (userId != userId_) {
            reset();
            userId_ = userId;
        }
        if (inFlight_ || (hasData_ && now - fetchedAt_ < kFreshFor) || now < retryAt_) return;
        spotify::Api* api = source::activeApi();
        if (!api) return;
        inFlight_ = true;
        const unsigned gen = generation_;
        // The continuation runs on the cache's own Lifetime (never renewed): a fetch started by a page that is
        // navigated away from still fills the cache, so the next Home shows the shelves immediately. That Lifetime
        // never expires, so the asking page guards the START of the work instead: at app exit the thread pool drains
        // its queue after ~App destroyed the Session (and its Api), and every page is gone by then.
        async(
            Priority::Normal, life_.ref(),
            [api, owner]() -> std::optional<std::vector<Shelf>> {
                if (owner.expired()) return std::nullopt;
                return api->home();
            },
            [this, gen](Result<std::optional<std::vector<Shelf>>> r) {
                if (gen != generation_) return;   // logged out / switched account meanwhile
                inFlight_ = false;
                if (r && !*r) {   // skipped: the asking page closed before a worker picked the fetch up
                    notify();     // a Home opened meanwhile saw this fetch in flight: let it ask again
                    return;
                }
                if (!r) {
                    int status = 0;
                    try {
                        std::rethrow_exception(r.error());
                    } catch (const spotify::ApiError& e) {
                        status = e.status;
                    } catch (...) {
                    }
                    retryAt_ = steady_clock::now() + (status == 429 ? minutes(10) : minutes(3));
                    ST_LOG_WARN("home", "Spotify home shelves unavailable (HTTP {}): {}; showing the library-only home",
                                status, r.errorMessage());
                    return;
                }
                shelves_ = std::move(**r);
                hasData_ = true;
                fetchedAt_ = steady_clock::now();
                ST_LOG_INFO("home", "{} Spotify home shelves", shelves_.size());
                notify();
            });
    }

    void reset() {
        ++generation_;
        inFlight_ = false;
        hasData_ = false;
        shelves_.clear();
        userId_.clear();
        retryAt_ = {};
    }

    // Fired (UI thread) when fresh shelves arrive; dies with the owner's Lifetime.
    void subscribe(Lifetime::Ref owner, std::function<void()> fn) {
        std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
        listeners_.emplace_back(std::move(owner), std::move(fn));
    }

private:
    static constexpr std::chrono::minutes kFreshFor{20};

    void notify() {
        std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
        const auto snapshot = listeners_;   // a listener may rebuild a page that re-subscribes
        for (const auto& [owner, fn] : snapshot)
            if (!owner.expired() && fn) fn();
    }

    std::string userId_;
    std::vector<Shelf> shelves_;
    bool hasData_ = false;
    bool inFlight_ = false;
    unsigned generation_ = 0;
    std::chrono::steady_clock::time_point fetchedAt_{}, retryAt_{};
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    Lifetime life_;
};

// Our own library sections already cover these Spotify shelves ("Your playlists" -> "Çalma listelerin"). Spotify
// titles its shelves in the UI language (Accept-Language), so they are compared with our titles as shown.
bool duplicatesLibrarySection(const Shelf& shelf) {
    const std::wstring own[] = {toUpperTr(tr(L"Çalma listelerin")), toUpperTr(tr(L"Kaydettiğin albümler")),
                                toUpperTr(tr(L"Takip ettiğin sanatçılar")), toUpperTr(tr(L"Son çalınanlar"))};
    const std::wstring t = toUpperTr(toWide(shelf.title));
    for (const auto& o : own)
        if (t == o) return true;
    return false;
}

// One Spotify shelf: header + a single row of cards. Playlists/albums get a hover play button ("#play").
void addSpotifyShelf(ui::Column* c, const Shelf& shelf) {
    c->add<SectionHeader>(toWide(shelf.title), toUpperTr(toWide(shelf.label)));
    auto* grid = addCardRow(c, 150, 1);
    c->setSpacingBefore(grid, 16);
    for (const auto& it : shelf.items) {
        Route open;
        bool playable = false;
        auto shape = MediaCard::Shape::Square;
        auto ph = Placeholder::Album;
        switch (it.kind) {
        case CardItem::Kind::Playlist:
            open = it.id == spotify::Api::kLikedSongsUri ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, it.id};
            playable = true;
            ph = Placeholder::Playlist;
            break;
        case CardItem::Kind::Album:
            open = {RouteKind::Album, it.id};
            playable = true;
            break;
        case CardItem::Kind::Artist:
            open = {RouteKind::Artist, it.id};
            shape = MediaCard::Shape::Circle;
            ph = Placeholder::Artist;
            break;
        case CardItem::Kind::Track:   // opens the track's album
            if (it.track.album.id.empty()) continue;
            open = {RouteKind::Album, it.track.album.id};
            break;
        }
        auto* card = grid->add<MediaCard>(toWide(it.title), toWide(it.subtitle), it.images, shape, ph);
        card->onOpen = [open] { ctx().router->navigate(open); };
        if (playable) {
            Route play = open;
            play.id += "#play";
            card->onPlay = [play] { ctx().router->navigate(play); };
        }
    }
}

// Two-column block: tiles (left) + fresh releases list (right).
class SplitBlock : public ui::Widget {
public:
    SplitBlock() { hitTestVisible = false; }
    ui::Widget* left = nullptr;
    ui::Widget* right = nullptr;
    float preferredHeight(float w) override {
        const float lh = left ? left->preferredHeight(leftW(w)) : 0;
        const float rh = right ? right->preferredHeight(w - leftW(w) - 48) : 0;
        return std::max(lh, rh);
    }
    void layout() override {
        const Rect r = rect();
        const float lw = right ? leftW(r.w) : r.w;
        if (left) left->setRect({0, 0, lw, left->preferredHeight(lw)});
        if (right) right->setRect({left ? lw + 48 : 0, 0, left ? r.w - lw - 48 : r.w, right->preferredHeight(r.w - lw - 48)});
    }

private:
    float leftW(float w) const { return (left && right) ? std::floor(w * 0.56f) : w; }
};

class HomePage : public ScrollPage {
public:
    HomePage() {
        // Rebuild when the Spotify session (and its library) changes: logging in swaps the whole feed.
        if (ctx().session)
            ctx().session->subscribe(life_.ref(), [this] { reload(); });
        // ...and when the personalized shelves arrive (or refresh).
        ShelfCache::instance().subscribe(life_.ref(), [this] {
            if (source::loggedIn()) buildSpotify();
        });
        reload();
    }

private:
    void reload() {
        if (source::loggedIn()) {
            buildSpotify();
            return;
        }
        if (!ctx().session || ctx().session->state() == spotify::SessionState::LoggedOut)
            ShelfCache::instance().reset();   // logged out: forget the previous user's feed
        showSkeleton(4);
        load();
    }

    // Logged in: Spotify's personalized shelves (fetched in the background, cached per user by ShelfCache),
    // then the user's library — playlists, saved albums, followed artists — plus what they've recently played on
    // this PC. The library part is built straight from the session snapshot (no per-open network call). Never
    // waits on the shelves: the library shows at once and the shelves slot in above it when they land; if the
    // home query fails (hash rotated, 429) the page simply stays library-only.
    void buildSpotify() {
        const auto& snap = ctx().session->library();
        const auto& prof = ctx().session->profile();
        auto& shelfCache = ShelfCache::instance();
        shelfCache.ensure(prof.id, life_.ref());
        const std::vector<Shelf>* shelves = shelfCache.shelvesFor(prof.id);
        const bool haveShelves = shelves && !shelves->empty();
        if (!snap.loaded && !haveShelves) {
            showSkeleton(4);
            return;
        }
        auto* c = resetContent();
        std::wstring meta = L"SPOTIFY";
        if (snap.loaded)
            meta += L" · " + i18n::plural(L"{} LİSTE", snap.playlists.size()) + L" · " +
                    toUpperTr(i18n::plural(L"{} albüm", snap.albums.size()));   // two counts: one plural each
        c->add<Greeting>(todayLabel(), meta);

        // --- Spotify's personalized shelves.
        if (haveShelves)
            for (const auto& shelf : *shelves)
                if (!duplicatesLibrarySection(shelf)) addSpotifyShelf(c, shelf);

        if (!snap.loaded) {   // library still loading: the session notify rebuilds with it
            contentReady();
            return;
        }

        // --- Son çalınanlar (local history, works across sources).
        const auto recent = ctx().library.recentAlbums(12);
        if (!recent.empty()) {
            wchar_t count[8];
            swprintf(count, 8, L"%02zu", recent.size());
            c->add<SectionHeader>(tr(L"Son çalınanlar"), count);
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            for (const auto& al : recent) {
                auto* card = grid->add<MediaCard>(toWide(al.name), L"", al.images);
                const std::string id = al.id;
                card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
            }
        }

        // --- Senin için listeler (built on this PC from the listening history; hidden while there is none).
        smart::addSection(c, 1);

        // --- Çalma listelerin (Spotify).
        if (!snap.playlists.empty()) {
            auto* h = c->add<SectionHeader>(tr(L"Çalma listelerin"), std::to_wstring(snap.playlists.size()),
                                            tr(L"Kitaplık"));
            h->onLink = [] { ctx().router->navigate({RouteKind::Library}); };
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            for (const auto& p : snap.playlists) {
                const bool liked = p.id == spotify::Api::kLikedSongsUri;
                const std::wstring sub = p.countKnown ? i18n::plural(L"{} şarkı", p.totalTracks)
                                         : !p.ownerName.empty() ? toWide(p.ownerName)
                                                                : std::wstring(tr(L"Çalma listesi"));
                auto* card = grid->add<MediaCard>(toWide(p.name), sub, p.images, MediaCard::Shape::Square, Placeholder::Playlist);
                const Route open = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
                Route play = open;
                play.id += "#play";
                card->onOpen = [open] { ctx().router->navigate(open); };
                card->onPlay = [play] { ctx().router->navigate(play); };
            }
        }

        // --- Kaydettiğin albümler.
        if (!snap.albums.empty()) {
            auto* h = c->add<SectionHeader>(tr(L"Kaydettiğin albümler"), std::to_wstring(snap.albums.size()),
                                            tr(L"Kitaplık"));
            h->onLink = [] { ctx().router->navigate({RouteKind::Library}); };
            auto* grid = addCardRow(c, 150, 2);
            c->setSpacingBefore(grid, 16);
            addAlbumCards(grid, snap.albums);
        }

        // --- Takip ettiğin sanatçılar.
        if (!snap.artists.empty()) {
            auto* h = c->add<SectionHeader>(tr(L"Takip ettiğin sanatçılar"), std::to_wstring(snap.artists.size()),
                                            tr(L"Kitaplık"));
            h->onLink = [] { ctx().router->navigate({RouteKind::Library}); };
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            addArtistCards(grid, snap.artists, tr(L"Sanatçı"));
        }
        contentReady();
    }

    struct Data {
        std::vector<Album> trendingAlbums;
        std::vector<Artist> trendingArtists;
        std::vector<Album> fresh;
        std::vector<Artist> forYou;          // similar to the user's top artists
        std::wstring forYouSeed;
        int failures = 0;
        std::string firstError;
    };

    void load() {
        // Seeds are MusicBrainz artists only: history played while logged in to Spotify carries "spotify:artist:"
        // URIs, which ListenBrainz/MusicBrainz reject (HTTP 400, then a wasted rate-limited tag-overlap fallback).
        auto seeds = ctx().library.topArtists(4);
        std::erase_if(seeds, [](const ArtistRef& a) { return source::isSpotifyId(a.id); });
        if (seeds.size() > 2) seeds.resize(2);
        async(
            Priority::High, life_.ref(),
            [seeds] {
                Data d;
                auto attempt = [&](auto&& fn) {
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        ++d.failures;
                        if (d.firstError.empty()) d.firstError = e.what();
                    }
                };
                attempt([&] { d.trendingAlbums = mb::trendingAlbums("week", 12); });
                attempt([&] { d.trendingArtists = mb::trendingArtists("week", 12); });
                attempt([&] { d.fresh = mb::freshReleases(14, 12); });
                if (!seeds.empty()) {
                    attempt([&] {
                        std::unordered_set<std::string> seen;
                        for (const auto& s : seeds) {
                            seen.insert(s.id);
                            for (auto& a : mb::similarArtists(s.id, 8))
                                if (seen.insert(a.id).second) d.forYou.push_back(std::move(a));
                        }
                        d.forYouSeed = toWide(seeds[0].name);
                    });
                }
                return d;
            },
            [this](Result<Data> r) {
                if (!r || (r->failures >= 3 && r->trendingAlbums.empty() && r->fresh.empty())) {
                    showError(toWide(r ? r->firstError : r.errorMessage()), [this] {
                        showSkeleton(4);
                        load();
                    });
                    return;
                }
                build(*r);
            });
    }

    static void addAlbumCards(ui::Grid* grid, const std::vector<Album>& albums) {
        for (const auto& a : albums) {
            auto* card = grid->add<MediaCard>(toWide(a.name), albumSubtitle(a), a.images);
            const std::string id = a.id;
            card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
            card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
        }
    }

    static void addArtistCards(ui::Grid* grid, const std::vector<Artist>& artists, const std::wstring& subtitle) {
        for (const auto& a : artists) {
            auto* card = grid->add<MediaCard>(toWide(a.name), subtitle, a.images, MediaCard::Shape::Circle, Placeholder::Artist);
            const std::string id = a.id;
            card->onOpen = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
        }
    }

    void build(const Data& d) {
        auto* c = resetContent();
        auto& lib = ctx().library;
        c->add<Greeting>(todayLabel(), toUpperTr(i18n::plural(L"{} çalma listesi", lib.playlists().size())) + L" · " +
                                           toUpperTr(i18n::plural(L"{} beğeni", lib.liked().size())));

        // --- Son çalınanlar (local history).
        const auto recent = lib.recentAlbums(12);
        if (!recent.empty()) {
            wchar_t count[8];
            swprintf(count, 8, L"%02zu", recent.size());
            auto* h = c->add<SectionHeader>(tr(L"Son çalınanlar"), count, tr(L"Kitaplık"));
            h->onLink = [] { ctx().router->navigate({RouteKind::Library}); };
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            for (const auto& al : recent) {
                std::wstring artist;
                for (const auto& hi : lib.history())
                    if (hi.track.album.id == al.id && !hi.track.artists.empty()) {
                        artist = toWide(hi.track.artists[0].name);
                        break;
                    }
                auto* card = grid->add<MediaCard>(toWide(al.name), artist, al.images);
                const std::string id = al.id;
                card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
            }
        }

        // --- Senin için listeler.
        smart::addSection(c, 1);

        // --- Bu hafta popüler (tiles) + Yeni çıkanlar (list).
        if (!d.trendingAlbums.empty() || !d.fresh.empty()) {
            auto* block = c->add<SplitBlock>();
            if (!d.trendingAlbums.empty()) {
                auto* box = block->add<ui::Box>();
                block->left = box;
                auto* head = box->add<SectionHeader>(tr(L"Bu hafta popüler"), L"LISTENBRAINZ");
                std::vector<ui::Widget*> tiles;
                const auto& top = d.trendingAlbums[0];
                auto* hero = box->add<Tile>(L"01 · " + toUpperTr(toWide(top.artists.empty() ? "" : top.artists[0].name)),
                                            toWide(top.name), Color::rgb(0x1F2A22), top.images, true);
                const std::string heroId = top.id;
                hero->onClick = [heroId] { ctx().router->navigate({RouteKind::Album, heroId}); };
                tiles.push_back(hero);
                for (size_t i = 1; i < d.trendingAlbums.size() && i <= 4; ++i) {
                    const auto& a = d.trendingAlbums[i];
                    wchar_t num[8];
                    swprintf(num, 8, L"%02zu", i + 1);
                    const std::wstring label = num + std::wstring(L" · ") + toUpperTr(tr(L"Albüm"));   // "02 · ALBÜM"
                    auto* tile = box->add<Tile>(label, toWide(a.name), tileColor(a.id), a.images);
                    const std::string id = a.id;
                    tile->onClick = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                    tiles.push_back(tile);
                }
                box->onPreferredHeight = [](float) { return 48.f + 16 + 210; };
                box->onLayout = [head, tiles](ui::Box& b) {
                    const float w = b.rect().w;
                    head->setRect({0, 0, w, 48});
                    const float top = 64, h = 210, gap = 12;
                    const float heroW = std::floor(w * 0.48f);
                    tiles[0]->setRect({0, top, heroW, h});
                    const float sx = heroW + gap, sw = (w - sx - gap) * 0.5f, sh = (h - gap) * 0.5f;
                    for (size_t i = 1; i < tiles.size(); ++i) {
                        const size_t k = i - 1;
                        tiles[i]->setRect({sx + (k % 2) * (sw + gap), top + (k / 2) * (sh + gap), sw, sh});
                    }
                };
            }
            if (!d.fresh.empty()) {
                auto* rc = block->add<ui::Column>(0.f);
                block->right = rc;
                rc->add<SectionHeader>(tr(L"Yeni çıkanlar"), tr(L"SON 14 GÜN"));
                rc->add<Spacer>(8.f);
                int n = 0;
                for (const auto& a : d.fresh) {
                    if (n++ >= 4) break;
                    std::wstring meta = a.primaryType == "Single" ? toUpperTr(tr(L"Tekli"))
                                        : a.primaryType == "EP"   ? L"EP"
                                                                  : toUpperTr(tr(L"Albüm"));
                    if (a.firstReleaseDate.size() >= 10) {
                        std::tm tm{};
                        tm.tm_year = std::stoi(a.firstReleaseDate.substr(0, 4)) - 1900;
                        tm.tm_mon = std::stoi(a.firstReleaseDate.substr(5, 2)) - 1;
                        tm.tm_mday = std::stoi(a.firstReleaseDate.substr(8, 2));
                        tm.tm_hour = 12;
                        meta += L" · " + trDate(std::mktime(&tm));
                    }
                    auto* row = rc->add<ListRow>(toWide(a.name), toWide(a.artists.empty() ? "" : a.artists[0].name), meta, a.images);
                    const std::string id = a.id;
                    row->onClick = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                }
            }
        }

        // --- Çalma listelerin (local).
        if (!lib.playlists().empty() || !lib.liked().empty()) {
            auto* h = c->add<SectionHeader>(tr(L"Çalma listelerin"), std::to_wstring(lib.playlists().size()),
                                            tr(L"Tümünü gör"));
            h->onLink = [] { ctx().router->navigate({RouteKind::Library}); };
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            if (!lib.liked().empty()) {
                auto* liked = grid->add<MediaCard>(tr(L"Beğenilen Şarkılar"),
                                                   i18n::plural(L"{} şarkı", lib.liked().size()), std::vector<Image>{},
                                                   MediaCard::Shape::Square, Placeholder::Playlist);
                liked->onOpen = [] { ctx().router->navigate({RouteKind::Liked}); };
                liked->onPlay = [] { ctx().router->navigate({RouteKind::Liked, "#play"}); };
            }
            for (const auto& p : lib.playlists()) {
                auto* card = grid->add<MediaCard>(toWide(p.name), i18n::plural(L"{} şarkı", p.totalTracks), p.images,
                                                  MediaCard::Shape::Square, Placeholder::Playlist);
                const std::string id = p.id;
                card->onOpen = [id] { ctx().router->navigate({RouteKind::Playlist, id}); };
                card->onPlay = [id] { ctx().router->navigate({RouteKind::Playlist, id + "#play"}); };
            }
        }

        // --- Sana özel: similar to your most played artist.
        if (!d.forYou.empty()) {
            c->add<SectionHeader>(tr(L"Sana özel"),
                                  i18n::format(tr(L"{} DİNLEYENLER BUNLARI DA SEVİYOR"), {toUpperTr(d.forYouSeed)}));
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            addArtistCards(grid, d.forYou, tr(L"Benzer sanatçı"));
        }

        // --- Popüler sanatçılar.
        if (!d.trendingArtists.empty()) {
            c->add<SectionHeader>(tr(L"Popüler sanatçılar"), tr(L"BU HAFTA"));
            auto* grid = addCardRow(c, 150, 1);
            c->setSpacingBefore(grid, 16);
            addArtistCards(grid, d.trendingArtists, tr(L"Sanatçı"));
        }

        // --- More trending albums.
        if (d.trendingAlbums.size() > 5) {
            c->add<SectionHeader>(tr(L"Öne çıkan albümler"));
            auto* grid = addCardRow(c, 150, 2);
            c->setSpacingBefore(grid, 16);
            addAlbumCards(grid, std::vector<Album>(d.trendingAlbums.begin() + 5, d.trendingAlbums.end()));
        }
        contentReady();
    }
};

} // namespace

std::unique_ptr<Page> makeHomePage() { return std::make_unique<HomePage>(); }

} // namespace st::app
