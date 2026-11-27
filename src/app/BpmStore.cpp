#include "app/BpmStore.h"

#include "core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace st::app::bpm {

using json = nlohmann::json;

namespace {
bool sameValues(const Entry& a, const Entry& b) {
    return std::abs(a.bpm - b.bpm) < 0.05f && a.source == b.source && a.stamp == b.stamp;
}
} // namespace

bool Store::put(Entry e) {
    e.bpm = std::isfinite(e.bpm) ? std::clamp(e.bpm, 0.f, 999.f) : 0.f;
    std::sort(e.ids.begin(), e.ids.end());
    e.ids.erase(std::unique(e.ids.begin(), e.ids.end()), e.ids.end());
    std::erase_if(e.ids, [](const std::string& s) { return s.empty(); });
    // A tag holds its ids against an analysis.
    if (e.source == Source::Analysis) {
        std::erase_if(e.ids, [&](const std::string& id) {
            const auto it = byId_.find(id);
            return it != byId_.end() && entries_[it->second].source == Source::Tag;
        });
        if (!e.key.empty())
            if (const auto it = byKey_.find(e.key); it != byKey_.end() && entries_[it->second].source == Source::Tag) e.key.clear();
    }
    if (e.ids.empty() && e.key.empty()) return false;
    // Nothing new: every id (and the key) already points at an entry with these values.
    const bool unchanged = std::all_of(e.ids.begin(), e.ids.end(), [&](const std::string& id) {
        const auto it = byId_.find(id);
        return it != byId_.end() && sameValues(entries_[it->second], e);
    }) && (e.key.empty() || [&] {
        const auto it = byKey_.find(e.key);
        return it != byKey_.end() && sameValues(entries_[it->second], e);
    }());
    if (unchanged) return false;
    // The ids and the key leave the entries that had them.
    for (const auto& id : e.ids)
        if (const auto it = byId_.find(id); it != byId_.end()) std::erase(entries_[it->second].ids, id);
    if (!e.key.empty())
        if (const auto it = byKey_.find(e.key); it != byKey_.end()) entries_[it->second].key.clear();
    const auto idx = static_cast<uint32_t>(entries_.size());
    for (const auto& id : e.ids) byId_[id] = idx;
    if (!e.key.empty()) byKey_[e.key] = idx;
    entries_.push_back(std::move(e));
    // Entries left with nothing are dropped once they are a good part of the store.
    const size_t orphans = std::count_if(entries_.begin(), entries_.end(), [](const Entry& x) { return x.ids.empty() && x.key.empty(); });
    if (orphans > 64 && orphans * 4 > entries_.size()) {
        std::erase_if(entries_, [](const Entry& x) { return x.ids.empty() && x.key.empty(); });
        reindex();
    }
    ++rev_;
    return true;
}

void Store::reindex() {
    byId_.clear();
    byKey_.clear();
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        for (const auto& id : entries_[i].ids) byId_[id] = i;
        if (!entries_[i].key.empty()) byKey_[entries_[i].key] = i;
    }
}

const Entry* Store::byId(const std::string& id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &entries_[it->second];
}

const Entry* Store::byKey(const std::wstring& key) const {
    const auto it = byKey_.find(key);
    return it == byKey_.end() ? nullptr : &entries_[it->second];
}

float Store::of(const std::string& id, const std::string& videoId, const std::wstring& key) const {
    if (!id.empty())
        if (const Entry* e = byId(id); e && e->bpm > 0) return e->bpm;
    if (!videoId.empty())
        if (const Entry* e = byId("yt:" + videoId); e && e->bpm > 0) return e->bpm;
    if (!key.empty())
        if (const Entry* e = byKey(key); e && e->bpm > 0) return e->bpm;
    return 0;
}

bool Store::current(const std::string& id, uint64_t stamp) const {
    const Entry* e = byId(id);
    return e && (stamp == 0 || e->stamp == 0 || e->stamp == stamp);
}

void Store::clearAnalysis() {
    const size_t before = entries_.size();
    std::erase_if(entries_, [](const Entry& e) { return e.source == Source::Analysis; });
    if (entries_.size() == before) return;
    reindex();
    ++rev_;
}

bool Store::forget(const std::vector<std::string>& ids) {
    bool changed = false;
    for (const auto& id : ids)
        if (const auto it = byId_.find(id); it != byId_.end()) {
            std::erase(entries_[it->second].ids, id);
            byId_.erase(it);
            changed = true;
        }
    if (changed) ++rev_;
    return changed;
}

size_t Store::known() const {
    size_t n = 0;
    for (const auto& [id, i] : byId_)
        if (entries_[i].bpm > 0) ++n;
    return n;
}

size_t Store::known(Source from) const {
    size_t n = 0;
    for (const auto& [id, i] : byId_)
        if (entries_[i].bpm > 0 && entries_[i].source == from) ++n;
    return n;
}

json Store::toJson() const {
    json list = json::array();
    for (const auto& e : entries_) {
        if (e.ids.empty() && e.key.empty()) continue;
        char stamp[24];
        std::snprintf(stamp, sizeof stamp, "%llx", static_cast<unsigned long long>(e.stamp));
        json j = {{"b", std::round(e.bpm * 10) / 10}, {"s", e.source == Source::Tag ? "t" : "a"}, {"t", e.at}, {"i", e.ids}};
        if (e.source == Source::Analysis) j["c"] = std::round(e.confidence * 100) / 100;
        if (e.stamp) j["f"] = stamp;
        if (!e.key.empty()) j["k"] = toUtf8(e.key);
        list.push_back(std::move(j));
    }
    return {{"v", 1}, {"e", std::move(list)}};
}

Store Store::fromJson(const json& j) {
    Store s;
    if (!j.is_object() || !j.contains("v") || !j["v"].is_number_integer() || j["v"].get<int>() != 1) return s;
    const auto list = j.find("e");
    if (list == j.end() || !list->is_array()) return s;
    for (const auto& x : *list) {
        if (!x.is_object()) continue;
        Entry e;
        if (const auto b = x.find("b"); b != x.end() && b->is_number()) e.bpm = b->get<float>();
        e.source = x.contains("s") && x["s"].is_string() && x["s"].get<std::string>() == "t" ? Source::Tag : Source::Analysis;
        if (const auto c = x.find("c"); c != x.end() && c->is_number()) e.confidence = c->get<float>();
        else if (e.source == Source::Tag) e.confidence = 1;
        if (const auto t = x.find("t"); t != x.end() && t->is_number_integer()) e.at = t->get<int64_t>();
        if (const auto f = x.find("f"); f != x.end() && f->is_string()) e.stamp = std::strtoull(f->get<std::string>().c_str(), nullptr, 16);
        if (const auto ids = x.find("i"); ids != x.end() && ids->is_array())
            for (const auto& id : *ids)
                if (id.is_string()) e.ids.push_back(id.get<std::string>());
        if (const auto k = x.find("k"); k != x.end() && k->is_string()) e.key = toWide(k->get<std::string>());
        s.put(std::move(e));
    }
    s.rev_ = 1;
    return s;
}

Store Store::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return fromJson(json::parse(ss.str(), nullptr, false));
}

bool Store::save(const json& j, const std::filesystem::path& file) {
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(-1, ' ', false, json::error_handler_t::replace);
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

uint64_t stampOf(int64_t size, uint64_t mtime) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : {static_cast<uint64_t>(size), mtime})
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    return h ? h : 1;
}

std::wstring label(float bpm) {
    if (!(bpm > 0)) return {};
    return std::to_wstring(static_cast<int>(std::lround(bpm)));
}

} // namespace st::app::bpm
