// "Senin için listeler": the app side of app/SmartLists (see SmartListsPage.h) — the build schedule, the fresh songs
// of the daily mixes, the cache file, the generated covers, the Library / Home rows and the list page.
#include "app/SmartListsPage.h"

#include "app/Blacklist.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using catalog::Image;
using catalog::Track;
namespace type = gfx::type;
using json = nlohmann::json;

namespace smart {

namespace {

constexpr int64_t kRebuildAfterMs = 20 * 60 * 1000;   // the history changed: rebuild at most this often
constexpr int kLikedPages = 5;                        // Spotify's liked songs: the newest 5 x 100
constexpr size_t kCoverLookups = 16;                  // artwork lookups per build for the covers
constexpr int kCacheVersion = 1;

std::filesystem::path cacheFile() { return paths::appData() / L"smart-lists.json"; }

struct State {
    bool wired = false;
    bool cacheTried = false;
    bool cacheLoading = false;
    bool building = false;
    std::vector<List> raw;             // as built (no blocklist)
    std::vector<List> shown;           // blocklist applied
    int64_t builtDay = -1;
    std::string builtUser;             // Spotify user id of the build ("" = logged out)
    size_t builtPlays = 0;             // history size at the build (change detection)
    int64_t builtAtMs = 0;             // steadyMs()
    bool builtOnce = false;            // a build or the cache has landed (else: still loading)
    // Kept for the day: Spotify's liked songs and the fresh songs of each mix lead.
    std::vector<Track> liked;
    int64_t likedDay = -1;
    std::string likedUser;
    std::unordered_map<std::string, std::vector<Track>> fresh;
    int64_t freshDay = -1;
    std::string freshUser;
    bool notifyPosted = false;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    Lifetime life;
    LocalClock clock;
};

State& state() {
    static State s;
    return s;
}

int64_t today() {
    auto& s = state();
    if (!s.clock) s.clock = systemLocalClock();
    return listen::dayNumber(s.clock(nowUnix()));
}

std::string currentUser() {
    if (!source::loggedIn() || !ctx().session) return {};
    return ctx().session->profile().id;
}

void notifyNow() {
    auto& s = state();
    std::erase_if(s.listeners, [](const auto& l) { return l.first.expired(); });
    const auto listeners = s.listeners;   // a listener may subscribe (a page rebuilding)
    for (const auto& [owner, fn] : listeners)
        if (!owner.expired() && fn) fn();
}

// Coalesced: several changes in one message loop turn become one notification.
void notify() {
    auto& s = state();
    if (s.notifyPosted) return;
    s.notifyPosted = true;
    Dispatcher::post([] {
        state().notifyPosted = false;
        notifyNow();
    });
}

// The shown lists: the built ones minus blocked songs, and without the lists that fell below their minimum.
void applyBlocklist() {
    auto& s = state();
    s.shown.clear();
    for (const auto& l : s.raw) {
        List copy = l;
        std::vector<Track> tracks;
        std::vector<bool> fresh;
        for (size_t i = 0; i < copy.tracks.size(); ++i) {
            if (blacklist::isBlocked(copy.tracks[i])) continue;
            tracks.push_back(std::move(copy.tracks[i]));
            fresh.push_back(i < copy.fresh.size() && copy.fresh[i]);
        }
        copy.tracks = std::move(tracks);
        copy.fresh = std::move(fresh);
        if (trim(copy)) s.shown.push_back(std::move(copy));
    }
}

std::string artKey(const Track& t) { return t.name + '\x1f' + (t.artists.empty() ? std::string{} : t.artists[0].name); }

// Covers: songs without artwork (the imported history) take it from the Stats page's cache. The first songs of every
// list go in one batch (one request at a time), `lookups` of them looked up; what is found fills every list.
void enrichCovers(size_t lookups) {
    auto& s = state();
    std::vector<Track> heads;
    std::unordered_set<std::string> seen;
    for (const auto& l : s.shown)
        for (size_t i = 0; i < l.tracks.size() && i < 8; ++i)
            if (l.tracks[i].album.images.empty() && seen.insert(artKey(l.tracks[i])).second) heads.push_back(l.tracks[i]);
    if (heads.empty()) return;
    enrichTrackArtwork(heads, lookups, s.life.ref(), [] {
        enrichCovers(0);   // what landed, from the cache now
        notify();
    });
    std::unordered_map<std::string, std::vector<Image>> found;
    for (const auto& t : heads)
        if (!t.album.images.empty()) found.emplace(artKey(t), t.album.images);
    if (found.empty()) return;
    for (auto& l : s.shown)
        for (auto& t : l.tracks)
            if (t.album.images.empty())
                if (const auto it = found.find(artKey(t)); it != found.end()) t.album.images = it->second;
}

// ---- Cache file ---------------------------------------------------------------------------------------------------

json tracksJson(const std::vector<Track>& tracks) {
    json a = json::array();
    for (const auto& t : tracks) a.push_back(toJson(t));
    return a;
}

std::vector<Track> tracksFrom(const json& a) {
    std::vector<Track> out;
    if (a.is_array())
        for (const auto& t : a)
            if (t.is_object()) out.push_back(trackFromJson(t));
    return out;
}

json listJson(const List& l) {
    json j = {{"id", l.id}, {"k", static_cast<int>(l.kind)}, {"n", l.number}, {"s", l.streams}, {"t", tracksJson(l.tracks)}};
    json fresh = json::array();
    for (size_t i = 0; i < l.fresh.size(); ++i)
        if (l.fresh[i]) fresh.push_back(i);
    if (!fresh.empty()) j["f"] = std::move(fresh);
    if (!l.artists.empty()) j["a"] = l.artists;
    if (!l.seedTrackId.empty()) j["st"] = l.seedTrackId;
    if (!l.seedArtistId.empty()) j["sa"] = l.seedArtistId;
    return j;
}

std::optional<List> listFrom(const json& j, int64_t day) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return std::nullopt;
    List l;
    l.id = j["id"].get<std::string>();
    const int k = j.value("k", -1);
    if (k < 0 || k > static_cast<int>(Kind::Year)) return std::nullopt;
    l.kind = static_cast<Kind>(k);
    l.number = j.value("n", 0);
    l.streams = j.value("s", 0);
    l.day = day;
    l.tracks = tracksFrom(j.value("t", json::array()));
    l.fresh.assign(l.tracks.size(), false);
    if (const auto f = j.find("f"); f != j.end() && f->is_array())
        for (const auto& i : *f)
            if (i.is_number_unsigned() && i.get<size_t>() < l.fresh.size()) l.fresh[i.get<size_t>()] = true;
    if (const auto a = j.find("a"); a != j.end() && a->is_array())
        for (const auto& n : *a)
            if (n.is_string()) l.artists.push_back(n.get<std::string>());
    l.seedTrackId = j.value("st", std::string{});
    l.seedArtistId = j.value("sa", std::string{});
    return l;
}

void saveCache() {
    auto& s = state();
    json j = {{"v", kCacheVersion}, {"day", s.builtDay}, {"user", s.builtUser}, {"plays", s.builtPlays}};
    json lists = json::array();
    for (const auto& l : s.raw) lists.push_back(listJson(l));
    j["lists"] = std::move(lists);
    if (s.freshDay == s.builtDay) {
        json fresh = json::object();
        for (const auto& [lead, tracks] : s.fresh) fresh[lead] = tracksJson(tracks);
        j["fresh"] = std::move(fresh);
        j["freshDay"] = s.freshDay;
        j["freshUser"] = s.freshUser;
    }
    if (!s.liked.empty() && s.likedDay == s.builtDay) {
        j["liked"] = tracksJson(s.liked);
        j["likedDay"] = s.likedDay;
        j["likedUser"] = s.likedUser;
    }
    auto text = std::make_shared<std::string>(j.dump(-1, ' ', false, json::error_handler_t::replace));
    async(Priority::Low, s.life.ref(), [text] {
        const auto file = cacheFile();
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << *text;
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
    }, [](Result<Unit>) {});
}

void loadCache() {
    auto& s = state();
    s.cacheTried = true;
    s.cacheLoading = true;
    async(
        Priority::Normal, s.life.ref(),
        [] {
            std::ifstream f(cacheFile(), std::ios::binary);
            if (!f) return json();
            std::ostringstream ss;
            ss << f.rdbuf();
            return json::parse(ss.str(), nullptr, false);
        },
        [](Result<json> r) {
            auto& s = state();
            s.cacheLoading = false;
            if (r && r->is_object() && r->value("v", 0) == kCacheVersion && !s.builtOnce) {
                const json& j = *r;
                const int64_t day = j.value("day", int64_t{-1});
                for (const auto& e : j.value("lists", json::array()))
                    if (auto l = listFrom(e, day)) s.raw.push_back(std::move(*l));
                s.builtDay = day;
                s.builtUser = j.value("user", std::string{});
                s.builtPlays = j.value("plays", size_t{0});
                s.builtAtMs = steadyMs();
                s.builtOnce = true;
                if (const auto fresh = j.find("fresh"); fresh != j.end() && fresh->is_object()) {
                    for (const auto& [lead, tracks] : fresh->items()) s.fresh[lead] = tracksFrom(tracks);
                    s.freshDay = j.value("freshDay", int64_t{-1});
                    s.freshUser = j.value("freshUser", std::string{});
                }
                if (const auto liked = j.find("liked"); liked != j.end()) {
                    s.liked = tracksFrom(*liked);
                    s.likedDay = j.value("likedDay", int64_t{-1});
                    s.likedUser = j.value("likedUser", std::string{});
                }
                applyBlocklist();
                enrichCovers(0);
                notify();
            }
            ensure();
        });
}

// ---- Building -----------------------------------------------------------------------------------------------------

bool isMbid(const std::string& id) {
    return id.size() == 36 && id[8] == '-' && id[13] == '-' && id[18] == '-' && id[23] == '-';
}

std::string leadKey(const List& mix) {
    if (!mix.seedTrackId.empty()) return mix.seedTrackId;
    if (!mix.seedArtistId.empty()) return mix.seedArtistId;
    return mix.artists.empty() ? std::string{} : "name:" + mix.artists[0];
}

struct Job {
    Input input;
    spotify::Api* api = nullptr;       // logged in: Spotify's liked songs and radios
    std::string user;
    int64_t day = 0;
    std::vector<Track> liked;          // what is known already (the local library's, or Spotify's of today)
    bool fetchLiked = false;
    std::unordered_map<std::string, std::vector<Track>> fresh;   // today's, per mix lead
};

struct Built {
    std::vector<List> lists;
    std::vector<Track> liked;
    bool likedFetched = false;
    std::unordered_map<std::string, std::vector<Track>> fresh;
    int64_t day = 0;
    std::string user;
    size_t plays = 0;
};

std::vector<Track> spotifyLiked(spotify::Api* api) {
    std::vector<Track> out;
    int offset = 0;
    for (int page = 0; page < kLikedPages; ++page) {
        if (page) Sleep(300);
        auto p = api->playlistTracks(spotify::Api::kLikedSongsUri, offset, 100);
        const int next = p.nextOffset >= 0 ? p.nextOffset : offset + static_cast<int>(p.items.size());
        for (auto& t : p.items) out.push_back(std::move(t));
        if (next <= offset || next >= p.total) break;
        offset = next;
    }
    return out;
}

// Fresh songs for one mix lead: Spotify's radio of its song (logged in), else ListenBrainz's similar artists' hits.
std::vector<Track> freshFor(spotify::Api* api, const List& mix) {
    std::vector<Track> out;
    if (api) {
        if (mix.seedTrackId.rfind("spotify:track:", 0) != 0) return out;
        const std::string uri = api->radioPlaylist(mix.seedTrackId);
        if (uri.empty()) return out;
        Sleep(300);
        out = api->playlistTracks(uri, 0, 50).items;
        return out;
    }
    if (!isMbid(mix.seedArtistId)) return out;
    const auto similar = mb::similarArtists(mix.seedArtistId, 4);
    for (size_t i = 0; i < similar.size() && i < 2; ++i)
        for (auto& t : mb::topTracksForArtist(similar[i].id, 8)) out.push_back(std::move(t));
    return out;
}

Built runJob(Job job) {
    Built b;
    b.day = job.day;
    b.user = job.user;
    b.plays = job.input.plays.size();
    job.input.clock = systemLocalClock();   // a clock per thread
    if (job.fetchLiked && job.api) {
        try {
            job.liked = spotifyLiked(job.api);
            b.likedFetched = true;
        } catch (const std::exception& e) {
            ST_LOG_WARN("smart", "liked songs: {}", e.what());   // keep the previous ones
        }
    }
    job.input.liked = job.liked;
    b.liked = std::move(job.liked);
    b.lists = build(job.input);
    // Fresh songs of the mixes (those of today are reused), then blended in.
    std::vector<std::wstring> known;
    known.reserve(job.input.tracks.size());
    for (const auto& t : job.input.tracks) known.push_back(ListenStats::keysFor(t).track);
    b.fresh = std::move(job.fresh);
    bool first = true;
    for (auto& l : b.lists) {
        if (l.kind != Kind::Mix) continue;
        const std::string lead = leadKey(l);
        if (lead.empty()) continue;
        if (!b.fresh.contains(lead)) {
            try {
                if (!first) Sleep(300);
                first = false;
                b.fresh[lead] = freshFor(job.api, l);
            } catch (const std::exception& e) {
                ST_LOG_WARN("smart", "fresh songs for {}: {}", lead, e.what());   // asked again at the next build
            }
        }
        if (const auto it = b.fresh.find(lead); it != b.fresh.end()) mixFresh(l, it->second, known);
    }
    return b;
}

void startBuild() {
    auto& s = state();
    s.building = true;
    Job job;
    const auto& stats = listenStats();
    job.input.tracks = stats.tracks();
    job.input.plays = stats.plays();
    job.input.now = nowUnix();
    job.day = today();
    job.user = currentUser();
    job.api = job.user.empty() ? nullptr : source::activeApi();
    if (!job.api) {
        job.liked = ctx().library.liked();
    } else if (s.likedDay == job.day && s.likedUser == job.user) {
        job.liked = s.liked;
    } else {
        job.fetchLiked = true;
        if (s.likedUser == job.user) job.liked = s.liked;   // yesterday's, if today's fetch fails
    }
    if (s.freshDay == job.day && s.freshUser == job.user) job.fresh = s.fresh;
    async(Priority::Low, s.life.ref(), [job = std::move(job)]() mutable { return runJob(std::move(job)); },
          [](Result<Built> r) {
              auto& s = state();
              s.building = false;
              if (!r) {
                  ST_LOG_WARN("smart", "building the lists failed: {}", r.errorMessage());
                  return;
              }
              s.raw = std::move(r->lists);
              s.builtDay = r->day;
              s.builtUser = r->user;
              s.builtPlays = r->plays;
              s.builtAtMs = steadyMs();
              s.builtOnce = true;
              if (r->likedFetched) {
                  s.liked = std::move(r->liked);
                  s.likedDay = r->day;
                  s.likedUser = r->user;
              }
              s.fresh = std::move(r->fresh);
              s.freshDay = r->day;
              s.freshUser = r->user;
              ST_LOG_INFO("smart", "{} list(s) built from {} plays", s.raw.size(), s.builtPlays);
              applyBlocklist();
              enrichCovers(kCoverLookups);
              saveCache();
              notify();
          });
}

void wire() {
    auto& s = state();
    s.wired = true;
    listenStats().subscribe(s.life.ref(), [] { ensure(); });
    if (ctx().session) ctx().session->subscribe(s.life.ref(), [] { ensure(); });
    blacklist::onChanged([] {
        applyBlocklist();
        enrichCovers(0);
        notify();
    });
}

} // namespace

