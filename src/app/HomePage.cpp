// Ana Sayfa (docs/DEVELOPMENT.md, "Home page"): the time-of-day greeting, the "continue where you left off" hero, the
// quick-access grid, then shelves that scroll sideways (app/HomeWidgets).
// Logged in: Yeni çıkanlar (followed artists), Spotify's personalized shelves ("home" feed: Made For You, Daily Mixes...),
// Senin için listeler, recently played (local history) and the user's library (playlists, saved albums, followed
// artists). Logged out: recently played, Senin için listeler, the local playlists, then what the open catalog gives:
// MusicBrainz / ListenBrainz (trending this week + fresh releases, artists similar to what you play most, popular
// artists, more albums) or YouTube Music (artists related to the one played most, then its home feed, new albums &
// singles, trending songs, charts and top artists, titled as YouTube Music titles them).
#include "app/HomeWidgets.h"
#include "app/InternetRadio.h"
#include "app/LibraryView.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PodcastUi.h"
#include "app/Podcasts.h"
#include "app/Shell.h"
#include "app/SmartListsPage.h"
#include "app/Social.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"
#include "ytmusic/YtMusic.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>

namespace st::app {

using gfx::accent;
using gfx::colors;
using home::Item;
using namespace catalog;
namespace type = gfx::type;

namespace {

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

std::function<void()> go(Route r) {
    return [r] { ctx().router->navigate(r); };
}
Route playing(Route r) {
    r.id += "#play";
    return r;
}

// ---- Resolving what the hero and the quick-access tiles show ---------------------------------------------------------
// Player contexts ("playlist:<id>", "album:<id>", "artist:<id>", "liked", "smart:<id>") -> an Item from what the app
// already holds (the Spotify snapshot or the local library, the listening history for albums / artists played song by
// song). Nothing when it is not known here (a playlist since deleted, an album only ever heard elsewhere).

const Playlist* findPlaylist(const std::string& id) {
    if (source::loggedIn()) return ctx().session->findPlaylist(id);
    return ctx().library.playlist(id);
}

std::optional<Item> likedItem() {
    Item it;
    it.title = tr(L"Beğenilen Şarkılar");
    int count = static_cast<int>(ctx().library.liked().size());
    if (source::loggedIn()) {
        count = -1;
        if (const auto* p = ctx().session->findPlaylist(spotify::Api::kLikedSongsUri); p && p->countKnown) count = p->totalTracks;
    }
    it.subtitle = count >= 0 ? i18n::plural(L"{} şarkı", static_cast<size_t>(count)) : std::wstring(tr(L"Çalma listesi"));
    it.likedTile = true;
    it.context = "liked";
    it.open = go({RouteKind::Liked});
    it.play = go({RouteKind::Liked, "#play"});
    return it;
}

std::optional<Item> playlistItem(const std::string& id) {
    if (id == spotify::Api::kLikedSongsUri) return likedItem();
    const Playlist* p = findPlaylist(id);
    if (!p) return std::nullopt;
    Item it;
    it.title = toWide(p->name);
    std::wstring sub = tr(L"Çalma listesi");
    if (!p->ownerName.empty() && !p->owned) sub += L" · " + toWide(p->ownerName);
    else if (p->countKnown) sub += L" · " + i18n::plural(L"{} şarkı", static_cast<size_t>(std::max(0, p->totalTracks)));
    it.subtitle = sub;
    it.images = p->images;
    it.placeholder = Placeholder::Playlist;
    it.context = "playlist:" + id;
    it.open = go({RouteKind::Playlist, id});
    it.play = go(playing({RouteKind::Playlist, id}));
    return it;
}

std::optional<Item> albumItem(const std::string& id) {
    const std::vector<Album>& saved = source::loggedIn() ? ctx().session->library().albums : ctx().library.albums();
    Item it;
    bool found = false;
    for (const auto& a : saved)
        if (a.id == id) {
            it.title = toWide(a.name);
            it.subtitle = toWide(a.artists.empty() ? std::string() : a.artists[0].name);
            it.images = a.images;
            found = true;
            break;
        }
    if (!found)
        for (const auto& h : ctx().library.history())
            if (h.track.album.id == id) {
                it.title = toWide(h.track.album.name);
                it.subtitle = toWide(h.track.artists.empty() ? std::string() : h.track.artists[0].name);
                it.images = h.track.album.images;
                found = !it.title.empty();
                break;
            }
    if (!found) return std::nullopt;
    it.subtitle = it.subtitle.empty() ? std::wstring(tr(L"Albüm")) : std::wstring(tr(L"Albüm")) + L" · " + it.subtitle;
    it.placeholder = Placeholder::Album;
    it.context = "album:" + id;
    it.open = go({RouteKind::Album, id});
    it.play = go(playing({RouteKind::Album, id}));
    return it;
}

std::optional<Item> artistItem(const std::string& id) {
    const std::vector<Artist>& followed = source::loggedIn() ? ctx().session->library().artists : ctx().library.artists();
    Item it;
    for (const auto& a : followed)
        if (a.id == id) {
            it.title = toWide(a.name);
            it.images = a.images;
            break;
        }
    if (it.title.empty())
        for (const auto& h : ctx().library.history())
            for (const auto& ar : h.track.artists)
                if (ar.id == id && it.title.empty()) it.title = toWide(ar.name);
    if (it.title.empty()) return std::nullopt;
    it.subtitle = tr(L"Sanatçı");
    it.circle = true;
    it.placeholder = Placeholder::Artist;
    it.context = "artist:" + id;
    it.open = go({RouteKind::Artist, id});   // (no autoplay for artists: no play button)
    return it;
}

std::optional<Item> smartItem(const std::string& id) {
    const auto* l = smart::find(id);
    if (!l) return std::nullopt;
    Item it;
    it.title = smart::title(*l);
    it.subtitle = smart::subtitle(*l);
    it.drawArt = [id](Canvas& c, const Rect& r) {
        if (const auto* list = smart::find(id)) smart::drawCover(c, *list, r, gfx::metrics::radiusSurface);
        else drawArtwork(c, {}, r, gfx::metrics::radiusSurface, Placeholder::Playlist);
    };
    it.tint = tileColor(id);
    it.context = "smart:" + id;
    it.open = go({RouteKind::SmartList, id});
    it.play = go({RouteKind::SmartList, id + "#play"});
    return it;
}

std::optional<Item> resolveContext(const std::string& context) {
    auto after = [&](std::string_view prefix) { return context.substr(prefix.size()); };
    if (context == "liked") return likedItem();
    if (context.rfind("playlist:", 0) == 0) return playlistItem(after("playlist:"));
    if (context.rfind("album:", 0) == 0) return albumItem(after("album:"));
    if (context.rfind("artist:", 0) == 0) return artistItem(after("artist:"));
    if (context.rfind("smart:", 0) == 0) return smartItem(after("smart:"));
    return std::nullopt;
}

// A podcast episode with its progress (not the one playing: that one's progress is live, see home::Hero).
Item episodeItem(const podcast::Episode& e) {
    Item it;
    it.title = toWide(e.title);
    it.subtitle = toWide(e.show);
    if (!e.image.empty()) it.images.push_back({e.image, 0, 0});
    it.placeholder = Placeholder::Album;
    it.context = "podcast:" + e.feedUrl;
    if (const auto* st = podcast::store().state(e.id())) {
        const int64_t dur = st->durationMs > 0 ? st->durationMs : e.durationMs;
        if (dur > 0) {
            it.progress = std::clamp(static_cast<float>(st->positionMs) / static_cast<float>(dur), 0.f, 1.f);
            const int64_t left = std::max<int64_t>(0, dur - st->positionMs);
            it.progressLabel = toUpperTr(i18n::plural(L"{} dk kaldı", static_cast<size_t>((left + 59999) / 60000)));
        }
    }
    it.open = go({RouteKind::Podcasts, "feed:" + e.feedUrl});
    it.play = [e] {
        if (!ctx().player) return;
        podcast::store().rememberQueue({e}, 0);   // a restored session still finds it (as the Podcastler page does)
        ctx().player->playContext(podcast::toTracks({e}), 0, {"podcast:" + e.feedUrl, toWide(e.show)});
    };
    return it;
}

// The hero: what the player has now (playing or paused, e.g. the restored session), else the newest of the library
// collections played here and an unfinished podcast episode, else the most recently played album of the history.
std::optional<Item> heroItem() {
    auto* p = ctx().player;
    if (const auto* t = p ? p->current() : nullptr) {
        const auto& pc = p->context();
        std::optional<Item> it;
        if (radio::isStationId(t->id)) {
            Item s;
            s.title = toWide(t->name);
            s.subtitle = p->liveTitle().empty() ? std::wstring(tr(L"Radyo")) : p->liveTitle();
            const auto images = t->album.images;
            s.drawArt = [images](Canvas& c, const Rect& r) { drawStationArt(c, images, r, gfx::metrics::radiusSurface); };
            s.images = images;
            s.open = go({RouteKind::Radio});
            it = std::move(s);
        } else if (catalog::isPodcastId(t->id)) {
            Item e;
            e.title = toWide(t->name);
            e.subtitle = pc.name.empty() ? toWide(t->album.name) : pc.name;
            e.images = t->album.images;
            e.placeholder = Placeholder::Album;
            e.progress = 0;   // live (Hero)
            const Track episode = *t;
            e.open = [episode] { openEpisodeShow(episode); };
            it = std::move(e);
        } else {
            it = resolveContext(pc.uri);
        }
        if (!it) {   // a search, the queue, local files...: the track itself
            Item s;
            s.title = toWide(t->name);
            s.subtitle = toWide(t->artistLine());
            s.images = t->album.images;
            s.placeholder = Placeholder::Album;
            s.open = [] {
                if (ctx().toggleNowPlaying) ctx().toggleNowPlaying(true);
            };
            it = std::move(s);
        }
        it->label = p->isPlaying() ? std::wstring(tr(L"ŞİMDİ ÇALIYOR")) : toUpperTr(tr(L"Kaldığın yerden devam et"));
        it->current = true;
        it->context = pc.uri;
        it->play = [] {
            if (ctx().player) ctx().player->togglePause();
        };
        return it;
    }
    std::optional<Item> best;
    int64_t bestAt = 0;
    for (const auto& [context, at] : libraryRecents().newest(12))
        if (auto it = resolveContext(context)) {
            best = std::move(it);
            bestAt = at;
            break;
        }
    if (const auto eps = podcast::store().inProgress(1); !eps.empty()) {
        const auto* st = podcast::store().state(eps[0].id());
        if (!best || (st && st->updatedAt > bestAt)) best = episodeItem(eps[0]);
    }
    if (!best)
        if (const auto recent = ctx().library.recentAlbums(1); !recent.empty()) best = albumItem(recent[0].id);
    if (best) best->label = toUpperTr(tr(L"Kaldığın yerden devam et"));
    return best;
}

// The quick-access tiles: Liked Songs, what is pinned on Spotify, the collections played most recently here, then the
// library's playlists in its order; an almost empty profile gets pages to start from. `skip`: the hero's context.
std::vector<Item> quickItems(const std::string& skip) {
    constexpr size_t kMax = 8;
    std::vector<Item> out;
    std::unordered_set<std::string> seen{skip};
    auto push = [&](std::optional<Item> it) {
        if (!it || out.size() >= kMax) return;
        const std::string key = it->context.empty() ? toUtf8(it->title) : it->context;
        if (!seen.insert(key).second) return;
        out.push_back(std::move(*it));
    };
    const bool spotify = source::loggedIn();
    if (spotify || !ctx().library.liked().empty()) push(likedItem());
    if (spotify) {
        const auto& lib = ctx().session->library();
        for (const auto& p : lib.playlists)
            if (const auto e = lib.entries.find(p.id); e != lib.entries.end() && e->second.pinned) push(playlistItem(p.id));
    }
    for (const auto& [context, at] : libraryRecents().newest(24)) push(resolveContext(context));
    if (spotify) {
        for (const auto& p : ctx().session->library().playlists) push(playlistItem(p.id));
    } else {
        for (const auto& p : ctx().library.playlists()) push(playlistItem(p.id));
    }
    if (out.size() < 4) {
        struct Dest {
            std::wstring name;
            const char* icon;
            RouteKind kind;
        };
        for (const Dest& pg : {Dest{tr(L"Radyo"), "radio", RouteKind::Radio}, Dest{tr(L"Podcastler"), "podcast", RouteKind::Podcasts},
                               Dest{tr(L"Yerel dosyalar"), "music-note", RouteKind::LocalFiles},
                               Dest{tr(L"İndirilenler"), "download", RouteKind::Downloads}}) {
            if (out.size() >= 6) break;
            Item it;
            it.title = pg.name;
            it.icon = pg.icon;
            it.open = go({pg.kind});
            push(std::move(it));
        }
    }
    return out;
}

void addTop(ui::Column* c, std::wstring meta1, std::wstring meta2) {
    c->add<home::Greeting>(std::move(meta1), std::move(meta2));
    auto* hero = c->add<home::Hero>([] { return heroItem(); });
    // The hero's context is the first tile otherwise; the grid follows it at the next visit.
    const auto now = heroItem();
    auto items = quickItems(now ? now->context : std::string());
    if (!items.empty()) {
        auto* grid = c->add<home::QuickGrid>(std::move(items));
        c->setSpacingBefore(grid, gfx::metrics::cardGap + 4);
    }
    (void)hero;
}

// ---- Spotify's personalized shelves ----------------------------------------------------------------------------------
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
    bool loading() const { return inFlight_; }

