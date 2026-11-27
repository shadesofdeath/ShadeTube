// Kurallı listeler in the app (see RuleListsPage.h): the store, the engine, the list page and the editor.
#include "app/RuleListsPage.h"

#include "app/Accounts.h"
#include "app/AppContext.h"
#include "app/Bpm.h"
#include "app/CollapsingHeader.h"
#include "app/ListenStats.h"
#include "app/LocalLibrary.h"
#include "app/PageTransition.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PlaylistTransfer.h"
#include "app/RuleSources.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using catalog::Image;
using catalog::Track;
using gfx::accent;
using gfx::colors;
using Microsoft::WRL::ComPtr;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;
using json = nlohmann::json;
using namespace rules;

namespace rulelists {

namespace {

constexpr int kFileVersion = 1;
constexpr int64_t kHistoryEveryMs = 15'000;   // the listening history is aggregated at most this often
constexpr int64_t kSaveAfterMs = 10'000;
constexpr size_t kPreviewSongs = 50;

// ---- Engine (job side) --------------------------------------------------------------------------------------------

struct Songs {
    std::vector<Track> tracks;   // (the editor's preview: only the first kPreviewSongs)
    size_t matched = 0;          // before the limit
    size_t count = 0;            // in the list (after the limit)
    int64_t totalMs = 0;         // of the list
};

struct Engine {
    Pool pool;
    struct Memo {
        uint64_t defHash = 0;
        std::array<uint64_t, 6> revs{};
        int64_t day = -1;
    };
    std::unordered_map<std::string, Memo> memo;
    std::vector<std::wstring> historyKeys;     // per ListenStats track index
    std::vector<PlayStats> historyStats;
};

struct FileSlice {
    std::string sliceId;
    Source source = Playlists;
    std::string name;
    int year = 0;
    std::filesystem::path file;
};

struct HistoryIn {
    std::shared_ptr<const std::vector<ListenStats::Track>> tracks;   // null: the same tracks as before
    std::vector<PlayStats> stats;                                    // per track index
    bool slice = false;                                              // the history is a source of some list
};

struct Job {
    std::shared_ptr<Engine> eng;
    std::vector<Slice> slices;
    std::vector<FileSlice> files;
    std::vector<std::string> removed;
    bool localSet = false;
    std::shared_ptr<const std::vector<local::Entry>> local;
    std::optional<HistoryIn> history;
    std::optional<std::unordered_set<std::string>> liked, downloaded;
    std::shared_ptr<const bpm::Store> bpm;
    int64_t now = 0, day = 0;
    std::vector<std::pair<std::string, Definition>> defs;
    std::unordered_set<std::string> full, forceFull, resetMemo;
    std::optional<Definition> draft;
    uint64_t draftToken = 0;
};

struct Summary {
    std::string id;
    size_t count = 0;
    int64_t totalMs = 0;
    std::vector<Image> covers;
};

struct JobOut {
    std::vector<Summary> summaries;
    std::vector<std::pair<std::string, Songs>> fulls;
    std::optional<Songs> draft;
    uint64_t draftToken = 0;
    double ms = 0;
    size_t songs = 0, bytes = 0, evaluated = 0;
};

uint64_t fnv(uint64_t h, std::string_view s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
template <class T>
uint64_t fnvPod(uint64_t h, const T& v) {
    return fnv(h, std::string_view(reinterpret_cast<const char*>(&v), sizeof v));
}
constexpr uint64_t kFnv = 1469598103934665603ull;

std::vector<Image> coversOf(const Pool& pool, const std::vector<Song>& songs, const std::vector<uint32_t>& order) {
    std::vector<Image> out;
    std::unordered_set<std::string> seen;
    for (uint32_t i : order) {
        const auto& imgs = pool.track(songs[i]).album.images;
        if (imgs.empty() || imgs.front().url.empty() || !seen.insert(imgs.front().url).second) continue;
        out.push_back(imgs.front());
        if (out.size() == 4) break;
    }
    return out;
}

JobOut runJob(Job j) {
    const auto t0 = std::chrono::steady_clock::now();
    JobOut out;
    Engine& e = *j.eng;
    Pool& pool = e.pool;
    for (const auto& id : j.removed) pool.removeSlice(id);
    for (auto& s : j.slices) pool.setSlice(prepare(std::move(s)));
    for (auto& f : j.files) {
        Slice s;
        s.id = f.sliceId;
        s.source = f.source;
        s.name = f.name;
        for (auto& t : rulesrc::readTracks(f.file)) s.items.push_back({std::move(t), f.year});
        pool.setSlice(prepare(std::move(s)));
    }
    if (j.localSet) {
        if (!j.local) {
            pool.removeSlice("local");
        } else {
            Slice s;
            s.id = "local";
            s.source = LocalFiles;
            s.items.reserve(j.local->size());
            for (const auto& en : *j.local) s.items.push_back({local::toTrack(en), en.year});
            pool.setSlice(prepare(std::move(s)));
        }
    }
    if (j.history) {
        auto& h = *j.history;
        if (h.tracks) {
            Slice s;
            s.id = "history";
            s.source = History;
            s.items.reserve(h.tracks->size());
            for (const auto& t : *h.tracks) s.items.push_back({ListenStats::toCatalog(t), 0});
            PreparedSlice p = prepare(std::move(s));
            e.historyKeys.clear();
            e.historyKeys.reserve(p.items.size());
            for (const auto& it : p.items) e.historyKeys.push_back(it.key);
            if (h.slice) pool.setSlice(std::move(p));
            else pool.removeSlice("history");
            e.historyStats.clear();   // all of them again below
        } else if (!h.slice) {
            pool.removeSlice("history");
        }
        // Statistics: only what changed when the tracks stayed (a play stored), else all of them.
        if (e.historyStats.size() == h.stats.size() && !h.tracks) {
            std::vector<std::pair<std::wstring, PlayStats>> changed;
            for (size_t i = 0; i < h.stats.size() && i < e.historyKeys.size(); ++i)
                if (!(h.stats[i] == e.historyStats[i])) changed.emplace_back(e.historyKeys[i], h.stats[i]);
            if (changed.size() < 2000) {
                pool.updateStats(changed);
            } else {
                std::unordered_map<std::wstring, PlayStats> all;
                for (size_t i = 0; i < h.stats.size() && i < e.historyKeys.size(); ++i)
                    if (h.stats[i].plays || h.stats[i].skips) all[e.historyKeys[i]] = h.stats[i];
                pool.setStats(std::move(all));
            }
        } else {
            std::unordered_map<std::wstring, PlayStats> all;
            all.reserve(h.stats.size());
            for (size_t i = 0; i < h.stats.size() && i < e.historyKeys.size(); ++i)
                if (h.stats[i].plays || h.stats[i].skips) all[e.historyKeys[i]] = h.stats[i];
            pool.setStats(std::move(all));
        }
        e.historyStats = std::move(h.stats);
    }
    if (j.liked) pool.setLiked(std::move(*j.liked));
    if (j.downloaded) pool.setDownloaded(std::move(*j.downloaded));
    if (j.bpm) {
        auto store = j.bpm;
        pool.setBpm([store](const Track& t, const std::wstring& key) { return store->of(t.id, t.videoId, key); });
    }
    const auto& songs = pool.songs();
    std::array<uint64_t, 6> revs{};
    for (size_t i = 0; i < revs.size(); ++i) revs[i] = pool.revision(static_cast<Dep>(1u << i));
    const Context base{j.now, j.day, {}};
    auto materialize = [&](const Result& r, size_t limit) {
        Songs s;
        s.matched = r.matched;
        s.count = r.songs.size();
        s.totalMs = r.totalMs;
        s.tracks.reserve(std::min(limit, r.songs.size()));
        for (size_t k = 0; k < r.songs.size() && k < limit; ++k) s.tracks.push_back(pool.rowTrack(songs[r.songs[k]]));
        return s;
    };
    for (const auto& id : j.resetMemo) e.memo.erase(id);
    for (const auto& [id, def] : j.defs) {
        const uint64_t h = fnv(kFnv, toJson(def).dump());
        const uint32_t deps = depsOf(def);
        auto& m = e.memo[id];
        bool needs = m.defHash != h || ((deps & DepDay) && m.day != j.day);
        for (size_t i = 0; i < revs.size() && !needs; ++i)
            if ((deps & (1u << i)) && m.revs[i] != revs[i]) needs = true;
        const bool force = j.forceFull.contains(id);
        if (!needs && !force) continue;
        Context c = base;
        c.listId = id;
        const Result r = evaluate(def, pool, c);
        ++out.evaluated;
        if (needs) {
            out.summaries.push_back({id, r.songs.size(), r.totalMs, coversOf(pool, songs, r.songs)});
            m = {h, revs, j.day};
        }
        if (j.full.contains(id)) out.fulls.emplace_back(id, materialize(r, SIZE_MAX));
    }
    for (auto it = e.memo.begin(); it != e.memo.end();)   // lists that are gone
        it = std::any_of(j.defs.begin(), j.defs.end(), [&](const auto& d) { return d.first == it->first; }) ? std::next(it) : e.memo.erase(it);
    if (j.draft) {
        Context c = base;
        c.listId = "draft";
        out.draft = materialize(evaluate(*j.draft, pool, c), kPreviewSongs);
        out.draftToken = j.draftToken;
        ++out.evaluated;
    }
    out.songs = songs.size();
    out.bytes = pool.memoryBytes();
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

// ---- State (UI thread) ----------------------------------------------------------------------------------------------

struct Watch {
    Lifetime::Ref owner;
    std::string id;
    std::function<void(const Songs&)> fn;
};

struct Draft {
    Lifetime::Ref owner;
    Definition def;
    std::function<void(const Songs&)> fn;
    uint64_t token = 0;
    bool sent = false;
};

struct State {
    bool wired = false, loaded = false;
    std::vector<List> lists;
    bool dirty = false;
    int64_t dirtySince = 0;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    bool notifyPosted = false, kickPosted = false;
    // Engine
    std::shared_ptr<Engine> engine;
    bool running = false, again = false;
    std::unordered_map<std::string, uint64_t> sent;   // slice id -> fingerprint the engine holds
    uint64_t sentLocalGen = ~0ull, sentLikedFp = 0, sentDownloadedFp = 0, sentBpmRev = ~0ull, sentHistoryFp = 0;
    bool sentHistorySlice = false;
    size_t sentHistoryTracks = SIZE_MAX;
    int64_t historyAt = 0;
    int64_t day = -1;
    LocalClock clock;
    std::vector<Watch> watchers;
    std::unordered_map<std::string, Songs> lastSongs;
    std::unordered_set<std::string> forceFull, resetMemo;
    std::optional<Draft> draft;
    uint64_t draftTokens = 0;
    Lifetime life;    // engine jobs, file work
    Lifetime subs;
};

State& state() {
    static State s;
    return s;
}

std::filesystem::path listsFile() { return accounts::file(L"rule-lists.json"); }

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

json listsJson() {
    json a = json::array();
    for (const auto& l : state().lists) {
        json covers = json::array();
        for (const auto& c : l.covers) covers.push_back(c.url);
        a.push_back({{"id", l.id},
                     {"created", l.created},
                     {"updated", l.updated},
                     {"def", toJson(l.def)},
                     {"last", {{"count", l.count}, {"ms", l.totalMs}, {"covers", covers}, {"done", l.evaluated}}}});
    }
    return {{"v", kFileVersion}, {"lists", a}};
}

void saveNow() {
    auto& s = state();
    s.dirty = false;
    auto text = std::make_shared<std::string>(listsJson().dump(-1, ' ', false, json::error_handler_t::replace));
    async(Priority::Low, s.life.ref(), [text, file = listsFile()] {
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << *text;
            f.flush();
            if (!f) return;
        }
        MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    }, [](st::Result<Unit>) {});
}

void markDirty() {
    auto& s = state();
    if (!s.dirty) s.dirtySince = steadyMs();
    s.dirty = true;
}

std::vector<List> parseLists(const json& j) {
    std::vector<List> out;
    if (!j.is_object() || !j.contains("lists") || !j["lists"].is_array()) return out;
    for (const auto& x : j["lists"]) {
        if (!x.is_object() || !x.contains("id") || !x["id"].is_string() || !x.contains("def")) continue;
        auto def = fromJson(x["def"]);
        if (!def) continue;
        List l;
        l.id = x["id"].get<std::string>();
        l.def = std::move(*def);
        if (x.contains("created") && x["created"].is_number_integer()) l.created = x["created"].get<int64_t>();
        if (x.contains("updated") && x["updated"].is_number_integer()) l.updated = x["updated"].get<int64_t>();
        if (const auto last = x.find("last"); last != x.end() && last->is_object()) {
            if (last->contains("count") && (*last)["count"].is_number_unsigned()) l.count = (*last)["count"].get<size_t>();
            if (last->contains("ms") && (*last)["ms"].is_number_integer()) l.totalMs = (*last)["ms"].get<int64_t>();
            if (last->contains("covers") && (*last)["covers"].is_array())
                for (const auto& c : (*last)["covers"])
                    if (c.is_string()) l.covers.push_back({c.get<std::string>(), 0, 0});
            if (last->contains("done") && (*last)["done"].is_boolean()) l.evaluated = (*last)["done"].get<bool>();
        }
        out.push_back(std::move(l));
    }
    return out;
}

void kick();

void load() {
    auto& s = state();
    async(
        Priority::Normal, s.life.ref(),
        [file = listsFile()] {
            std::ifstream f(file, std::ios::binary);
            if (!f) return std::vector<List>{};
            std::ostringstream ss;
            ss << f.rdbuf();
            return parseLists(json::parse(ss.str(), nullptr, false));
        },
        [](st::Result<std::vector<List>> r) {
            auto& st = state();
            st.loaded = true;
            if (r) st.lists = std::move(*r);
            ST_LOG_INFO("rules", "{} rule list(s)", st.lists.size());
            notify();
            kick();
        });
}

bool live(const Lifetime::Ref& r) { return !r.expired(); }

bool active() {
    auto& s = state();
    std::erase_if(s.watchers, [](const Watch& w) { return !live(w.owner); });
    if (s.draft && !live(s.draft->owner)) s.draft.reset();
    return !s.lists.empty() || s.draft || !s.watchers.empty();
}

int64_t today() {
    auto& s = state();
    if (!s.clock) s.clock = systemLocalClock();
    return listen::dayNumber(s.clock(nowUnix()));
}

uint64_t tracksFp(const std::vector<Track>& tracks, std::string_view name = {}) {
    uint64_t h = fnv(kFnv, name);
    for (const auto& t : tracks) {
        h = fnv(h, t.id);
        h = fnv(h, t.name);
        h = fnvPod(h, t.addedAt);
    }
    return fnvPod(h, tracks.size());
}

uint64_t idsFp(const std::unordered_set<std::string>& ids) {
    uint64_t x = 0;
    for (const auto& id : ids) x ^= fnv(kFnv, id) * 0x9E3779B97F4A7C15ull;
    return fnvPod(x, ids.size());
}

std::vector<PlayStats> aggregate(const ListenStats& st) {
    const auto& tracks = st.tracks();
    std::vector<PlayStats> out(tracks.size());
    for (const auto& p : st.plays()) {
        if (p.track >= out.size()) continue;
        PlayStats& a = out[p.track];
        if (listen::counts(p.listenedMs, tracks[p.track].durationMs)) {
            ++a.plays;
            if (!a.first || p.startedAt < a.first) a.first = p.startedAt;
            if (p.startedAt > a.last) a.last = p.startedAt;
        } else {
            ++a.skips;
        }
    }
    return out;
}

void release() {
    auto& s = state();
    if (s.running) return;
    if (s.engine) ST_LOG_INFO("rules", "no rule list in use: the pool is released");
    s.engine.reset();
    s.sent.clear();
    s.sentLocalGen = ~0ull;
    s.sentLikedFp = s.sentDownloadedFp = s.sentHistoryFp = 0;
    s.sentBpmRev = ~0ull;
    s.sentHistoryTracks = SIZE_MAX;
    s.sentHistorySlice = false;
    s.lastSongs.clear();
    rulesrc::setWanted(0);
}

void onDone(JobOut out);

void startJob() {
    auto& s = state();
    if (!s.loaded) return;
    if (!active()) {
        release();
        return;
    }
    if (s.running) {
        s.again = true;
        return;
    }
    Job j;
    if (!s.engine) s.engine = std::make_shared<Engine>();
    j.eng = s.engine;
    uint32_t src = 0, deps = 0;
    for (const auto& l : s.lists) {
        src |= l.def.sources;
        deps |= depsOf(l.def);
    }
    if (s.draft) {   // the editor's draft: its sources, and every fact a rule may read while it is edited
        src |= s.draft->def.sources;
        deps |= depsOf(s.draft->def) | DepPlays | DepBpm;
    }
    rulesrc::setWanted(src);
    // Inputs still loading would give lists that change a moment later (counts jumping in the sidebar): wait for them
    // (their notifications kick again).
    if (((deps & DepPlays) || (src & History)) && !listenStats().loaded()) return;
    if ((src & LocalFiles) && !LocalLibrary::get().loaded()) return;
    if ((deps & DepBpm) && !bpm::loaded()) return;
    if (source::loggedIn() && ctx().session && !ctx().session->library().loaded) return;
    const bool in = source::loggedIn() && ctx().session;
    std::unordered_set<std::string> desired;
    // A local collection: copied into a slice only when its fingerprint changed.
    auto offer = [&](const std::string& id, Source kind, const std::string& name, const std::vector<Track>& tracks) {
        desired.insert(id);
        const uint64_t fp = tracksFp(tracks, name);
        auto& cur = s.sent[id];
        if (cur == fp) return;
        cur = fp;
        Slice sl{id, kind, name, {}};
        sl.items.reserve(tracks.size());
        for (const auto& t : tracks) sl.items.push_back({t, 0});
        j.slices.push_back(std::move(sl));
    };
    auto& lib = ctx().library;
    if ((src & Liked) && !in) offer("liked", Liked, {}, lib.liked());
    if ((src & Playlists) && !in)
        for (const auto& p : lib.playlists())
            if (const auto* tracks = lib.playlistTracks(p.id)) offer("playlist:" + p.id, Playlists, p.name, *tracks);
    for (const auto& c : rulesrc::collections()) {
        const std::string id = c.kind == Liked ? std::string("liked") : c.kind == Playlists ? "playlist:" + c.id : "album:" + c.id;
        desired.insert(id);
        const uint64_t fp = fnvPod(fnvPod(fnv(kFnv, c.name), c.fetchedAt), c.count);
        auto& cur = s.sent[id];
        if (cur == fp) continue;
        cur = fp;
        j.files.push_back({id, c.kind, c.name, c.year, c.file});
    }
    if (src & LocalFiles) {
        desired.insert("local");
        auto& ll = LocalLibrary::get();
        if (ll.generation() != s.sentLocalGen) {
            s.sentLocalGen = ll.generation();
            j.localSet = true;
            j.local = ll.sharedEntries();
            s.sent["local"] = 1;
        }
    }
    // Downloads: fingerprinted first (the manager notifies on every bit of progress); copied only when they changed.
    {
        uint64_t fp = kFnv;
        size_t done = 0;
        for (const auto& d : ctx().downloads.items())
            if (d.state == DlState::Done) {
                fp = fnvPod(fnv(fp, d.track.id), d.addedAt);
                ++done;
            }
        fp = fnvPod(fp, done);
        if (src & Downloads) desired.insert("downloads");
        if (fp != s.sentDownloadedFp || ((src & Downloads) && !s.sent.contains("downloads"))) {
            s.sentDownloadedFp = fp;
            std::unordered_set<std::string> ids;
            Slice sl{"downloads", Downloads, {}, {}};
            for (const auto& d : ctx().downloads.items())
                if (d.state == DlState::Done) {
                    ids.insert(d.track.id);
                    if (src & Downloads) {
                        Track t = d.track;
                        t.addedAt = d.addedAt;
                        sl.items.push_back({std::move(t), 0});
                    }
                }
            j.downloaded = std::move(ids);
            if (src & Downloads) {
                s.sent["downloads"] = fp;
                j.slices.push_back(std::move(sl));
            }
        }
    }
    // The listening history: its songs (a source) and the play statistics (facts for every song).
    const auto& stats = listenStats();
    const bool wantSlice = (src & History) != 0;
    const bool wantStats = (deps & DepPlays) != 0 || wantSlice;
    if (wantSlice) desired.insert("history");
    if ((wantStats || wantSlice) && stats.loaded()) {
        const uint64_t fp = fnvPod(fnvPod(fnvPod(kFnv, stats.tracks().size()), stats.playCount()),
                                   stats.plays().empty() ? int64_t{0} : stats.plays().back().startedAt);
        const int64_t now = steadyMs();
        if ((fp != s.sentHistoryFp && (s.sentHistoryFp == 0 || now - s.historyAt >= kHistoryEveryMs)) || wantSlice != s.sentHistorySlice) {
            HistoryIn h;
            if (stats.tracks().size() != s.sentHistoryTracks || wantSlice != s.sentHistorySlice)
                h.tracks = std::make_shared<const std::vector<ListenStats::Track>>(stats.tracks());
            h.stats = aggregate(stats);
            h.slice = wantSlice;
            j.history = std::move(h);
            s.sentHistoryFp = fp;
            s.sentHistoryTracks = stats.tracks().size();
            s.sentHistorySlice = wantSlice;
            s.historyAt = now;
        } else if (fp != s.sentHistoryFp) {
            s.again = true;   // due later (kHistoryEveryMs): the housekeeping tick kicks again
        }
    }
    // Liked ids (fingerprinted in place, copied only when they changed), BPMs.
    {
        uint64_t fp = 0;
        if (in) {
            fp = idsFp(ctx().session->library().likedIds);
        } else {
            for (const auto& t : lib.liked()) fp ^= fnv(kFnv, t.id) * 0x9E3779B97F4A7C15ull;
            fp = fnvPod(fp, lib.liked().size());
        }
        if (fp != s.sentLikedFp) {
            s.sentLikedFp = fp;
            std::unordered_set<std::string> liked;
            if (in) liked = ctx().session->library().likedIds;
            else
                for (const auto& t : lib.liked()) liked.insert(t.id);
            j.liked = std::move(liked);
        }
    }
    if (bpm::revision() != s.sentBpmRev) {
        s.sentBpmRev = bpm::revision();
        j.bpm = bpm::snapshot();
    }
    for (auto it = s.sent.begin(); it != s.sent.end();) {
        if (!desired.contains(it->first)) {
            j.removed.push_back(it->first);
            if (it->first == "local") s.sentLocalGen = ~0ull;
            it = s.sent.erase(it);
        } else {
            ++it;
        }
    }
    j.now = nowUnix();
    j.day = today();
    s.day = j.day;
    for (const auto& l : s.lists) j.defs.emplace_back(l.id, l.def);
    for (const auto& w : s.watchers) j.full.insert(w.id);
    j.forceFull = std::move(s.forceFull);
    s.forceFull.clear();
    j.resetMemo = std::move(s.resetMemo);
    s.resetMemo.clear();
    if (s.draft && !s.draft->sent) {
        j.draft = s.draft->def;
        j.draftToken = s.draft->token;
        s.draft->sent = true;
    }
    s.running = true;
    async(Priority::Low, s.life.ref(), [j = std::move(j)]() mutable { return runJob(std::move(j)); },
          [](st::Result<JobOut> r) {
              auto& st = state();
              st.running = false;
              if (!r) {
                  ST_LOG_WARN("rules", "evaluation failed: {}", r.errorMessage());
                  return;
              }
              onDone(std::move(*r));
          });
}

void kick() {
    auto& s = state();
    if (s.kickPosted) return;
    s.kickPosted = true;
    Dispatcher::post([] {
        state().kickPosted = false;
        startJob();
    });
}

void onDone(JobOut out) {
    auto& s = state();
    if (out.evaluated > 0)
        ST_LOG_INFO("rules", "{} list(s) evaluated in {:.1f} ms ({} songs in the pool, ~{:.1f} MB)", out.evaluated, out.ms,
                    out.songs, out.bytes / 1048576.0);
    bool changed = false;
    const bool provisional = rulesrc::busy();   // a collection is still coming: the counts may grow
    for (auto& sum : out.summaries) {
        for (auto& l : s.lists) {
            if (l.id != sum.id) continue;
            if (provisional) s.resetMemo.insert(l.id);   // evaluated again (and kept) once the cache settles
            if (l.count != sum.count || l.totalMs != sum.totalMs || l.covers.size() != sum.covers.size() ||
                !std::equal(l.covers.begin(), l.covers.end(), sum.covers.begin(), [](const Image& a, const Image& b) { return a.url == b.url; }) ||
                !l.evaluated) {
                l.count = sum.count;
                l.totalMs = sum.totalMs;
                l.covers = std::move(sum.covers);
                l.evaluated = true;
                changed = true;
            }
        }
    }
    if (changed) {
        markDirty();
        notify();
    }
    for (auto& [id, songs] : out.fulls) {
        s.lastSongs[id] = songs;
        const auto ws = s.watchers;
        for (const auto& w : ws)
            if (w.id == id && live(w.owner) && w.fn) w.fn(songs);
    }
    if (out.draft && s.draft && s.draft->token == out.draftToken && live(s.draft->owner) && s.draft->fn) {
        const auto fn = s.draft->fn;
        fn(*out.draft);
    }
    if (s.again || (s.draft && !s.draft->sent) || !s.forceFull.empty()) {
        s.again = false;
        kick();
    }
}

// Watching a list's songs (its page): `fn` now when they are known, and whenever they change.
void watch(Lifetime::Ref owner, const std::string& id, std::function<void(const Songs&)> fn) {
    auto& s = state();
    s.watchers.push_back({owner, id, fn});
    if (const auto it = s.lastSongs.find(id); it != s.lastSongs.end() && s.engine) fn(it->second);
    else s.forceFull.insert(id);
    kick();
}

void preview(Lifetime::Ref owner, Definition def, std::function<void(const Songs&)> fn) {
    auto& s = state();
    s.draft = Draft{owner, std::move(def), std::move(fn), ++s.draftTokens, false};
    kick();
}

void endPreview() {
    state().draft.reset();
    kick();
}

void tick() {
    auto& s = state();
    if (!s.loaded) return;
    if (s.dirty && steadyMs() - s.dirtySince >= kSaveAfterMs) saveNow();
    if (!active()) {
        if (s.engine) release();
        return;
    }
    if (s.day >= 0 && today() != s.day) kick();   // "in the last N days", random orders
    else if (s.again && !s.running) kick();
    // Watched lists nobody shows any more: their songs go.
    for (auto it = s.lastSongs.begin(); it != s.lastSongs.end();)
        it = std::any_of(s.watchers.begin(), s.watchers.end(), [&](const Watch& w) { return w.id == it->first; }) ? std::next(it)
                                                                                                           : s.lastSongs.erase(it);
}

std::string newId() {
    std::random_device rd;
    char buf[16];
    std::snprintf(buf, sizeof buf, "r%08x", static_cast<unsigned>(rd()));
    return buf;
}

// ---- Labels -----------------------------------------------------------------------------------------------------------

std::wstring fieldLabel(Field f) {
    switch (f) {
    case Field::Title: return tr(L"Başlık");
    case Field::Artist: return tr(L"Sanatçı");
    case Field::Album: return tr(L"Albüm");
    case Field::Year: return tr(L"Çıkış yılı");
    case Field::Plays: return tr(L"Dinlenme sayısı");
    case Field::Skips: return tr(L"Atlanma sayısı");
    case Field::Duration: return tr(L"Süre");
    case Field::Bpm: return L"BPM";
    case Field::Added: return tr(L"Eklenme tarihi");
    case Field::LikedAt: return tr(L"Beğenilme tarihi");
    case Field::LastPlayed: return tr(L"Son dinlenme");
    case Field::FirstPlayed: return tr(L"İlk dinlenme");
    case Field::Liked: return tr(L"Beğenildi");
    case Field::Downloaded: return tr(L"İndirildi");
    case Field::LocalFile: return tr(L"Yerel dosya");
    case Field::Explicit: return tr(L"Açık içerik");
    case Field::Playlist: return tr(L"Çalma listesi");
    case Field::Catalog: return tr(L"Kaynak");
    default: return {};
    }
}

std::wstring opLabel(Field f, Op o) {
    switch (o) {
    case Op::Contains: return tr(L"içerir");
    case Op::NotContains: return tr(L"içermez");
    case Op::Is: return f == Field::Artist ? tr(L"şudur") : tr(L"eşittir");
    case Op::IsNot: return f == Field::Artist ? tr(L"şu değildir") : tr(L"eşit değildir");
    case Op::StartsWith: return tr(L"ile başlar");
    case Op::Equals: return tr(L"eşittir");
    case Op::NotEquals: return tr(L"eşit değildir");
    case Op::Less: return tr(L"küçüktür");
    case Op::Greater: return tr(L"büyüktür");
    case Op::Between: return tr(L"arasında");
    case Op::Known: return tr(L"biliniyor");
    case Op::Unknown: return tr(L"bilinmiyor");
    case Op::InLast: return tr(L"son N gün içinde");
    case Op::NotInLast: return tr(L"son N gün içinde değil");
    case Op::Before: return tr(L"şu tarihten önce");
    case Op::After: return tr(L"şu tarihten sonra");
    case Op::Never: return tr(L"hiç");
    case Op::Yes: return tr(L"evet");
    case Op::No: return tr(L"hayır");
    case Op::In: return f == Field::Playlist ? tr(L"içinde") : tr(L"şudur");
    case Op::NotIn: return f == Field::Playlist ? tr(L"içinde değil") : tr(L"şu değildir");
    default: return {};
    }
}

std::wstring sortLabel(SortKey k) {
    switch (k) {
    case SortKey::Title: return tr(L"Başlık");
    case SortKey::Artist: return tr(L"Sanatçı");
    case SortKey::Album: return tr(L"Albüm");
    case SortKey::Year: return tr(L"Çıkış yılı");
    case SortKey::Added: return tr(L"Eklenme tarihi");
    case SortKey::LikedAt: return tr(L"Beğenilme tarihi");
    case SortKey::Plays: return tr(L"Dinlenme sayısı");
    case SortKey::Skips: return tr(L"Atlanma sayısı");
    case SortKey::LastPlayed: return tr(L"Son dinlenme");
    case SortKey::FirstPlayed: return tr(L"İlk dinlenme");
    case SortKey::Duration: return tr(L"Süre");
    case SortKey::Bpm: return L"BPM";
    case SortKey::Random: return tr(L"Rastgele (her gün yeni)");
    default: return {};
    }
}

std::wstring sourceLabel(Source s) {
    switch (s) {
    case Liked: return tr(L"Beğenilenler");
    case Playlists: return tr(L"Çalma listeleri");
    case Albums: return tr(L"Kayıtlı albümler");
    case LocalFiles: return tr(L"Yerel dosyalar");
    case Downloads: return tr(L"İndirilenler");
    default: return tr(L"Dinleme geçmişi");
    }
}

std::wstring catalogLabel(Catalog c) {
    switch (c) {
    case Catalog::Spotify: return L"Spotify";
    case Catalog::YouTube: return L"YouTube Music";
    case Catalog::MusicBrainz: return L"MusicBrainz";
    case Catalog::Local: return tr(L"Yerel dosya");
    default: return tr(L"Diğer (içe aktarılan)");
    }
}

size_t ruleCount(const Definition& d) {
    size_t n = 0;
    for (const auto& g : d.groups) n += g.rules.size();
    return n;
}

// The list page's line: how it is made.
std::wstring describe(const Definition& d) {
    std::vector<std::wstring> parts;
    const size_t n = ruleCount(d);
    if (n == 0) parts.push_back(tr(L"Kaynaklarındaki tüm şarkılar"));
    else if (n == 1) parts.push_back(tr(L"1 kurala uyan şarkılar"));
    else
        parts.push_back(d.any ? i18n::plural(L"{} kuraldan birine uyan şarkılar", static_cast<int64_t>(n))
                              : i18n::plural(L"{} kuralın hepsine uyan şarkılar", static_cast<int64_t>(n)));
    parts.push_back(i18n::format(tr(L"sıra: {}"), {sortLabel(d.sort)}));
    if (d.limit == Limit::Songs) parts.push_back(i18n::plural(L"en fazla {} şarkı", d.limitValue));
    else if (d.limit == Limit::Minutes) parts.push_back(i18n::plural(L"en fazla {} dakika", d.limitValue));
    std::wstring s;
    for (const auto& p : parts) s += (s.empty() ? L"" : L" · ") + p;
    return s + L". " + tr(L"Kaynakları değiştikçe kendini günceller.");
}

void showFileToast(const std::wstring& message, const std::filesystem::path& file) {
    ui::Toasts::show(ctx().window, message, ui::ToastKind::Success, tr(L"Dosyayı göster"), [file] {
        const std::wstring arg = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    });
}

std::wstring safeFileName(std::wstring s) {
    for (auto& ch : s)
        if (wcschr(L"<>:\"/\\|?*", ch) || ch < 32) ch = L'_';
    if (s.empty()) s = tr(L"Kurallı liste");
    return s.substr(0, 120);
}

void exportDefinition(const std::string& id) {
    Dispatcher::post([id] {
        const List* l = find(id);
        if (!l) return;
        const Definition def = l->def;
        ComPtr<IFileSaveDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        const std::wstring kind = tr(L"ShadeTube kurallı liste (JSON)");
        const COMDLG_FILTERSPEC types[] = {{kind.c_str(), L"*.json"}};
        dlg->SetFileTypes(1, types);
        dlg->SetDefaultExtension(L"json");
        dlg->SetFileName(safeFileName(toWide(def.name)).c_str());
        dlg->SetTitle(tr(L"Kuralları dışa aktar"));
        dlg->SetOkButtonLabel(tr(L"Dışa aktar"));
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        ComPtr<IShellItem> docs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&docs)))) dlg->SetDefaultFolder(docs.Get());
        if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        const std::filesystem::path file = path;
        CoTaskMemFree(path);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << toJson(def).dump(2, ' ', false, json::error_handler_t::replace);
        f.flush();
        if (!f) {
            toast(tr(L"Kurallar dışa aktarılamadı"), true);
            return;
        }
        showFileToast(i18n::format(tr(L"\"{}\" kuralları dışa aktarıldı"), {toWide(def.name)}), file);
    });
}

