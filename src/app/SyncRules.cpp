#include "app/SyncRules.h"

#include "core/Log.h"

#include <algorithm>
#include <fstream>

namespace st::app::sync {

using json = nlohmann::json;
using catalog::Track;

namespace {

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Liked: return "liked";
    case Kind::Album: return "album";
    default: return "playlist";
    }
}

Kind kindFrom(const std::string& s) {
    if (s == "liked") return Kind::Liked;
    if (s == "album") return Kind::Album;
    return Kind::Playlist;
}

bool hasPrefix(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

} // namespace

bool isSpotifyRule(const Rule& r) { return hasPrefix(r.id, "spotify:"); }

const Rule* findRule(const State& s, const std::string& id) {
    for (const auto& r : s.rules)
        if (r.id == id) return &r;
    return nullptr;
}

Rule* findRule(State& s, const std::string& id) {
    for (auto& r : s.rules)
        if (r.id == id) return &r;
    return nullptr;
}

// ---- Persistence ---------------------------------------------------------------------------------------------------

json toJson(const State& s) {
    json rules = json::array();
    for (const auto& r : s.rules) {
        json imgs = json::array();
        for (const auto& i : r.images) imgs.push_back({{"u", i.url}, {"w", i.width}, {"h", i.height}});
        json e{{"id", r.id},   {"k", kindName(r.kind)}, {"n", r.name},       {"img", std::move(imgs)},
               {"at", r.addedAt}, {"ls", r.lastSync},   {"tracks", r.trackIds}};
        if (r.error != ListError::None) {
            e["err"] = static_cast<int>(r.error);
            e["ed"] = r.errorDetail;
            e["fails"] = r.failures;
            e["retry"] = r.retryAt;
        }
        rules.push_back(std::move(e));
    }
    json attempts = json::object();
    for (const auto& [id, a] : s.attempts) attempts[id] = json::array({a.count, a.last});
    return json{{"version", 1}, {"rules", std::move(rules)}, {"attempts", std::move(attempts)}};
}

State stateFromJson(const json& j) {
    State s;
    if (!j.is_object()) return s;
    if (auto it = j.find("rules"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            Rule r;
            r.id = e.value("id", "");
            if (r.id.empty() || findRule(s, r.id)) continue;
            r.kind = kindFrom(e.value("k", "playlist"));
            r.name = e.value("n", "");
            if (auto im = e.find("img"); im != e.end() && im->is_array())
                for (const auto& i : *im)
                    if (i.is_object() && i.value("u", "") != "")
                        r.images.push_back({i.value("u", ""), i.value("w", 0), i.value("h", 0)});
            r.addedAt = e.value("at", int64_t{0});
            r.lastSync = e.value("ls", int64_t{0});
            if (auto tr = e.find("tracks"); tr != e.end() && tr->is_array())
                for (const auto& t : *tr)
                    if (t.is_string()) r.trackIds.push_back(t.get<std::string>());
            const int err = e.value("err", 0);
            r.error = err >= 1 && err <= 3 ? static_cast<ListError>(err) : ListError::None;
            r.errorDetail = e.value("ed", "");
            r.failures = std::max(0, e.value("fails", 0));
            r.retryAt = e.value("retry", int64_t{0});
            s.rules.push_back(std::move(r));
        }
    }
    if (auto it = j.find("attempts"); it != j.end() && it->is_object()) {
        for (const auto& [id, v] : it->items()) {
            if (!v.is_array() || v.size() != 2 || !v[0].is_number_integer() || !v[1].is_number_integer()) continue;
            s.attempts[id] = {std::max(0, v[0].get<int>()), v[1].get<int64_t>()};
        }
    }
    return s;
}

State loadState(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    json j = json::parse(f, nullptr, false);
    f.close();
    if (!j.is_object()) {
        ST_LOG_WARN("sync", "sync.json is damaged; starting empty (kept as sync.json.bad)");
        std::error_code ec;
        auto bad = file;
        bad += L".bad";
        std::filesystem::copy_file(file, bad, std::filesystem::copy_options::overwrite_existing, ec);
        return {};
    }
    return stateFromJson(j);
}

