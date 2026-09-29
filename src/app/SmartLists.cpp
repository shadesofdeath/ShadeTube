#include "app/SmartLists.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace st::app::smart {

namespace {

constexpr int64_t kDay = 86'400;
constexpr int64_t kSessionGapS = 30 * 60;      // a pause longer than this starts a new listening session
constexpr int64_t kMixWindowDays = 730;        // sessions of the last two years relate artists
constexpr size_t kSessionArtists = 5;          // an artist relates to the last few other artists of its session
constexpr size_t kMixNeighbors = 4;            // artists a mix adds to its lead artist
constexpr size_t kMixPerArtist = 12;           // familiar songs of one artist at most

bool excludedId(const std::string& id) { return id.rfind("podcast:", 0) == 0 || id.rfind("radio:", 0) == 0; }

bool excluded(const ListenStats::Track& t) { return t.name.empty() || excludedId(t.id); }

// Per-song totals over the streams.
struct Agg {
    int streams = 0;
    int recent = 0;              // streams in the last 30 days
    int64_t first = std::numeric_limits<int64_t>::max();
    int64_t last = 0;
    double score = 0;            // every stream, recent ones worth more
};

uint64_t fnv(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Weighted pick of `k` indexes without replacement (Efraimidis-Spirakis), in the order picked.
std::vector<size_t> weightedPick(const std::vector<double>& weights, size_t k, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> uni(std::numeric_limits<double>::min(), 1.0);
    std::vector<std::pair<double, size_t>> keyed;
    keyed.reserve(weights.size());
    for (size_t i = 0; i < weights.size(); ++i) keyed.emplace_back(std::pow(uni(rng), 1.0 / std::max(weights[i], 1e-9)), i);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<size_t> out;
    for (size_t i = 0; i < keyed.size() && out.size() < k; ++i) out.push_back(keyed[i].second);
    return out;
}

template <class T>
void seededShuffle(std::vector<T>& v, uint64_t seed) {
    std::mt19937_64 rng(seed);
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[std::uniform_int_distribution<size_t>(0, i - 1)(rng)]);
}

// Reorders so that the same artist rarely plays twice in a row (a greedy pass; keeps the order otherwise).
void spreadArtists(std::vector<uint32_t>& order, const std::vector<uint32_t>& artistOf) {
    for (size_t i = 1; i < order.size(); ++i) {
        if (artistOf[order[i]] != artistOf[order[i - 1]]) continue;
        for (size_t j = i + 1; j < order.size(); ++j)
            if (artistOf[order[j]] != artistOf[order[i - 1]]) {
                std::swap(order[i], order[j]);
                break;
            }
    }
}

List makeList(std::string id, Kind kind, int64_t day) {
    List l;
    l.id = std::move(id);
    l.kind = kind;
    l.day = day;
    return l;
}

void addTrack(List& l, const ListenStats::Track& t) {
    l.tracks.push_back(ListenStats::toCatalog(t));
    l.fresh.push_back(false);
}

size_t minSongs(const List& l) {
    switch (l.kind) {
    case Kind::Discoveries: return kMinDiscoveries;
    case Kind::Mix: return kMixMinFamiliar;
    case Kind::Year: return kMinYearSongs;
    default: return kMinSongs;
    }
}

size_t maxSongs(const List& l) {
    switch (l.kind) {
    case Kind::Best: return kMaxBest;
    case Kind::Mix: return kMixFamiliar + kMixFresh;
    default: return kMaxSongs;
    }
}

} // namespace

uint64_t seedFor(const std::string& listId, int64_t day) {
    return fnv(listId, fnv(std::to_string(day)));
}

std::wstring songKey(const catalog::Track& t) { return ListenStats::keysFor(ListenStats::fromCatalog(t)).track; }

