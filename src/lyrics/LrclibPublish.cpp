#include "lyrics/LrclibPublish.h"

#include "core/Http.h"
#include "core/Log.h"
#include "lyrics/Lyrics.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <thread>
#include <vector>

namespace st::lyrics::lrclib {

using json = nlohmann::json;

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

Hash digestOf(const uint32_t state[8]) {
    Hash out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return out;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

YoutubeExplode::Http::HttpResponse sendVia(const Transport& t, const YoutubeExplode::Http::HttpRequest& r,
                                           const YoutubeExplode::CancellationToken& ct) {
    if (t) return t(r, ct);
    return http::send(r, ct);
}

std::string messageOf(const std::string& body, std::string* name = nullptr) {
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return {};
    if (name && j.contains("name") && j["name"].is_string()) *name = j["name"].get<std::string>();
    return j.contains("message") && j["message"].is_string() ? j["message"].get<std::string>() : std::string();
}

} // namespace

Hash sha256(std::string_view data) {
    uint32_t state[8];
    std::memcpy(state, kInit, sizeof state);
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size();
    const uint64_t bits = static_cast<uint64_t>(n) * 8;
    while (n >= 64) {
        compress(state, p);
        p += 64;
        n -= 64;
    }
    uint8_t block[128] = {};
    std::memcpy(block, p, n);
    block[n] = 0x80;
    const size_t total = n + 9 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; ++i) block[total - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    compress(state, block);
    if (total == 128) compress(state, block + 64);
    return digestOf(state);
}

std::string toHex(const Hash& h) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(64);
    for (uint8_t b : h) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

bool parseTarget(std::string_view hex, Hash& out) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hexValue(hex[i * 2]), lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

bool meetsTarget(const Hash& hash, const Hash& target) {
    for (size_t i = 0; i < hash.size(); ++i) {
        if (hash[i] > target[i]) return false;
        if (hash[i] < target[i]) return true;
    }
    return true;   // equal
}

std::optional<Challenge> parseChallenge(const std::string& body) {
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("prefix") || !j.contains("target") || !j["prefix"].is_string() || !j["target"].is_string())
        return std::nullopt;
    Challenge c{j["prefix"].get<std::string>(), j["target"].get<std::string>()};
    Hash t{};
    if (c.prefix.empty() || c.prefix.size() > 512 || !parseTarget(c.target, t)) return std::nullopt;
    return c;
}

std::optional<uint64_t> solve(const Challenge& c, const YoutubeExplode::CancellationToken& ct, unsigned threads,
                              uint64_t maxTries) {
    Hash target{};
    if (!parseTarget(c.target, target) || c.prefix.empty()) return std::nullopt;
    if (threads == 0) threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> found{UINT64_MAX};
    const std::string& prefix = c.prefix;
    // The message is the prefix and up to 20 digits: one 64-byte block while it fits in 55 bytes (the usual 32-char
    // prefix), built once per thread with only the digits and the length rewritten per try.
    const bool oneBlock = prefix.size() + 20 <= 55;
    auto worker = [&](unsigned first) {
        uint8_t block[64];
        std::memset(block, 0, sizeof block);
        std::memcpy(block, prefix.data(), oneBlock ? prefix.size() : 0);
        std::string message = prefix;
        size_t lastLen = 0;
        uint64_t sinceCheck = 0;
        for (uint64_t n = first; !done.load(std::memory_order_relaxed); n += threads) {
            if (maxTries && n >= maxTries) return;
            if (++sinceCheck >= 16384) {
                sinceCheck = 0;
                if (ct.isCancellationRequested()) {
                    done = true;
                    return;
                }
            }
            char digits[24];
            const auto r = std::to_chars(digits, digits + sizeof digits, n);
            const size_t len = static_cast<size_t>(r.ptr - digits);
            Hash h;
            if (oneBlock) {
                const size_t total = prefix.size() + len;
                if (len != lastLen) {   // a new digit count: clear the tail behind the digits
                    std::memset(block + prefix.size(), 0, 64 - prefix.size());
                    lastLen = len;
                }
                std::memcpy(block + prefix.size(), digits, len);
                block[total] = 0x80;
                const uint64_t bits = static_cast<uint64_t>(total) * 8;
                for (int i = 0; i < 8; ++i) block[63 - i] = static_cast<uint8_t>(bits >> (8 * i));
                uint32_t state[8];
                std::memcpy(state, kInit, sizeof state);
                compress(state, block);
                h = digestOf(state);
            } else {
                message.resize(prefix.size());
                message.append(digits, len);
                h = sha256(message);
            }
            if (meetsTarget(h, target)) {
                uint64_t prev = found.load();
                while (n < prev && !found.compare_exchange_weak(prev, n)) {
                }
                done = true;
                return;
            }
        }
    };
    if (threads == 1) {
        worker(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker, t);
        for (auto& t : pool) t.join();
    }
    const uint64_t n = found.load();
    if (n == UINT64_MAX) return std::nullopt;
    return n;
}

