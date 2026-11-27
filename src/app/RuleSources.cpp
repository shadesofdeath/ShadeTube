// The rule lists' cached collections (see RuleSources.h).
#include "app/RuleSources.h"

#include "app/Accounts.h"
#include "app/AppContext.h"
#include "app/CatalogJson.h"
#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace st::app::rulesrc {

using json = nlohmann::json;
using catalog::Track;

namespace {

constexpr int64_t kGapMs = 1'500;                  // between two collections
constexpr int64_t kLikedMinSec = 5 * 60;           // Liked Songs: at most this often for a changed count
constexpr int64_t kChangedMinSec = 60;             // a playlist whose count changed
constexpr int64_t kOldSec = 6 * 3600;              // playlists / Liked Songs fetched again after this
constexpr int64_t kAlbumOldSec = 7 * 86'400;
constexpr int64_t kFailWaitSec = 15 * 60;
constexpr int64_t kRateLimitWaitSec = 10 * 60;

struct State {
    bool wired = false, loaded = false, loading = false;
    uint32_t wanted = 0;
    std::vector<Collection> items;   // the index (every collection fetched for this account)
    std::unordered_map<std::string, int64_t> failedAt, editedAt;
    int64_t pausedUntil = 0;         // unix seconds (Spotify's 429)
    int64_t nextAt = 0;              // steadyMs()
    bool fetching = false;
    std::string fetchingId;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    bool notifyPosted = false;
    Lifetime life;                   // fetches and file work of the active account
    Lifetime subs;
};

State& state() {
    static State s;
    return s;
}

std::filesystem::path indexFile() { return accounts::file(L"rule-sources.json"); }
std::filesystem::path listDir() { return accounts::dataDir() / L"rule-sources"; }

std::string fileNameFor(const std::string& id) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : id) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx.json", static_cast<unsigned long long>(h));
    return buf;
}

void notify() {
    auto& s = state();
    if (s.notifyPosted) return;
    s.notifyPosted = true;
    Dispatcher::post([] {
        auto& st = state();
        st.notifyPosted = false;
        std::erase_if(st.listeners, [](const auto& l) { return l.first.expired(); });
        const auto ls = st.listeners;
        for (const auto& [owner, fn] : ls)
            if (!owner.expired() && fn) fn();
    });
}

json indexJson() {
    json items = json::array();
    for (const auto& c : state().items)
        items.push_back({{"id", c.id},
                         {"kind", rules::sourceName(c.kind)},
                         {"name", c.name},
                         {"year", c.year},
                         {"file", toUtf8(c.file.filename().wstring())},
                         {"at", c.fetchedAt},
                         {"count", c.count},
                         {"expected", c.expected}});
    return {{"v", 1}, {"user", accounts::currentUser()}, {"items", std::move(items)}};
}

bool writeFile(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void saveIndex() {
    auto text = std::make_shared<std::string>(indexJson().dump(-1, ' ', false, json::error_handler_t::replace));
    async(Priority::Low, state().life.ref(), [text, file = indexFile()] { writeFile(file, *text); }, [](Result<Unit>) {});
}

void load() {
    auto& s = state();
    s.loading = true;
    async(
        Priority::Low, s.life.ref(),
        [file = indexFile(), dir = listDir()] {
            std::vector<Collection> out;
            std::ifstream f(file, std::ios::binary);
            if (!f) return out;
            std::ostringstream ss;
            ss << f.rdbuf();
            const json j = json::parse(ss.str(), nullptr, false);
            if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) return out;
            for (const auto& x : j["items"]) {
                if (!x.is_object() || !x.contains("id") || !x["id"].is_string()) continue;
                Collection c;
                c.id = x["id"].get<std::string>();
                const std::string kind = x.contains("kind") && x["kind"].is_string() ? x["kind"].get<std::string>() : "";
                c.kind = kind == "liked" ? rules::Liked : kind == "albums" ? rules::Albums : rules::Playlists;
                if (x.contains("name") && x["name"].is_string()) c.name = x["name"].get<std::string>();
                if (x.contains("year") && x["year"].is_number_integer()) c.year = x["year"].get<int>();
                if (x.contains("file") && x["file"].is_string()) c.file = dir / toWide(x["file"].get<std::string>());
                if (x.contains("at") && x["at"].is_number_integer()) c.fetchedAt = x["at"].get<int64_t>();
                if (x.contains("count") && x["count"].is_number_integer()) c.count = x["count"].get<int>();
                if (x.contains("expected") && x["expected"].is_number_integer()) c.expected = x["expected"].get<int>();
                std::error_code ec;
                if (c.file.empty() || !std::filesystem::exists(c.file, ec)) continue;   // its list is gone: fetched again
                out.push_back(std::move(c));
            }
            return out;
        },
        [](Result<std::vector<Collection>> r) {
            auto& st = state();
            st.loading = false;
            st.loaded = true;
            if (r) st.items = std::move(*r);
            ST_LOG_INFO("rules", "{} cached collection(s)", st.items.size());
            notify();
        });
}

