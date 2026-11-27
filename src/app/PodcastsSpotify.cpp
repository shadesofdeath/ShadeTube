// Spotify podcasts in Podcastler (see PodcastsSpotify.h): the followed-shows list, the feed lookup queue, the import and
// the import preview's row.
#include "app/PodcastsSpotify.h"

#include "app/AppContext.h"
#include "app/Podcasts.h"
#include "app/Social.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace st::app {

using catalog::PodcastShow;
using gfx::colors;
namespace type = gfx::type;

std::string podcastDirectoryCountry() {
    wchar_t geo[16] = {};
    if (GetUserDefaultGeoName(geo, 16) == 3 && iswalpha(geo[0]) && iswalpha(geo[1])) return toUtf8(toUpperTr(geo));
    if (const auto& r = Settings::get().region; r.size() == 2) return r;
    return "US";
}

namespace spotifyshows {
namespace {

constexpr int64_t kListTtlMs = 10 * 60'000;
constexpr int64_t kListRetryMs = 60'000;
constexpr int64_t kFailedRetryMs = 2 * 60'000;

struct State {
    // The followed shows.
    std::vector<PodcastShow> list;
    ListState state = ListState::NoSession;
    bool loading = false;
    int64_t loadedMs = 0;
    std::string account;
    Lifetime listLife;
    std::unordered_map<std::string, PodcastShow> seen;
    // Lookups.
    std::deque<PodcastShow> queue;
    std::string active;
    std::unordered_map<std::string, int64_t> failedMs;
    Lifetime lookupLife;
    // Import.
    bool importing = false;
    Lifetime importLife;
    std::vector<std::pair<std::weak_ptr<const void>, std::function<void()>>> listeners;
};

State& st() {
    static State s;
    return s;
}

void notify() {
    Dispatcher::post([] {
        auto& s = st();
        std::erase_if(s.listeners, [](const auto& l) { return l.first.expired(); });
        const auto snapshot = s.listeners;
        for (const auto& [owner, fn] : snapshot)
            if (!owner.expired() && fn) fn();
    });
}

bool queued(const std::string& uri) {
    const auto& q = st().queue;
    return std::any_of(q.begin(), q.end(), [&](const PodcastShow& s) { return s.id == uri; });
}

void pump() {
    auto& s = st();
    if (!s.active.empty() || s.queue.empty()) return;
    const PodcastShow show = s.queue.front();
    s.queue.pop_front();
    s.active = show.id;
    const auto* link = podcast::store().spotifyLink(show.id);
    const std::string rejected = link ? link->rejectedFeed : std::string();
    const std::string country = podcastDirectoryCountry();
    async(
        Priority::Low, s.lookupLife.ref(),
        [show, country, rejected] { return podcast::Client::shared().findFeed(show.name, show.publisher, country, rejected); },
        [show](Result<std::optional<podcast::DirectoryEntry>> r) {
            auto& s = st();
            s.active.clear();
            if (!r) {
                ST_LOG_WARN("podcasts", "spotify show {}: directory lookup failed: {}", show.id, r.errorMessage());
                s.failedMs[show.id] = steadyMs();
            } else {
                podcast::SpotifyLink l;
                l.showName = show.name;
                l.showPublisher = show.publisher;
                if (const auto* img = catalog::pickImage(show.images, 300)) l.showImage = img->url;
                l.checkedAt = nowUnix();
                if (const auto& e = *r) {
                    l.feedUrl = e->feedUrl;
                    l.appleId = e->appleId;
                    l.title = e->title;
                    l.author = e->author;
                    l.image = e->image;
                    l.score = podcast::matchScore(show.name, show.publisher, *e);
                }
                ST_LOG_INFO("podcasts", "spotify show {}: {}", show.id, l.matched() ? "public feed found" : "no public feed");
                s.failedMs.erase(show.id);
                podcast::store().setSpotifyLink(show.id, std::move(l));
            }
            notify();
            pump();
        });
}

void importNext(std::shared_ptr<std::vector<podcast::SpotifyLink>> items, size_t i, int ok, int failed) {
    auto& s = st();
    if (i >= items->size()) {
        s.importing = false;
        if (failed == 0) toast(i18n::plural(L"{} podcast'e abone olundu", ok));
        else toast(i18n::format(tr(L"{} podcast'e abone olundu, {} podcast eklenemedi"), {std::to_wstring(ok), std::to_wstring(failed)}),
                   ok == 0);
        notify();
        return;
    }
    const podcast::SpotifyLink link = (*items)[i];
    async(
        Priority::High, s.importLife.ref(), [feed = link.feedUrl] { return podcast::Client::shared().feed(feed, false, false); },
        [items, i, ok, failed, link](Result<podcast::Show> r) mutable {
            if (r) {
                podcast::Show show = std::move(*r);
                if (show.appleId.empty()) show.appleId = link.appleId;
                podcast::store().subscribe(show, false, nowUnix());
                ++ok;
            } else {
                ST_LOG_WARN("podcasts", "import: feed failed: {}", r.errorMessage());
                ++failed;
            }
            importNext(std::move(items), i + 1, ok, failed);
        });
}

} // namespace

// ---- The followed shows --------------------------------------------------------------------------------------------------

ListState listState() { return st().state; }
const std::vector<PodcastShow>& followed() { return st().list; }

void ensureFollowed(bool force) {
    auto& s = st();
    spotify::Api* api = source::activeApi();
    // Dev (SHADETUBE_SPOTIFY_DEMO, app/Social.h): library_shows.json there (a raw libraryV3 answer of shows, `data` or
    // not) is the followed list while logged out, so the Spotify section and the import preview can be looked at.
    if (const auto dir = socialDemoDir(); !api && !dir.empty() && std::filesystem::exists(dir / "library_shows.json")) {
        if (s.state == ListState::Ready && s.account == "demo") return;
        std::ifstream f(dir / "library_shows.json", std::ios::binary);
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        s.list = spotify::Api::parseLibraryShows(j.is_object() && j.contains("data") ? j["data"] : j);
        s.account = "demo";
        s.state = ListState::Ready;
        s.loadedMs = steadyMs();
        for (const auto& show : s.list) s.seen[show.id] = show;
        notify();
        return;
    }
    if (!api) {
        if (s.state != ListState::NoSession || !s.list.empty()) {
            s.list.clear();
            s.state = ListState::NoSession;
            s.account.clear();
            s.listLife.renew();
            s.loading = false;
        }
        return;
    }
    const std::string account = ctx().session->profile().username;
    if (account != s.account || s.state == ListState::NoSession) {
        s.list.clear();
        s.account = account;
        s.state = ListState::Loading;
        s.loadedMs = 0;
        s.listLife.renew();
        s.loading = false;
    }
    if (s.loading) return;
    const int64_t age = steadyMs() - s.loadedMs;
    if (!force && s.state == ListState::Ready && age < kListTtlMs) return;
    if (!force && s.state == ListState::Failed && age < kListRetryMs) return;
    s.loading = true;
    if (s.list.empty()) s.state = ListState::Loading;
    async(Priority::Normal, s.listLife.ref(), [api] { return api->libraryShows(); },
          [account](Result<std::vector<PodcastShow>> r) {
              auto& s = st();
              s.loading = false;
              s.loadedMs = steadyMs();
              if (account != s.account) return;
              if (r) {
                  s.list = std::move(*r);
                  s.state = ListState::Ready;
                  for (const auto& show : s.list) s.seen[show.id] = show;
              } else {
                  ST_LOG_WARN("podcasts", "spotify shows: {}", r.errorMessage());
                  if (s.list.empty()) s.state = ListState::Failed;
              }
              notify();
          });
}

void remember(const PodcastShow& show) {
    if (show.id.rfind("spotify:show:", 0) == 0 && !show.name.empty()) st().seen[show.id] = show;
}

const PodcastShow* known(const std::string& uri) {
    const auto& seen = st().seen;
    const auto it = seen.find(uri);
    return it == seen.end() ? nullptr : &it->second;
}

// ---- Lookups ---------------------------------------------------------------------------------------------------------------

Lookup lookup(const std::string& uri) {
    auto& s = st();
    auto& store = podcast::store();
    if (store.spotifyLinkFresh(uri, nowUnix())) return store.spotifyLink(uri)->matched() ? Lookup::Matched : Lookup::NoFeed;
    if (s.active == uri || queued(uri)) return Lookup::Pending;
    if (const auto it = s.failedMs.find(uri); it != s.failedMs.end() && steadyMs() - it->second < kFailedRetryMs)
        return Lookup::Failed;
    return Lookup::Unknown;
}

void resolve(const PodcastShow& show, bool urgent, bool force) {
    if (show.id.rfind("spotify:show:", 0) != 0 || show.name.empty()) return;
    auto& s = st();
    remember(show);
    if (!force && podcast::store().spotifyLinkFresh(show.id, nowUnix())) return;
    if (s.active == show.id) return;
    if (const auto it = std::find_if(s.queue.begin(), s.queue.end(), [&](const PodcastShow& q) { return q.id == show.id; });
        it != s.queue.end()) {
        if (urgent) {
            const PodcastShow moved = *it;
            s.queue.erase(it);
            s.queue.push_front(moved);
        }
        return;
    }
    if (const auto it = s.failedMs.find(show.id);
        !force && !urgent && it != s.failedMs.end() && steadyMs() - it->second < kFailedRetryMs)
        return;
    s.failedMs.erase(show.id);
    if (urgent) s.queue.push_front(show);
    else s.queue.push_back(show);
    notify();
    pump();
}

void resolveAll(const std::vector<PodcastShow>& shows) {
    for (const auto& show : shows) resolve(show, false);
}

void reject(const std::string& uri) {
    podcast::store().rejectSpotifyLink(uri, nowUnix());
    notify();
}

void subscribe(std::weak_ptr<const void> owner, std::function<void()> fn) {
    st().listeners.emplace_back(std::move(owner), std::move(fn));
}

// ---- Import -------------------------------------------------------------------------------------------------------------

bool subscribed(const std::string& uri) {
    const auto* l = podcast::store().spotifyLink(uri);
    return l && l->matched() && podcast::store().isSubscribed(l->feedUrl);
}

bool importable(const std::string& uri) { return lookup(uri) == Lookup::Matched && !subscribed(uri); }

bool importing() { return st().importing; }

void importShows(const std::vector<std::string>& uris) {
    auto& s = st();
    if (s.importing) {
        toast(tr(L"Podcastler zaten ekleniyor"));
        return;
    }
    auto items = std::make_shared<std::vector<podcast::SpotifyLink>>();
    std::vector<std::string> feeds;
    for (const auto& uri : uris) {
        if (!importable(uri)) continue;
        const auto* l = podcast::store().spotifyLink(uri);
        if (std::find(feeds.begin(), feeds.end(), l->feedUrl) != feeds.end()) continue;   // two shows, one feed
        feeds.push_back(l->feedUrl);
        items->push_back(*l);
    }
    if (items->empty()) {
        toast(tr(L"Abone olunacak podcast yok"));
        return;
    }
    s.importing = true;
    toast(i18n::plural(L"{} podcast ekleniyor…", static_cast<long long>(items->size())));
    notify();
    importNext(std::move(items), 0, 0, 0);
}

std::wstring cardSubtitle(const PodcastShow& show) {
    switch (lookup(show.id)) {
    case Lookup::Matched:
        if (subscribed(show.id)) return tr(L"Abonesin");
        break;
    case Lookup::NoFeed: return tr(L"Yalnızca Spotify'da");
    default: break;
    }
    return show.publisher.empty() ? std::wstring(tr(L"Podcast")) : toWide(show.publisher);
}

std::wstring webUrl(const std::string& uri) {
    const std::string id = uri.rfind("spotify:show:", 0) == 0 ? uri.substr(13) : uri;
    return L"https://open.spotify.com/show/" + toWide(id);
}

// ---- ShowRow --------------------------------------------------------------------------------------------------------------

ShowRow::ShowRow(PodcastShow show) : show_(std::move(show)) {
    focusable = true;
    title_ = gfx::Text(toWide(show_.name), type::body);
    publisher_ = gfx::Text(show_.publisher.empty() ? std::wstring(tr(L"Podcast")) : toWide(show_.publisher), type::secondary);
    status_ = gfx::Text({}, type::secondary);
    detail_ = gfx::Text({}, type::caption);
    toggle_ = add<ui::Toggle>();
    toggle_->onChange = [this](bool on) {
        if (onCheck) onCheck(on);
    };
    refresh();
}

void ShowRow::setChecked(bool on) { toggle_->setOn(on, false); }
bool ShowRow::checked() const { return toggle_->on(); }

void ShowRow::refresh() {
    state_ = lookup(show_.id);
    const auto* l = podcast::store().spotifyLink(show_.id);
    const bool sub = subscribed(show_.id);
    std::wstring status, detail;
    statusIcon_.clear();
    switch (state_) {
    case Lookup::Matched:
        if (sub) {
            status = tr(L"Abonesin");
            statusIcon_ = "check";
        } else {
            status = tr(L"Herkese açık besleme bulundu");
        }
        if (l) detail = l->author.empty() ? toWide(l->title) : toWide(l->title) + L" · " + toWide(l->author);
        break;
    case Lookup::NoFeed:
        status = tr(L"Herkese açık besleme yok");
        detail = tr(L"Yalnızca Spotify'da: ShadeTube çalamaz");
        statusIcon_ = "lock";
        break;
    case Lookup::Pending: status = tr(L"Aranıyor…"); break;
    case Lookup::Failed:
        status = tr(L"Kontrol edilemedi");
        detail = tr(L"Podcast dizinine ulaşılamadı");
        statusIcon_ = "warning";
        break;
    case Lookup::Unknown: status = tr(L"Sırada"); break;
    }
    status_.setText(status);
    detail_.setText(detail);
    toggle_->setVisible(state_ == Lookup::Matched && !sub);
    invalidate();
}

void ShowRow::layout() {
    const Rect r = rect();
    toggle_->setRect({r.w - 52, r.h * 0.5f - 14, 44, 28});
}

void ShowRow::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hover_));
    const Rect art{r.x + 4, r.cy() - 24, 48, 48};
    drawArtwork(c, show_.images, art, gfx::radii().art, Placeholder::Album);
    const float right = r.right() - 64;   // the toggle's column
    const float split = r.x + std::max(220.f, (r.w - 64) * 0.48f);
    const float tx = art.right() + 14, tw = std::max(0.f, split - tx - 16);
    c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
    c.text(publisher_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
    float sx = split;
    const bool muted = state_ == Lookup::NoFeed || state_ == Lookup::Pending || state_ == Lookup::Unknown;
    if (!statusIcon_.empty()) {
        const Color ic = state_ == Lookup::Failed ? col.warning : state_ == Lookup::Matched ? gfx::accent().base : col.fgTertiary;
        c.icon(statusIcon_, {sx, r.cy() - (detail_.empty() ? 8.f : 17.f), 16, 16}, ic);
        sx += 22;
    }
    const float sw = std::max(0.f, right - sx - 8);
    const Color sc = muted ? col.fgTertiary : col.fgPrimary;
    if (detail_.empty()) {
        c.text(status_, {sx, r.y, sw, r.h}, sc, gfx::VAlign::Center);
    } else {
        c.text(status_, {sx, r.cy() - 19, sw, 20}, sc, gfx::VAlign::Center);
        c.text(detail_, {sx, r.cy() + 1, sw, 18}, col.fgTertiary, gfx::VAlign::Center);
    }
    c.hline(tx, r.right(), r.bottom() - 1, col.hairSubtle);
    paintChildren(c);
}

bool ShowRow::onMouseDown(const ui::MouseEvent& e) {
    if (e.button != ui::MouseButton::Left) return false;
    pressed_ = true;
    return true;
}

void ShowRow::onMouseUp(const ui::MouseEvent& e) {
    const bool click = pressed_ && rect().contains(e.pos);
    pressed_ = false;
    if (!click || !onOpen) return;
    const auto open = onOpen;   // may navigate away (this row is deleted)
    open();
}

void ShowRow::onMouseEnter() { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
void ShowRow::onMouseLeave() {
    pressed_ = false;
    hover_.to(0, ui::motion::microMs(ui::motion::fast));
}

bool ShowRow::onActivate() {
    if (toggle_->visible()) return toggle_->onActivate();
    if (!onOpen) return false;
    const auto open = onOpen;
    open();
    return true;
}

} // namespace spotifyshows
} // namespace st::app
