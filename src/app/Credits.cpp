#include "app/Credits.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/SpotifyApi.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace st::app::credits {

using catalog::Credits;
using json = nlohmann::json;
namespace fs = std::filesystem;
using OperationCanceled = YoutubeExplode::Exceptions::OperationCanceledException;

namespace {

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::mutex g_mutex;
fs::path g_dir;   // empty = paths::cacheDir() / "credits"
struct Entry {
    Credits credits;
    uint64_t used = 0;
};
std::map<std::string, Entry> g_memory;
uint64_t g_clock = 0;

fs::path cacheFile(const std::string& key) {
    if (key.empty()) return {};
    std::string safe;
    for (char c : key)
        safe.push_back((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ? c : '_');
    fs::path dir;
    {
        std::lock_guard lock(g_mutex);
        dir = g_dir.empty() ? paths::cacheDir() / L"credits" : g_dir;
    }
    return dir / (safe + ".json");
}

void remember(const std::string& key, const Credits& c) {
    std::lock_guard lock(g_mutex);
    g_memory[key] = {c, ++g_clock};
    while (g_memory.size() > kMemoryEntries) {
        auto oldest = std::min_element(g_memory.begin(), g_memory.end(),
                                       [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        g_memory.erase(oldest);
    }
}

void writeFile(const fs::path& p, const std::string& text) {
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return;
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

bool isMbid(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string toJson(const Credits& c, int64_t now) {
    json groups = json::array();
    for (const auto& g : c.groups) {
        json list = json::array();
        for (const auto& p : g) list.push_back(json::array({p.name, p.artistId, p.roles}));
        groups.push_back(std::move(list));
    }
    return json{{"v", 1}, {"at", now}, {"provider", c.provider}, {"groups", std::move(groups)}, {"sources", c.sources}}.dump();
}

std::optional<Credits> fromJson(const std::string& text, int64_t now) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != 1) return std::nullopt;
    const auto at = j.contains("at") && j["at"].is_number_integer() ? j["at"].get<int64_t>() : 0;
    Credits c;
    c.provider = j.value("provider", std::string());
    if (auto it = j.find("groups"); it != j.end() && it->is_array()) {
        for (size_t g = 0; g < it->size() && g < catalog::kCreditGroups; ++g) {
            const json& list = (*it)[g];
            if (!list.is_array()) continue;
            for (const auto& p : list) {
                if (!p.is_array() || p.size() != 3 || !p[0].is_string() || !p[1].is_string() || !p[2].is_array()) continue;
                catalog::Credit person{p[0].get<std::string>(), p[1].get<std::string>(), {}};
                for (const auto& r : p[2])
                    if (r.is_string()) person.roles.push_back(r.get<std::string>());
                if (!person.name.empty()) c.groups[g].push_back(std::move(person));
            }
        }
    }
    if (auto it = j.find("sources"); it != j.end() && it->is_array())
        for (const auto& s : *it)
            if (s.is_string()) c.sources.push_back(s.get<std::string>());
    if (now - at > (c.empty() ? kNoneTtlSec : kFoundTtlSec)) return std::nullopt;   // expired
    return c;
}

std::optional<Credits> cached(const std::string& key) {
    if (key.empty()) return std::nullopt;
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_memory.find(key); it != g_memory.end()) {
            it->second.used = ++g_clock;
            return it->second.credits;
        }
    }
    const fs::path p = cacheFile(key);
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto c = fromJson(text, nowUnix());
    if (c) remember(key, *c);
    return c;
}

std::optional<Credits> fetch(const Request& r, const CT& ct) {
    if (auto c = cached(r.cacheKey)) return c;
    if (r.providers.empty()) return Credits{};   // nobody to ask (no artist to search by): nobody known
    std::optional<Credits> best;
    bool failed = false;
    for (const auto& provider : r.providers) {
        if (!provider) continue;
        std::optional<Credits> answer;
        try {
            answer = provider(ct);
        } catch (const OperationCanceled&) {
            throw;
        } catch (const std::exception& e) {
            if (ct.isCancellationRequested()) throw;
            ST_LOG_WARN("credits", "provider failed for {}: {}", r.cacheKey, e.what());
        }
        if (!answer) {
            failed = true;
            continue;
        }
        if (!answer->empty()) {
            best = std::move(answer);
            break;
        }
        if (!best) best = std::move(answer);   // "nobody" (its sources, if any, are kept)
    }
    if (!best) return std::nullopt;
    if (!best->empty() || !failed) {
        remember(r.cacheKey, *best);
        writeFile(cacheFile(r.cacheKey), toJson(*best, nowUnix()));
    }
    return best;
}

Request requestFor(spotify::Api* api, const catalog::Track& t) {
    Request r;
    r.cacheKey = t.id.empty() ? t.name : t.id;
    if (api && t.id.rfind("spotify:track:", 0) == 0) {
        r.providers.push_back([api, id = t.id](const CT& ct) -> std::optional<Credits> {
            try {
                return api->trackCredits(id, ct);
            } catch (const spotify::ApiError& e) {
                if (ct.isCancellationRequested()) throw;
                ST_LOG_WARN("credits", "spotify credits for {} failed: {}", id, e.what());
                return std::nullopt;
            }
        });
    }
    // By name only with a title and an artist: a title alone finds somebody else's song of that name.
    const std::string artist = t.artists.empty() ? std::string() : t.artists.front().name;
    if ((t.name.empty() || artist.empty()) && !isMbid(t.id)) return r;
    r.providers.push_back([id = t.id, title = t.name, artist, durationMs = t.durationMs,
                           albumId = t.album.id](const CT& ct) -> std::optional<Credits> {
        try {
            // The recording itself, or the song looked up by name (empty: MusicBrainz doesn't know it).
            Credits c = isMbid(id) ? mb::recordingCredits(id, ct) : mb::songCredits(title, artist, durationMs, ct);
            if (c.sources.empty() && isMbid(albumId)) {
                try {
                    if (const auto album = mb::album(albumId, ct); !album.label.empty()) c.addSource(album.label);
                } catch (const OperationCanceled&) {
                    throw;
                } catch (const std::exception& e) {
                    ST_LOG_DEBUG("credits", "album label for {}: {}", albumId, e.what());
                }
            }
            return c;
        } catch (const OperationCanceled&) {
            throw;
        } catch (const std::exception& e) {
            if (ct.isCancellationRequested()) throw;
            ST_LOG_WARN("credits", "musicbrainz credits for '{}' failed: {}", title, e.what());
            return std::nullopt;
        }
    });
    return r;
}

std::vector<catalog::Credit> summary(const Credits& c, size_t max) {
    std::vector<catalog::Credit> out;
    auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            auto low = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
            if (low(a[i]) != low(b[i])) return false;
        }
        return true;
    };
    auto take = [&](const catalog::Credit& p) {
        auto it = std::find_if(out.begin(), out.end(), [&](const catalog::Credit& x) { return same(x.name, p.name); });
        if (it == out.end()) {
            if (out.size() < max) out.push_back(p);
            return;
        }
        if (it->artistId.empty()) it->artistId = p.artistId;
        for (const auto& r : p.roles)
            if (std::find(it->roles.begin(), it->roles.end(), r) == it->roles.end()) it->roles.push_back(r);
    };
    for (const auto& g : c.groups)
        for (size_t i = 0; i < g.size() && i < 2; ++i) take(g[i]);
    for (const auto& g : c.groups)
        for (const auto& p : g) take(p);   // the rest fills up and adds roles to the people already listed
    return out;
}

void setCacheDir(fs::path dir) {
    std::lock_guard lock(g_mutex);
    g_dir = std::move(dir);
}

void clearMemory() {
    std::lock_guard lock(g_mutex);
    g_memory.clear();
}

} // namespace st::app::credits
