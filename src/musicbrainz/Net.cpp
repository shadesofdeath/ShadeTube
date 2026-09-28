#include "musicbrainz/Net.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "musicbrainz/MusicBrainz.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace st::mb::net {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;
using CT = YoutubeExplode::CancellationToken;

namespace {

constexpr const char* kUserAgent = "ShadeTube/0.1 ( https://github.com/shadesofdeath/ShadeTube )";
constexpr size_t kLruMaxEntries = 200;
constexpr size_t kLruMaxBytes = 24u * 1024 * 1024;

std::atomic<int> g_netRequests{0}, g_memHits{0}, g_diskHits{0};

// ---- cancellation-aware sleeping --------------------------------------------------------------------------
void sleepUntil(SteadyClock::time_point t, const CT& ct) {
    for (;;) {
        ct.throwIfCancellationRequested();
        const auto now = SteadyClock::now();
        if (now >= t) return;
        std::this_thread::sleep_for(std::min<SteadyClock::duration>(t - now, 50ms));
    }
}

// ---- MusicBrainz: global 1 request / second ----------------------------------------------------------------
std::mutex g_mbMutex;
SteadyClock::time_point g_mbNext{};

void mbAcquireSlot(const CT& ct) {
    SteadyClock::time_point slot;
    {
        std::lock_guard lock(g_mbMutex);
        slot = std::max(SteadyClock::now(), g_mbNext);
        g_mbNext = slot + 1000ms;
    }
    sleepUntil(slot, ct);
}

void mbPushBack(SteadyClock::duration d) {
    std::lock_guard lock(g_mbMutex);
    g_mbNext = std::max(g_mbNext, SteadyClock::now() + d);
}

// ---- ListenBrainz: honour X-RateLimit-* ----------------------------------------------------------------------
std::mutex g_lbMutex;
SteadyClock::time_point g_lbNotBefore{};
std::string g_lbToken;

void lbWait(const CT& ct) {
    SteadyClock::time_point t;
    {
        std::lock_guard lock(g_lbMutex);
        t = g_lbNotBefore;
    }
    sleepUntil(t, ct);
}

int headerInt(const http::HttpResponse& r, const std::string& name, int fallback) {
    if (auto v = r.header(name)) {
        try {
            return std::stoi(*v);
        } catch (...) {
        }
    }
    return fallback;
}

void lbObserve(const http::HttpResponse& r) {
    const int remaining = headerInt(r, "X-RateLimit-Remaining", -1);
    const int resetIn = std::clamp(headerInt(r, "X-RateLimit-Reset-In", 1), 0, 60);
    if (r.statusCode == 429 || remaining == 0) {
        std::lock_guard lock(g_lbMutex);
        g_lbNotBefore = std::max(g_lbNotBefore, SteadyClock::now() + std::chrono::seconds(std::max(resetIn, 1)));
    }
}

// ---- cache ---------------------------------------------------------------------------------------------------
struct LruEntry {
    std::string key;
    std::shared_ptr<const std::string> body;
    SysClock::time_point storedAt;
};

std::mutex g_lruMutex;
std::list<LruEntry> g_lru;   // front = most recent
std::unordered_map<std::string, std::list<LruEntry>::iterator> g_lruIndex;
size_t g_lruBytes = 0;

void lruPut(const std::string& key, std::shared_ptr<const std::string> body, SysClock::time_point storedAt) {
    std::lock_guard lock(g_lruMutex);
    if (auto it = g_lruIndex.find(key); it != g_lruIndex.end()) {
        g_lruBytes -= it->second->body->size();
        g_lru.erase(it->second);
        g_lruIndex.erase(it);
    }
    if (body->size() > kLruMaxBytes / 4) return;   // don't let one huge body evict everything
    g_lruBytes += body->size();
    g_lru.push_front({key, std::move(body), storedAt});
    g_lruIndex[key] = g_lru.begin();
    while (g_lru.size() > kLruMaxEntries || g_lruBytes > kLruMaxBytes) {
        auto& back = g_lru.back();
        g_lruBytes -= back.body->size();
        g_lruIndex.erase(back.key);
        g_lru.pop_back();
    }
}

std::shared_ptr<const std::string> lruGet(const std::string& key, std::chrono::seconds ttl) {
    std::lock_guard lock(g_lruMutex);
    auto it = g_lruIndex.find(key);
    if (it == g_lruIndex.end()) return nullptr;
    if (SysClock::now() - it->second->storedAt > ttl) return nullptr;
    g_lru.splice(g_lru.begin(), g_lru, it->second);
    return it->second->body;
}

fs::path cacheRoot() {
    static const fs::path dir = [] {
        fs::path d = paths::cacheDir() / L"mb";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path cacheFile(const std::string& key) { return cacheRoot() / std::format("{:016x}.json", fnv1a(key)); }

struct DiskEntry {
    std::string body;
    SysClock::time_point storedAt;
};

// File format: "<key>\n<body>" (key line guards against hash collisions).
std::optional<DiskEntry> diskGet(const std::string& key) {
    const fs::path p = cacheFile(key);
    std::error_code ec;
    const auto ftime = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line != key) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    DiskEntry e;
    e.body = std::move(ss).str();
    e.storedAt = std::chrono::clock_cast<SysClock>(ftime);
    return e;
}

void diskPut(const std::string& key, const std::string& body) {
    static std::atomic<uint64_t> counter{0};
    const fs::path finalPath = cacheFile(key);
    fs::path tmp = finalPath;
    tmp += std::format(".{:x}.{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()), counter.fetch_add(1));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << key << '\n' << body;
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, finalPath, ec);   // MoveFileEx(REPLACE_EXISTING): atomic replace on NTFS
    if (ec) {
        ST_LOG_WARN("mb", "cache write failed: {}", ec.message());
        fs::remove(tmp, ec);
    }
}

std::string requestKey(const Request& r) {
    std::string k = r.method + " " + r.url;
    if (!r.body.empty()) k += " " + std::format("{:016x}", fnv1a(r.body));
    return k;
}

std::optional<nlohmann::json> parseBody(const std::string& body) {
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

bool isRetryableStatus(int s) { return s == 503 || s == 502 || s == 504 || s == 429 || s == 500; }

// One network round trip with service etiquette. Returns the final response (possibly non-2xx).
http::HttpResponse sendWithPolicy(const Request& r, const CT& ct) {
    http::HttpRequest req;
    req.method = r.method;
    req.url = r.url;
    req.headers.emplace_back("User-Agent", kUserAgent);
    req.headers.emplace_back("Accept", "application/json");
    if (!r.body.empty()) {
        req.body = r.body;
        req.headers.emplace_back("Content-Type", "application/json");
    }
    if (r.service == Service::ListenBrainz && r.sendToken) {
        std::lock_guard lock(g_lbMutex);
        if (!g_lbToken.empty()) req.headers.emplace_back("Authorization", "Token " + g_lbToken);
    }

    constexpr int kMaxRetries = 3;
    http::HttpResponse resp;
    for (int attempt = 0;; ++attempt) {
        switch (r.service) {
        case Service::MusicBrainz: mbAcquireSlot(ct); break;
        case Service::ListenBrainz: lbWait(ct); break;
        case Service::Wikidata: ct.throwIfCancellationRequested(); break;
        }
        ++g_netRequests;
        resp = http::send(req, ct);
        if (r.service == Service::ListenBrainz) lbObserve(resp);
        if (!isRetryableStatus(resp.statusCode) || attempt >= kMaxRetries) return resp;

        // Back off: MusicBrainz 1 s, 2 s, 4 s (shared with every thread); ListenBrainz waits Reset-In on 429.
        const auto backoff = std::chrono::seconds(1 << attempt);
        ST_LOG_WARN("mb", "HTTP {} for {} (attempt {}), backing off", resp.statusCode, r.url, attempt + 1);
        if (r.service == Service::MusicBrainz) {
            mbPushBack(backoff);
        } else if (r.service == Service::ListenBrainz && resp.statusCode == 429) {
            // lbObserve already scheduled g_lbNotBefore; lbWait honours it on the next attempt.
            continue;
        } else {
            sleepUntil(SteadyClock::now() + backoff, ct);
        }
    }
}

} // namespace

uint64_t fnv1a(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::optional<nlohmann::json> fetch(const Request& r, const CT& ct) {
    ct.throwIfCancellationRequested();
    const std::string key = requestKey(r);

    if (auto body = lruGet(key, r.ttl)) {
        if (auto j = parseBody(*body)) {
            ++g_memHits;
            return j;
        }
    }

    std::optional<DiskEntry> stale;
    if (auto disk = diskGet(key)) {
        if (SysClock::now() - disk->storedAt <= r.ttl) {
            if (auto j = parseBody(disk->body)) {
                ++g_diskHits;
                lruPut(key, std::make_shared<const std::string>(std::move(disk->body)), disk->storedAt);
                return j;
            }
        } else {
            stale = std::move(disk);
        }
    }
    if (r.cacheOnly) return std::nullopt;

    auto useStale = [&]() -> std::optional<nlohmann::json> {
        if (!stale) return std::nullopt;
        ST_LOG_WARN("mb", "serving stale cache for {}", r.url);
        return parseBody(stale->body);
    };

    http::HttpResponse resp;
    try {
        resp = sendWithPolicy(r, ct);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception&) {
        if (ct.isCancellationRequested()) throw;
        if (auto j = useStale()) return j;
        throw;
    }

    if (!resp.isSuccessStatusCode()) {
        if (resp.statusCode >= 500 || resp.statusCode == 429)
            if (auto j = useStale()) return j;
        std::string msg = std::format("HTTP {} from {}", resp.statusCode, r.url);
        if (auto j = parseBody(resp.body); j && j->is_object()) {
            if (auto it = j->find("error"); it != j->end() && it->is_string()) msg += ": " + it->get<std::string>();
        }
        throw ApiError(resp.statusCode, msg);
    }

    auto j = parseBody(resp.body);
    if (!j) {
        if (auto s = useStale()) return s;
        throw ApiError(resp.statusCode, "invalid JSON from " + r.url);
    }
    if (!r.store) return j;
    diskPut(key, resp.body);
    lruPut(key, std::make_shared<const std::string>(std::move(resp.body)), SysClock::now());
    return j;
}

std::optional<std::string> cacheGet(const std::string& key, std::chrono::seconds ttl) {
    if (auto body = lruGet(key, ttl)) return *body;
    if (auto disk = diskGet(key); disk && SysClock::now() - disk->storedAt <= ttl) {
        lruPut(key, std::make_shared<const std::string>(disk->body), disk->storedAt);
        return std::move(disk->body);
    }
    return std::nullopt;
}

void cachePut(const std::string& key, const std::string& value) {
    diskPut(key, value);
    lruPut(key, std::make_shared<const std::string>(value), SysClock::now());
}

void setListenBrainzToken(std::string token) {
    std::lock_guard lock(g_lbMutex);
    g_lbToken = std::move(token);
}

bool hasListenBrainzToken() {
    std::lock_guard lock(g_lbMutex);
    return !g_lbToken.empty();
}

Stats stats() { return {g_netRequests.load(), g_memHits.load(), g_diskHits.load()}; }

void clearMemoryCache() {
    std::lock_guard lock(g_lruMutex);
    g_lru.clear();
    g_lruIndex.clear();
    g_lruBytes = 0;
}

} // namespace st::mb::net