Collection* find(const std::string& id) {
    for (auto& c : state().items)
        if (c.id == id) return &c;
    return nullptr;
}

// What should be cached now: {id, kind, name, the library's count (-1 = unknown)}.
struct Want {
    std::string id;
    rules::Source kind;
    std::string name;
    int count = -1;
    int year = 0;
};

std::vector<Want> wantedNow() {
    auto& s = state();
    std::vector<Want> out;
    if (source::loggedIn() && ctx().session) {
        const auto& snap = ctx().session->library();
        if (!snap.loaded) return out;
        if (s.wanted & rules::Liked) out.push_back({"liked", rules::Liked, "Liked Songs", static_cast<int>(snap.likedIds.size())});
        if (s.wanted & rules::Playlists)
            for (const auto& p : snap.playlists)
                if (p.id != spotify::Api::kLikedSongsUri && p.id.rfind("spotify:playlist:", 0) == 0)
                    out.push_back({p.id, rules::Playlists, p.name, p.countKnown ? p.totalTracks : -1});
        if (s.wanted & rules::Albums)
            for (const auto& a : snap.albums) out.push_back({a.id, rules::Albums, a.name, a.totalTracks > 0 ? a.totalTracks : -1});
    } else if (s.wanted & rules::Albums) {
        for (const auto& a : ctx().library.albums()) out.push_back({a.id, rules::Albums, a.name, -1});
    }
    return out;
}

bool due(const Want& w, int64_t now) {
    auto& s = state();
    if (const auto f = s.failedAt.find(w.id); f != s.failedAt.end() && now - f->second < kFailWaitSec) return false;
    const Collection* c = find(w.id);
    if (!c) return true;
    const int64_t age = now - c->fetchedAt;
    if (const auto e = s.editedAt.find(w.id); e != s.editedAt.end() && e->second > c->fetchedAt) return true;
    if (w.kind == rules::Albums) return age >= kAlbumOldSec;
    if (age >= kOldSec) return true;
    if (w.count >= 0 && w.count != c->expected) return age >= (w.kind == rules::Liked ? kLikedMinSec : kChangedMinSec);
    return false;
}

struct Fetched {
    std::string id;
    std::string name;
    int year = 0;
    std::vector<Track> tracks;
};

void trim(Track& t) {
    t.uid.clear();
    if (t.album.images.size() > 1) {
        const catalog::Image* best = catalog::pickImage(t.album.images, 300);
        catalog::Image keep = best ? *best : t.album.images.front();
        t.album.images = {std::move(keep)};
    }
}

// Worker: every page of a Spotify list (300 ms apart), or an album.
Fetched fetch(spotify::Api* api, const Want& w) {
    Fetched f;
    f.id = w.id;
    f.name = w.name;
    if (w.kind == rules::Albums) {
        catalog::Album a = source::album(api, w.id);
        f.name = a.name;
        f.year = std::atoi(a.year().c_str());
        for (auto& t : a.tracks) {
            if (t.album.id.empty() && t.album.name.empty()) t.album = {a.id, a.name, a.images};
            if (t.album.images.empty()) t.album.images = a.images;
            f.tracks.push_back(std::move(t));
        }
    } else {
        if (!api) throw spotify::ApiError(401, "not logged in");
        const std::string uri = w.kind == rules::Liked ? std::string(spotify::Api::kLikedSongsUri) : w.id;
        int offset = 0;
        for (int page = 0; page < 200; ++page) {   // at most 20 000 rows
            if (page) std::this_thread::sleep_for(std::chrono::milliseconds(300));
            auto p = api->playlistTracks(uri, offset, 100);
            const int next = p.nextOffset >= 0 ? p.nextOffset : offset + static_cast<int>(p.items.size());
            for (auto& t : p.items) f.tracks.push_back(std::move(t));
            if (next <= offset || next >= p.total) break;
            offset = next;
        }
    }
    for (auto& t : f.tracks) trim(t);
    std::erase_if(f.tracks, [](const Track& t) { return t.id.empty() || t.id.rfind("spotify:episode:", 0) == 0; });
    return f;
}