// A definition waiting for the editor (a new list, an import, a copy): the editor page takes it.
std::optional<Definition> g_pendingDraft;

void openEditorFor(Definition d) {
    g_pendingDraft = std::move(d);
    ctx().router->navigate({RouteKind::SmartList, "rule-new"});
}

Definition starter() {
    Definition d;
    d.name = toUtf8(tr(L"Yeni kurallı liste"));
    Group g;
    g.rules.push_back({Field::Artist, Op::Contains, {}, 0, 0});
    d.groups.push_back(std::move(g));
    d.sort = SortKey::Added;
    d.descending = true;
    return d;
}

void importDefinition() {
    Dispatcher::post([] {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        const std::wstring kind = tr(L"ShadeTube kurallı liste (JSON)"), all = tr(L"Tüm dosyalar");
        const COMDLG_FILTERSPEC types[] = {{kind.c_str(), L"*.json"}, {all.c_str(), L"*.*"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(tr(L"Kurallı liste dosyası seç"));
        if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        const std::filesystem::path file = path;
        CoTaskMemFree(path);
        std::ifstream f(file, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string error;
        int dropped = 0;
        auto d = ss.str().size() > (4 << 20) ? std::nullopt : fromJson(json::parse(ss.str(), nullptr, false), &error, &dropped);
        if (!d) {
            toast(tr(L"Bu dosya bir ShadeTube kurallı listesi değil"), true);
            return;
        }
        if (dropped > 0) toast(i18n::plural(L"Bu ShadeTube sürümünün tanımadığı {} kural atlandı", dropped));
        if (d->name.empty()) d->name = toUtf8(file.stem().wstring());
        openEditorFor(std::move(*d));
    });
}

void confirmDelete(const std::string& id) {
    const List* l = find(id);
    if (!l) return;
    ui::Dialog::confirm(
        ctx().window, tr(L"Kurallı liste silinsin mi?"),
        i18n::format(tr(L"\"{}\" ve kuralları silinir. Şarkılara dokunulmaz."), {toWide(l->def.name)}), tr(L"Sil"),
        [id] {
            auto& s = state();
            std::erase_if(s.lists, [&](const List& x) { return x.id == id; });
            markDirty();
            saveNow();
            notify();
            kick();
            const auto& cur = ctx().router->current();
            if (cur.kind == RouteKind::SmartList && (cur.id.rfind("rule:" + id, 0) == 0 || cur.id == "rule-edit:" + id))
                ctx().router->navigate({RouteKind::Library}, true);
            toast(tr(L"Kurallı liste silindi"));
        },
        true);
}

} // namespace

// ---- Public -----------------------------------------------------------------------------------------------------------

void init() {
    auto& s = state();
    if (s.wired) return;
    s.wired = true;
    rulesrc::init();
    load();
    auto& subs = s.subs;
    ctx().library.subscribe(subs.ref(), [] { kick(); });
    if (ctx().session) ctx().session->subscribe(subs.ref(), [] { kick(); });
    LocalLibrary::get().subscribe(subs.ref(), [] {
        if (LocalLibrary::get().generation() != state().sentLocalGen) kick();
    });
    ctx().downloads.subscribe(subs.ref(), [] { kick(); });
    listenStats().subscribe(subs.ref(), [] { kick(); });
    bpm::subscribe(subs.ref(), [] { kick(); });
    rulesrc::subscribe(subs.ref(), [] { kick(); });
    ctx().housekeepingHooks.push_back([] { tick(); });
    ctx().persistHooks.push_back([] {
        if (state().dirty) {
            const std::string text = listsJson().dump(-1, ' ', false, json::error_handler_t::replace);
            std::ofstream f(listsFile(), std::ios::binary | std::ios::trunc);
            f << text;
            state().dirty = false;
        }
    });
    ctx().accountLeaveHooks.push_back([] {
        auto& st = state();
        if (st.dirty) {
            const std::string text = listsJson().dump(-1, ' ', false, json::error_handler_t::replace);
            std::ofstream f(listsFile(), std::ios::binary | std::ios::trunc);
            f << text;
            st.dirty = false;
        }
        st.life.renew();
        st.running = false;
        st.lists.clear();
        st.loaded = false;
        release();
        notify();
    });
    ctx().accountEnterHooks.push_back([] { load(); });
}

const std::vector<List>& lists() { return state().lists; }

const List* find(const std::string& id) {
    for (const auto& l : state().lists)
        if (l.id == id) return &l;
    return nullptr;
}

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { state().listeners.emplace_back(std::move(owner), std::move(fn)); }

std::wstring subtitle(const List& l) {
    std::wstring s = tr(L"Kurallı liste");
    if (l.evaluated) s += L" · " + i18n::plural(L"{} şarkı", static_cast<int64_t>(l.count));
    return s;
}

void drawCover(gfx::Canvas& c, const List& l, const gfx::Rect& r, float radius) {
    const auto& col = colors();
    const Color tone = tileFill(tileColor(l.id));
    const float band = std::round(r.h * 0.3f);
    const gfx::Rect art{r.x, r.y, r.w, r.h - band};
    c.pushRoundedClip(r, radius);
    c.fillRect(r, tone);
    if (l.covers.size() >= 4) {
        const float w = art.w / 2, h = art.h / 2;
        for (size_t i = 0; i < 4; ++i)
            drawArtwork(c, {l.covers[i]}, {art.x + (i % 2) * w, art.y + (i / 2) * h, w, h}, 0, Placeholder::Album, false, Priority::Low);
    } else if (!l.covers.empty()) {
        drawArtwork(c, {l.covers[0]}, art, 0, Placeholder::Album, false, Priority::Low);
    } else {
        const float s = std::round(art.h * 0.34f);
        c.icon("filter", art.center(s, s), col.fgSecondary);
    }
    const gfx::Rect b{r.x, r.bottom() - band, r.w, band};
    c.fillRect(b, tone);
    c.fillRect({b.x, b.y, b.w, std::max(2.f, std::round(r.w / 80))}, accent().base);
    const float pad = std::round(r.w * 0.07f);
    const float labelSize = std::clamp(r.w * 0.05f, 8.f, 11.f);
    const float titleSize = std::clamp(r.w * 0.11f, 13.f, 26.f);
    c.text(toUpperTr(tr(L"Kurallı")), type::monoLabel.withSize(labelSize), {b.x + pad, b.y + band * 0.18f, b.w - pad * 2, labelSize * 1.5f},
           col.fgSecondary);
    c.text(toWide(l.def.name), type::title.withSize(titleSize), {b.x + pad, b.y + band * 0.18f + labelSize * 1.5f, b.w - pad * 2, titleSize * 1.3f},
           col.fgPrimary);
    c.popLayer();
}

std::vector<ui::MenuItem> menuItems(const std::string& id) {
    const Route open{RouteKind::SmartList, "rule:" + id};
    ui::MenuItem del{tr(L"Sil"), "trash", L"", [id] { confirmDelete(id); }};
    del.destructive = true;
    return {{tr(L"Aç"), "arrow-up-right", L"", [open] { ctx().router->navigate(open); }},
            {tr(L"Çal"), "play", L"", [id] { ctx().router->navigate({RouteKind::SmartList, "rule:" + id + "#play"}); }},
            {tr(L"Kuralları düzenle"), "sliders", L"", [id] { ctx().router->navigate({RouteKind::SmartList, "rule-edit:" + id}); }},
            {tr(L"Kopyasını oluştur"), "plus", L"", [id] {
                 if (const List* l = find(id)) {
                     Definition d = l->def;
                     d.name = toUtf8(i18n::format(tr(L"{} (kopya)"), {toWide(d.name)}));
                     openEditorFor(std::move(d));
                 }
             }},
            {tr(L"Kuralları dışa aktar…"), "share", L"", [id] { exportDefinition(id); }},
            ui::MenuItem::sep(),
            std::move(del)};
}

void showMenu(const std::string& id, gfx::Point windowPos) { ui::Menu::open(ctx().window, windowPos, menuItems(id)); }

std::vector<ui::MenuItem> createMenuItems() {
    return {{tr(L"Yeni kurallı liste…"), "filter", L"", [] { openEditorFor(starter()); }},
            {tr(L"Kurallı liste içe aktar…"), "folder", L"", [] { importDefinition(); }}};
}

namespace {

// ---- The list page ----------------------------------------------------------------------------------------------------

SharedCover ruleCover(const ui::Widget& owner, const gfx::Rect& local, std::shared_ptr<const List> list) {
    SharedCover s;
    s.rect = owner.toWindow(local);
    s.radius = 2;
    s.owner = &owner;
    s.key = "rule:" + list->id;
    s.paint = [list](gfx::Canvas& c, const gfx::Rect& r, float radius) { drawCover(c, *list, r, radius); };
    return s;
}

class RuleHeader : public HeroHeader {
public:
    RuleHeader() {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        edit_ = add<Button>(ButtonKind::IconOutline, L"", "sliders");
        download_ = add<Button>(ButtonKind::IconOutline, L"", "download");
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        edit_->setTooltip(tr(L"Kuralları düzenle"));
        download_->setTooltip(tr(L"Şarkıları indir"));
        more_->setTooltip(tr(L"Diğer seçenekler"));
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        why_ = gfx::Text({}, type::secondary, wrap);
    }
    void setList(const List& l, size_t count, int64_t totalMs) {
        list_ = l;
        shared_ = std::make_shared<const List>(l);
        setTitle(toWide(l.def.name));
        why_.setText(describe(l.def));
        count_ = count;
        totalMs_ = totalMs;
        const gfx::Hsl h = gfx::toHsl(tileColor(l.id));
        setTintColor(gfx::fromHsl({h.h, std::max(h.s, 0.35f), 0.5f}));
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
        for (auto* b : {edit_, download_, more_}) {
            b->setRect({x, y + 8, 40, 40});
            x += 40 + 14;
        }
        const float tx = kCoverSize + 32, tw = std::max(0.f, rect().w - tx);
        const float th = fitTitle(tw, {88.f, 72.f, 56.f, 44.f, 36.f}, 56.f, type::displayXL);
        whyW_ = std::min(tw, 640.f);
        whyH_ = why_.empty() ? 0.f : std::ceil(why_.measure(whyW_).h);
        float ty = kCoverSize - th - 30 - (why_.empty() ? 0.f : whyH_ + 6);
        labelY_ = ty - 26;
        contentTop_ = labelY_;
        titleBox_ = {tx - titleSize_ * 0.04f, ty, tw, th};
        ty += th + 10;
        whyY_ = ty;
        if (!why_.empty()) ty += whyH_ + 6;
        metaY_ = ty;
    }
    void drawCover(Canvas& c, const gfx::Rect& r, float radius) override { rulelists::drawCover(c, list_, r, radius); }
    SharedCover sharedCover(const ui::Widget& owner, const gfx::Rect& local) override {
        return shared_ ? ruleCover(owner, local, shared_) : SharedCover{};
    }

    Button *edit_, *download_, *more_;

protected:
    void paintContent(Canvas& c) override {
        const gfx::Rect r = rect();
        const auto& col = colors();
        const float tx = r.x + kCoverSize + 32, tw = std::max(0.f, r.right() - tx);
        c.text(toUpperTr(tr(L"Kurallı liste")) + L" · " + toUpperTr(i18n::plural(L"{} kural", static_cast<int64_t>(ruleCount(list_.def)))),
               type::monoLabel, {tx, r.y + labelY_, tw, 14}, col.fgSecondary);
        if (!why_.empty()) c.text(why_, {tx, r.y + whyY_, whyW_, whyH_}, col.fgSecondary);
        std::wstring meta = L"ShadeTube · " + i18n::plural(L"{} şarkı", static_cast<int64_t>(count_));
        if (totalMs_ > 0) meta += L" · " + totalDuration(totalMs_);
        c.text(meta, type::secondary, {tx, r.y + metaY_ + 2, tw, 20}, col.fgSecondary);
    }

private:
    List list_;
    std::shared_ptr<const List> shared_;
    gfx::Text why_;
    size_t count_ = 0;
    int64_t totalMs_ = 0;
    float labelY_ = 0, whyY_ = 0, whyW_ = 0, whyH_ = 0, metaY_ = 0;
};

class RuleListPage : public ScrollPage {
public:
    RuleListPage(std::string id, bool autoplay) : id_(std::move(id)), autoplay_(autoplay) {
        enableCollapse();
        subscribe(life_.ref(), [this] { listChanged(); });
        showSkeleton(8);
        if (!find(id_) && state().loaded) {
            showMissing();
            return;
        }
        watch(life_.ref(), id_, [this](const Songs& s) { songsArrived(s); });
    }
    bool coverSlot(SharedCover& out) override {
        if (header_) return headerCoverSlot(out);
        return loadingCoverSlot(out);
    }

private:
    std::string contextUri() const { return "smart:rule:" + id_; }
    bool isPlayingThis() const {
        auto* p = ctx().player;
        return p && p->isPlaying() && p->context().uri == contextUri();
    }

    void listChanged() {
        const List* l = find(id_);
        if (!l) {
            if (state().loaded) showMissing();
            return;
        }
        if (header_) header_->setList(*l, songs_.tracks.size(), songs_.totalMs);
    }

    void showMissing() {
        header_ = nullptr;
        table_ = nullptr;
        auto* c = resetContent();
        c->add<MessagePanel>("filter", tr(L"Bu liste yok"), tr(L"Kurallı liste silinmiş ya da başka bir hesaba ait."),
                             tr(L"Kitaplığa dön"), [] { ctx().router->navigate({RouteKind::Library}); });
        contentReady();
    }

    void songsArrived(const Songs& s) {
        const List* l = find(id_);
        if (!l) return;
        std::vector<std::string> ids;
        ids.reserve(s.tracks.size());
        for (const auto& t : s.tracks) ids.push_back(t.id);
        songs_ = s;
        if (header_ && ids == ids_) {
            header_->setList(*l, s.tracks.size(), s.totalMs);
            table_->setTracks(songs_.tracks);   // (BPM / play facts may differ: same rows)
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

    void build(const List& l) {
        auto* c = resetContent(28.f);
        header_ = c->add<RuleHeader>();
        header_->setList(l, songs_.tracks.size(), songs_.totalMs);
        TrackTable::Options opts;
        table_ = c->add<TrackTable>(opts);
        attachHeader(header_, table_, table_);
        header_->isPlaying = [this] { return isPlayingThis(); };
        table_->onPlay = [this](int i) { playFrom(i, false); };
        header_->play_->onClick = [this] { playFrom(-1, false); };
        header_->shuffle_->onClick = [this] { playFrom(-1, true); };
        header_->edit_->onClick = [id = id_] { ctx().router->navigate({RouteKind::SmartList, "rule-edit:" + id}); };
        header_->download_->onClick = [this] { downloadAll(); };
        header_->more_->onClick = [this] {
            const gfx::Rect br = header_->more_->toWindow(header_->more_->rect());
            const auto tracks = table_->displayedTracks();
            const std::string id = id_;
            const List* l = find(id);
            const std::string name = l ? l->def.name : std::string();
            std::vector<ui::MenuItem> items{
                {tr(L"Tümünü sıraya ekle"), "queue", L"", [tracks] {
                     ctx().player->enqueue(tracks);
                     toast(tr(L"Sıraya eklendi"));
                 }},
                {tr(L"Çalma listesi olarak kaydet"), "plus", L"", [tracks] {
                     promptNewPlaylist([](const std::string& pid) { ctx().router->navigate({RouteKind::Playlist, pid}); }, tracks);
                 }},
                {tr(L"Dışa aktar…"), "share", L"", [name, tracks] { transfer::exportTracks(name, tracks); }},
                ui::MenuItem::sep()};
            for (auto& it : menuItems(id))
                if (it.label != tr(L"Aç") && it.label != tr(L"Çal") && it.label != tr(L"Kuralları düzenle")) items.push_back(std::move(it));
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        table_->setTracks(songs_.tracks);
        if (songs_.tracks.empty())
            emptyState(c, "filter", tr(L"Kurallara uyan şarkı yok"),
                       tr(L"Kuralları gevşetmeyi ya da başka kaynaklar eklemeyi dene."), tr(L"Kuralları düzenle"),
                       [id = id_] { ctx().router->navigate({RouteKind::SmartList, "rule-edit:" + id}); }, "sliders");
        scroll_->contentChanged();
        contentReady();
        if (autoplay_) {
            autoplay_ = false;
            playFrom(-1, false);
        }
    }

    void downloadAll() {
        std::vector<Track> todo;
        for (const auto& t : songs_.tracks)
            if (t.id.rfind("local:", 0) != 0 && !ctx().downloads.isDownloaded(t.id) && !ctx().downloads.isActive(t.id)) todo.push_back(t);
        if (todo.empty()) {
            toast(tr(L"Bu listenin şarkıları zaten indirilmiş"));
            return;
        }
        ctx().downloads.enqueue(todo);
        toast(i18n::plural(L"{} şarkı indirme sırasına eklendi", static_cast<int64_t>(todo.size())));
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
        p->playContext(std::move(tracks), displayIndex, {contextUri(), l ? toWide(l->def.name) : std::wstring{}});
    }

    std::string id_;
    bool autoplay_;
    RuleHeader* header_ = nullptr;
    TrackTable* table_ = nullptr;
    Songs songs_;
    std::vector<std::string> ids_;
};

// ---- The editor ----------------------------------------------------------------------------------------------------

// Parsing what the value boxes hold.
std::optional<double> parseNumber(const std::wstring& s) {
    std::string t = toUtf8(s);
    std::replace(t.begin(), t.end(), ',', '.');
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}
// "3:30" -> 210 s; "3" / "3,5" -> minutes.
std::optional<double> parseDuration(const std::wstring& s) {
    const auto colon = s.find(L':');
    if (colon == std::wstring::npos) {
        const auto m = parseNumber(s);
        return m ? std::optional<double>(*m * 60) : std::nullopt;
    }
    const auto m = parseNumber(s.substr(0, colon)), sec = parseNumber(s.substr(colon + 1));
    if (!m || !sec) return std::nullopt;
    return *m * 60 + *sec;
}
std::wstring durationText(double seconds) {
    const int s = static_cast<int>(std::lround(seconds));
    wchar_t buf[32];
    swprintf(buf, 32, L"%d:%02d", s / 60, s % 60);
    return buf;
}
// "2024-05-31" / "31.05.2024" -> local midnight (unix seconds).
std::optional<double> parseDate(const std::wstring& s) {
    int a = 0, b = 0, c = 0;
    std::tm tm{};
    if (swscanf_s(s.c_str(), L"%d-%d-%d", &a, &b, &c) == 3) {
        tm.tm_year = a - 1900;
        tm.tm_mon = b - 1;
        tm.tm_mday = c;
    } else if (swscanf_s(s.c_str(), L"%d.%d.%d", &a, &b, &c) == 3) {
        tm.tm_year = c - 1900;
        tm.tm_mon = b - 1;
        tm.tm_mday = a;
    } else {
        return std::nullopt;
    }
    if (tm.tm_year < 0 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) return std::nullopt;
    tm.tm_isdst = -1;
    const time_t t = mktime(&tm);
    if (t <= 0) return std::nullopt;
    return static_cast<double>(t);
}
std::wstring dateText(double unix) {
    const time_t t = static_cast<time_t>(unix);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return {};
    wchar_t buf[32];
    wcsftime(buf, 32, L"%Y-%m-%d", &tm);
    return buf;
}
std::wstring numberText(double v) {
    if (std::abs(v - std::round(v)) < 1e-9) return std::to_wstring(static_cast<long long>(std::llround(v)));
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", v);
    return buf;
}

constexpr Field kFieldMenu[] = {Field::Title,     Field::Artist,     Field::Album,      Field::Year,     Field::Duration,
                                Field::Bpm,       Field::Plays,      Field::Skips,      Field::LastPlayed, Field::FirstPlayed,
                                Field::Added,     Field::LikedAt,    Field::Liked,      Field::Downloaded, Field::LocalFile,
                                Field::Explicit,  Field::Playlist,   Field::Catalog};

// The playlists a rule can name (what the sidebar lists): Spotify's while signed in, else the local ones.
std::vector<std::pair<std::string, std::wstring>> playlistChoices() {
    std::vector<std::pair<std::string, std::wstring>> out;
    if (source::loggedIn() && ctx().session) {
        for (const auto& p : ctx().session->library().playlists)
            if (p.id != spotify::Api::kLikedSongsUri) out.emplace_back(p.id, toWide(p.name));
    } else {
        for (const auto& p : ctx().library.playlists()) out.emplace_back(p.id, toWide(p.name));
    }
    return out;
}

// One rule: field ▾ | operator ▾ | value(s) | ×.
class RuleRow : public ui::Widget {
public:
    static constexpr float kH = 44;
    RuleRow(Rule r, std::function<void(const Rule&, bool structural)> changed, std::function<void()> removed)
        : rule_(std::move(r)), changed_(std::move(changed)), removed_(std::move(removed)) {
        field_ = add<Button>(ButtonKind::Secondary, fieldLabel(rule_.field));
        field_->setDropdown(true);
        field_->setTooltip(tr(L"Alan"));
        field_->onClick = [this] { fieldMenu(); };
        op_ = add<Button>(ButtonKind::Secondary, opLabel(rule_.field, rule_.op));
        op_->setDropdown(true);
        op_->setTooltip(tr(L"Koşul"));
        op_->onClick = [this] { opMenu(); };
        remove_ = add<Button>(ButtonKind::Icon, L"", "close");
        remove_->setTooltip(tr(L"Kuralı kaldır"));
        remove_->onClick = [this] {
            const auto fn = removed_;
            fn();
        };
        makeValues();
    }
    float preferredHeight(float) override { return kH; }
    void layout() override {   // (children in local coordinates; the row keeps a reading width)
        const float w = std::min(rect().w, 960.f);
        const float fw = std::min(200.f, std::max(140.f, field_->naturalWidth() + 8));
        field_->setRect({0, 2, fw, 40});
        const float ow = std::min(220.f, std::max(120.f, op_->naturalWidth() + 8));
        op_->setRect({fw + 8, 2, ow, 40});
        const float x = fw + 8 + ow + 8;
        const float right = w - 40;
        remove_->setRect({right + 4, 6, 32, 32});
        const float room = std::max(60.f, right - 8 - x);
        if (choice_) {
            choice_->setRect({x, 2, std::min(room, std::max(160.f, choice_->naturalWidth() + 8)), 40});
        } else if (a_ && b_) {
            const float lw = std::ceil(and_->naturalWidth());
            const float bw = std::min(140.f, (room - lw - 16) / 2);
            a_->setRect({x, 0, bw, 44});
            and_->setRect({x + bw + 8, 0, lw, 44});
            b_->setRect({x + bw + 8 + lw + 8, 0, bw, 44});
        } else if (a_) {
            const bool wide = typeOf(rule_.field) == Type::Text;
            const float aw = wide ? std::min(room, 360.f) : std::min(room, unit_ ? 120.f : 160.f);
            a_->setRect({x, 0, aw, 44});
            if (unit_) unit_->setRect({x + aw + 8, 0, std::ceil(unit_->naturalWidth()), 44});
        }
    }
    ui::Widget* firstValue() const { return a_ ? static_cast<ui::Widget*>(a_) : choice_ ? static_cast<ui::Widget*>(choice_) : field_; }
    Button* fieldButton() const { return field_; }

private:
    void notify(bool structural) {
        const auto fn = changed_;
        fn(rule_, structural);
    }

    void makeValues() {
        const Type t = typeOf(rule_.field);
        const Op o = rule_.op;
        auto box = [this](std::wstring text, std::wstring placeholder) {
            auto* b = add<ui::TextBox>(ui::TextBox::Look::Field, std::move(placeholder));
            b->setText(std::move(text));
            return b;
        };
        if (t == Type::Text) {
            a_ = box(toWide(rule_.text), rule_.field == Field::Artist ? tr(L"Sanatçı adı") : tr(L"Metin"));
            a_->onChange = [this](const std::wstring& s) {
                rule_.text = toUtf8(s);
                notify(false);
            };
        } else if (t == Type::Number && o != Op::Known && o != Op::Unknown) {
            const bool dur = rule_.field == Field::Duration;
            auto text = [&](double v) { return v == 0 && !touched_ ? std::wstring() : dur ? durationText(v) : numberText(v); };
            const std::wstring hint = dur ? L"3:00" : rule_.field == Field::Year ? L"2020" : rule_.field == Field::Bpm ? L"120" : L"10";
            a_ = box(text(rule_.a), hint);
            a_->onChange = [this, dur](const std::wstring& s) {
                const auto v = dur ? parseDuration(s) : parseNumber(s);
                a_->setError(!s.empty() && !v);
                rule_.a = v.value_or(0);
                touched_ = true;
                notify(false);
            };
            if (o == Op::Between) {
                and_ = add<ui::Label>(tr(L"ile"), type::secondary, ui::Tone::Secondary);
                b_ = box(text(rule_.b), dur ? L"5:00" : rule_.field == Field::Bpm ? L"130" : L"20");
                b_->onChange = [this, dur](const std::wstring& s) {
                    const auto v = dur ? parseDuration(s) : parseNumber(s);
                    b_->setError(!s.empty() && !v);
                    rule_.b = v.value_or(0);
                    notify(false);
                };
            } else if (dur) {
                unit_ = add<ui::Label>(tr(L"dk:sn"), type::caption, ui::Tone::Tertiary);
            }
        } else if (t == Type::Date && o != Op::Never) {
            if (o == Op::InLast || o == Op::NotInLast) {
                a_ = box(rule_.a > 0 ? numberText(rule_.a) : std::wstring(), L"30");
                a_->onChange = [this](const std::wstring& s) {
                    const auto v = parseNumber(s);
                    a_->setError(!s.empty() && (!v || *v < 1));
                    rule_.a = v.value_or(0);
                    notify(false);
                };
            } else {
                a_ = box(rule_.a > 0 ? dateText(rule_.a) : std::wstring(), tr(L"YYYY-AA-GG"));
                a_->onChange = [this](const std::wstring& s) {
                    const auto v = parseDate(s);
                    a_->setError(!s.empty() && !v);
                    rule_.a = v.value_or(0);
                    notify(false);
                };
            }
        } else if (t == Type::Set) {
            choice_ = add<Button>(ButtonKind::Secondary, choiceLabel());
            choice_->setDropdown(true);
            choice_->onClick = [this] { choiceMenu(); };
        }
    }

    std::wstring choiceLabel() const {
        if (rule_.field == Field::Catalog) {
            const auto c = catalogFromName(rule_.text);
            return c ? catalogLabel(*c) : std::wstring(tr(L"Seç…"));
        }
        for (const auto& [id, name] : playlistChoices())
            if (id == rule_.text) return name;
        return rule_.text.empty() ? std::wstring(tr(L"Liste seç…")) : std::wstring(tr(L"(silinmiş liste)"));
    }

    void dropdown(Button* anchor, std::vector<ui::MenuItem> items) {
        const gfx::Rect br = anchor->toWindow(anchor->rect());
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    }

    void fieldMenu() {
        std::vector<ui::MenuItem> items;
        auto ref = life_.ref();
        Type last = typeOf(kFieldMenu[0]);
        for (Field f : kFieldMenu) {
            if (typeOf(f) != last && !(last == Type::Number && typeOf(f) == Type::Date) && !(last == Type::Bool && typeOf(f) == Type::Set))
                items.push_back(ui::MenuItem::sep());
            last = typeOf(f);
            ui::MenuItem it{fieldLabel(f), "", L"", [this, ref, f] {
                                if (ref.expired() || f == rule_.field) return;
                                const bool sameType = typeOf(f) == typeOf(rule_.field);
                                rule_.field = f;
                                const auto ops = opsFor(f);
                                if (!sameType || std::find(ops.begin(), ops.end(), rule_.op) == ops.end()) {
                                    rule_.op = defaultOp(f);
                                    rule_.text.clear();
                                    rule_.a = rule_.b = 0;
                                }
                                if (f == Field::Catalog && !catalogFromName(rule_.text)) rule_.text = "spotify";
                                if (f == Field::Playlist && typeOf(f) != Type::Set) rule_.text.clear();
                                notify(true);
                            }};
            it.checked = f == rule_.field;
            items.push_back(std::move(it));
        }
        dropdown(field_, std::move(items));
    }

    void opMenu() {
        std::vector<ui::MenuItem> items;
        auto ref = life_.ref();
        for (Op o : opsFor(rule_.field)) {
            ui::MenuItem it{opLabel(rule_.field, o), "", L"", [this, ref, o] {
                                if (ref.expired() || o == rule_.op) return;
                                const Type t = typeOf(rule_.field);
                                const bool dateKind = [&](Op x) { return x == Op::InLast || x == Op::NotInLast; }(o) !=
                                                      [&](Op x) { return x == Op::InLast || x == Op::NotInLast; }(rule_.op);
                                rule_.op = o;
                                if (t == Type::Date && dateKind) rule_.a = 0;
                                notify(true);
                            }};
            it.checked = o == rule_.op;
            items.push_back(std::move(it));
        }
        dropdown(op_, std::move(items));
    }

    void choiceMenu() {
        std::vector<ui::MenuItem> items;
        auto ref = life_.ref();
        if (rule_.field == Field::Catalog) {
            for (Catalog c : {Catalog::Spotify, Catalog::YouTube, Catalog::MusicBrainz, Catalog::Local, Catalog::Other}) {
                ui::MenuItem it{catalogLabel(c), "", L"", [this, ref, c] {
                                    if (ref.expired()) return;
                                    rule_.text = catalogName(c);
                                    notify(true);
                                }};
                it.checked = rule_.text == catalogName(c);
                items.push_back(std::move(it));
            }
        } else {
            const auto choices = playlistChoices();
            if (choices.empty()) {
                ui::MenuItem none{tr(L"Henüz çalma listen yok"), "", L"", {}};
                none.enabled = false;
                items.push_back(std::move(none));
            }
            for (const auto& [id, name] : choices) {
                ui::MenuItem it{name, "playlist", L"", [this, ref, id] {
                                    if (ref.expired()) return;
                                    rule_.text = id;
                                    notify(true);
                                }};
                it.checked = rule_.text == id;
                items.push_back(std::move(it));
            }
        }
        dropdown(choice_, std::move(items));
    }

    Rule rule_;
    std::function<void(const Rule&, bool)> changed_;
    std::function<void()> removed_;
    Button *field_ = nullptr, *op_ = nullptr, *remove_ = nullptr, *choice_ = nullptr;
    ui::TextBox *a_ = nullptr, *b_ = nullptr;
    ui::Label *and_ = nullptr, *unit_ = nullptr;
    bool touched_ = false;
    Lifetime life_;
};

// A group of rules (one level): its own "all / any", its rows, "+ Kural"; outlined. A one-rule group is a plain row.
class GroupBox : public ui::Widget {
public:
    GroupBox(bool outlined) : outlined_(outlined) {}
    ui::Column* rows = nullptr;
    Segmented* match = nullptr;
    Button* addRule = nullptr;
    Button* remove = nullptr;
    float preferredHeight(float width) override {
        const float pad = outlined_ ? 16.f : 0.f;
        float h = pad * 2 + rows->preferredHeight(width - pad * 2);
        if (outlined_) h += 40 + 8 + 40 + 4;
        return h;
    }
    void layout() override {   // (children in local coordinates)
        const float w = rect().w;
        const float pad = outlined_ ? 16.f : 0.f;
        float y = pad;
        if (outlined_) {
            match->setRect({pad, y, match->naturalWidth(), 40});
            remove->setRect({w - pad - remove->naturalWidth(), y, remove->naturalWidth(), 40});
            y += 48;
        }
        const float rh = rows->preferredHeight(w - pad * 2);
        rows->setRect({pad, y, w - pad * 2, rh});
        y += rh;
        if (outlined_) addRule->setRect({pad, y + 4, addRule->naturalWidth(), 40});
    }
    void paint(Canvas& c) override {
        if (outlined_) c.strokeRounded(rect(), gfx::radii().surface, colors().hairDefault);
        paintChildren(c);
    }

private:
    bool outlined_;
};

class RuleEditorPage : public ScrollPage {
public:
    // `id`: the list edited ("" = a new one, from `draft`); without a draft the list's own rules, once they are loaded.
    RuleEditorPage(std::string id, std::optional<Definition> draft) : id_(std::move(id)) {
        if (draft) {
            def_ = std::move(*draft);
            ready_ = true;
            build();
            schedulePreview();
            return;
        }
        showLoading(Loading::Rows);
        subscribe(life_.ref(), [this] { tryLoad(); });
        tryLoad();
    }
    ~RuleEditorPage() override { endPreview(); }

private:
    bool isNew() const { return id_.empty(); }

    void tryLoad() {
        if (ready_) return;
        if (const List* l = find(id_)) {
            def_ = l->def;
            ready_ = true;
            build();
            schedulePreview();
        } else if (state().loaded) {
            ready_ = true;
            auto* c = resetContent();
            c->add<MessagePanel>("filter", tr(L"Bu liste yok"), tr(L"Kurallı liste silinmiş ya da başka bir hesaba ait."),
                                 tr(L"Kitaplığa dön"), [] { ctx().router->navigate({RouteKind::Library}); });
            contentReady();
        }
    }

    void build() {
        auto* c = resetContent(24.f);
        auto* title = c->add<PageTitle>(isNew() ? std::wstring(tr(L"Yeni kurallı liste")) : std::wstring(tr(L"Kuralları düzenle")),
                                        tr(L"Şarkılar kurallara göre seçilir; kaynakları değiştikçe liste kendini günceller."),
                                        toUpperTr(tr(L"Kurallı liste")));
        auto* cancel = title->addAction(ButtonKind::Ghost, tr(L"Vazgeç"), "arrow-back");
        cancel->onClick = [] { ctx().router->back(); };
        auto* save = title->addAction(ButtonKind::Secondary, tr(L"Kaydet"), "check");
        save->onClick = [this] { commit(); };
        // Name
        {
            auto* box = c->add<ui::Box>();
            auto* caption = box->add<ui::Label>(tr(L"Ad"), type::caption, ui::Tone::Secondary);
            name_ = box->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Listenin adı"));
            name_->setText(toWide(def_.name));
            name_->onChange = [this](const std::wstring& s) { def_.name = toUtf8(s); };
            name_->onSubmit = [this](const std::wstring&) { commit(); };
            box->onPreferredHeight = [](float) { return 16.f + 6 + 44; };
            box->onLayout = [caption, this](ui::Box& b) {
                const float w = std::min(b.rect().w, 520.f);
                caption->setRect({0, 0, w, 16});
                name_->setRect({0, 22, w, 44});
            };
        }
        // Rules
        {
            auto* sec = c->add<ui::Column>(12.f);
            sec->add<SectionHeader>(tr(L"Kurallar"));
            auto* top = sec->add<ui::Box>();
            auto* lead = top->add<ui::Label>(tr(L"Şarkılar şunlara uysun:"), type::secondary, ui::Tone::Secondary);
            auto* seg = top->add<Segmented>(std::vector<std::wstring>{tr(L"Tüm kurallara"), tr(L"Herhangi bir kurala")}, def_.any ? 1 : 0);
            seg->onChange = [this](int i) {
                def_.any = i == 1;
                schedulePreview();
            };
            top->onPreferredHeight = [](float) { return 40.f; };
            top->onLayout = [lead, seg](ui::Box&) {
                const float lw = std::ceil(lead->naturalWidth()), sw = seg->naturalWidth();
                lead->setRect({0, 0, lw, 40});
                seg->setRect({lw + 12, 0, sw, 40});
            };
            rulesCol_ = sec->add<ui::Column>(10.f);
            auto* adds = sec->add<ui::Box>();
            auto* addRule = adds->add<Button>(ButtonKind::Ghost, tr(L"Kural ekle"), "plus");
            auto* addGroup = adds->add<Button>(ButtonKind::Ghost, tr(L"Grup ekle"), "plus");
            addGroup->setTooltip(tr(L"Kendi \"hepsi / herhangi biri\" seçimi olan bir kural grubu"));
            addRule->onClick = [this] {
                Group g;
                g.rules.push_back({Field::Title, Op::Contains, {}, 0, 0});
                def_.groups.push_back(std::move(g));
                rebuildRules(static_cast<int>(def_.groups.size()) - 1, 0);
            };
            addGroup->onClick = [this] {
                Group g;
                g.any = true;
                g.rules.push_back({Field::Artist, Op::Is, {}, 0, 0});
                g.rules.push_back({Field::Artist, Op::Is, {}, 0, 0});
                def_.groups.push_back(std::move(g));
                rebuildRules(static_cast<int>(def_.groups.size()) - 1, 0);
            };
            adds->onPreferredHeight = [](float) { return 40.f; };
            adds->onLayout = [addRule, addGroup](ui::Box&) {
                const float w1 = addRule->naturalWidth();
                addRule->setRect({0, 0, w1, 40});
                addGroup->setRect({w1 + 8, 0, addGroup->naturalWidth(), 40});
            };
            rebuildRules(-1, -1);
        }
        // Order and limit
        {
            auto* sec = c->add<ui::Column>(12.f);
            sec->add<SectionHeader>(tr(L"Sıralama ve sınır"));
            auto* row = sec->add<ui::Box>();
            auto* sortTag = row->add<ui::Label>(toUpperTr(tr(L"Sırala")), type::monoLabel, ui::Tone::Tertiary);
            sort_ = row->add<Button>(ButtonKind::Secondary, sortLabel(def_.sort));
            sort_->setDropdown(true);
            sort_->onClick = [this] { sortMenu(); };
            dir_ = row->add<Segmented>(std::vector<std::wstring>{tr(L"Azalan"), tr(L"Artan")}, def_.descending ? 0 : 1);
            dir_->onChange = [this](int i) {
                def_.descending = i == 0;
                schedulePreview();
            };
            auto* limitTag = row->add<ui::Label>(toUpperTr(tr(L"Sınır")), type::monoLabel, ui::Tone::Tertiary);
            limit_ = row->add<Button>(ButtonKind::Secondary, limitLabel());
            limit_->setDropdown(true);
            limit_->onClick = [this] { limitMenu(); };
            limitBox_ = row->add<ui::TextBox>(ui::TextBox::Look::Field, L"100");
            limitBox_->setText(std::to_wstring(def_.limitValue));
            limitBox_->onChange = [this](const std::wstring& s) {
                const auto v = parseNumber(s);
                const int max = def_.limit == Limit::Minutes ? kMaxLimitMinutes : kMaxLimitSongs;
                const bool ok = v && *v >= 1 && *v <= max;
                limitBox_->setError(!s.empty() && !ok);
                if (ok) {
                    def_.limitValue = static_cast<int>(*v);
                    schedulePreview();
                }
            };
            row->onPreferredHeight = [](float w) { return w < 760 ? 44.f * 2 + 12 : 44.f; };
            row->onLayout = [this, sortTag, limitTag](ui::Box& b) {
                const float w = b.rect().w;
                const float st = std::ceil(sortTag->naturalWidth());
                sortTag->setRect({0, 2, st, 40});
                const float sw = std::max(180.f, sort_->naturalWidth() + 8);
                sort_->setRect({st + 10, 2, sw, 40});
                dir_->setVisible(def_.sort != SortKey::Random);
                float x = st + 10 + sw + 12;
                dir_->setRect({x, 2, dir_->naturalWidth(), 40});
                x += dir_->naturalWidth() + 32;
                float y = 0;
                if (w < 760) {
                    x = 0;
                    y = 44 + 12;
                }
                const float lt = std::ceil(limitTag->naturalWidth());
                limitTag->setRect({x, y + 2, lt, 40});
                const float lw = std::max(180.f, limit_->naturalWidth() + 8);
                limit_->setRect({x + lt + 10, y + 2, lw, 40});
                limitBox_->setVisible(def_.limit != Limit::None);
                limitBox_->setRect({x + lt + 10 + lw + 12, y, 110, 44});
            };
        }
        // Sources
        {
            auto* sec = c->add<ui::Column>(12.f);
            sec->add<SectionHeader>(tr(L"Kaynaklar"), toUpperTr(tr(L"Şarkıların aranacağı yerler")));
            auto* row = sec->add<ui::Box>();
            std::vector<Button*> chips;
            for (Source src : {Liked, Playlists, Albums, LocalFiles, Downloads, History}) {
                // Outlined pills: a check while the source is in, a plus while it is out.
                auto* b = row->add<Button>(ButtonKind::Secondary, sourceLabel(src), (def_.sources & src) ? "check" : "plus");
                b->setTooltip((def_.sources & src) ? tr(L"Kaynaklardan çıkar") : tr(L"Kaynaklara ekle"));
                b->onClick = [this, b, src] {
                    const uint32_t next = def_.sources ^ src;
                    if (next == 0) return;   // at least one source
                    def_.sources = next;
                    const bool on = (def_.sources & src) != 0;
                    b->setIcon(on ? "check" : "plus");
                    b->setTooltip(on ? tr(L"Kaynaklardan çıkar") : tr(L"Kaynaklara ekle"));
                    b->requestLayout();
                    schedulePreview();
                };
                chips.push_back(b);
            }
            row->onPreferredHeight = [chips](float w) {
                float x = 0, y = 0;
                for (auto* b : chips) {
                    const float bw = b->naturalWidth();
                    if (x > 0 && x + bw > w) {
                        x = 0;
                        y += 44;
                    }
                    x += bw + 8;
                }
                return y + 36;
            };
            row->onLayout = [chips](ui::Box& b) {
                const float w = b.rect().w;
                float x = 0, y = 0;
                for (auto* ch : chips) {
                    const float bw = ch->naturalWidth();
                    if (x > 0 && x + bw > w) {
                        x = 0;
                        y += 44;
                    }
                    ch->setRect({x, y, bw, 36});
                    x += bw + 8;
                }
            };
        }
        // Preview
        {
            auto* sec = c->add<ui::Column>(16.f);
            previewHeader_ = sec->add<SectionHeader>(tr(L"Önizleme"), toUpperTr(tr(L"Hesaplanıyor…")));
            TrackTable::Options o;
            o.showAdded = true;
            table_ = sec->add<TrackTable>(o);
            table_->onPlay = [this](int i) {
                ctx().player->playContext(table_->displayedTracks(), i, {"preview:rules", toWide(def_.name)});
            };
            more_ = sec->add<ui::Label>(L"", type::secondary, ui::Tone::Tertiary);
            empty_ = noResultsState(sec, tr(L"Bu kurallara uyan şarkı yok."));
            empty_->setVisible(false);
        }
        scroll_->contentChanged();
        contentReady();
        name_->focus();
    }

    // The rule rows / group boxes from def_; `focusGroup` / `focusRule`: the row whose value box takes the focus.
    void rebuildRules(int focusGroup, int focusRule) {
        rulesCol_->clearChildren();
        ui::Widget* focus = nullptr;
        for (int gi = 0; gi < static_cast<int>(def_.groups.size()); ++gi) {
            const Group& g = def_.groups[gi];
            const bool outlined = g.rules.size() > 1 || g.any;
            auto* box = rulesCol_->add<GroupBox>(outlined);
            box->rows = box->add<ui::Column>(8.f);
            if (outlined) {
                box->match = box->add<Segmented>(std::vector<std::wstring>{tr(L"Hepsi uysun"), tr(L"Biri yeter")}, g.any ? 1 : 0);
                box->match->onChange = [this, gi](int i) {
                    if (gi < static_cast<int>(def_.groups.size())) def_.groups[gi].any = i == 1;
                    schedulePreview();
                };
                box->remove = box->add<Button>(ButtonKind::Ghost, tr(L"Grubu kaldır"), "trash");
                box->remove->onClick = [this, gi] { post([this, gi] {
                    if (gi < static_cast<int>(def_.groups.size())) def_.groups.erase(def_.groups.begin() + gi);
                    rebuildRules(-1, -1);
                }); };
                box->addRule = box->add<Button>(ButtonKind::Ghost, tr(L"Gruba kural ekle"), "plus");
                box->addRule->onClick = [this, gi] {
                    if (gi >= static_cast<int>(def_.groups.size())) return;
                    def_.groups[gi].rules.push_back(def_.groups[gi].rules.back());
                    def_.groups[gi].rules.back().text.clear();
                    rebuildRules(gi, static_cast<int>(def_.groups[gi].rules.size()) - 1);
                };
            }
            for (int ri = 0; ri < static_cast<int>(g.rules.size()); ++ri) {
                auto* row = box->rows->add<RuleRow>(
                    g.rules[ri],
                    [this, gi, ri](const Rule& r, bool structural) {
                        if (gi >= static_cast<int>(def_.groups.size()) || ri >= static_cast<int>(def_.groups[gi].rules.size())) return;
                        def_.groups[gi].rules[ri] = r;
                        if (structural) post([this, gi, ri] { rebuildRules(gi, ri); });
                        else schedulePreview();
                    },
                    [this, gi, ri] {
                        post([this, gi, ri] {
                            if (gi >= static_cast<int>(def_.groups.size())) return;
                            auto& rules = def_.groups[gi].rules;
                            if (ri < static_cast<int>(rules.size())) rules.erase(rules.begin() + ri);
                            if (rules.empty()) def_.groups.erase(def_.groups.begin() + gi);
                            rebuildRules(-1, -1);
                        });
                    });
                if (gi == focusGroup && ri == focusRule) focus = row->firstValue();
            }
        }
        if (def_.groups.empty())
            rulesCol_->add<ui::Label>(tr(L"Kural yok: seçili kaynaklardaki bütün şarkılar listeye girer."), type::secondary, ui::Tone::Tertiary);
        rulesCol_->requestLayout();
        scroll_->contentChanged();
        if (focus) {
            if (auto* w = window(); w && w->focusVisible()) w->focusByKeyboard(focus);
            else focus->focus();
        }
        schedulePreview();
    }

    // Runs `fn` after the current event (the widget that asked is replaced by it).
    void post(std::function<void()> fn) {
        Dispatcher::post([ref = life_.ref(), fn = std::move(fn)] {
            if (!ref.expired()) fn();
        });
    }

    std::wstring limitLabel() const {
        switch (def_.limit) {
        case Limit::Songs: return tr(L"En fazla … şarkı");
        case Limit::Minutes: return tr(L"En fazla … dakika");
        default: return tr(L"Sınır yok");
        }
    }

    void sortMenu() {
        std::vector<ui::MenuItem> items;
        for (int k = 0; k < static_cast<int>(SortKey::Count); ++k) {
            const auto key = static_cast<SortKey>(k);
            if (key == SortKey::Random) items.push_back(ui::MenuItem::sep());
            ui::MenuItem it{sortLabel(key), "", L"", [this, key, ref = life_.ref()] {
                                if (ref.expired()) return;
                                def_.sort = key;
                                // Times and counts start with the newest / most, texts from A.
                                def_.descending = !(key == SortKey::Title || key == SortKey::Artist || key == SortKey::Album);
                                sort_->setLabel(sortLabel(key));
                                dir_->select(def_.descending ? 0 : 1);
                                sort_->parent()->requestLayout();
                                schedulePreview();
                            }};
            it.checked = key == def_.sort;
            items.push_back(std::move(it));
        }
        const gfx::Rect br = sort_->toWindow(sort_->rect());
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    }

    void limitMenu() {
        std::vector<ui::MenuItem> items;
        for (Limit l : {Limit::None, Limit::Songs, Limit::Minutes}) {
            const std::wstring label = l == Limit::None ? tr(L"Sınır yok") : l == Limit::Songs ? tr(L"En fazla … şarkı") : tr(L"En fazla … dakika");
            ui::MenuItem it{label, "", L"", [this, l, ref = life_.ref()] {
                                if (ref.expired()) return;
                                def_.limit = l;
                                if (l == Limit::Minutes && def_.limitValue > kMaxLimitMinutes) def_.limitValue = 60;
                                limit_->setLabel(limitLabel());
                                limitBox_->setText(std::to_wstring(def_.limitValue));
                                limit_->parent()->requestLayout();
                                schedulePreview();
                                if (l != Limit::None) limitBox_->focus();
                            }};
            it.checked = l == def_.limit;
            items.push_back(std::move(it));
        }
        const gfx::Rect br = limit_->toWindow(limit_->rect());
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    }

    void schedulePreview() {
        preview(life_.ref(), def_, [this](const Songs& s) { showPreview(s); });
    }

    void showPreview(const Songs& s) {
        // "128 ŞARKI · 7 SA 12 DK"; with a limit that cut something: "501 EŞLEŞEN · LİSTEDE 100 ŞARKI · 7 SA 42 DK".
        std::wstring meta = i18n::plural(L"{} şarkı", static_cast<int64_t>(s.count));
        if (s.matched > s.count) meta = i18n::plural(L"{} eşleşen", static_cast<int64_t>(s.matched)) + L" · " + i18n::format(tr(L"listede {}"), {meta});
        if (s.totalMs > 0 && s.count > 0) meta += L" · " + totalDuration(s.totalMs);
        previewHeader_->setMeta(toUpperTr(meta));
        table_->setTracks(s.tracks);
        const size_t shown = s.tracks.size();
        more_->setText(s.count > shown ? i18n::plural(L"ve {} şarkı daha (kaydedince hepsi listede)", static_cast<int64_t>(s.count - shown))
                                       : std::wstring());
        empty_->setVisible(s.tracks.empty());
        if (auto* p = table_->parent()) p->requestLayout();
        scroll_->contentChanged();
    }

    void commit() {
        auto& s = state();
        if (def_.name.empty() || std::all_of(def_.name.begin(), def_.name.end(), [](char ch) { return ch == ' '; }))
            def_.name = toUtf8(tr(L"Kurallı liste"));
        const int64_t now = nowUnix();
        std::string id = id_;
        if (isNew()) {
            List l;
            l.id = id = newId();
            l.def = def_;
            l.created = l.updated = now;
            s.lists.insert(s.lists.begin(), std::move(l));
        } else {
            for (auto& l : s.lists)
                if (l.id == id_) {
                    l.def = def_;
                    l.updated = now;
                }
        }
        markDirty();
        saveNow();
        notify();
        kick();
        toast(isNew() ? tr(L"Kurallı liste oluşturuldu") : tr(L"Kurallar kaydedildi"));
        ctx().router->navigate({RouteKind::SmartList, "rule:" + id}, true);
    }

    std::string id_;
    Definition def_;
    bool ready_ = false;
    ui::TextBox* name_ = nullptr;
    ui::Column* rulesCol_ = nullptr;
    Button *sort_ = nullptr, *limit_ = nullptr;
    Segmented* dir_ = nullptr;
    ui::TextBox* limitBox_ = nullptr;
    SectionHeader* previewHeader_ = nullptr;
    TrackTable* table_ = nullptr;
    ui::Label* more_ = nullptr;
    ui::Widget* empty_ = nullptr;
};

} // namespace

} // namespace rulelists

void initRuleLists() { rulelists::init(); }

std::unique_ptr<Page> makeRuleListPage(const std::string& rawId) {
    using namespace rulelists;
    if (rawId == "rule-new") {
        Definition d = g_pendingDraft ? std::move(*g_pendingDraft) : starter();
        g_pendingDraft.reset();
        return std::make_unique<RuleEditorPage>(std::string(), std::optional<Definition>(std::move(d)));
    }
    if (rawId.rfind("rule-edit:", 0) == 0) return std::make_unique<RuleEditorPage>(rawId.substr(10), std::nullopt);
    std::string id = rawId.rfind("rule:", 0) == 0 ? rawId.substr(5) : rawId;
    bool autoplay = false;
    if (const auto hash = id.find("#play"); hash != std::string::npos) {
        id.resize(hash);
        autoplay = true;
    }
    return std::make_unique<RuleListPage>(id, autoplay);
}

} // namespace st::app