const std::vector<List>& lists() { return state().shown; }

const List* find(const std::string& id) {
    for (const auto& l : state().shown)
        if (l.id == id) return &l;
    return nullptr;
}

void ensure() {
    auto& s = state();
    if (!s.wired) wire();
    if (!s.cacheTried) {
        loadCache();   // then ensure() again
        return;
    }
    if (s.cacheLoading || s.building || !listenStats().loaded()) return;
    // A saved Spotify session still connecting: build once it has (its notification calls ensure() again), not twice.
    if (ctx().session && ctx().session->state() == spotify::SessionState::Connecting) return;
    const std::string user = currentUser();
    const size_t plays = listenStats().playCount();
    const bool due = !s.builtOnce || s.builtDay != today() || s.builtUser != user ||
                     (plays != s.builtPlays && steadyMs() - s.builtAtMs >= kRebuildAfterMs);
    if (due) startBuild();
}

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { state().listeners.emplace_back(std::move(owner), std::move(fn)); }

std::wstring title(const List& l) {
    switch (l.kind) {
    case Kind::Mix: return i18n::format(tr(L"Günün karışımı {}"), {std::to_wstring(l.number)});
    case Kind::Month: return tr(L"Bu ayın favorileri");
    case Kind::Discoveries: return tr(L"Yeni keşiflerin");
    case Kind::Rediscover: return tr(L"Tekrar keşfet");
    case Kind::Forgotten: return tr(L"Unutulan beğeniler");
    case Kind::Best: return tr(L"Tüm zamanların en iyileri");
    case Kind::Year: return i18n::format(tr(L"{} en iyileri"), {std::to_wstring(l.number)});
    }
    return {};
}