void fetchNext() {
    auto& s = state();
    if (!s.loaded || s.fetching || s.wanted == 0) return;
    const int64_t now = nowUnix();
    const auto wants = wantedNow();
    // Collections no longer in the library: dropped (their files too).
    std::vector<std::filesystem::path> gone;
    std::erase_if(s.items, [&](const Collection& c) {
        const bool spotifyItem = c.id == "liked" || c.id.rfind("spotify:", 0) == 0;
        if (spotifyItem != source::loggedIn()) return false;   // another mode's: kept
        if (c.kind == rules::Albums && !(s.wanted & rules::Albums)) return false;
        if (c.kind == rules::Playlists && !(s.wanted & rules::Playlists)) return false;
        if (c.kind == rules::Liked && !(s.wanted & rules::Liked)) return false;
        if (source::loggedIn() && !ctx().session->library().loaded) return false;
        const bool still = std::any_of(wants.begin(), wants.end(), [&](const Want& w) { return w.id == c.id; });
        if (!still) gone.push_back(c.file);
        return !still;
    });
    if (!gone.empty()) {
        saveIndex();
        async(Priority::Low, s.life.ref(), [gone] {
            for (const auto& f : gone) {
                std::error_code ec;
                std::filesystem::remove(f, ec);
            }
        }, [](Result<Unit>) {});
        notify();
    }
    const Want* next = nullptr;
    for (const auto& w : wants) {
        const bool spotify = w.kind != rules::Albums || w.id.rfind("spotify:", 0) == 0;
        if (spotify && now < s.pausedUntil) continue;
        if (due(w, now)) {
            next = &w;
            break;
        }
    }
    if (!next) return;
    s.fetching = true;
    s.fetchingId = next->id;
    const Want w = *next;
    spotify::Api* api = source::activeApi();
    const std::string user = accounts::currentUser();
    const auto dir = listDir();
    async(
        Priority::Low, s.life.ref(),
        [api, w, dir] {
            Fetched f = fetch(api, w);
            json a = json::array();
            for (const auto& t : f.tracks) a.push_back(toJson(t));
            const auto file = dir / toWide(fileNameFor(w.id));
            if (!writeFile(file, a.dump(-1, ' ', false, json::error_handler_t::replace))) throw std::runtime_error("cannot write the list");
            return std::make_pair(std::move(f), file);
        },
        [w, user](Result<std::pair<Fetched, std::filesystem::path>> r) {
            auto& st = state();
            st.fetching = false;
            st.fetchingId.clear();
            st.nextAt = steadyMs() + kGapMs;
            if (accounts::currentUser() != user) return;   // fetched for the account that left
            if (!r) {
                st.failedAt[w.id] = nowUnix();
                try {
                    std::rethrow_exception(r.error());
                } catch (const spotify::ApiError& e) {
                    if (e.status == 429) st.pausedUntil = nowUnix() + std::max<int64_t>(kRateLimitWaitSec, e.retryAfterSec);
                    ST_LOG_WARN("rules", "fetching {} failed: {}", w.id, e.what());
                } catch (const std::exception& e) {
                    ST_LOG_WARN("rules", "fetching {} failed: {}", w.id, e.what());
                } catch (...) {
                }
                return;
            }
            auto& [f, file] = *r;
            Collection c;
            c.id = w.id;
            c.kind = w.kind;
            c.name = f.name.empty() ? w.name : f.name;
            c.year = f.year;
            c.file = file;
            c.fetchedAt = nowUnix();
            c.count = static_cast<int>(f.tracks.size());
            c.expected = w.count;
            if (Collection* old = find(w.id)) *old = std::move(c);
            else st.items.push_back(std::move(c));
            st.failedAt.erase(w.id);
            ST_LOG_INFO("rules", "cached {} ({} tracks)", w.id, f.tracks.size());
            saveIndex();
            notify();
        });
}

void tick() {
    auto& s = state();
    if (s.wanted == 0) return;
    if (!s.loaded) {
        if (!s.loading) load();
        return;
    }
    if (s.fetching || steadyMs() < s.nextAt) return;
    fetchNext();
}

} // namespace

void init() {
    auto& s = state();
    if (s.wired) return;
    s.wired = true;
    ctx().housekeepingHooks.push_back([] { tick(); });
    ctx().accountLeaveHooks.push_back([] {
        auto& st = state();
        st.life.renew();   // a fetch for the account that is leaving
        st.fetching = st.loading = false;
        st.items.clear();
        st.loaded = false;
        st.failedAt.clear();
        st.editedAt.clear();
        notify();
    });
    ctx().accountEnterHooks.push_back([] { notify(); });
    if (ctx().session)
        ctx().session->subscribePlaylistEdits(s.subs.ref(), [](const spotify::PlaylistEdit& e) {
            state().editedAt[e.uri] = nowUnix();
            state().nextAt = 0;
        });
}

void setWanted(uint32_t sources) {
    auto& s = state();
    sources &= rules::Liked | rules::Playlists | rules::Albums;
    if (sources == s.wanted) return;
    const bool more = (sources & ~s.wanted) != 0;
    s.wanted = sources;
    if (more) s.nextAt = 0;
    tick();
}

std::vector<Collection> collections() {
    auto& s = state();
    std::vector<Collection> out;
    const bool in = source::loggedIn();
    for (const auto& c : s.items) {
        const bool spotifyItem = c.id == "liked" || c.id.rfind("spotify:", 0) == 0;
        if (spotifyItem != in) continue;
        if (!(s.wanted & c.kind)) continue;
        out.push_back(c);
    }
    return out;
}

bool busy() { return state().fetching || state().loading; }

std::vector<Track> readTracks(const std::filesystem::path& file) {
    std::vector<Track> out;
    std::ifstream f(file, std::ios::binary);
    if (!f) return out;
    std::ostringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& t : j)
        if (t.is_object()) out.push_back(trackFromJson(t));
    return out;
}

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { state().listeners.emplace_back(std::move(owner), std::move(fn)); }

} // namespace st::app::rulesrc
