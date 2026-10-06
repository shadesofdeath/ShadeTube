// Yeni çıkanlar: new releases of the artists the user follows on Spotify (see app/Social.h). The refresh service
// (Spotify's "What's New" feed + the latest releases of the artists the user plays most, cached on disk, at most every
// few hours, backing off on HTTP 429), the page, the Home shelf, the sidebar badge and the notifications.
#include "app/NewReleases.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Social.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace st::app {

using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;
using CTS = YoutubeExplode::CancellationTokenSource;

namespace {

constexpr int64_t kRefreshSec = 4 * 3600;        // the feed is refreshed at most this often
constexpr int64_t kManualGapSec = 60;            // "Yenile" can't hammer Spotify either
constexpr int64_t kErrorBackoffSec = 30 * 60;    // after a failure (not rate limited)
constexpr int64_t kRateLimitMinSec = 15 * 60;    // after a 429 without (or with a shorter) Retry-After
constexpr double kStartDelayMs = 30'000;         // after start / login: let the library load first (it 429s easily)
constexpr size_t kTopArtists = 6;                // most played, not followed artists checked one by one
constexpr size_t kMaxFallbackArtists = 60;       // followed artists checked one by one when the feed fails

struct Service {
    releases::Store store;
    bool inFlight = false;
    std::wstring error;              // last refresh failure ("" = fine)
    double readyAt = 0;              // steadyMs() before which no automatic refresh starts
    std::shared_ptr<CTS> cancel;
    Lifetime life;                   // renewed at exit: queued work never starts after the Session is gone
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
};
Service& svc() {
    static Service s;
    return s;
}

std::filesystem::path storeFile() { return paths::appData() / L"new-releases.json"; }

void notifyListeners() {
    auto& s = svc();
    std::erase_if(s.listeners, [](const auto& l) { return l.first.expired(); });
    const auto snapshot = s.listeners;   // a listener may rebuild a page that subscribes again
    for (const auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
    if (ctx().window) ctx().window->invalidate();   // the sidebar badge
}

// The user the releases belong to: the Spotify profile, or "demo" for the dev demo.
std::string currentUser() {
    if (source::loggedIn()) return ctx().session ? ctx().session->profile().id : std::string{};
    return socialDemoDir().empty() ? std::string{} : std::string("demo");
}

bool storeIsCurrent() {
    const std::string user = currentUser();
    return !user.empty() && svc().store.user == user;
}

std::wstring typeLabel(const Album& a) {
    return a.primaryType == "Single" ? std::wstring(tr(L"Tekli"))
           : a.primaryType == "EP"   ? std::wstring(L"EP")
           : a.primaryType == "Compilation" ? std::wstring(tr(L"Derleme"))
                                            : std::wstring(tr(L"Albüm"));
}

std::wstring artistsOf(const Album& a) {
    std::string s;
    for (size_t i = 0; i < a.artists.size() && i < 2; ++i) s += (i ? ", " : "") + a.artists[i].name;
    return toWide(s);
}

// Applies a finished refresh: merge, first-run seeding, notifications, save, listeners.
void applyFetched(const std::vector<Album>& albums, int64_t now) {
    auto& st = svc().store;
    const int64_t today = releases::localToday();
    const bool first = st.fetchedAt == 0;
    std::vector<std::string> added;
    st.items = releases::merge(std::move(st.items), albums, today, now, &added);
    st.fetchedAt = now;
    if (first) {
        releases::seedSeen(st, today);
    } else if (Settings::get().newReleaseNotifications && ctx().trayBalloon) {
        const auto fresh = releases::notifiable(st, added, today);
        if (fresh.size() == 1) {
            const Album& a = fresh[0]->album;
            ctx().trayBalloon(i18n::format(tr(L"Yeni {}: {}"), {typeLabel(a), artistsOf(a)}), toWide(a.name), false);
        } else if (fresh.size() > 1) {
            std::wstring names;
            for (size_t i = 0; i < fresh.size() && i < 3; ++i) names += (i ? L", " : L"") + artistsOf(fresh[i]->album);
            if (fresh.size() > 3) names += L"…";
            ctx().trayBalloon(i18n::plural(L"{} yeni çıkan", static_cast<int64_t>(fresh.size())), names, false);
        }
        if (!fresh.empty()) ST_LOG_INFO("releases", "{} new release(s) notified", fresh.size());
    }
    releases::save(st, storeFile());
    ST_LOG_INFO("releases", "{} release(s) fetched, {} new, {} listed", albums.size(), added.size(), st.items.size());
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Starts a refresh when one is due (`manual`: the page's "Yenile", which skips the 4 h rule but not the back-off).
void refresh(bool manual) {
    auto& s = svc();
    if (s.inFlight) return;
    const std::string user = currentUser();
    if (user.empty()) return;
    if (s.store.user != user) {   // another account (or the first run): its own releases
        s.store.clear();
        s.store.user = user;
        s.error.clear();
        if (!manual) s.readyAt = steadyMs() + kStartDelayMs;
    }
    const int64_t now = nowUnix();
    if (user == "demo") {   // dev: the feed from SHADETUBE_SPOTIFY_DEMO/whatsnew.json, once per run
        if (s.store.fetchedAt > 0 && !manual) return;
        const auto j = nlohmann::json::parse(readFile(socialDemoDir() / L"whatsnew.json"), nullptr, false);
        const auto& data = j.is_object() && j.contains("data") ? j["data"] : j;
        applyFetched(spotify::Api::parseWhatsNewFeed(data), now);
        notifyListeners();
        return;
    }
    if (now < s.store.retryAt) {
        if (manual)
            toast(i18n::format(tr(L"Spotify şu an çok fazla istek alıyor. {} sonra yeniden dene."),
                               {totalDuration((s.store.retryAt - now) * 1000)}));
        return;
    }
    if (manual ? now - s.store.fetchedAt < kManualGapSec
               : (static_cast<double>(steadyMs()) < s.readyAt || now - s.store.fetchedAt < kRefreshSec))
        return;
    spotify::Api* api = source::activeApi();
    if (!api) return;

    // Artists checked one by one: the most played ones the user doesn't follow (cheap: a handful), and the followed
    // ones only when the feed itself is unavailable.
    const auto& lib = ctx().session->library();
    std::unordered_set<std::string> followed;
    std::vector<std::string> followedUris;
    for (const auto& a : lib.artists)
        if (a.id.rfind("spotify:artist:", 0) == 0 && followed.insert(a.id).second) followedUris.push_back(a.id);
    if (followedUris.size() > kMaxFallbackArtists) followedUris.resize(kMaxFallbackArtists);
    std::vector<std::string> top;
    for (const auto& a : ctx().library.topArtists(40))
        if (a.id.rfind("spotify:artist:", 0) == 0 && !followed.count(a.id) && top.size() < kTopArtists) top.push_back(a.id);

    s.inFlight = true;
    s.cancel = std::make_shared<CTS>();
    const auto ct = s.cancel->token();
    struct Out {
        std::vector<Album> albums;
        bool feedOk = false;
        int rateLimitedSec = -1;   // >= 0: Spotify said 429 (Retry-After, 0 = none given)
        std::string error;
        int checked = 0;
    };
    ST_LOG_INFO("releases", "refreshing ({}){}", manual ? "manual" : "scheduled", lib.loaded ? "" : ", library not loaded");
    if (manual) notifyListeners();   // the page shows "Yenileniyor…"
    async(
        Priority::Low, s.life.ref(),
        [api, ct, top, followedUris] {
            Out o;
            try {
                o.albums = api->whatsNewReleases(50, ct);
                o.feedOk = true;
            } catch (const spotify::ApiError& e) {
                if (e.status == 429) {
                    o.rateLimitedSec = e.retryAfterSec;
                    return o;
                }
                o.error = e.what();
                ST_LOG_WARN("releases", "What's New feed unavailable ({}); checking the followed artists one by one", e.what());
            }
            std::vector<std::string> artists = top;
            if (!o.feedOk) artists.insert(artists.begin(), followedUris.begin(), followedUris.end());
            // A release counts as new for a month here (the feed has its own window).
            const int64_t since = releases::localToday() - 31;
            for (const auto& uri : artists) {
                if (ct.isCancellationRequested()) break;
                try {
                    for (auto& a : api->artistLatestReleases(uri, ct))
                        if (releases::dayNumber(a.firstReleaseDate) >= since) o.albums.push_back(std::move(a));
                    ++o.checked;
                } catch (const spotify::ApiError& e) {
                    if (e.status == 429) {
                        o.rateLimitedSec = e.retryAfterSec;
                        break;
                    }
                    if (o.error.empty()) o.error = e.what();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));   // gentle on the rate limit
            }
            return o;
        },
        [](Result<Out> r) {
            auto& s = svc();
            s.inFlight = false;
            const int64_t now = nowUnix();
            if (!r) {
                s.error = toWide(r.errorMessage());
                s.store.retryAt = now + kErrorBackoffSec;
                notifyListeners();
                return;
            }
            if (r->rateLimitedSec >= 0) {
                s.store.retryAt = now + std::max<int64_t>(r->rateLimitedSec, kRateLimitMinSec);
                ST_LOG_WARN("releases", "rate limited by Spotify: next try in {} s", s.store.retryAt - now);
            }
            if (!r->feedOk && r->checked == 0) {   // nothing worked: keep what is cached
                if (r->rateLimitedSec < 0) s.store.retryAt = now + kErrorBackoffSec;
                s.error = r->rateLimitedSec >= 0 ? std::wstring(tr(L"Spotify şu an çok fazla istek alıyor."))
                                                 : toWide(r->error);
                releases::save(s.store, storeFile());
                notifyListeners();
                return;
            }
            s.error.clear();
            applyFetched(r->albums, now);
            notifyListeners();
        });
}

// ---------------------------------------------------------------------------------------------------
// The page

std::wstring bucketTitle(releases::Bucket b) {
    switch (b) {
    case releases::Bucket::ThisWeek: return tr(L"Bu hafta");
    case releases::Bucket::LastWeek: return tr(L"Geçen hafta");
    case releases::Bucket::ThisMonth: return tr(L"Bu ay");
    case releases::Bucket::Earlier: return tr(L"Daha önce");
    }
    return {};
}

MediaCard* addReleaseCard(ui::Grid* grid, const Album& a) {
    auto* card = grid->add<MediaCard>(toWide(a.name), typeLabel(a) + L" · " + artistsOf(a), a.images);
    const std::string id = a.id;
    card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
    card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
    card->onContext = [id, artist = a.artists.empty() ? std::string() : a.artists[0].id](gfx::Point wp) {
        std::vector<ui::MenuItem> items{
            {tr(L"Aç"), "arrow-up-right", L"", [id] { ctx().router->navigate({RouteKind::Album, id}); }},
            {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); }}};
        if (artist.rfind("spotify:artist:", 0) == 0)
            items.push_back({tr(L"Sanatçıya git"), "artist", L"", [artist] { ctx().router->navigate({RouteKind::Artist, artist}); }});
        ui::Menu::open(ctx().window, wp, std::move(items));
    };
    return card;
}

class NewReleasesPage : public ScrollPage {
public:
    NewReleasesPage() {
        subscribeNewReleases(life_.ref(), [this] { rebuild(); });
        if (ctx().session) ctx().session->subscribe(life_.ref(), [this] { rebuild(); });
        refresh(false);
        rebuild();
    }

private:
    void rebuild() {
        if (!socialAvailable()) {
            auto* c = resetContent();
            c->add<MessagePanel>("sparkle", tr(L"Yeni çıkanlar"),
                                 tr(L"Takip ettiğin sanatçıların yeni albümlerini ve teklilerini görmek için Spotify ile "
                                    L"bağlan."),
                                 tr(L"Spotify ile bağlan"), [] {
                                     if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
                                 });
            contentReady();
            return;
        }
        auto& s = svc();
        const bool current = storeIsCurrent();
        if (!current || s.store.items.empty()) {
            if (s.inFlight || !current || (s.store.fetchedAt == 0 && s.error.empty())) {
                showSkeleton(4);
                return;
            }
            if (!s.error.empty() && s.store.fetchedAt == 0) {
                showError(s.error, [] { refresh(true); });
                return;
            }
        }
        auto* c = resetContent(24.f);
        addHeader(c);
        const int64_t today = releases::localToday();
        // What is unseen now keeps its "YENİ" pill while the page is up (it is marked seen below).
        for (const auto& i : s.store.items)
            if (!s.store.seen.count(i.album.id)) fresh_.insert(i.album.id);
        if (s.store.items.empty()) {
            c->add<MessagePanel>("sparkle", tr(L"Henüz yeni bir şey yok"),
                                 tr(L"Takip ettiğin sanatçılar yeni bir albüm ya da tekli yayımladığında burada görünür."));
            contentReady();
            return;
        }
        const auto groups = releases::group(s.store.items, filter_, today);
        if (groups.empty())
            c->add<ui::Label>(tr(L"Bu filtrede yeni çıkan yok."), type::secondary, ui::Tone::Tertiary);
        for (const auto& g : groups) {
            c->add<SectionHeader>(bucketTitle(g.bucket), std::to_wstring(g.items.size()));
            auto* grid = addCardRow(c, 150, 0);
            c->setSpacingBefore(grid, 16);
            for (const auto* item : g.items) {
                auto* card = addReleaseCard(grid, item->album);
                if (fresh_.count(item->album.id)) card->setBadge(toUpperTr(tr(L"Yeni")));
            }
        }
        contentReady();
        // Everything listed has now been seen: the sidebar badge clears.
        const bool unseen = std::any_of(s.store.items.begin(), s.store.items.end(),
                                        [&](const releases::Item& i) { return !s.store.seen.count(i.album.id); });
        if (unseen) {
            releases::markAllSeen(s.store);
            releases::save(s.store, storeFile());
            if (ctx().window) ctx().window->invalidate();
        }
    }