std::wstring subtitle(const List& l) {
    if (l.kind == Kind::Mix && !l.artists.empty()) {
        std::wstring names = toWide(l.artists[0]);
        if (l.artists.size() > 1) names += L", " + toWide(l.artists[1]);
        return l.artists.size() > 2 ? i18n::format(tr(L"{} ve daha fazlası"), {names}) : names;
    }
    return i18n::plural(L"{} şarkı", static_cast<int64_t>(l.tracks.size()));
}

std::wstring why(const List& l) {
    switch (l.kind) {
    case Kind::Mix: {
        std::wstring s = i18n::format(tr(L"{} ve birlikte dinlediğin sanatçılar. Her gün yenilenir."),
                                      {l.artists.empty() ? std::wstring{} : toWide(l.artists[0])});
        const auto fresh = std::count(l.fresh.begin(), l.fresh.end(), true);
        if (fresh > 0) s += L" " + i18n::plural(L"{} yeni öneri içerir.", fresh);
        return s;
    }
    case Kind::Month:
        return i18n::format(tr(L"Son 30 günde en çok dinlediklerin ({}). Her gün yenilenir."),
                            {i18n::plural(L"{} dinleme", l.streams)});
    case Kind::Discoveries: return tr(L"Son 30 günde ilk kez dinleyip yeniden açtığın şarkılar. Her gün yenilenir.");
    case Kind::Rediscover: return tr(L"Çok dinlediğin ama üç aydır açmadığın şarkılar. Her gün yeni bir seçki.");
    case Kind::Forgotten:
        return i18n::format(tr(L"Beğendiğin ama neredeyse hiç dinlemediğin şarkılar ({}). Her gün yeni bir seçki."),
                            {i18n::plural(L"{} şarkı", l.streams)});
    case Kind::Best:
        return i18n::format(tr(L"Tüm zamanlarda en çok dinlediklerin, yakın zamanda dinlediklerin önde ({})."),
                            {i18n::plural(L"{} dinleme", l.streams)});
    case Kind::Year:
        return i18n::format(tr(L"{} yılında en çok dinlediğin şarkılar ({})."),
                            {std::to_wstring(l.number), i18n::plural(L"{} dinleme", l.streams)});
    }
    return {};
}

