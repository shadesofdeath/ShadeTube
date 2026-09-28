// Spotify radio, endless playback and the kara liste's player wiring (see Radio.h).
#include "app/Radio.h"

#include "app/AppContext.h"
#include "app/Blacklist.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Window.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>

namespace st::app {

using catalog::Track;

namespace {

constexpr int kRadioTracks = 100;           // first page of the radio playlist (Spotify builds ~50)
constexpr size_t kEndlessSeedsPerQueue = 8; // radio fetches per queue before endless playback gives up
constexpr size_t kPlaylistSeeds = 2;        // tracks of a playlist tried as the seed of its radio

struct RadioFetch {
    std::string uri;                // the radio playlist ("" = Spotify has no radio for the seeds)
    std::vector<Track> tracks;
    std::vector<Track> candidates;  // a playlist without a radio: its first tracks (the UI thread picks seeds)
    bool failed = false;
    bool cancelled = false;         // the guard expired between two requests (a newer request / app exit)
    int status = 0;                 // ApiError status (0 = network / other)
    std::string error;
};

// Worker thread. The radio playlist of `seed`, "" when Spotify has none (empty answer or 404).
std::string radioOf(spotify::Api* api, const std::string& seed) {
    try {
        return api->radioPlaylist(seed);
    } catch (const spotify::ApiError& e) {
        if (e.status == 404) return {};
        throw;
    }
}

// Worker thread: the first of `seeds` that has a radio -> that radio's first page. Spotify serves no radio for
// playlists (verified: user and generated playlists answer with nothing), so a lone playlist seed without one returns
// its first tracks as `candidates` instead: the UI thread picks unblocked ones and asks again. `guard` is checked
// before every request, so a superseded fetch or one outliving the app (and its Api) stops at the next call.
RadioFetch fetchRadio(spotify::Api* api, const std::vector<std::string>& seeds, const Lifetime::Ref& guard) {
    RadioFetch f;
    try {
        for (const auto& seed : seeds) {
            if (guard.expired()) {
                f.cancelled = true;
                return f;
            }
            f.uri = radioOf(api, seed);
            if (!f.uri.empty()) break;
            if (seeds.size() == 1 && seed.rfind("spotify:playlist:", 0) == 0) {
                if (guard.expired()) {
                    f.cancelled = true;
                    return f;
                }
                f.candidates = api->playlistTracks(seed, 0, 20).items;
                return f;
            }
        }
        if (!f.uri.empty()) {
            if (guard.expired()) {
                f.cancelled = true;
                return f;
            }
            f.tracks = api->playlistTracks(f.uri, 0, kRadioTracks).items;
        }
    } catch (const spotify::ApiError& e) {
        f.failed = true;
        f.status = e.status;
        f.error = e.what();
    } catch (const std::exception& e) {
        f.failed = true;
        f.error = e.what();
    }
    return f;
}

std::wstring failureText(const RadioFetch& f) {
    if (f.status == 429) return tr(L"Spotify şu an çok fazla istek alıyor. Biraz sonra tekrar dene.");
    if (f.status == 401 || f.status == 403) return tr(L"Spotify oturumu doğrulanamadı. Biraz sonra tekrar dene.");
    return tr(L"Radyo yüklenemedi. Bağlantını kontrol edip tekrar dene.");
}

// What the player may use from a radio: playable, not blocked, not in `seen` (track ids; updated).
std::vector<Track> usableTracks(std::vector<Track> tracks, std::unordered_set<std::string>& seen) {
    std::vector<Track> out;
    for (auto& t : tracks) {
        if (!t.playable || t.name.empty() || blacklist::isBlocked(t)) continue;
        if (!t.id.empty() && !seen.insert(t.id).second) continue;
        out.push_back(std::move(t));
    }
    return out;
}

bool isTrackSeed(const Track& t) {
    return t.playable && t.id.rfind("spotify:track:", 0) == 0 && !radioSeed(t.id).empty();
}

Lifetime g_radioLife;   // startRadio: a newer request supersedes the one in flight

// One "Şarkı radyosu" / "Radyo başlat" request, checked again when its result arrives.
struct RadioRequest {
    std::string seed;
    std::wstring name;
    uint64_t generation = 0;   // Player::queueGeneration() when asked: another queue started meanwhile wins
    bool seedPlaying = false;  // the seed is the song that was playing: the radio follows it instead of restarting
};

void runRadio(RadioRequest req, std::vector<std::string> seeds, bool firstStep);

void onRadio(const RadioRequest& req, Result<RadioFetch> r, bool firstStep) {
    auto* p = ctx().player;
    if (!p || !r || r->cancelled) return;
    if (p->queueGeneration() != req.generation || !radioAvailable()) {
        ST_LOG_INFO("radio", "{}: result dropped (another queue started / logged out)", req.seed);
        return;
    }
    if (r->failed) {
        ST_LOG_WARN("radio", "{} failed: {}", req.seed, r->error);
        toast(failureText(*r), true);
        return;
    }
    if (r->uri.empty()) {
        // A playlist without its own radio: the radio of its first unblocked tracks.
        std::vector<std::string> seeds;
        if (firstStep)
            for (const auto& t : r->candidates)
                if (seeds.size() < kPlaylistSeeds && isTrackSeed(t) && !blacklist::isBlocked(t)) seeds.push_back(t.id);
        if (!seeds.empty()) {
            runRadio(req, std::move(seeds), false);
            return;
        }
        toast(tr(L"Spotify bunun için radyo sunmuyor"), true);
        return;
    }
    std::unordered_set<std::string> seen;
    auto tracks = usableTracks(std::move(r->tracks), seen);
    if (tracks.empty()) {
        toast(tr(L"Radyoda çalınabilecek şarkı yok"), true);
        return;
    }
    ST_LOG_INFO("radio", "{} -> {} ({} tracks)", req.seed, r->uri, tracks.size());
    const player::PlayContext context{"playlist:" + r->uri, i18n::format(tr(L"{} radyosu"), {req.name})};
    // The radio of the song that was playing: whatever plays now (that song, or the one after it if it ended
    // meanwhile) keeps playing and the radio becomes what comes next.
    if (req.seedPlaying && p->current() && p->status() != player::Status::Idle && p->status() != player::Status::Error) {
        std::erase_if(tracks, [&](const Track& t) { return t.id == req.seed; });
        p->replaceUpcoming(std::move(tracks), context);
        toast(i18n::format(tr(L"Radyo hazır: sıradaki şarkılar {} radyosundan"), {req.name}));
        return;
    }
    p->playContext(std::move(tracks), 0, context);
}

void runRadio(RadioRequest req, std::vector<std::string> seeds, bool firstStep) {
    spotify::Api* api = source::activeApi();
    if (!api) return;
    async(Priority::High, g_radioLife.ref(),
          [api, seeds = std::move(seeds), guard = g_radioLife.ref()] { return fetchRadio(api, seeds, guard); },
          [req = std::move(req), firstStep](Result<RadioFetch> r) { onRadio(req, std::move(r), firstStep); });
}

// Endless playback, per queue (Player::queueGeneration).
struct Endless {
    Lifetime life;                         // renewed with the queue: a fetch for an old queue is dropped
    uint64_t generation = 0;
    bool inFlight = false;
    std::unordered_set<std::string> tried; // seeds already fetched for this queue
};
Endless& endless() {
    static Endless e;
    return e;
}

} // namespace

std::string radioSeed(const std::string& id) {
    for (const char* kind : {"spotify:track:", "spotify:album:", "spotify:artist:", "spotify:playlist:"}) {
        const size_t n = std::strlen(kind);
        if (id.size() <= n || id.compare(0, n, kind) != 0) continue;
        const std::string_view rest(id.data() + n, id.size() - n);
        const bool base62 = rest.size() <= 64 && std::all_of(rest.begin(), rest.end(), [](char c) {
                                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                            });
        return base62 ? id : std::string{};
    }
    return {};
}

bool radioAvailable() { return ctx().session && source::loggedIn(); }

void startRadio(const std::string& seedUri, const std::wstring& seedName) {
    const std::string seed = radioSeed(seedUri);
    if (seed.empty()) {
        toast(tr(L"Bunun için Spotify radyosu yok"), true);
        return;
    }
    auto* p = ctx().player;
    if (!radioAvailable() || !p) {
        toast(tr(L"Radyo için Spotify bağlantısı gerekir"), true);
        return;
    }
    RadioRequest req;
    req.seed = seed;
    req.name = seedName.empty() ? std::wstring(L"Spotify") : seedName;
    req.generation = p->queueGeneration();
    const Track* cur = p->current();
    req.seedPlaying = cur && cur->id == seed && p->status() != player::Status::Idle && p->status() != player::Status::Error;
    toast(tr(L"Radyo hazırlanıyor…"));
    ST_LOG_INFO("radio", "start: {}", seed);
    g_radioLife.renew();
    runRadio(std::move(req), {seed}, true);
}

void endlessCheck(bool fromPlayer) {
    auto* p = ctx().player;
    if (!p || !Settings::get().endlessPlayback || !radioAvailable()) return;
    if (p->repeat() != RepeatMode::Off || !p->current()) return;
    // A queue that already ended only resumes through the player itself (its queue-low hook), never because a
    // setting or the kara liste changed later.
    if (!fromPlayer && p->status() == player::Status::Idle) return;
    auto& e = endless();
    if (p->queueGeneration() != e.generation) {
        e.life.renew();
        e.generation = p->queueGeneration();
        e.inFlight = false;
        e.tried.clear();
    }
    if (e.inFlight || p->remainingPlayable(2) > 1 || e.tried.size() >= kEndlessSeedsPerQueue) return;
    // Seed: the current track, else the most recent Spotify track of the queue. Blocked tracks never seed.
    std::string seed;
    auto consider = [&](const Track& t) {
        const std::string s = t.id.rfind("spotify:track:", 0) == 0 ? radioSeed(t.id) : std::string{};
        if (s.empty() || e.tried.contains(s) || blacklist::isBlocked(t)) return false;
        seed = s;
        return true;
    };
    if (!consider(*p->current())) {
        const auto& items = p->items();
        const auto& order = p->order();
        for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i)
            if (consider(items[order[i]])) break;
    }
    if (seed.empty()) return;   // not a Spotify queue, or every seed was tried
    spotify::Api* api = source::activeApi();
    if (!api) return;
    e.tried.insert(seed);
    e.inFlight = true;
    const uint64_t gen = e.generation;
    ST_LOG_INFO("radio", "endless: queue low, fetching the radio of {}", seed);
    async(Priority::Low, e.life.ref(),
          [api, seed, guard = e.life.ref()] { return fetchRadio(api, {seed}, guard); },
          [seed, gen, fromPlayer](Result<RadioFetch> r) {
              auto& en = endless();
              en.inFlight = false;
              auto* pl = ctx().player;
              if (!pl || !r || r->cancelled || pl->queueGeneration() != gen) return;
              if (r->failed) {   // no retry here: the next track start may try another seed
                  ST_LOG_WARN("radio", "endless: {} failed: {}", seed, r->error);
                  return;
              }
              std::unordered_set<std::string> seen;
              for (const auto& t : pl->items())
                  if (!t.id.empty()) seen.insert(t.id);
              auto tracks = usableTracks(std::move(r->tracks), seen);
              if (tracks.empty()) {
                  ST_LOG_INFO("radio", "endless: nothing new from {}", seed);
                  endlessCheck(fromPlayer);   // another seed, bounded by `tried`
                  return;
              }
              // The setting, repeat or the login may have changed while the radio loaded.
              if (!Settings::get().endlessPlayback || pl->repeat() != RepeatMode::Off || !radioAvailable()) {
                  ST_LOG_INFO("radio", "endless: dropped the radio of {} (setting / repeat / login changed)", seed);
                  en.tried.erase(seed);   // turning it back on may fetch it again
                  return;
              }
              const int n = pl->extend(tracks);
              ST_LOG_INFO("radio", "endless: appended {} tracks (radio of {})", n, seed);
              if (n > 0) toast(i18n::plural(L"Sonsuz çalma: {} benzer şarkı sıraya eklendi", n));
          });
}

void initPlaybackFeatures() {
    blacklist::load();
    if (auto* p = ctx().player) {
        p->shouldSkip = [](const Track& t) { return blacklist::isBlocked(t); };
        p->onQueueLow = [] { endlessCheck(true); };
    }
    // A block / unblock: the player re-picks its prepared next track, lists repaint their badges, and blocking the
    // rest of the queue may leave it low.
    blacklist::onChanged([] {
        if (auto* p = ctx().player) p->skipRulesChanged();
        if (auto* w = ctx().window) w->invalidate();
        endlessCheck();
    });
    // App exit (App::persist runs before the player and the Spotify session are destroyed): radio / endless work that
    // is still queued is skipped and a fetch between two requests stops before touching the Api again.
    ctx().persistHooks.push_back([] {
        g_radioLife.renew();
        endless().life.renew();
        endless().inFlight = false;
    });
}

} // namespace st::app