    void addHeader(ui::Column* c) {
        auto& s = svc();
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Yeni çıkanlar"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        auto* again = top->add<Button>(ButtonKind::Ghost, s.inFlight ? tr(L"Yenileniyor…") : tr(L"Yenile"), "refresh");
        again->setEnabled(!s.inFlight);
        again->onClick = [] { refresh(true); };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, again](ui::Box& b) {
            const float w = b.rect().w;
            title->setRect({0, 0, w * 0.6f, 64});
            const float aw = again->naturalWidth();
            again->setRect({w - aw, 12, aw, 40});
        };
        std::wstring meta = tr(L"Takip ettiğin ve en çok dinlediğin sanatçılardan");
        if (s.store.fetchedAt > 0) meta += L" · " + i18n::format(tr(L"güncellendi: {}"), {relativeTime(s.store.fetchedAt)});
        if (!s.error.empty()) meta += L" · " + std::wstring(tr(L"son yenileme başarısız"));
        c->add<ui::Label>(meta, type::secondary, ui::Tone::Tertiary);

        // Filter chips: Tümü / Albümler / Tekliler ve EP'ler.
        auto* chips = c->add<ui::Box>();
        std::vector<Button*> buttons;
        const std::pair<releases::Filter, const wchar_t*> options[] = {
            {releases::Filter::All, tr(L"Tümü")},
            {releases::Filter::Albums, tr(L"Albümler")},
            {releases::Filter::SinglesAndEps, tr(L"Tekliler ve EP'ler")},
        };
        for (const auto& [f, label] : options) {
            auto* b = chips->add<Button>(ButtonKind::Chip, label);
            b->setActive(f == filter_);
            b->onClick = [this, f = f] {
                filter_ = f;
                // Rebuilt after the click: the chip that asked is replaced.
                Dispatcher::post([this, ref = life_.ref()] {
                    if (!ref.expired()) rebuild();
                });
            };
            buttons.push_back(b);
        }
        chips->onPreferredHeight = [](float) { return 32.f; };
        chips->onLayout = [buttons](ui::Box&) {
            float x = 0;
            for (auto* b : buttons) {
                const float w = b->naturalWidth();
                b->setRect({x, 0, w, 32});
                x += w + 8;
            }
        };
        c->setSpacingBefore(chips, 16);
    }