bool saveState(const State& s, const std::filesystem::path& file) {
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson(s).dump();
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

// ---- Planning ------------------------------------------------------------------------------------------------------

bool downloadable(const Track& t) {
    if (t.id.empty() || !t.playable) return false;
    return !hasPrefix(t.id, "radio:") && !hasPrefix(t.id, "local:") && !hasPrefix(t.id, "podcast:");
}

int64_t estimateBytes(int durationMs, int mp3Kbps) {
    const int64_t ms = durationMs > 0 ? durationMs : 210000;   // unknown: a typical 3.5 min song
    const int64_t kbps = mp3Kbps > 0 ? mp3Kbps : 130;           // original: ~128 kbps AAC + container
    return ms * kbps / 8 + 64 * 1024;                            // + cover art and the tag
}

bool retryDue(const Attempt& a, int64_t now) {
    if (a.count <= 0) return true;
    if (a.count >= kMaxAttempts) return false;
    const int64_t wait = a.count == 1 ? 3600 : 6 * 3600;
    return now >= a.last + wait;
}

std::vector<std::string> ruleTrackIds(const std::vector<Track>& tracks) {
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    ids.reserve(tracks.size());
    for (const auto& t : tracks)
        if (downloadable(t) && seen.insert(t.id).second) ids.push_back(t.id);
    return ids;
}

Plan plan(const std::vector<Track>& tracks, const ItemLookup& lookup, const std::function<bool(const Track&)>& blocked,
          const std::unordered_map<std::string, Attempt>& attempts, const Limits& limits, int64_t now) {
    Plan p;
    std::unordered_set<std::string> seen;
    int64_t used = limits.usedBytes;
    for (const auto& t : tracks) {
        if (!downloadable(t) || !seen.insert(t.id).second) continue;
        const ItemInfo info = lookup ? lookup(t.id) : ItemInfo{};
        using St = ItemInfo::St;
        if (info.state == St::Done || info.state == St::Queued || info.state == St::Downloading) continue;
        if (blocked && blocked(t)) continue;
        if (info.state == St::Failed) {
            const auto it = attempts.find(t.id);
            const Attempt a = it != attempts.end() ? it->second : Attempt{1, 0};   // unknown: one attempt, long ago
            if (a.count >= kMaxAttempts) {
                ++p.gaveUp;
                continue;
            }
            if (!retryDue(a, now)) {
                ++p.waitingRetry;
                continue;
            }
        }
        const int64_t est = estimateBytes(t.durationMs, limits.mp3Kbps);
        if (limits.capBytes > 0 && used + est > limits.capBytes) {
            ++p.skippedCap;
            continue;
        }
        used += est;
        p.queuedBytes += est;
        p.toQueue.push_back(t);
    }
    return p;
}

std::unordered_set<std::string> wantedIds(const std::vector<Rule>& rules) {
    std::unordered_set<std::string> ids;
    for (const auto& r : rules) ids.insert(r.trackIds.begin(), r.trackIds.end());
    return ids;
}

Drop dropped(const std::vector<std::pair<std::string, ItemInfo>>& items, const std::unordered_set<std::string>& wanted,
             bool deleteFiles) {
    Drop d;
    using St = ItemInfo::St;
    for (const auto& [id, info] : items) {
        if (!info.synced || wanted.contains(id)) continue;
        if (info.state == St::Done) {
            if (deleteFiles) d.remove.push_back(id);
        } else if (info.state != St::None) {
            d.cancel.push_back(id);
        }
    }
    return d;
}

bool suspiciousShrink(size_t before, size_t after) { return after == 0 && before > 0; }

Progress progress(const Rule& r, const ItemLookup& lookup) {
    Progress p;
    p.total = static_cast<int>(r.trackIds.size());
    if (!lookup) return p;
    using St = ItemInfo::St;
    for (const auto& id : r.trackIds) {
        switch (lookup(id).state) {
        case St::Done: ++p.done; break;
        case St::Queued:
        case St::Downloading: ++p.active; break;
        case St::Failed: ++p.failed; break;
        default: break;
        }
    }
    return p;
}

// ---- Scheduling ----------------------------------------------------------------------------------------------------

int64_t relistInterval(const Rule& r) {
    if (!isSpotifyRule(r)) return r.kind == Kind::Album ? 24 * 3600 : 45 * 60;
    if (r.kind == Kind::Liked) return 6 * 3600;
    if (r.kind == Kind::Album) return 24 * 3600;
    return 45 * 60;
}

int64_t minRelistGap(const Rule& r) {
    if (!isSpotifyRule(r)) return 0;
    return r.kind == Kind::Liked ? 5 * 60 : 60;
}

bool ruleDue(const Rule& r, int64_t now, bool pending) {
    if (now < r.retryAt) return false;
    if (r.lastSync == 0) return true;
    const int64_t since = now - r.lastSync;
    if (since < 0) return true;   // the clock went back: list again rather than wait for ever
    if (r.changedAt > r.lastSync && now - r.changedAt >= kChangeDebounceSec && since >= minRelistGap(r)) return true;
    if (pending && since >= kPendingRelistSec) return true;
    return since >= relistInterval(r);
}

int64_t backoffSeconds(int failures, bool rateLimited) {
    static constexpr int64_t kSteps[] = {60, 5 * 60, 15 * 60, 60 * 60};
    const int i = std::clamp(failures, 1, 4) - 1;
    const int64_t s = kSteps[i];
    return rateLimited ? std::max<int64_t>(s, 10 * 60) : s;
}

} // namespace st::app::sync
