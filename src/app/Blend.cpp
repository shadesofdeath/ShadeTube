// Blend: taste profiles, the shared playlist and the taste match (see Blend.h).
#include "app/Blend.h"

#include "core/Utf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace st::app::blend {

using json = nlohmann::json;

namespace {

constexpr int64_t kDay = 86'400;
constexpr double kHalfLifeDays = 60;        // within the window
constexpr double kHalfLifeAllDays = 365;    // over the whole history
constexpr size_t kMaxText = 300;

bool excludedId(const std::string& id) { return id.rfind("podcast:", 0) == 0 || id.rfind("radio:", 0) == 0; }

bool hex16(std::string_view s) {
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool httpUrl(std::string_view u) { return u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0; }

std::string str(const json& j, const char* key, size_t max = kMaxText) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    std::string s = it->get<std::string>();
    if (s.size() > max) {
        s.resize(max);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();   // a whole character
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
    }
    return s;
}

int64_t num(const json& j, const char* key, int64_t def = 0) {
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    if (it == j.end()) return def;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(std::min<uint64_t>(it->get<uint64_t>(), INT64_MAX));
    if (it->is_number_float()) {
        const double d = it->get<double>();
        return std::isfinite(d) ? static_cast<int64_t>(std::clamp(d, -9e18, 9e18)) : def;
    }
    return def;
}

int weightOf(double score, double top) { return std::clamp(static_cast<int>(std::lround(100.0 * score / top)), 1, 100); }

TasteTrack tasteOf(const ListenStats::Track& t, const std::string& key) {
    TasteTrack x;
    x.key = key;
    x.id = t.id;
    x.title = t.name;
    for (size_t i = 0; i < t.artists.size() && i < 4; ++i) x.artists.push_back(t.artists[i]);
    x.album = t.albumName;
    x.albumId = t.albumId;
    if (httpUrl(t.image)) x.cover = t.image;
    x.durationMs = std::max<int64_t>(0, t.durationMs);
    return x;
}

catalog::Track catalogOf(const TasteTrack& t) {
    catalog::Track c;
    c.id = t.id.empty() ? "blend:" + t.key : t.id;
    c.name = t.title;
    c.artists = t.artists;
    c.album.id = t.albumId;
    c.album.name = t.album;
    if (!t.cover.empty()) c.album.images.push_back({t.cover, 300, 300});
    c.durationMs = static_cast<int>(std::min<int64_t>(t.durationMs, std::numeric_limits<int>::max()));
    c.playable = true;
    return c;
}

// Weighted overlap of two weighted sets, 0..1: the sum over shared keys of min(share in a, share in b).
template <class Item>
double overlap(const std::vector<Item>& a, const std::vector<Item>& b) {
    int64_t wa = 0, wb = 0;
    std::unordered_map<std::string, int64_t> inB;
    for (const auto& x : a) wa += x.weight;
    for (const auto& x : b) {
        wb += x.weight;
        inB[x.key] += x.weight;
    }
    if (wa <= 0 || wb <= 0) return 0;
    std::unordered_map<std::string, int64_t> inA;
    for (const auto& x : a) inA[x.key] += x.weight;
    // Integer cross products (shares compared without rounding), one division at the end.
    int64_t sum = 0;
    for (const auto& [k, w] : inA)
        if (const auto it = inB.find(k); it != inB.end()) sum += std::min(w * wb, it->second * wa);
    return static_cast<double>(sum) / (static_cast<double>(wa) * static_cast<double>(wb));
}

} // namespace

bool TasteTrack::operator==(const TasteTrack& o) const {
    if (key != o.key || id != o.id || title != o.title || album != o.album || albumId != o.albumId || cover != o.cover ||
        durationMs != o.durationMs || weight != o.weight || artists.size() != o.artists.size())
        return false;
    for (size_t i = 0; i < artists.size(); ++i)
        if (artists[i].id != o.artists[i].id || artists[i].name != o.artists[i].name) return false;
    return true;
}

std::string keyOf(const std::wstring& foldedKey) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : toUtf8(foldedKey)) {
        h ^= c;
        h *= 1099511628211ull;
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4) s[static_cast<size_t>(i)] = kDigits[h & 15];
    return s;
}