namespace {

// The short name on the cover's band.
std::wstring bandTitle(const List& l) {
    switch (l.kind) {
    case Kind::Mix: return i18n::format(tr(L"Karışım {}"), {std::to_wstring(l.number)});
    case Kind::Month: return tr(L"Bu ay");
    case Kind::Discoveries: return tr(L"Keşifler");
    case Kind::Rediscover: return tr(L"Tekrar keşfet");
    case Kind::Forgotten: return tr(L"Beğeniler");
    case Kind::Best: return tr(L"En iyiler");
    case Kind::Year: return std::to_wstring(l.number);
    }
    return {};
}

std::wstring bandLabel(const List& l) {
    return toUpperTr(l.kind == Kind::Year ? std::wstring(tr(L"Yıl özeti")) : std::wstring(tr(L"Senin için")));
}

const char* kindIcon(Kind k) {
    switch (k) {
    case Kind::Mix: return "shuffle";
    case Kind::Month: return "calendar";
    case Kind::Discoveries: return "search";
    case Kind::Rediscover: return "refresh";
    case Kind::Forgotten: return "heart";
    case Kind::Best: return "stats";
    case Kind::Year: return "calendar";
    }
    return "playlist";
}

// Up to four distinct covers from the list's songs, in list order.
std::vector<std::vector<Image>> coversOf(const List& l) {
    std::vector<std::vector<Image>> out;
    std::unordered_set<std::string> seen;
    for (const auto& t : l.tracks) {
        const auto* img = catalog::pickImage(t.album.images, 300);
        if (!img || img->url.empty() || !seen.insert(img->url).second) continue;
        out.push_back(t.album.images);
        if (out.size() == 4) break;
    }
    return out;
}

} // namespace

