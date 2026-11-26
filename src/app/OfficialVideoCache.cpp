#include "app/OfficialVideoCache.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace st::app {

using json = nlohmann::json;

std::string OfficialDecision::verdictOf(const std::string& id) const {
    for (const auto& [v, why] : tried)
        if (v == id) return why;
    return {};
}

OfficialVideoCache::OfficialVideoCache(std::filesystem::path file) : file_(std::move(file)) {}

void OfficialVideoCache::load() {
    entries_.clear();
    std::ifstream f(file_, std::ios::binary);
    if (!f) return;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    for (const auto& [id, v] : j.items()) {
        if (!v.is_object()) continue;
        OfficialDecision d;
        d.audioVideoId = v.value("a", "");
        d.videoId = v.value("v", "");
        d.title = v.value("t", "");
        d.channel = v.value("c", "");
        d.thumbnailUrl = v.value("th", "");
        d.durationSec = v.value("d", 0);
        d.score = v.value("s", 0.0);
        d.coverage = v.value("cov", 0.0);
        d.verdict = v.value("why", "");
        d.manual = v.value("m", false);
        d.version = v.value("ver", 0);
        d.decidedAt = v.value("at", int64_t{0});
        if (auto it = v.find("seg"); it != v.end() && it->is_array())
            for (const auto& s : *it) {
                if (!s.is_array() || s.size() < 5) continue;
                video::AlignSegment g;
                g.audioStart = s[0].get<double>();
                g.audioEnd = s[1].get<double>();
                g.videoStart = s[2].get<double>();
                g.slope = s[3].get<double>();
                g.score = s[4].get<double>();
                if (g.audioEnd > g.audioStart && g.slope > 0.9 && g.slope < 1.1) d.segments.push_back(g);
            }
        if (auto it = v.find("rej"); it != v.end() && it->is_array())
            for (const auto& r : *it)
                if (r.is_string()) d.rejected.push_back(r.get<std::string>());
        if (auto it = v.find("tried"); it != v.end() && it->is_object())
            for (const auto& [k, why] : it->items())
                if (why.is_string()) d.tried.emplace_back(k, why.get<std::string>());
        if (d.audioVideoId.empty()) continue;
        entries_.emplace(id, std::move(d));
    }
}

void OfficialVideoCache::flush() {
    if (!dirty_) return;
    dirty_ = false;
    json j = json::object();
    for (const auto& [id, d] : entries_) {
        json seg = json::array();
        for (const auto& s : d.segments) seg.push_back({s.audioStart, s.audioEnd, s.videoStart, s.slope, s.score});
        json tried = json::object();
        for (const auto& [v, why] : d.tried) tried[v] = why;
        j[id] = {{"a", d.audioVideoId}, {"v", d.videoId},          {"t", d.title},      {"c", d.channel},    {"th", d.thumbnailUrl},
                 {"d", d.durationSec},  {"s", d.score},            {"cov", d.coverage}, {"why", d.verdict},  {"m", d.manual},
                 {"ver", d.version},    {"at", d.decidedAt},       {"seg", seg},        {"rej", d.rejected}, {"tried", tried}};
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump();
        if (!f) return;
    }
    std::filesystem::rename(tmp, file_, ec);
}

std::optional<OfficialDecision> OfficialVideoCache::find(const std::string& trackId, const std::string& audioVideoId, int64_t now) const {
    const auto it = entries_.find(trackId);
    if (it == entries_.end()) return std::nullopt;
    const OfficialDecision& d = it->second;
    if (d.audioVideoId != audioVideoId) return std::nullopt;   // another upload plays the song now
    if (d.manual) return d;                                      // the user's choice
    if (d.version != OfficialDecision::kVersion) return std::nullopt;
    // Maybe there is one by now: a week after nothing was found, a month after the candidates didn't match.
    if (!d.hasVideo() && now - d.decidedAt > (d.verdict == "no candidates" ? kNoCandidatesTtlSec : kNoneTtlSec)) return std::nullopt;
    return d;
}

const OfficialDecision* OfficialVideoCache::raw(const std::string& trackId) const {
    const auto it = entries_.find(trackId);
    return it == entries_.end() ? nullptr : &it->second;
}

void OfficialVideoCache::put(const std::string& trackId, OfficialDecision d) {
    if (trackId.empty()) return;
    entries_[trackId] = std::move(d);
    if (entries_.size() > kMaxEntries) {
        std::vector<std::pair<int64_t, std::string>> ages;
        for (const auto& [id, e] : entries_) ages.emplace_back(e.decidedAt, id);
        std::sort(ages.begin(), ages.end());
        size_t excess = entries_.size() - kMaxEntries;
        for (size_t i = 0; excess > 0 && i < ages.size(); ++i)
            if (ages[i].second != trackId) {
                entries_.erase(ages[i].second);
                --excess;
            }
    }
    dirty_ = true;
}

void OfficialVideoCache::erase(const std::string& trackId) {
    if (entries_.erase(trackId)) dirty_ = true;
}

} // namespace st::app