Profile makeProfile(const std::vector<ListenStats::Track>& tracks, const std::vector<ListenStats::Play>& plays, int64_t now) {
    Profile p;
    p.at = now - ((now % 3600) + 3600) % 3600;
    // One score per song key (the history already merges a song across sources; the key makes sure).
    std::vector<std::string> keys(tracks.size());
    std::vector<ListenStats::Keys> folded(tracks.size());
    std::vector<bool> excluded(tracks.size(), false);
    for (size_t i = 0; i < tracks.size(); ++i) {
        excluded[i] = tracks[i].name.empty() || excludedId(tracks[i].id);
        if (excluded[i]) continue;
        folded[i] = ListenStats::keysFor(tracks[i]);
        keys[i] = keyOf(folded[i].track);
    }
    std::vector<double> score(tracks.size(), 0.0);
    auto tally = [&](int64_t since, double halfLife) {
        std::fill(score.begin(), score.end(), 0.0);
        size_t songs = 0;
        for (const auto& pl : plays) {
            if (pl.track >= tracks.size() || excluded[pl.track] || pl.startedAt < since) continue;
            if (!listen::counts(pl.listenedMs, tracks[pl.track].durationMs)) continue;
            const double ageDays = static_cast<double>(std::max<int64_t>(0, now - pl.startedAt)) / kDay;
            if (score[pl.track] == 0.0) ++songs;
            score[pl.track] += std::pow(0.5, ageDays / halfLife);
        }
        return songs;
    };
    if (tally(now - kWindowDays * kDay, kHalfLifeDays) < kMinWindowTracks) {
        tally(std::numeric_limits<int64_t>::min(), kHalfLifeAllDays);
        p.windowDays = 0;
    }

    // Songs, heaviest first (ties by key: the same order on every PC).
    std::map<std::string, std::pair<double, size_t>> byKey;   // key -> score, first track index
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (score[i] <= 0) continue;
        auto [it, added] = byKey.try_emplace(keys[i], score[i], i);
        if (!added) it->second.first += score[i];
    }
    std::vector<std::pair<std::string, std::pair<double, size_t>>> ranked(byKey.begin(), byKey.end());
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    if (!ranked.empty()) {
        const double top = ranked.front().second.first;
        for (size_t r = 0; r < ranked.size() && p.tracks.size() < kProfileTracks; ++r) {
            TasteTrack t = tasteOf(tracks[ranked[r].second.second], ranked[r].first);
            t.weight = weightOf(ranked[r].second.first, top);
            p.tracks.push_back(std::move(t));
        }
    }

    // Artists: every credited artist of a song gets its weight (features count), heaviest first.
    struct ArtistAcc {
        double score = 0;
        std::string id, name;
    };
    std::map<std::string, ArtistAcc> artists;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (score[i] <= 0) continue;
        for (size_t k = 0; k < tracks[i].artists.size() && k < folded[i].artists.size(); ++k) {
            if (folded[i].artists[k].empty()) continue;
            auto& a = artists[keyOf(folded[i].artists[k])];
            a.score += score[i];
            if (a.name.empty()) a.name = tracks[i].artists[k].name;
            if (a.id.empty()) a.id = tracks[i].artists[k].id;
        }
    }
    std::vector<std::pair<std::string, ArtistAcc>> arank(artists.begin(), artists.end());
    std::stable_sort(arank.begin(), arank.end(), [](const auto& a, const auto& b) { return a.second.score > b.second.score; });
    if (!arank.empty()) {
        const double top = arank.front().second.score;
        for (size_t r = 0; r < arank.size() && p.artists.size() < kProfileArtists; ++r)
            p.artists.push_back({arank[r].first, arank[r].second.id, arank[r].second.name, weightOf(arank[r].second.score, top)});
    }
    return p;
}

bool usable(const Profile& p) { return p.tracks.size() >= kMinProfileTracks; }

json toJson(const Profile& p) {
    json tracks = json::array();
    for (const auto& t : p.tracks) {
        json artists = json::array();
        for (const auto& a : t.artists) artists.push_back(json::array({a.id, a.name}));
        json j{{"k", t.key}, {"i", t.id}, {"n", t.title}, {"a", std::move(artists)}, {"w", t.weight}};
        if (!t.album.empty()) j["al"] = t.album;
        if (!t.albumId.empty()) j["ali"] = t.albumId;
        if (!t.cover.empty()) j["c"] = t.cover;
        if (t.durationMs > 0) j["d"] = t.durationMs;
        tracks.push_back(std::move(j));
    }
    json artists = json::array();
    for (const auto& a : p.artists) {
        json j{{"k", a.key}, {"n", a.name}, {"w", a.weight}};
        if (!a.id.empty()) j["i"] = a.id;
        artists.push_back(std::move(j));
    }
    return {{"v", kProfileVersion}, {"m", p.member}, {"n", p.name}, {"at", p.at}, {"win", p.windowDays},
            {"t", std::move(tracks)}, {"r", std::move(artists)}};
}