void drawCover(gfx::Canvas& c, const List& l, const Rect& r, float radius) {
    const auto& col = colors();
    const Color tone = tileFill(tileColor(l.id));
    const float band = std::round(r.h * 0.3f);
    const Rect art{r.x, r.y, r.w, r.h - band};
    c.pushRoundedClip(r, radius);
    c.fillRect(r, tone);
    const auto covers = coversOf(l);
    if (covers.size() >= 4) {
        const float w = art.w / 2, h = art.h / 2;
        for (size_t i = 0; i < 4; ++i)
            drawArtwork(c, covers[i], {art.x + (i % 2) * w, art.y + (i / 2) * h, w, h}, 0, Placeholder::Album, false, Priority::Low);
    } else if (!covers.empty()) {
        drawArtwork(c, covers[0], art, 0, Placeholder::Album, false, Priority::Low);
    } else {
        const float s = std::round(art.h * 0.34f);
        c.icon(kindIcon(l.kind), art.center(s, s), col.fgSecondary);
    }
    // The band: an accent rule, the kind in mono and the short name, sized to the cover.
    const Rect b{r.x, r.bottom() - band, r.w, band};
    c.fillRect(b, tone);
    c.fillRect({b.x, b.y, b.w, std::max(2.f, std::round(r.w / 80))}, accent().base);
    const float pad = std::round(r.w * 0.07f);
    const float labelSize = std::clamp(r.w * 0.05f, 8.f, 11.f);
    const float titleSize = std::clamp(r.w * 0.11f, 13.f, 26.f);
    c.text(bandLabel(l), type::monoLabel.withSize(labelSize), {b.x + pad, b.y + band * 0.18f, b.w - pad * 2, labelSize * 1.5f},
           col.fgSecondary);
    c.text(bandTitle(l), type::title.withSize(titleSize), {b.x + pad, b.y + band * 0.18f + labelSize * 1.5f, b.w - pad * 2, titleSize * 1.3f},
           col.fgPrimary);
    c.popLayer();
}