    // Starts a background fetch when nothing fresh is cached for this user and no retry back-off is pending.
    // The Api* is captured here, on the UI thread; the request runs on a worker. `owner` = the asking page.
    // `userId` "ytm" is YouTube Music's home (no account: the same feed for everyone in the region).
    void ensure(const std::string& userId, Lifetime::Ref owner) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (userId != userId_) {
            // Another account (app/Accounts): the one switched away from is kept, so switching back shows its shelves
            // at once (refreshed when stale like any other).
            Parked leaving{userId_, std::move(shelves_), fetchedAt_, hasData_};
            reset();
            if (parked_.hasData && parked_.userId == userId && !userId.empty()) {
                shelves_ = std::move(parked_.shelves);
                fetchedAt_ = parked_.fetchedAt;
                hasData_ = true;
            }
            parked_ = std::move(leaving);
            userId_ = userId;
        }
        if (inFlight_ || (hasData_ && now - fetchedAt_ < kFreshFor) || now < retryAt_) return;
        const bool ytmHome = userId == kYtmUser;
        spotify::Api* api = ytmHome ? nullptr : source::activeApi();
        if (!api && !ytmHome) return;
        inFlight_ = true;
        failed_ = false;
        const unsigned gen = generation_;
        // The continuation runs on the cache's own Lifetime (never renewed): a fetch started by a page that is
        // navigated away from still fills the cache, so the next Home shows the shelves immediately. That Lifetime
        // never expires, so the asking page guards the START of the work instead: at app exit the thread pool drains
        // its queue after ~App destroyed the Session (and its Api), and every page is gone by then.
        async(
            Priority::Normal, life_.ref(),
            [api, owner]() -> std::optional<std::vector<Shelf>> {
                if (owner.expired()) return std::nullopt;
                return api ? api->home() : ytm::home();
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
                    } catch (const ytm::ApiError& e) {
                        status = e.status;
                    } catch (...) {
                    }
                    failed_ = true;
                    error_ = r.errorMessage();
                    retryAt_ = steady_clock::now() + (status == 429 ? minutes(10) : userId_ == kYtmUser ? seconds(20) : minutes(3));
                    ST_LOG_WARN("home", "{} home shelves unavailable (HTTP {}): {}", userId_ == kYtmUser ? "YouTube Music" : "Spotify",
                                status, r.errorMessage());
                    notify();   // the skeleton shelves go
                    return;
                }
                shelves_ = std::move(**r);
                hasData_ = true;
                fetchedAt_ = steady_clock::now();
                ST_LOG_INFO("home", "{} {} home shelves", shelves_.size(), userId_ == kYtmUser ? "YouTube Music" : "Spotify");
                notify();
            });
    }

    void reset() {
        ++generation_;
        inFlight_ = false;
        hasData_ = false;
        failed_ = false;
        shelves_.clear();
        userId_.clear();
        retryAt_ = {};
    }
    // The last fetch failed (and nothing is cached): its error, for a "try again" panel.
    bool failed() const { return failed_ && !hasData_; }
    const std::string& error() const { return error_; }
    // "Tekrar dene": the next ensure() fetches at once.
    void retryNow() { retryAt_ = {}; }

    static constexpr const char* kYtmUser = "ytm";

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

    struct Parked {
        std::string userId;
        std::vector<Shelf> shelves;
        std::chrono::steady_clock::time_point fetchedAt{};
        bool hasData = false;
    };
    Parked parked_;   // the shelves of the account switched away from last
    std::string userId_;
    std::vector<Shelf> shelves_;
    bool hasData_ = false;
    bool inFlight_ = false;
    bool failed_ = false;
    std::string error_;
    unsigned generation_ = 0;
    std::chrono::steady_clock::time_point fetchedAt_{}, retryAt_{};
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    Lifetime life_;
};