std::optional<Profile> profileFromJson(const json& j) {
    if (!j.is_object() || num(j, "v") != kProfileVersion) return std::nullopt;
    Profile p;
    p.member = str(j, "m", 32);
    if (!hex16(p.member)) return std::nullopt;
    p.name = str(j, "n", 120);
    p.at = std::max<int64_t>(0, num(j, "at"));
    p.windowDays = static_cast<int>(std::clamp<int64_t>(num(j, "win", kWindowDays), 0, 3650));
    std::set<std::string> seen;
    if (const auto t = j.find("t"); t != j.end() && t->is_array())
        for (const auto& v : *t) {
            if (p.tracks.size() >= kProfileTracks) break;
            TasteTrack x;
            x.key = str(v, "k", 32);
            x.id = str(v, "i", 200);
            x.title = str(v, "n");
            if (!hex16(x.key) || x.title.empty() || !seen.insert(x.key).second) continue;
            if (const auto a = v.find("a"); a != v.end() && a->is_array())
                for (const auto& e : *a) {
                    if (x.artists.size() >= 4) break;
                    if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_string() && !e[1].get<std::string>().empty())
                        x.artists.push_back({e[0].get<std::string>().substr(0, 200), e[1].get<std::string>().substr(0, kMaxText)});
                }
            x.album = str(v, "al");
            x.albumId = str(v, "ali", 200);
            x.cover = str(v, "c", 2048);
            if (!httpUrl(x.cover)) x.cover.clear();
            x.durationMs = std::clamp<int64_t>(num(v, "d"), 0, 24ll * 3600 * 1000);
            x.weight = static_cast<int>(std::clamp<int64_t>(num(v, "w", 1), 1, 100));
            p.tracks.push_back(std::move(x));
        }
    seen.clear();
    if (const auto r = j.find("r"); r != j.end() && r->is_array())
        for (const auto& v : *r) {
            if (p.artists.size() >= kProfileArtists) break;
            TasteArtist a{str(v, "k", 32), str(v, "i", 200), str(v, "n"), static_cast<int>(std::clamp<int64_t>(num(v, "w", 1), 1, 100))};
            if (!hex16(a.key) || a.name.empty() || !seen.insert(a.key).second) continue;
            p.artists.push_back(std::move(a));
        }
    return p;
}

Mix build(std::vector<Profile> profiles) {
    // Members by id; one profile each (the newest), at most kMaxMembers.
    std::stable_sort(profiles.begin(), profiles.end(), [](const Profile& a, const Profile& b) {
        return a.member != b.member ? a.member < b.member : a.at > b.at;
    });
    profiles.erase(std::unique(profiles.begin(), profiles.end(), [](const Profile& a, const Profile& b) { return a.member == b.member; }),
                   profiles.end());
    if (profiles.size() > kMaxMembers) profiles.resize(kMaxMembers);
    Mix m;
    for (const auto& p : profiles) {
        m.members.push_back(p.member);
        m.names.push_back(p.name);
    }
    m.match = tasteMatch(profiles);

    // Every song once, with the members who have it.
    struct Entry {
        const TasteTrack* track = nullptr;   // from the first member (by id) who has it
        std::vector<int> from;
        int sum = 0;
    };
    std::map<std::string, Entry> byKey;
    for (size_t i = 0; i < profiles.size(); ++i) {
        std::set<std::string> own;
        for (const auto& t : profiles[i].tracks) {
            if (!own.insert(t.key).second) continue;
            Entry& e = byKey[t.key];
            if (!e.track) e.track = &t;
            e.from.push_back(static_cast<int>(i));
            e.sum += t.weight;
        }
    }
    std::set<std::string> used;
    // 1. Loved by two or more: by how many, then how much, then key.
    std::vector<std::pair<std::string, const Entry*>> shared;
    for (const auto& [k, e] : byKey)
        if (e.from.size() >= 2) shared.emplace_back(k, &e);
    std::stable_sort(shared.begin(), shared.end(), [](const auto& a, const auto& b) {
        if (a.second->from.size() != b.second->from.size()) return a.second->from.size() > b.second->from.size();
        return a.second->sum > b.second->sum;
    });
    for (const auto& [k, e] : shared) {
        if (m.rows.size() >= kMaxRows) break;
        m.rows.push_back({catalogOf(*e->track), e->from});
        used.insert(k);
        ++m.shared;
    }
    // 2. Then one from each member in turn, each in their own order.
    std::vector<size_t> next(profiles.size(), 0);
    bool any = true;
    while (m.rows.size() < kMaxRows && any) {
        any = false;
        for (size_t i = 0; i < profiles.size() && m.rows.size() < kMaxRows; ++i) {
            const auto& tracks = profiles[i].tracks;
            while (next[i] < tracks.size() && used.count(tracks[next[i]].key)) ++next[i];
            if (next[i] >= tracks.size()) continue;
            const TasteTrack& t = tracks[next[i]++];
            used.insert(t.key);
            m.rows.push_back({catalogOf(t), {static_cast<int>(i)}});
            any = true;
        }
    }
    return m;
}