std::string publishToken(const Challenge& c, uint64_t nonce) { return c.prefix + ":" + std::to_string(nonce); }

std::string publishBody(const Submission& s) {
    json j{{"trackName", s.title}, {"artistName", s.artist}, {"albumName", s.album}, {"duration", s.durationSec}};
    if (!s.plainLyrics.empty()) j["plainLyrics"] = s.plainLyrics;
    if (!s.syncedLyrics.empty()) j["syncedLyrics"] = s.syncedLyrics;
    return j.dump();
}

bool publishable(const Submission& s) {
    return !s.title.empty() && !s.artist.empty() && s.durationSec >= 1 && s.durationSec <= 3600 &&
           (!s.syncedLyrics.empty() || !s.plainLyrics.empty());
}

Outcome publish(const Submission& s, const YoutubeExplode::CancellationToken& ct, const Transport& transport,
                const std::function<void()>& onSolving) {
    Outcome out;
    if (!publishable(s)) {
        out.kind = Outcome::Kind::Rejected;
        out.message = "incomplete track signature";
        return out;
    }
    const std::string ua = userAgent();
    try {
        YoutubeExplode::Http::HttpRequest req;
        req.method = "POST";
        req.url = std::string(kBaseUrl) + "/api/request-challenge";
        req.headers = {{"User-Agent", ua}, {"Accept", "application/json"}};
        auto resp = sendVia(transport, req, ct);
        if (resp.statusCode != 200) {
            out.status = resp.statusCode;
            out.message = messageOf(resp.body);
            ST_LOG_WARN("lyrics", "lrclib challenge HTTP {}", resp.statusCode);
            return out;
        }
        const auto challenge = parseChallenge(resp.body);
        if (!challenge) {
            ST_LOG_WARN("lyrics", "lrclib challenge: unexpected answer");
            return out;
        }
        if (onSolving) onSolving();
        const auto nonce = solve(*challenge, ct);
        if (!nonce) {
            out.kind = ct.isCancellationRequested() ? Outcome::Kind::Cancelled : Outcome::Kind::Failed;
            return out;
        }
        req.url = std::string(kBaseUrl) + "/api/publish";
        req.headers = {{"User-Agent", ua},
                       {"Accept", "application/json"},
                       {"Content-Type", "application/json"},
                       {"X-Publish-Token", publishToken(*challenge, *nonce)}};
        req.body = publishBody(s);
        resp = sendVia(transport, req, ct);
        out.status = resp.statusCode;
        std::string name;
        out.message = messageOf(resp.body, &name);
        if (resp.statusCode == 201 || resp.statusCode == 200) out.kind = Outcome::Kind::Published;
        else if (resp.statusCode == 400 && name == "IncorrectPublishTokenError") out.kind = Outcome::Kind::BadToken;
        else if (resp.statusCode >= 400 && resp.statusCode < 500 && resp.statusCode != 429) out.kind = Outcome::Kind::Rejected;
        else out.kind = Outcome::Kind::Failed;
        ST_LOG_INFO("lyrics", "lrclib publish: HTTP {}", resp.statusCode);
        return out;
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        out.kind = Outcome::Kind::Cancelled;
        return out;
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyrics", "lrclib publish failed: {}", e.what());
        out.kind = ct.isCancellationRequested() ? Outcome::Kind::Cancelled : Outcome::Kind::Failed;
        return out;
    }
}

} // namespace st::lyrics::lrclib