std::vector<List> build(const Input& in) {
    const auto local = [&in](int64_t unix) { return in.clock ? in.clock(unix) : unix; };
    const int64_t now = in.now;
    const int64_t today = listen::dayNumber(local(now));
    const size_t n = in.tracks.size();

    // --- Per song and per (first) artist.
    std::vector<Agg> agg(n);
    std::vector<uint32_t> artistOf(n, 0);
    std::vector<std::wstring> keys(n);
    std::unordered_map<std::wstring, uint32_t> artistByKey, trackByKey;
    std::vector<std::string> artistName, artistId;
    for (size_t i = 0; i < n; ++i) {
        const auto k = ListenStats::keysFor(in.tracks[i]);
        keys[i] = k.track;
        trackByKey.try_emplace(k.track, static_cast<uint32_t>(i));
        const auto [it, added] = artistByKey.try_emplace(k.firstArtist, static_cast<uint32_t>(artistName.size()));
        if (added) {
            const auto& as = in.tracks[i].artists;
            artistName.push_back(as.empty() ? std::string{} : as[0].name);
            artistId.push_back(as.empty() ? std::string{} : as[0].id);
        } else if (artistId[it->second].empty() && !in.tracks[i].artists.empty()) {
            artistId[it->second] = in.tracks[i].artists[0].id;
        }
        artistOf[i] = it->second;
    }
    const size_t artists = artistName.size();
    std::vector<int> artistStreams(artists, 0), artistRecent(artists, 0);   // recent: last 90 days

    // Streams: song totals, artist totals, year totals and the sessions that relate artists.
    std::unordered_map<uint64_t, int> cooc;              // artist pair (low << 32 | high) -> times heard together
    std::unordered_map<int, std::unordered_map<uint32_t, int>> yearStreams;
    std::vector<uint32_t> session;                       // the last distinct artists of the running session
    int64_t sessionEnd = std::numeric_limits<int64_t>::min();
    for (const auto& p : in.plays) {
        if (p.track >= n) continue;
        const auto& t = in.tracks[p.track];
        if (excluded(t) || !listen::counts(p.listenedMs, t.durationMs)) continue;
        Agg& a = agg[p.track];
        const int64_t age = std::max<int64_t>(0, now - p.startedAt);
        ++a.streams;
        if (age <= 30 * kDay) ++a.recent;
        a.first = std::min(a.first, p.startedAt);
        a.last = std::max(a.last, p.startedAt);
        a.score += 0.3 + 0.7 * std::exp2(-static_cast<double>(age) / (365.0 * kDay));
        const uint32_t ar = artistOf[p.track];
        ++artistStreams[ar];
        if (age <= 90 * kDay) ++artistRecent[ar];
        int y = 0, m = 0, d = 0;
        listen::civil(listen::dayNumber(local(p.startedAt)), y, m, d);
        ++yearStreams[y][p.track];
        if (age > kMixWindowDays * kDay || artistName[ar].empty()) continue;
        if (p.startedAt > sessionEnd + kSessionGapS) session.clear();
        sessionEnd = std::max(sessionEnd, p.startedAt + p.listenedMs / 1000);
        std::erase(session, ar);
        for (const uint32_t b : session) {
            const uint64_t key = (static_cast<uint64_t>(std::min(ar, b)) << 32) | std::max(ar, b);
            ++cooc[key];
        }
        session.insert(session.begin(), ar);
        if (session.size() > kSessionArtists) session.pop_back();
    }

    std::vector<uint32_t> streamed;   // songs with a stream
    for (uint32_t i = 0; i < n; ++i)
        if (agg[i].streams > 0) streamed.push_back(i);
    auto ranked = [&](auto&& keep, auto&& before) {
        std::vector<uint32_t> v;
        for (const uint32_t i : streamed)
            if (keep(agg[i])) v.push_back(i);
        std::sort(v.begin(), v.end(), [&](uint32_t x, uint32_t y) {
            if (before(agg[x], agg[y])) return true;
            if (before(agg[y], agg[x])) return false;
            return keys[x] < keys[y];   // total order: the same input always gives the same list
        });
        return v;
    };

    std::vector<List> out;

    // --- Günün karışımı: a lead artist the user plays now + the artists they play it with.
    {
        std::vector<uint32_t> leads;
        for (uint32_t a = 0; a < artists; ++a)
            if (artistStreams[a] >= 5 && !artistName[a].empty()) leads.push_back(a);
        std::sort(leads.begin(), leads.end(), [&](uint32_t x, uint32_t y) {
            const int sx = artistRecent[x] * 2 + artistStreams[x], sy = artistRecent[y] * 2 + artistStreams[y];
            return sx != sy ? sx > sy : artistName[x] < artistName[y];
        });
        std::vector<bool> used(artists, false);
        std::vector<std::vector<std::pair<double, uint32_t>>> related(artists);
        for (const auto& [key, count] : cooc) {
            if (count < 2) continue;
            const auto a = static_cast<uint32_t>(key >> 32), b = static_cast<uint32_t>(key & 0xFFFFFFFFu);
            const double s = count / std::sqrt(static_cast<double>(std::max(1, artistStreams[a])) * std::max(1, artistStreams[b]));
            related[a].emplace_back(s, b);
            related[b].emplace_back(s, a);
        }
        int number = 0;
        for (const uint32_t lead : leads) {
            if (out.size() >= kMaxMixes || used[lead]) continue;
            auto& rel = related[lead];
            std::sort(rel.begin(), rel.end(), [&](const auto& x, const auto& y) {
                return x.first != y.first ? x.first > y.first : artistName[x.second] < artistName[y.second];
            });
            std::vector<uint32_t> members{lead};
            for (const auto& [s, b] : rel)
                if (!used[b] && members.size() < 1 + kMixNeighbors && !artistName[b].empty()) members.push_back(b);
            // Familiar songs of the mix's artists, the best liked ones more likely, at most a few per artist.
            std::vector<uint32_t> cand;
            for (const uint32_t i : streamed)
                if (std::find(members.begin(), members.end(), artistOf[i]) != members.end()) cand.push_back(i);
            if (cand.size() < kMixMinFamiliar) continue;
            List mix = makeList("mix:" + std::to_string(number + 1), Kind::Mix, today);
            std::vector<double> w;
            for (const uint32_t i : cand) w.push_back(agg[i].score);
            std::vector<uint32_t> picked;
            std::unordered_map<uint32_t, size_t> perArtist;
            for (const size_t k : weightedPick(w, cand.size(), seedFor(mix.id, today))) {
                const uint32_t i = cand[k];
                if (perArtist[artistOf[i]] >= kMixPerArtist) continue;
                ++perArtist[artistOf[i]];
                picked.push_back(i);
                if (picked.size() >= kMixFamiliar) break;
            }
            if (picked.size() < kMixMinFamiliar) continue;
            spreadArtists(picked, artistOf);
            for (const uint32_t i : picked) {
                addTrack(mix, in.tracks[i]);
                mix.streams += agg[i].streams;
            }
            for (const uint32_t a : members) {
                used[a] = true;
                mix.artists.push_back(artistName[a]);
            }
            mix.seedArtistId = artistId[lead];
            uint32_t top = 0;
            bool any = false;
            for (const uint32_t i : cand)
                if (artistOf[i] == lead && (!any || agg[i].streams > agg[top].streams)) {
                    top = i;
                    any = true;
                }
            if (any) mix.seedTrackId = in.tracks[top].id;
            mix.number = ++number;
            out.push_back(std::move(mix));
        }
    }

    // --- Bu ayın favorileri.
    {
        List l = makeList("month", Kind::Month, today);
        for (const uint32_t i : ranked([](const Agg& a) { return a.recent >= 2; },
                                       [](const Agg& x, const Agg& y) { return x.recent != y.recent ? x.recent > y.recent : x.last > y.last; })) {
            if (l.tracks.size() >= kMaxSongs) break;
            addTrack(l, in.tracks[i]);
            l.streams += agg[i].recent;
        }
        if (l.tracks.size() >= minSongs(l)) out.push_back(std::move(l));
    }

    // --- Yeni keşiflerin.
    {
        List l = makeList("discoveries", Kind::Discoveries, today);
        const int64_t since = now - 30 * kDay;
        for (const uint32_t i : ranked([since](const Agg& a) { return a.first >= since && a.streams >= 2; },
                                       [](const Agg& x, const Agg& y) { return x.streams != y.streams ? x.streams > y.streams : x.first > y.first; })) {
            if (l.tracks.size() >= kMaxSongs) break;
            addTrack(l, in.tracks[i]);
            l.streams += agg[i].streams;
        }
        if (l.tracks.size() >= minSongs(l)) out.push_back(std::move(l));
    }

    // --- Tekrar keşfet: a daily pick among the old favorites.
    {
        List l = makeList("rediscover", Kind::Rediscover, today);
        const int64_t before = now - 90 * kDay;
        auto cand = ranked([before](const Agg& a) { return a.streams >= 4 && a.last < before; },
                           [](const Agg& x, const Agg& y) { return x.streams != y.streams ? x.streams > y.streams : x.last > y.last; });
        if (cand.size() > 2 * kMaxSongs) cand.resize(2 * kMaxSongs);
        std::vector<double> w;
        for (const uint32_t i : cand) w.push_back(agg[i].streams);
        for (const size_t k : weightedPick(w, kMaxSongs, seedFor(l.id, today))) {
            addTrack(l, in.tracks[cand[k]]);
            l.streams += agg[cand[k]].streams;
        }
        if (l.tracks.size() >= minSongs(l)) out.push_back(std::move(l));
    }

    // --- Unutulan beğeniler: liked, hardly ever streamed, not lately.
    {
        List l = makeList("forgotten", Kind::Forgotten, today);
        std::unordered_set<std::wstring> seen;
        std::vector<const catalog::Track*> cand;
        for (const auto& t : in.liked) {
            if (t.name.empty() || !t.playable || excludedId(t.id)) continue;
            const std::wstring k = songKey(t);
            if (!seen.insert(k).second) continue;
            if (const auto it = trackByKey.find(k); it != trackByKey.end()) {
                const Agg& a = agg[it->second];
                if (a.streams > 1 || (a.streams == 1 && now - a.last < 60 * kDay)) continue;
            }
            cand.push_back(&t);
        }
        seededShuffle(cand, seedFor(l.id, today));
        for (const auto* t : cand) {
            if (l.tracks.size() >= kMaxSongs) break;
            l.tracks.push_back(*t);
            l.fresh.push_back(false);
        }
        l.streams = static_cast<int>(cand.size());   // liked songs waiting to be played
        if (l.tracks.size() >= minSongs(l)) out.push_back(std::move(l));
    }

    // --- Tüm zamanların en iyileri.
    {
        List l = makeList("best", Kind::Best, today);
        for (const uint32_t i : ranked([](const Agg& a) { return a.streams >= 3; },
                                       [](const Agg& x, const Agg& y) { return x.score > y.score; })) {
            if (l.tracks.size() >= kMaxBest) break;
            addTrack(l, in.tracks[i]);
            l.streams += agg[i].streams;
        }
        if (l.tracks.size() >= minSongs(l)) out.push_back(std::move(l));
    }

    // --- The years (newest first).
    {
        std::vector<int> years;
        for (const auto& [y, songs] : yearStreams) years.push_back(y);
        std::sort(years.rbegin(), years.rend());
        size_t made = 0;
        for (const int y : years) {
            if (made >= kMaxYears) break;
            const auto& songs = yearStreams[y];
            if (songs.size() < kMinYearSongs) continue;
            std::vector<std::pair<int, uint32_t>> v;   // (streams that year, song)
            v.reserve(songs.size());
            for (const auto& [song, count] : songs) v.emplace_back(count, song);
            std::sort(v.begin(), v.end(), [&](const auto& x, const auto& yv) {
                if (x.second == yv.second) return false;
                if (x.first != yv.first) return x.first > yv.first;
                if (agg[x.second].score != agg[yv.second].score) return agg[x.second].score > agg[yv.second].score;
                return keys[x.second] < keys[yv.second];
            });
            List l = makeList("year:" + std::to_string(y), Kind::Year, today);
            l.number = y;
            for (const auto& [count, i] : v) {
                if (l.tracks.size() >= kMaxSongs) break;
                addTrack(l, in.tracks[i]);
                l.streams += count;
            }
            out.push_back(std::move(l));
            ++made;
        }
    }
    return out;
}