// Dev (SHADETUBE_SPOTIFY_DEMO, app/Social.h): home.json in that folder (Spotify's raw home answer) gives the logged-out
// Home the personalized shelves too, so their layout can be checked without a session.
const std::vector<Shelf>& demoShelves() {
    static const std::vector<Shelf> shelves = [] {
        std::vector<Shelf> out;
        const auto dir = socialDemoDir();
        if (dir.empty()) return out;
        std::ifstream f(dir / L"home.json", std::ios::binary);
        if (!f) return out;
        const auto j = nlohmann::json::parse(f, nullptr, false);
        if (!j.is_discarded() && j.contains("data")) out = spotify::Api::parseHome(j.at("data"));
        return out;
    }();
    return shelves;
}

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

// One Spotify / YouTube Music shelf: header + a sideways row of cards. Playlists/albums get a hover play button
// ("#play"). YouTube Music's songs play from the card (the shelf's songs from that one on) and open their album.
void addSpotifyShelf(ui::Column* c, const Shelf& shelf, const std::wstring& fallbackLabel = {}) {
    const std::wstring label = shelf.label.empty() ? fallbackLabel : toUpperTr(toWide(shelf.label));
    ui::Widget* row = home::addShelf(c, toWide(shelf.title), label);
    std::vector<Track> songs;   // the shelf's YouTube Music songs, in order (what a song card plays)
    for (const auto& it : shelf.items)
        if (it.kind == CardItem::Kind::Track && ytm::isTrackId(it.track.id)) songs.push_back(it.track);
    const std::wstring shelfName = toWide(shelf.title);
    for (const auto& it : shelf.items) {
        Route open;
        bool playable = false;
        auto shape = MediaCard::Shape::Square;
        auto ph = Placeholder::Album;
        const bool ytmSong = it.kind == CardItem::Kind::Track && ytm::isTrackId(it.track.id);
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
            if (it.track.album.id.empty() && !ytmSong) continue;
            if (!it.track.album.id.empty()) open = {RouteKind::Album, it.track.album.id};
            break;
        }
        auto* card = row->add<MediaCard>(toWide(it.title), toWide(it.subtitle), it.images, shape, ph);
        card->previewTarget = it.kind == CardItem::Kind::Track ? std::optional(preview::songTarget(it.track))
                                                               : preview::collectionTarget(open, toWide(it.title), it.images);
        if (ytmSong) {
            int index = 0;
            for (size_t i = 0; i < songs.size(); ++i)
                if (songs[i].id == it.track.id) index = static_cast<int>(i);
            auto play = [songs, index, shelfName] { ctx().player->playContext(songs, index, {"ytm:home", shelfName}); };
            card->onPlay = play;
            if (open.id.empty()) card->onOpen = play;   // a music video: no album page
            else card->onOpen = [open] { ctx().router->navigate(open); };
            continue;
        }
        card->onOpen = [open] { ctx().router->navigate(open); };
        if (playable) card->onPlay = [play = playing(open)] { ctx().router->navigate(play); };
    }
}