bool sameMix(const Mix& a, const Mix& b) {
    if (a.members != b.members || a.names != b.names || a.shared != b.shared || a.match != b.match || a.rows.size() != b.rows.size())
        return false;
    for (size_t i = 0; i < a.rows.size(); ++i) {
        const auto &x = a.rows[i], &y = b.rows[i];
        if (x.from != y.from || x.track.id != y.track.id || x.track.name != y.track.name || x.track.durationMs != y.track.durationMs)
            return false;
    }
    return true;
}

int tasteMatch(const std::vector<Profile>& profiles) {
    if (profiles.size() < 2) return -1;
    double total = 0;
    int pairs = 0;
    for (size_t i = 0; i < profiles.size(); ++i)
        for (size_t k = i + 1; k < profiles.size(); ++k) {
            const double c = 0.7 * overlap(profiles[i].artists, profiles[k].artists) + 0.3 * overlap(profiles[i].tracks, profiles[k].tracks);
            total += 100.0 * (1.0 - std::pow(1.0 - std::clamp(c, 0.0, 1.0), 3.0));
            ++pairs;
        }
    return std::clamp(static_cast<int>(std::lround(total / pairs)), 0, 100);
}

// ---- Store ------------------------------------------------------------------------------------------------------------

json toJson(const Blend& b) {
    json others = json::array();
    for (const auto& p : b.others) others.push_back(toJson(p));
    json j{{"link", b.link}, {"me", b.me}, {"others", std::move(others)}, {"left", b.left}, {"creator", b.creator},
           {"created", b.createdAt}, {"synced", b.syncedAt}, {"changed", b.othersChangedAt}};
    if (b.mine) j["mine"] = toJson(*b.mine);
    return j;
}

std::optional<Blend> blendFromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    Blend b;
    b.link = str(j, "link", 600);
    b.me = str(j, "me", 32);
    if (b.link.empty() || !hex16(b.me)) return std::nullopt;
    if (const auto m = j.find("mine"); m != j.end())
        if (auto p = profileFromJson(*m); p && p->member == b.me) b.mine = std::move(*p);
    if (const auto o = j.find("others"); o != j.end() && o->is_array())
        for (const auto& v : *o)
            if (auto p = profileFromJson(v); p && p->member != b.me && b.others.size() < kMaxMembers * 2) b.others.push_back(std::move(*p));
    if (const auto l = j.find("left"); l != j.end() && l->is_array())
        for (const auto& v : *l)
            if (v.is_string() && hex16(v.get<std::string>()) && b.left.size() < 64) b.left.push_back(v.get<std::string>());
    if (const auto c = j.find("creator"); c != j.end() && c->is_boolean()) b.creator = c->get<bool>();
    b.createdAt = num(j, "created");
    b.syncedAt = num(j, "synced");
    b.othersChangedAt = num(j, "changed");
    return b;
}

std::vector<Profile> profilesOf(const Blend& b) {
    std::vector<Profile> out;
    if (b.mine) out.push_back(*b.mine);
    for (const auto& p : b.others)
        if (std::find(b.left.begin(), b.left.end(), p.member) == b.left.end()) out.push_back(p);
    return out;
}

} // namespace st::app::blend