namespace {

// A smart list's card: the generated cover, the name and a line under it; hover play button like MediaCard.
class SmartCard : public ui::Widget {
public:
    explicit SmartCard(List list)
        : list_(std::move(list)), title_(title(list_), type::body.withSize(13)), subtitle_(subtitle(list_), type::caption) {
        focusable = true;
    }
    float preferredHeight(float width) override { return width + 10 + 18 + 4 + 16; }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            menu(e.windowPos);
            return false;
        }
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, 80);
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, 160);
        if (!rect().contains(e.pos)) return;
        open(playRect().contains(e.pos));
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = playRect().contains(e.pos);
        if (hot != playHot_) {
            playHot_ = hot;
            invalidate();
        }
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        playHot_ = false;
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        press_.snap(1);
        press_.to(0, 160);
        open(false);
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) return false;
        const Rect a = toWindow(artRect());
        menu({a.x + 12, a.bottom() - 12});
        return true;
    }
    Rect focusRect() const override { return rect(); }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        const Rect art = artRect();
        c.pushScale(1 - 0.01f * press_, {art.cx(), art.cy()});
        if (h > 0.01f) c.shadow(art, 2, 24, 12, col.shadowCard.mulAlpha(h));
        drawCover(c, list_, art, 2);
        if (h > 0.01f) {
            const Rect pr = playRect().offset(0, 8 * (1 - ui::ease(ui::Ease::Decelerate, h)));
            c.pushOpacity(h);
            c.fillCircle({pr.cx(), pr.cy()}, 24, playHot_ ? accent().hover : accent().base);
            c.icon("play", pr.center(16, 16).offset(1, 0), accent().onAccent);
            c.popLayer();
        }
        c.popTransform();
        const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
        c.text(title_, tr, col.fgPrimary, gfx::VAlign::Center);
        c.text(subtitle_, {rect().x, tr.bottom() + 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
    }

private:
    Rect artRect() const { return {rect().x, rect().y, rect().w, rect().w}; }
    // Just above the band, so the name on the cover stays readable.
    Rect playRect() const {
        const Rect a = artRect();
        return {a.right() - 12 - 48, a.bottom() - std::round(a.h * 0.3f) - 12 - 48, 48, 48};
    }
    void open(bool play) const {
        const std::string id = list_.id + (play ? "#play" : "");   // navigating destroys this card: copy first
        ctx().router->navigate({RouteKind::SmartList, id});
    }
    void menu(gfx::Point at) const {
        const std::string id = list_.id;
        const std::vector<Track> tracks = list_.tracks;
        const std::wstring name = title(list_);
        ui::Menu::open(ctx().window, at,
                       {{tr(L"Aç"), "arrow-up-right", L"", [id] { ctx().router->navigate({RouteKind::SmartList, id}); }},
                        {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::SmartList, id + "#play"}); }},
                        {tr(L"Tümünü sıraya ekle"), "queue", L"", [tracks] {
                             ctx().player->enqueue(tracks);
                             toast(tr(L"Sıraya eklendi"));
                         }},
                        ui::MenuItem::sep(),
                        {tr(L"Çalma listesi olarak kaydet"), "plus", L"", [tracks] {
                             promptNewPlaylist([](const std::string& pid) { ctx().router->navigate({RouteKind::Playlist, pid}); },
                                               tracks);
                         }}});
    }

    List list_;
    gfx::Text title_, subtitle_;
    ui::Anim hover_, press_;
    bool playHot_ = false;
};

// "Senin için listeler": header + cards, rebuilt when the lists change; hidden while there is none.
class SmartSection : public ui::Column {
public:
    explicit SmartSection(int maxRows) : ui::Column(0), maxRows_(maxRows) {
        hitTestVisible = false;
        subscribe(life_.ref(), [this] { rebuild(); });
        rebuild();
        ensure();
    }

private:
    void rebuild() {
        clearChildren();
        const auto& all = lists();
        setVisible(!all.empty());
        if (all.empty()) {
            requestLayout();
            return;
        }
        add<SectionHeader>(tr(L"Senin için listeler"), toUpperTr(tr(L"Her gün yenilenir")));
        auto* grid = add<ui::Grid>(maxRows_ == 1 ? 150.f : 170.f, gfx::metrics::cardGap,
                                   [](float w) { return w + 10 + 18 + 4 + 16; }, maxRows_);
        setSpacingBefore(grid, 16);
        for (const auto& l : all) grid->add<SmartCard>(l);
        requestLayout();
        invalidate();
    }