// Two-column block: tiles (left) + fresh releases list (right).
class SplitBlock : public ui::Widget {
public:
    SplitBlock() { hitTestVisible = false; }
    ui::Widget* left = nullptr;
    ui::Widget* right = nullptr;
    float preferredHeight(float w) override {
        if (stacked(w)) {
            const float lh = left ? left->preferredHeight(w) : 0;
            const float rh = right ? right->preferredHeight(w) : 0;
            return lh + rh + (left && right ? gfx::metrics::sectionGap : 0);
        }
        const float lh = left ? left->preferredHeight(leftW(w)) : 0;
        const float rh = right ? right->preferredHeight(w - leftW(w) - 48) : 0;
        return std::max(lh, rh);
    }
    void layout() override {
        const Rect r = rect();
        if (stacked(r.w)) {   // a narrow page: one under the other
            float y = 0;
            if (left) {
                left->setRect({0, 0, r.w, left->preferredHeight(r.w)});
                y = left->rect().h + gfx::metrics::sectionGap;
            }
            if (right) right->setRect({0, y, r.w, right->preferredHeight(r.w)});
            return;
        }
        const float lw = right ? leftW(r.w) : r.w;
        if (left) left->setRect({0, 0, lw, left->preferredHeight(lw)});
        if (right) right->setRect({left ? lw + 48 : 0, 0, left ? r.w - lw - 48 : r.w, right->preferredHeight(r.w - lw - 48)});
    }

private:
    bool stacked(float w) const { return left && right && w < 760; }
    float leftW(float w) const { return (left && right) ? std::floor(w * 0.56f) : w; }
};