    releases::Filter filter_ = releases::Filter::All;
    std::unordered_set<std::string> fresh_;
};

} // namespace

// ---------------------------------------------------------------------------------------------------

int newReleasesUnseenCount() {
    if (!socialAvailable() || !storeIsCurrent()) return 0;
    return releases::unseenCount(svc().store, releases::localToday());
}

void subscribeNewReleases(Lifetime::Ref owner, std::function<void()> fn) {
    auto& l = svc().listeners;
    std::erase_if(l, [](const auto& e) { return e.first.expired(); });
    l.emplace_back(std::move(owner), std::move(fn));
}

void addNewReleasesShelf(ui::Column* c) {
    if (!socialAvailable() || !storeIsCurrent()) return;
    const int64_t today = releases::localToday();
    std::vector<const releases::Item*> recent;
    for (const auto& i : svc().store.items)
        if (i.day >= today - releases::kRecentDays && recent.size() < 12) recent.push_back(&i);
    if (recent.empty()) return;
    auto* h = c->add<SectionHeader>(tr(L"Yeni çıkanlar"), tr(L"SON 14 GÜN"), tr(L"Tümünü gör"));
    h->onLink = [] { ctx().router->navigate({RouteKind::NewReleases}); };
    auto* grid = addCardRow(c, 150, 1);
    c->setSpacingBefore(grid, 16);
    for (const auto* i : recent) {
        auto* card = addReleaseCard(grid, i->album);
        if (!svc().store.seen.count(i->album.id)) card->setBadge(toUpperTr(tr(L"Yeni")));
    }
}

std::unique_ptr<Page> makeNewReleasesPage() { return std::make_unique<NewReleasesPage>(); }

void initNewReleases() {
    auto& s = svc();
    s.store = releases::load(storeFile());
    s.readyAt = steadyMs() + kStartDelayMs;
    // Every ~2 s: a refresh when one is due (4 h, the back-off and the start delay decide; cheap otherwise).
    ctx().housekeepingHooks.push_back([] { refresh(false); });
    // Exit: whatever is queued never starts (the Session goes away with App), a running refresh stops between artists.
    ctx().persistHooks.push_back([] {
        auto& s = svc();
        if (s.cancel) s.cancel->cancel();
        s.life.renew();
        if (!s.store.user.empty()) releases::save(s.store, storeFile());
    });
    // Logging in / out changes whose releases these are (badge, Home shelf).
    static Lifetime sessionLife;
    if (ctx().session)
        ctx().session->subscribe(sessionLife.ref(), [] {
            static std::string lastUser;
            const std::string user = currentUser();
            if (user == lastUser) return;
            lastUser = user;
            if (!user.empty() && svc().store.user != user) svc().readyAt = steadyMs() + kStartDelayMs;
            notifyListeners();
        });
}

} // namespace st::app