    int maxRows_;
    Lifetime life_;
};

// ---- The list page --------------------------------------------------------------------------------------------------

class SmartHeader : public ui::Widget {
public:
    SmartHeader() {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        add_ = add<Button>(ButtonKind::IconOutline, L"", "plus");
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        add_->setTooltip(tr(L"Çalma listesine ekle"));
        more_->setTooltip(tr(L"Diğer seçenekler"));
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        title_ = gfx::Text({}, type::displayXL, wrap);
        why_ = gfx::Text({}, type::secondary, wrap);
    }
    void setList(const List& l) {
        list_ = l;
        title_.setText(title(l));
        why_.setText(why(l));
        int64_t ms = 0;
        for (const auto& t : l.tracks) ms += t.durationMs;
        totalMs_ = ms;
        requestLayout();
        invalidate();
    }
    float preferredHeight(float) override { return 232 + 28 + 56 + 24; }
    void layout() override {
        const float y = 232 + 28;
        play_->setRect({0, y, 56, 56});
        const float sw = shuffle_->naturalWidth();
        shuffle_->setRect({56 + 16, y + 8, sw, 40});
        float x = 56 + 16 + sw + 14;
        for (auto* b : {add_, more_}) {
            b->setRect({x, y + 8, 40, 40});
            x += 40 + 14;
        }
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const Color tone = tileColor(list_.id);
        const Color wash = gfx::isLightTheme() ? gfx::Theme::adaptAccent(tone, true).withAlpha(0.12f) : tone.withAlpha(0.35f);
        c.fillLinearGradient({r.x - 48, r.y - 36, r.w + 96, 420}, {0, r.y - 36}, {0, r.y + 384}, wash, wash.withAlpha(0));
        const Rect cover{r.x, r.y, 232, 232};
        c.shadow(cover, 2, 40, 18, col.shadowCard.mulAlpha(0.35f / 0.45f));
        drawCover(c, list_, cover, 2);
        const float tx = cover.right() + 32, tw = r.right() - tx;
        float size = 88;
        for (float s : {88.f, 72.f, 56.f, 44.f, 36.f}) {
            size = s;
            title_.setStyle(type::displayXL.withSize(s));
            const float h = title_.measure(tw).h;
            if (s > 56 ? h <= s * 1.05f : h <= s * 2.1f) break;
        }
        const float th = title_.measure(tw).h;
        const float dw = std::min(tw, 640.f);
        const float dh = why_.empty() ? 0.f : std::ceil(why_.measure(dw).h);
        float y = cover.bottom() - th - 30 - (why_.empty() ? 0.f : dh + 6);
        const std::wstring label = list_.kind == Kind::Year
                                       ? toUpperTr(tr(L"Yıl özeti")) + L" · " + std::to_wstring(list_.number)
                                       : toUpperTr(tr(L"Senin için")) + L" · " + toUpperTr(tr(L"Otomatik liste"));
        c.text(label, type::monoLabel, {tx, y - 26, tw, 14}, col.fgSecondary);
        c.text(title_, {tx - size * 0.04f, y, tw, th}, col.fgPrimary);
        y += th + 10;
        if (!why_.empty()) {
            c.text(why_, {tx, y, dw, dh}, col.fgSecondary);
            y += dh + 6;
        }
        std::wstring meta = L"ShadeTube · " + i18n::plural(L"{} şarkı", static_cast<int64_t>(list_.tracks.size()));
        if (totalMs_ > 0) meta += L" · " + totalDuration(totalMs_);
        c.text(meta, type::secondary, {tx, y + 2, tw, 20}, col.fgSecondary);
        paintChildren(c);
    }

    ui::PlayButton* play_;
    Button *shuffle_, *add_, *more_;

private:
    List list_;
    gfx::Text title_, why_;
    int64_t totalMs_ = 0;
};

class SmartListPage : public ScrollPage {
public:
    SmartListPage(std::string id, bool autoplay) : id_(std::move(id)), autoplay_(autoplay) {
        subscribe(life_.ref(), [this] { refresh(); });
        ensure();
        refresh();
    }

private:
    std::string contextUri() const { return "smart:" + id_; }
    bool isPlayingThis() const {
        auto* p = ctx().player;
        return p && p->isPlaying() && p->context().uri == contextUri();
    }