void addAlbumCards(ui::Widget* row, const std::vector<Album>& albums) {
    for (const auto& a : albums) {
        auto* card = row->add<MediaCard>(toWide(a.name), albumSubtitle(a), a.images);
        const std::string id = a.id;
        card->previewTarget = preview::collectionTarget({RouteKind::Album, id}, toWide(a.name), a.images);
        card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
        card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    }
}

void addArtistCards(ui::Widget* row, const std::vector<Artist>& artists, const std::wstring& subtitle) {
    for (const auto& a : artists) {
        auto* card = row->add<MediaCard>(toWide(a.name), subtitle, a.images, MediaCard::Shape::Circle, Placeholder::Artist);
        const std::string id = a.id;
        card->previewTarget = preview::collectionTarget({RouteKind::Artist, id}, toWide(a.name), a.images);
        card->onOpen = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
    }
}

// "Son çalınanlar": the albums of the local listening history (works across sources), newest first.
void addRecentShelf(ui::Column* c) {
    auto& lib = ctx().library;
    const auto recent = lib.recentAlbums(16);
    if (recent.empty()) return;
    wchar_t count[8];
    swprintf(count, 8, L"%02zu", recent.size());
    ui::Widget* row = home::addShelf(c, tr(L"Son çalınanlar"), count, tr(L"Tümünü gör"),
                                     go({RouteKind::Library, "history"}));
    for (const auto& al : recent) {
        std::wstring artist;
        for (const auto& hi : lib.history())
            if (hi.track.album.id == al.id && !hi.track.artists.empty()) {
                artist = toWide(hi.track.artists[0].name);
                break;
            }
        auto* card = row->add<MediaCard>(toWide(al.name), artist, al.images);
        const std::string id = al.id;
        card->previewTarget = preview::collectionTarget({RouteKind::Album, id}, toWide(al.name), al.images);
        card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
        card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    }
}