void mixFresh(List& mix, const std::vector<catalog::Track>& candidates, const std::vector<std::wstring>& knownKeys) {
    std::unordered_set<std::wstring> known(knownKeys.begin(), knownKeys.end());
    for (const auto& t : mix.tracks) known.insert(songKey(t));
    std::vector<catalog::Track> fresh;
    for (const auto& c : candidates) {
        if (fresh.size() >= kMixFresh) break;
        if (c.name.empty() || !c.playable || excludedId(c.id)) continue;
        if (!known.insert(songKey(c)).second) continue;
        fresh.push_back(c);
    }
    if (fresh.empty()) return;
    seededShuffle(fresh, seedFor(mix.id + "#fresh", mix.day));
    std::vector<catalog::Track> tracks;
    std::vector<bool> flags;
    size_t f = 0;
    for (size_t i = 0; i < mix.tracks.size(); ++i) {
        tracks.push_back(std::move(mix.tracks[i]));
        flags.push_back(i < mix.fresh.size() && mix.fresh[i]);
        if (i % 2 == 1 && f < fresh.size()) {   // one new song after every two familiar ones
            tracks.push_back(fresh[f++]);
            flags.push_back(true);
        }
    }
    while (f < fresh.size() && tracks.size() < maxSongs(mix)) {
        tracks.push_back(fresh[f++]);
        flags.push_back(true);
    }
    mix.tracks = std::move(tracks);
    mix.fresh = std::move(flags);
}

bool trim(List& list) {
    const size_t cap = maxSongs(list);
    if (list.tracks.size() > cap) list.tracks.resize(cap);
    list.fresh.resize(list.tracks.size(), false);
    size_t familiar = 0;
    for (size_t i = 0; i < list.tracks.size(); ++i)
        if (!list.fresh[i]) ++familiar;
    return (list.kind == Kind::Mix ? familiar : list.tracks.size()) >= minSongs(list);
}

} // namespace st::app::smart