    // A build landed / covers were found: rebuild when the songs changed, else just repaint the header.
    void refresh() {
        const List* l = find(id_);
        if (!l) {
            if (state().builtOnce && !state().building) showMissing();
            else if (!header_) showSkeleton(8);
            return;
        }
        std::vector<std::string> ids;
        for (const auto& t : l->tracks) ids.push_back(t.id);
        if (header_ && ids == ids_) {
            header_->setList(*l);
            return;
        }
        ids_ = std::move(ids);
        const float y = scroll_->scrollY();
        build(*l);
        if (y > 0) {
            scroll_->layout();
            scroll_->scrollTo(y, false);
        }
    }

    void showMissing() {
        header_ = nullptr;
        table_ = nullptr;
        ids_.clear();
        auto* c = resetContent();
        c->add<MessagePanel>("playlist", tr(L"Bu liste şu an yok"),
                             tr(L"Otomatik listeler dinledikçe oluşur ve her gün yenilenir; bu liste için yeterli dinleme "
                                L"kalmamış olabilir."),
                             tr(L"Kitaplığa dön"), [] { ctx().router->navigate({RouteKind::Library}); });
        contentReady();
    }

    void build(const List& l) {
        auto* c = resetContent(28.f);
        header_ = c->add<SmartHeader>();
        header_->setList(l);
        TrackTable::Options opts;
        opts.showAdded = false;
        table_ = c->add<TrackTable>(opts);
        tracks_ = l.tracks;
        table_->onPlay = [this](int i) { playFrom(i, false); };
        header_->play_->onClick = [this] { playFrom(-1, false); };
        header_->shuffle_->onClick = [this] { playFrom(-1, true); };
        header_->add_->onClick = [this] {
            const Rect br = header_->add_->toWindow(header_->add_->rect());
            showAddToPlaylistMenu(table_->displayedTracks(), {br.x, br.bottom() + 6});
        };
        header_->more_->onClick = [this] {
            const Rect br = header_->more_->toWindow(header_->more_->rect());
            const auto tracks = table_->displayedTracks();
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6},
                           {{tr(L"Tümünü sıraya ekle"), "queue", L"", [tracks] {
                                 ctx().player->enqueue(tracks);
                                 toast(tr(L"Sıraya eklendi"));
                             }},
                            {tr(L"Çalma listesi olarak kaydet"), "plus", L"", [tracks] {
                                 promptNewPlaylist(
                                     [](const std::string& pid) { ctx().router->navigate({RouteKind::Playlist, pid}); }, tracks);
                             }}});
        };
        // Songs from the imported history have no covers: from the Stats page's cache, a few more looked up.
        enrichTrackArtwork(tracks_, 24, life_.ref(), [this] { artLanded(); });
        table_->setTracks(tracks_);
        scroll_->contentChanged();
        contentReady();
        if (autoplay_) {
            autoplay_ = false;
            playFrom(-1, false);
        }
    }

    void artLanded() {
        if (artPosted_ || !table_) return;
        artPosted_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            artPosted_ = false;
            if (!table_) return;
            enrichTrackArtwork(tracks_, 0, life_.ref(), {});
            table_->setTracks(tracks_);
            invalidate();
        });
    }

    void playFrom(int displayIndex, bool shuffle) {
        auto* p = ctx().player;
        if (!p || !table_ || !header_) return;
        if (displayIndex < 0 && isPlayingThis()) {
            p->togglePause();
            return;
        }
        if (displayIndex < 0 && p->context().uri == contextUri() && p->status() == player::Status::Paused) {
            p->play();
            return;
        }
        auto tracks = table_->displayedTracks();
        if (tracks.empty()) return;
        if (shuffle) p->setShuffle(true);
        const List* l = find(id_);
        p->playContext(std::move(tracks), displayIndex, {contextUri(), l ? title(*l) : std::wstring{}});
    }

    std::string id_;
    bool autoplay_;
    SmartHeader* header_ = nullptr;
    TrackTable* table_ = nullptr;
    std::vector<Track> tracks_;
    std::vector<std::string> ids_;
    bool artPosted_ = false;
};

} // namespace

void addSection(ui::Column* c, int maxRows) { c->add<SmartSection>(maxRows); }

} // namespace smart

std::unique_ptr<Page> makeSmartListPage(const std::string& rawId) {
    std::string id = rawId;
    bool autoplay = false;
    if (const auto hash = id.find("#play"); hash != std::string::npos) {
        id.resize(hash);
        autoplay = true;
    }
    return std::make_unique<smart::SmartListPage>(id, autoplay);
}

} // namespace st::app