// Dev (SHADETUBE_HOME_DEMO=<steps>, docs 5.4): "scroll:<dips>" scrolls the page, "shelf:<n>[@<k>]" glides shelf k
// (0 = the first that scrolls) by n pages, once the page is built. For screenshots of the lower sections and a
// scrolled shelf.
struct HomeDemo {
    float scroll = 0;
    int shelfPages = 0, shelfIndex = 0;
    bool any() const { return scroll > 0 || shelfPages != 0; }
};
HomeDemo homeDemo() {
    static const HomeDemo demo = [] {
        HomeDemo d;
        wchar_t buf[128] = {};
        if (GetEnvironmentVariableW(L"SHADETUBE_HOME_DEMO", buf, 128) == 0) return d;
        const std::string spec = toUtf8(buf);
        size_t pos = 0;
        while (pos <= spec.size()) {
            const size_t comma = spec.find(',', pos);
            const std::string step = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (step.rfind("scroll:", 0) == 0) d.scroll = static_cast<float>(std::atof(step.c_str() + 7));
            if (step.rfind("shelf:", 0) == 0) {
                d.shelfPages = std::atoi(step.c_str() + 6);
                if (const size_t at = step.find('@'); at != std::string::npos) d.shelfIndex = std::atoi(step.c_str() + at + 1);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return d;
    }();
    return demo;
}

void collectShelves(ui::Widget* w, std::vector<home::Shelf*>& out) {
    if (auto* s = dynamic_cast<home::Shelf*>(w)) {
        if (s->scrollable()) out.push_back(s);
        return;
    }
    for (const auto& ch : w->children()) collectShelves(ch.get(), out);
}

class HomePage : public ScrollPage {
public:
    HomePage() {
        // Rebuild when the Spotify session (and its library) changes: logging in swaps the whole feed.
        if (ctx().session)
            ctx().session->subscribe(life_.ref(), [this] { reload(); });
        // ...and when the personalized shelves arrive (or refresh); logged out with YouTube Music, its home feed.
        ShelfCache::instance().subscribe(life_.ref(), [this] {
            if (source::loggedIn()) buildSpotify();
            else if (source::ytmMode()) fillYtm();
        });
        // ...and when new releases of followed artists come in (the shelf on top).
        subscribeNewReleases(life_.ref(), [this] {
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
        if ((!ctx().session || ctx().session->state() == spotify::SessionState::LoggedOut) && !source::ytmMode())
            ShelfCache::instance().reset();   // logged out: forget the previous user's feed
        buildLocal();
        if (source::ytmMode()) loadYtm();
        else load();
    }

    // Logged out with YouTube Music: its home feed (ShelfCache, kept for the app session) under the local sections,
    // and "Sana özel": the artists YouTube Music relates to the one played most here.
    void loadYtm() {
        auto& cache = ShelfCache::instance();
        cache.ensure(ShelfCache::kYtmUser, life_.ref());
        fillYtm();
        auto seeds = ctx().library.topArtists(6);
        std::erase_if(seeds, [](const ArtistRef& a) { return !source::isYtmId(a.id); });
        if (seeds.empty()) return;
        const ArtistRef seed = seeds[0];
        async(Priority::Normal, netLife_.ref(), [seed] { return ytm::artistPage(seed.id).similar; },
              [this, seed](Result<std::vector<Artist>> r) {
                  if (!r || r->empty()) return;
                  forYou_ = std::move(*r);
                  forYouSeed_ = toWide(seed.name);
                  fillYtm();
              });
    }

    void fillYtm() {
        if (!net_) return;
        auto& cache = ShelfCache::instance();
        const std::vector<Shelf>* shelves = cache.shelvesFor(ShelfCache::kYtmUser);
        net_->clearChildren();
        if (!forYou_.empty())
            addArtistCards(home::addShelf(net_, tr(L"Sana özel"),
                                          i18n::format(tr(L"{} DİNLEYENLER BUNLARI DA SEVİYOR"), {toUpperTr(forYouSeed_)})),
                           forYou_, tr(L"Benzer sanatçı"));
        if (shelves && !shelves->empty()) {
            for (const auto& shelf : *shelves) addSpotifyShelf(net_, shelf, L"YOUTUBE MUSIC");
        } else if (cache.failed()) {
            net_->add<MessagePanel>("offline", tr(L"Bir şeyler ters gitti"), toWide(cache.error()), tr(L"Tekrar dene"), [this] {
                ShelfCache::instance().retryNow();
                Dispatcher::post([this, ref = life_.ref()] {
                    if (ref.expired()) return;
                    ShelfCache::instance().ensure(ShelfCache::kYtmUser, life_.ref());
                    fillYtm();
                });
            });
        } else {
            net_->add<home::ShelfSkeleton>();
            net_->add<home::ShelfSkeleton>(true);
        }
        scroll_->contentChanged();
        applyDemo();
    }

    // While the library and the shelves are both still on their way: the greeting over placeholders.
    void showHomeSkeleton() {
        built_ = false;
        dropCatalog();
        auto* c = resetContent();
        c->add<home::Greeting>(todayLabel(), L"SPOTIFY");
        c->add<home::TopSkeleton>();
        c->add<home::ShelfSkeleton>();
        c->add<home::ShelfSkeleton>();
        scroll_->contentChanged();
    }

    void ready() {
        contentReady();
        applyDemo();
    }

    // Logged in: Spotify's personalized shelves (fetched in the background, cached per user by ShelfCache),
    // then the user's library — playlists, saved albums, followed artists — plus what they've recently played on
    // this PC. The library part is built straight from the session snapshot (no per-open network call). Never
    // waits on the shelves: the library shows at once and the shelves slot in above it when they land (placeholders
    // meanwhile); if the home query fails (hash rotated, 429) the page simply stays library-only.
    void buildSpotify() {
        const auto& snap = ctx().session->library();
        const auto& prof = ctx().session->profile();
        auto& shelfCache = ShelfCache::instance();
        shelfCache.ensure(prof.id, life_.ref());
        const std::vector<Shelf>* shelves = shelfCache.shelvesFor(prof.id);
        const bool haveShelves = shelves && !shelves->empty();
        if (!snap.loaded && !haveShelves) {
            showHomeSkeleton();
            return;
        }
        dropCatalog();
        auto* c = resetContent();
        std::wstring meta = L"SPOTIFY";
        if (snap.loaded)
            meta += L" · " + i18n::plural(L"{} LİSTE", snap.playlists.size()) + L" · " +
                    toUpperTr(i18n::plural(L"{} albüm", snap.albums.size()));   // two counts: one plural each
        addTop(c, todayLabel(), meta);

        // --- Yeni çıkanlar: new releases of followed artists from the last two weeks (when there are any).
        addNewReleasesShelf(c, home::shelfRow);

        // --- Spotify's personalized shelves (placeholders while the first fetch runs).
        if (haveShelves) {
            for (const auto& shelf : *shelves)
                if (!duplicatesLibrarySection(shelf)) addSpotifyShelf(c, shelf);
        } else if (shelfCache.loading()) {
            c->add<home::ShelfSkeleton>();
            c->add<home::ShelfSkeleton>();
        }

        // --- Senin için listeler (built on this PC from the listening history; hidden while there is none).
        smart::addSection(c, home::shelfRow);

        if (!snap.loaded) {   // library still loading: the session notify rebuilds with it
            c->add<home::ShelfSkeleton>();
            ready();
            return;
        }

        // --- Son çalınanlar (local history, works across sources).
        addRecentShelf(c);

        // --- Çalma listelerin (Spotify).
        if (!snap.playlists.empty()) {
            ui::Widget* row = home::addShelf(c, tr(L"Çalma listelerin"), std::to_wstring(snap.playlists.size()),
                                             tr(L"Tümünü gör"), go({RouteKind::Library}));
            for (const auto& p : snap.playlists) {
                const bool liked = p.id == spotify::Api::kLikedSongsUri;
                const std::wstring sub = p.countKnown ? i18n::plural(L"{} şarkı", p.totalTracks)
                                         : !p.ownerName.empty() ? toWide(p.ownerName)
                                                                : std::wstring(tr(L"Çalma listesi"));
                auto* card = row->add<MediaCard>(toWide(p.name), sub, p.images, MediaCard::Shape::Square, Placeholder::Playlist);
                const Route open = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
                card->previewTarget = preview::collectionTarget(open, toWide(p.name), p.images);
                card->onOpen = [open] { ctx().router->navigate(open); };
                card->onPlay = [play = playing(open)] { ctx().router->navigate(play); };
            }
        }

        // --- Kaydettiğin albümler.
        if (!snap.albums.empty())
            addAlbumCards(home::addShelf(c, tr(L"Kaydettiğin albümler"), std::to_wstring(snap.albums.size()),
                                         tr(L"Tümünü gör"), go({RouteKind::Library, "albums"})),
                          snap.albums);

        // --- Takip ettiğin sanatçılar.
        if (!snap.artists.empty())
            addArtistCards(home::addShelf(c, tr(L"Takip ettiğin sanatçılar"), std::to_wstring(snap.artists.size()),
                                          tr(L"Tümünü gör"), go({RouteKind::Library, "artists"})),
                           snap.artists, tr(L"Sanatçı"));
        ready();
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

    // Logged out: everything this PC holds at once (greeting, hero, quick access, recently played, Senin için
    // listeler, the local playlists), the MusicBrainz / ListenBrainz sections under them as placeholders until load()
    // fills them in.
    void buildLocal() {
        auto* c = resetContent();
        auto& lib = ctx().library;
        addTop(c, todayLabel(),
               toUpperTr(i18n::plural(L"{} çalma listesi", lib.playlists().size())) + L" · " +
                   toUpperTr(i18n::plural(L"{} beğeni", lib.liked().size())));

        // --- Dev (SHADETUBE_SPOTIFY_DEMO): the social shelf and Spotify's shelves from the fixtures.
        addNewReleasesShelf(c, home::shelfRow);
        for (const auto& shelf : demoShelves())
            if (!duplicatesLibrarySection(shelf)) addSpotifyShelf(c, shelf);

        // --- Son çalınanlar (local history).
        addRecentShelf(c);

        // --- Senin için listeler.
        smart::addSection(c, home::shelfRow);

        // --- Çalma listelerin (local).
        if (!lib.playlists().empty() || !lib.liked().empty()) {
            ui::Widget* row = home::addShelf(c, tr(L"Çalma listelerin"), std::to_wstring(lib.playlists().size()),
                                             tr(L"Tümünü gör"), go({RouteKind::Library}));
            if (!lib.liked().empty()) {
                auto* liked = row->add<MediaCard>(tr(L"Beğenilen Şarkılar"), i18n::plural(L"{} şarkı", lib.liked().size()),
                                                  std::vector<Image>{}, MediaCard::Shape::Square, Placeholder::Playlist);
                liked->previewTarget = preview::collectionTarget({RouteKind::Liked}, tr(L"Beğenilen Şarkılar"), {});
                liked->onOpen = [] { ctx().router->navigate({RouteKind::Liked}); };
                liked->onPlay = [] { ctx().router->navigate({RouteKind::Liked, "#play"}); };
            }
            for (const auto& p : lib.playlists()) {
                auto* card = row->add<MediaCard>(toWide(p.name), i18n::plural(L"{} şarkı", p.totalTracks), p.images,
                                                 MediaCard::Shape::Square, Placeholder::Playlist);
                const std::string id = p.id;
                card->previewTarget = preview::collectionTarget({RouteKind::Playlist, id}, toWide(p.name), p.images);
                card->onOpen = [id] { ctx().router->navigate({RouteKind::Playlist, id}); };
                card->onPlay = [id] { ctx().router->navigate({RouteKind::Playlist, id + "#play"}); };
            }
        }

        // --- The open catalog's sections: placeholders until they arrive.
        netLife_.renew();   // a load still running for an earlier build lands nowhere
        net_ = c->add<ui::Column>(gfx::metrics::sectionGap);
        net_->add<home::ShelfSkeleton>();
        net_->add<home::ShelfSkeleton>(true);
        contentReady();
    }

    void load() {
        // Seeds are MusicBrainz artists only: history played while logged in to Spotify carries "spotify:artist:"
        // URIs, which ListenBrainz/MusicBrainz reject (HTTP 400, then a wasted rate-limited tag-overlap fallback).
        auto seeds = ctx().library.topArtists(4);
        std::erase_if(seeds, [](const ArtistRef& a) { return a.id.empty() || source::isSpotifyId(a.id) || source::isYtmId(a.id); });
        if (seeds.size() > 2) seeds.resize(2);
        async(
            Priority::High, netLife_.ref(),
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
                if (!net_) return;
                net_->clearChildren();
                if (!r || (r->failures >= 3 && r->trendingAlbums.empty() && r->fresh.empty())) {
                    net_->add<MessagePanel>("offline", tr(L"Bir şeyler ters gitti"),
                                            toWide(r ? r->firstError : r.errorMessage()), tr(L"Tekrar dene"), [this] {
                                                net_->clearChildren();
                                                net_->add<home::ShelfSkeleton>();
                                                load();
                                            });
                } else {
                    buildCatalog(*r);
                }
                scroll_->contentChanged();
                applyDemo();
            });
    }

    // The open catalog's sections (logged out).
    void buildCatalog(const Data& d) {
        auto* c = net_;
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
                hero->previewTarget = preview::collectionTarget({RouteKind::Album, heroId}, toWide(top.name), top.images);
                hero->onClick = [heroId] { ctx().router->navigate({RouteKind::Album, heroId}); };
                tiles.push_back(hero);
                for (size_t i = 1; i < d.trendingAlbums.size() && i <= 4; ++i) {
                    const auto& a = d.trendingAlbums[i];
                    wchar_t num[8];
                    swprintf(num, 8, L"%02zu", i + 1);
                    const std::wstring label = num + std::wstring(L" · ") + toUpperTr(tr(L"Albüm"));   // "02 · ALBÜM"
                    auto* tile = box->add<Tile>(label, toWide(a.name), tileColor(a.id), a.images);
                    const std::string id = a.id;
                    tile->previewTarget = preview::collectionTarget({RouteKind::Album, id}, toWide(a.name), a.images);
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
                    row->previewTarget = preview::collectionTarget({RouteKind::Album, id}, toWide(a.name), a.images);
                    row->onClick = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                }
            }
        }

        // --- Sana özel: similar to your most played artist.
        if (!d.forYou.empty())
            addArtistCards(home::addShelf(c, tr(L"Sana özel"),
                                          i18n::format(tr(L"{} DİNLEYENLER BUNLARI DA SEVİYOR"), {toUpperTr(d.forYouSeed)})),
                           d.forYou, tr(L"Benzer sanatçı"));

        // --- Popüler sanatçılar.
        if (!d.trendingArtists.empty())
            addArtistCards(home::addShelf(c, tr(L"Popüler sanatçılar"), tr(L"BU HAFTA")), d.trendingArtists, tr(L"Sanatçı"));

        // --- More trending albums.
        if (d.trendingAlbums.size() > 5)
            addAlbumCards(home::addShelf(c, tr(L"Öne çıkan albümler")),
                          std::vector<Album>(d.trendingAlbums.begin() + 5, d.trendingAlbums.end()));
    }

    // Dev (SHADETUBE_HOME_DEMO): once, after the page has its content.
    void applyDemo() {
        const HomeDemo demo = homeDemo();
        if (!demo.any() || demoDone_) return;
        demoDone_ = true;
        Dispatcher::post([this, demo, ref = life_.ref()] {
            if (ref.expired()) return;
            scroll_->layout();
            if (demo.scroll > 0) scroll_->scrollTo(demo.scroll, false);
            if (demo.shelfPages != 0) {
                std::vector<home::Shelf*> shelves;
                collectShelves(col_, shelves);
                if (demo.shelfIndex >= 0 && demo.shelfIndex < static_cast<int>(shelves.size()))
                    shelves[demo.shelfIndex]->scrollPages(demo.shelfPages);
            }
        });
    }

    // The logged-out page's open catalog part goes with a rebuild into the logged-in page.
    void dropCatalog() {
        netLife_.renew();
        net_ = nullptr;
    }

    ui::Column* net_ = nullptr;   // logged out: the open catalog's sections
    Lifetime netLife_;            // its load
    std::vector<Artist> forYou_;  // YouTube Music: artists related to the one played most here
    std::wstring forYouSeed_;
    bool demoDone_ = false;
};

} // namespace

std::unique_ptr<Page> makeHomePage() { return std::make_unique<HomePage>(); }

} // namespace st::app
