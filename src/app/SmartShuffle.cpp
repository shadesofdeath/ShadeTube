// Smart shuffle + "Geliştir": the recommendation pools, the queue top-up, the menus' actions and the settings row
// (see SmartShuffle.h).
#include "app/SmartShuffle.h"

#include "app/AppContext.h"
#include "app/Blacklist.h"
#include "app/InternetRadio.h"
#include "app/LocalLibrary.h"
#include "app/SettingsWidgets.h"
#include "app/SmartMix.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace st::app::smartshuffle {

using catalog::Track;

namespace {

constexpr int64_t kPoolTtlMs = 30 * 60 * 1000;   // a collection's recommendations are fetched again after this
constexpr int64_t kBackoffMs = 10 * 60 * 1000;   // Spotify said 429: no radios for a while
constexpr size_t kMaxPools = 12;
constexpr size_t kSpotifySeeds = 3;              // song radios per collection
constexpr int kRadioTracks = 50;
constexpr size_t kMbArtists = 2;                 // most frequent MusicBrainz artists of the collection
constexpr int kSimilarPerArtist = 3;
constexpr int kTopPerArtist = 4;
constexpr size_t kLocalExtra = 30;               // local-library songs by the collection's artists
constexpr size_t kMinPool = 20;                  // fewer from Spotify: ask ListenBrainz too

struct Pool {
    std::vector<Track> tracks;
    int64_t fetchedAt = 0;
    bool loading = false;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> waiters;   // run when the fetch lands
};

struct Seeds {
    std::vector<std::string> spotify;       // spotify:track: URIs (song radios)
    std::vector<std::string> artists;       // MusicBrainz artist ids (similar artists)
    std::vector<std::string> artistNames;   // folded first artists (local library)
};

struct Fetched {
    std::vector<Track> tracks;
    bool rateLimited = false;
};

std::unordered_map<std::string, Pool> g_pools;
std::unordered_map<std::string, smartmix::Exclusions> g_hidden;   // key -> "−" (and added) songs, this session
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_listeners;
int64_t g_backoffUntil = 0;
Lifetime g_life;
bool g_syncPosted = false;
bool g_applying = false;
struct Planned {
    uint64_t gen = 0;
    int pos = -1;
    size_t size = 0;
} g_planned;   // the queue as it was last planned (nothing changed: nothing to plan)

void notifyListeners() {
    std::erase_if(g_listeners, [](const auto& l) { return l.first.expired(); });
    const auto listeners = g_listeners;   // a listener may subscribe / rebuild its page
    for (const auto& [ref, fn] : listeners)
        if (!ref.expired()) fn();
}

bool isMbid(const std::string& id) { return id.size() == 36 && id[8] == '-' && id.find(':') == std::string::npos; }

std::string contextKey(const std::string& uri) {
    if (uri == "liked") return "liked";
    if (uri.rfind("playlist:", 0) == 0) return uri.substr(9);
    return {};
}

bool notASong(const Track& t) { return radio::isStationId(t.id) || catalog::isPodcastId(t.id); }

Seeds seedsOf(const std::vector<Track>& collection, uint64_t seed) {
    Seeds s;
    std::vector<const Track*> spotify;
    std::unordered_map<std::string, int> artistCount;
    std::unordered_set<std::string> names;
    for (const auto& t : collection) {
        if (t.recommended || notASong(t)) continue;
        if (t.id.rfind("spotify:track:", 0) == 0) spotify.push_back(&t);
        if (t.artists.empty()) continue;
        if (isMbid(t.artists[0].id)) ++artistCount[t.artists[0].id];
        if (const std::string n = smartmix::foldName(t.artists[0].name); !n.empty() && names.size() < 40) names.insert(n);
    }
    if (!spotify.empty()) {   // spread over the list, starting somewhere that depends on the collection
        const size_t n = std::min(kSpotifySeeds, spotify.size());
        const size_t start = static_cast<size_t>(seed % spotify.size());
        for (size_t k = 0; k < n; ++k) s.spotify.push_back(spotify[(start + k * spotify.size() / n) % spotify.size()]->id);
    }
    std::vector<std::pair<std::string, int>> ranked(artistCount.begin(), artistCount.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    for (size_t k = 0; k < ranked.size() && k < kMbArtists; ++k) s.artists.push_back(ranked[k].first);
    s.artistNames.assign(names.begin(), names.end());
    return s;
}

// Worker thread. `guard` is checked before every request, so a fetch outliving the app stops at the next one.
Fetched fetchPool(spotify::Api* api, const Seeds& seeds, const Lifetime::Ref& guard) {
    Fetched f;
    if (api) {
        for (const auto& seed : seeds.spotify) {
            if (guard.expired()) return f;
            try {
                const std::string radio = api->radioPlaylist(seed);
                if (radio.empty() || guard.expired()) continue;
                for (auto& t : api->playlistTracks(radio, 0, kRadioTracks).items) f.tracks.push_back(std::move(t));
            } catch (const spotify::ApiError& e) {
                if (e.status == 429) {
                    f.rateLimited = true;
                    break;
                }
                if (e.status != 404) ST_LOG_WARN("smartshuffle", "song radio of {} failed: {}", seed, e.what());
            } catch (const std::exception& e) {
                ST_LOG_WARN("smartshuffle", "song radio of {} failed: {}", seed, e.what());
            }
        }
    }
    if (f.tracks.size() < kMinPool) {
        for (const auto& artist : seeds.artists) {
            if (guard.expired()) return f;
            try {
                int n = 0;
                for (const auto& similar : mb::similarArtists(artist, 8)) {
                    if (n++ >= kSimilarPerArtist || guard.expired()) break;
                    for (auto& t : mb::topTracksForArtist(similar.id, kTopPerArtist)) f.tracks.push_back(std::move(t));
                }
            } catch (const std::exception& e) {
                ST_LOG_WARN("smartshuffle", "similar artists of {} failed: {}", artist, e.what());
            }
        }
    }
    return f;
}

// UI thread: songs of the local library by the collection's artists.
std::vector<Track> localByArtists(const std::vector<std::string>& foldedNames) {
    std::vector<Track> out;
    if (foldedNames.empty()) return out;
    const std::unordered_set<std::string> names(foldedNames.begin(), foldedNames.end());
    for (const auto& t : LocalLibrary::get().tracks()) {
        if (out.size() >= kLocalExtra) break;
        if (!t.artists.empty() && names.contains(smartmix::foldName(t.artists[0].name))) out.push_back(t);
    }
    return out;
}

void prunePools() {
    while (g_pools.size() > kMaxPools) {
        auto oldest = g_pools.end();
        for (auto it = g_pools.begin(); it != g_pools.end(); ++it)
            if (!it->second.loading && (oldest == g_pools.end() || it->second.fetchedAt < oldest->second.fetchedAt)) oldest = it;
        if (oldest == g_pools.end()) return;
        g_pools.erase(oldest);
    }
}

void deliver(const std::string& key, const std::vector<Track>& collection, const Lifetime::Ref& guard,
             const std::function<void(std::vector<Track>)>& done) {
    if (guard.expired()) return;
    smartmix::Exclusions exclude;
    for (const auto& t : collection)
        if (!t.recommended) exclude.add(t);
    if (const auto it = g_hidden.find(key); it != g_hidden.end()) {
        exclude.ids.insert(it->second.ids.begin(), it->second.ids.end());
        exclude.keys.insert(it->second.keys.begin(), it->second.keys.end());
    }
    const auto& pool = g_pools[key].tracks;
    auto recs = smartmix::candidates(pool, exclude, [](const Track& t) { return blacklist::isBlocked(t) || notASong(t); },
                                     smartmix::hash(key));
    done(std::move(recs));
}

void applyToQueue(uint64_t gen, std::vector<Track> recs) {
    auto* p = ctx().player;
    if (!p || p->queueGeneration() != gen || mode() != Mode::Smart || p->currentOrderIndex() < 0) return;
    smartmix::Exclusions queued;
    for (const auto& t : p->items()) queued.add(t);
    std::erase_if(recs, [&](const Track& t) { return queued.contains(t); });
    std::vector<bool> isRec;
    isRec.reserve(p->order().size());
    for (int item : p->order()) isRec.push_back(p->items()[item].recommended);
    const int pos = p->currentOrderIndex();
    const auto points = smartmix::insertionPoints(isRec, pos, smartmix::hash(p->context().uri, gen * 1000003ull + pos));
    const size_t n = std::min(points.size(), recs.size());
    g_applying = true;
    for (size_t k = n; k-- > 0;) p->insertAt(points[k], {recs[k]});   // from the last: earlier indices stay valid
    g_applying = false;
    g_planned = {gen, p->currentOrderIndex(), p->order().size()};
    if (n > 0) ST_LOG_DEBUG("smartshuffle", "{} recommendation(s) mixed into the queue", n);
}

void syncQueue() {
    auto* p = ctx().player;
    if (!p || mode() != Mode::Smart) return;
    const Track* cur = p->current();
    if (!cur || notASong(*cur)) return;
    const uint64_t gen = p->queueGeneration();
    if (g_planned.gen == gen && g_planned.pos == p->currentOrderIndex() && g_planned.size == p->order().size()) return;
    std::vector<Track> own;
    for (const auto& t : p->items())
        if (!t.recommended && !notASong(t)) own.push_back(t);
    if (own.size() < 2) return;   // a lone song: nothing to fit around
    std::string key = contextKey(p->context().uri);
    if (key.empty()) key = "queue:" + p->context().uri;
    recommend(key, own, g_life.ref(), [gen](std::vector<Track> recs) { applyToQueue(gen, std::move(recs)); });
}

void scheduleSync() {
    if (g_syncPosted || g_applying) return;
    g_syncPosted = true;
    Dispatcher::post([] {
        g_syncPosted = false;
        syncQueue();
    });
}

} // namespace

// The player hook (initSmartShuffle): plan once the current event is over.
void playerChanged() { scheduleSync(); }

Mode mode() {
    auto* p = ctx().player;
    const bool on = p ? p->shuffle() : Settings::get().shuffle;
    if (!on) return Mode::Off;
    return Settings::get().smartShuffle ? Mode::Smart : Mode::Shuffle;
}

void setMode(Mode m) {
    const Mode before = mode();
    if (m == before) return;
    auto* p = ctx().player;
    if (before == Mode::Smart && p) p->dropRecommendations();
    auto& s = Settings::get();
    s.smartShuffle = m == Mode::Smart;
    s.markDirty();
    g_planned = {};
    if (p) {
        const bool shuffleChanges = p->shuffle() != (m != Mode::Off);
        p->setShuffle(m != Mode::Off);
        if (!shuffleChanges && p->onChanged) p->onChanged();   // shuffle stays on: the buttons still show the mode
    }
    if (m == Mode::Smart) scheduleSync();
}

void cycleMode() {
    const Mode m = mode();
    const Mode next = m == Mode::Off ? Mode::Shuffle : m == Mode::Shuffle ? Mode::Smart : Mode::Off;
    setMode(next);
    if (next == Mode::Smart) toast(tr(L"Akıllı karıştırma açık: listene uyan önerilen şarkılar araya karışır"));
}

std::wstring modeLabel(Mode m) {
    switch (m) {
    case Mode::Smart: return tr(L"Akıllı karıştırma");
    case Mode::Shuffle: return tr(L"Karıştır");
    default: return tr(L"Karıştır");
    }
}

bool enhanced(const std::string& key) {
    const auto& v = Settings::get().enhancedCollections;
    return !key.empty() && std::find(v.begin(), v.end(), key) != v.end();
}

void setEnhanced(const std::string& key, bool on) {
    if (key.empty() || enhanced(key) == on) return;
    auto& v = Settings::get().enhancedCollections;
    if (on) v.push_back(key);
    else std::erase(v, key);
    Settings::get().markDirty();
    notifyListeners();
}

void recommend(const std::string& key, const std::vector<Track>& collection, Lifetime::Ref guard,
               std::function<void(std::vector<Track>)> done) {
    auto& pool = g_pools[key];
    const int64_t now = steadyMs();
    const bool fresh = pool.fetchedAt > 0 && now - pool.fetchedAt < kPoolTtlMs;
    if (fresh && !pool.loading) {
        deliver(key, collection, guard, done);
        return;
    }
    pool.waiters.push_back({guard, [key, collection, guard, done] { deliver(key, collection, guard, done); }});
    if (pool.loading) return;
    pool.loading = true;
    const Seeds seeds = seedsOf(collection, smartmix::hash(key));
    spotify::Api* api = now < g_backoffUntil ? nullptr : source::activeApi();
    async(Priority::Low, g_life.ref(), [api, seeds, stop = g_life.ref()] { return fetchPool(api, seeds, stop); },
          [key, names = seeds.artistNames](Result<Fetched> r) {
              auto& pool = g_pools[key];
              pool.loading = false;
              std::vector<Track> tracks;
              if (r) {
                  tracks = std::move(r->tracks);
                  if (r->rateLimited) g_backoffUntil = steadyMs() + kBackoffMs;
              }
              for (auto& t : localByArtists(names)) tracks.push_back(std::move(t));
              ST_LOG_INFO("smartshuffle", "{} recommendation candidate(s) for {}", tracks.size(), key);
              pool.tracks = std::move(tracks);
              pool.fetchedAt = steadyMs();
              const auto waiters = std::move(pool.waiters);
              pool.waiters.clear();
              for (const auto& [ref, fn] : waiters)
                  if (!ref.expired()) fn();
              prunePools();
          });
}

void hide(const std::string& key, const Track& t) {
    g_hidden[key].add(t);
    notifyListeners();
}

bool isHidden(const std::string& key, const Track& t) {
    const auto it = g_hidden.find(key);
    return it != g_hidden.end() && it->second.contains(t);
}

std::string queueCollectionKey() {
    auto* p = ctx().player;
    return p ? contextKey(p->context().uri) : std::string();
}

bool canAdd(const std::string& key, const Track& t) {
    const std::string k = key.empty() ? queueCollectionKey() : key;
    if (k.empty() || notASong(t)) return false;
    if (k == "liked") return ctx().library.canLike(t.id) && !ctx().library.isLiked(t.id);
    if (source::loggedIn() && ctx().session) {
        const auto* pl = ctx().session->findPlaylist(k);
        return pl && pl->editable && t.id.rfind("spotify:track:", 0) == 0;
    }
    return ctx().library.playlist(k) != nullptr;
}

void add(const std::string& key, const Track& t) {
    const std::string k = key.empty() ? queueCollectionKey() : key;
    if (!canAdd(k, t)) return;
    Track own = t;
    own.recommended = false;
    if (k == "liked") {
        ctx().library.setLiked(own, true);
        toast(tr(L"Beğenilen Şarkılar'a eklendi"));
    } else {
        addToPlaylistWithToast(k, {own});
    }
    if (auto* p = ctx().player) p->clearRecommended(t.id);
    g_hidden[k].add(t);   // in the collection now: never offered again (a Spotify list reloads a moment later)
    notifyListeners();
}

void stop(const std::string& key) {
    const std::string queueKey = queueCollectionKey();
    const std::string k = key.empty() ? queueKey : key;
    if (!k.empty() && enhanced(k)) setEnhanced(k, false);
    if (key.empty() || k == queueKey) {
        if (mode() == Mode::Smart) setMode(Mode::Shuffle);
        else if (auto* p = ctx().player) p->dropRecommendations();
    }
    toast(tr(L"Öneriler kapatıldı"));
}

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { g_listeners.emplace_back(std::move(owner), std::move(fn)); }

namespace {
gfx::Text& badgeText() {
    static auto* text = new gfx::Text(toUpperTr(tr(L"Önerilen")), gfx::type::monoBadge);   // never freed: static exit
    return *text;
}
} // namespace

float badgeWidth() { return std::ceil(badgeText().measure().w) + 16 + 6; }

float drawBadge(gfx::Canvas& c, float x, float cy) {
    const auto& acc = gfx::accent();
    const float w = badgeWidth();
    const gfx::Rect r{x, std::round(cy - 7), w, 14};
    c.fillRounded(r, 2, acc.tint12);
    c.icon("sparkle", {r.x + 5, r.cy() - 4, 8, 8}, acc.base);
    c.text(badgeText(), {r.x + 16, r.y, w - 16, r.h}, acc.base, gfx::VAlign::Center);
    return w;
}

} // namespace st::app::smartshuffle

namespace st::app {

void initSmartShuffle() {
    ctx().playerChangedHooks.push_back([] { smartshuffle::playerChanged(); });
}

void buildSmartShuffleRows(ui::Column* c, const std::function<void()>&) {
    using smartshuffle::Mode;
    settingsToggle(c, tr(L"Akıllı karıştırma"),
                   tr(L"Karıştırma açıkken çalan listeye uyan önerilen şarkılar her 3-4 şarkıda bir araya karışır ve "
                      L"\"Önerilen\" işaretiyle görünür. Karıştır düğmesine ikinci kez basarak da açılır. Öneriler "
                      L"Spotify radyosundan (bağlıyken), ListenBrainz'in benzer sanatçılarından ve yerel dosyalarından gelir."),
                   smartshuffle::mode() == Mode::Smart, [](bool v) {
                       const Mode m = smartshuffle::mode();
                       smartshuffle::setMode(v ? Mode::Smart : (m == Mode::Off ? Mode::Off : Mode::Shuffle));
                   });
}

} // namespace st::app
