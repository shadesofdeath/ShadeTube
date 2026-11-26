// Developer test for the song cache (player/AudioCache; offline: no internet needed):
//   1. The store: a stream stored under its video + itag is found for the stream choice (profile) that picked it only,
//      findFormat() adds another profile, findAny() serves any format (Opus only when allowed) or the track's last
//      video; what is not cacheable (SoundCloud, no itag, odd ids) is refused; the index survives a reload with every
//      field (match, loudness, track ids, SponsorBlock data, also data set before the stream was stored).
//   2. Safety: ".part" leftovers, orphan files and entries whose file is gone or has another length are dropped by
//      load(); a file changed in its head, its tail or a sampled block, or truncated later, fails the lookup's check and
//      is dropped (a byte between the samples is not seen: the hash is a cheap one).
//   3. Size: LRU eviction under a small limit (a lookup refreshes an entry), a lower limit + trim(), a stream larger
//      than a quarter of the limit, off (limit 0), a nearly full disk (a free-space probe) shrinking the cache and
//      refusing to fill it; removeVideo(), clear().
//   4. The playback buffer's completion hook against an in-process HTTP server answering ranged requests: called once
//      with the exact bytes, also after a far seek restarted the download; never for a cancelled download.
//   5. The engine: a WAV streamed over HTTP with StreamSource::onComplete lands in the cache, and the cached file plays.
//   6. A real player::Player with SHADETUBE_OFFLINE=1 (YouTube unreachable): pinned songs play from the cache with
//      their match, the next one is preloaded from it (a gapless handoff, no resolve in between), a preview plays from
//      it, another quality setting still plays the cached format once resolving fails, and a damaged file is dropped
//      instead of played.
//   audiocache_test
// Runs in a temporary profile (SHADETUBE_DATA_DIR); parts 5 and 6 need an audio output device.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "audio/AudioEngine.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Dispatcher.h"
#include "core/Http.h"
#include "core/Settings.h"
#include "player/AudioCache.h"
#include "player/Player.h"
#include "youtube/MatchService.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using st::player::AudioCache;
using st::player::CachedSong;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

constexpr int64_t kKB = 1024;
constexpr int64_t kHugeFree = 1ll << 40;

std::string randomBytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(rng());
    return s;
}

// A stream as MatchService would hand it out.
st::youtube::Resolved resolved(const std::string& videoId, int itag, const std::string& mime = "audio/mp4", int kbps = 128) {
    st::youtube::Resolved r;
    r.match.videoId = videoId;
    r.match.title = "Video " + videoId;
    r.match.channel = "Channel";
    r.match.durationSec = 200;
    r.match.score = 87;
    r.stream.itag = itag;
    r.stream.mimeType = mime;
    r.stream.codec = mime == "audio/webm" ? "opus" : "mp4a.40.2";
    r.stream.bitrateKbps = kbps;
    r.stream.loudnessDb = -6.5;
    r.stream.source = "youtube";
    return r;
}

bool put(AudioCache& c, const std::string& videoId, int itag, const std::string& bytes, const std::string& profile = "wh",
         const std::string& trackId = "t:1", const std::string& mime = "audio/mp4", int kbps = 128) {
    auto song = AudioCache::songFor(resolved(videoId, itag, mime, kbps), profile, trackId);
    return song && c.store(*song, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int64_t>(bytes.size()));
}

void patchFile(const fs::path& p, int64_t offset, char value) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(offset);
    f.put(value);
}

size_t filesIn(const fs::path& dir) {
    std::error_code ec;
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename() != L"index.json") ++n;
    return n;
}

// ---- 1-3: the store ------------------------------------------------------------------------------------------------

void testStore(const fs::path& tmp) {
    std::printf("store: profiles, formats, fallbacks, persistence\n");
    const fs::path dir = tmp / L"store";
    const std::string a = randomBytes(300 * kKB, 1), b = randomBytes(200 * kKB, 2);
    {
        AudioCache c(dir);
        c.setFreeSpaceProbe([] { return kHugeFree; });
        c.setLimit(64ll << 20);
        CHECK(!AudioCache::songFor(resolved("SoundCloud1", 0), "wh", "x"));   // no itag
        auto sc = resolved("AAAAAAAAAAA", 140);
        sc.stream.source = "soundcloud";
        CHECK(!AudioCache::songFor(sc, "wh", "x"));
        CHECK(!AudioCache::songFor(resolved("../evil", 140), "wh", "x"));
        CHECK(!put(c, "AAAAAAAAAAA", 140, std::string(1000, 'x')));       // too small to be a song
        // The sponsor data arrives before the stream is complete (it is looked up when the song starts).
        c.setSponsor("AAAAAAAAAAA", "c:sponsor,", R"([[1000,5000,"sponsor",200000]])");
        CHECK(put(c, "AAAAAAAAAAA", 251, a, "wh", "spotify:track:one", "audio/webm", 160));
        CHECK(!put(c, "AAAAAAAAAAA", 251, a, "wh", "spotify:track:two", "audio/webm", 160));   // there already
        CHECK(c.stats().count == 1 && c.stats().bytes == 300 * kKB);
        CHECK(c.find("AAAAAAAAAAA", "mh", "") == std::nullopt);   // another quality picks another format
        auto hit = c.find("AAAAAAAAAAA", "wh", "spotify:track:one");
        CHECK(hit && hit->song.key == "AAAAAAAAAAA-251" && fs::file_size(hit->path) == a.size());
        CHECK(hit && fs::path(hit->path).filename() == L"AAAAAAAAAAA-251.webm");
        // The other profile resolved to the same itag: it learns it.
        CHECK(c.findFormat("AAAAAAAAAAA", 251, "ml", "") && c.find("AAAAAAAAAAA", "ml", ""));
        // Offline fallback: Opus only when the engine decodes it; an MP4 of the same video serves otherwise.
        CHECK(!c.findAny("AAAAAAAAAAA", "", false));
        CHECK(put(c, "AAAAAAAAAAA", 140, b, "mh", "spotify:track:one", "audio/mp4", 128));
        auto any = c.findAny("AAAAAAAAAAA", "", false);
        CHECK(any && any->song.itag == 140);
        any = c.findAny("AAAAAAAAAAA", "", true);
        CHECK(any && any->song.itag == 251);   // the higher bit rate first
        // No video known: the one the track played from last.
        any = c.findAny("", "spotify:track:one", true);
        CHECK(any && any->song.videoId == "AAAAAAAAAAA");
        CHECK(!c.findAny("", "spotify:track:unknown", true));
        CHECK(c.hasVideo("AAAAAAAAAAA") && !c.hasVideo("BBBBBBBBBBB"));
        CHECK(c.sponsor("AAAAAAAAAAA", "c:sponsor,") == std::optional<std::string>(R"([[1000,5000,"sponsor",200000]])"));
        CHECK(!c.sponsor("AAAAAAAAAAA", "c:other,"));
        c.flush();
    }
    {
        AudioCache c(dir);   // a new run
        c.setLimit(64ll << 20);
        CHECK(c.stats().count == 0);   // nothing before load()
        c.load();
        CHECK(c.stats().count == 2 && c.stats().bytes == 500 * kKB);
        const auto hit = c.find("AAAAAAAAAAA", "wh", "");
        CHECK(hit.has_value());
        if (hit) {
            const CachedSong& s = hit->song;
            CHECK(s.match.title == "Video AAAAAAAAAAA" && s.match.channel == "Channel" && s.match.durationSec == 200);
            CHECK(s.match.score == 87 && s.loudnessDb && std::abs(*s.loudnessDb + 6.5) < 1e-9);
            CHECK(s.mimeType == "audio/webm" && s.codec == "opus" && s.bitrateKbps == 160 && s.source == "youtube");
            CHECK(std::find(s.trackIds.begin(), s.trackIds.end(), "spotify:track:one") != s.trackIds.end());
            CHECK(std::find(s.trackIds.begin(), s.trackIds.end(), "spotify:track:two") != s.trackIds.end());
            CHECK(std::find(s.profiles.begin(), s.profiles.end(), "ml") != s.profiles.end());
            const auto st = s.stream();
            CHECK(st.url.empty() && st.itag == 251 && st.contentLength == 300 * kKB && st.source == "cache");
        }
        CHECK(c.sponsor("AAAAAAAAAAA", "c:sponsor,").has_value());
        c.setSponsor("AAAAAAAAAAA", "c:sponsor,", "[]");
        CHECK(c.dirty());
        c.flush();
        c.removeVideo("AAAAAAAAAAA");
        CHECK(c.stats().count == 0 && filesIn(dir) == 0);
    }
}

void testSafety(const fs::path& tmp) {
    std::printf("safety: leftovers, orphans, damaged files\n");
    const fs::path dir = tmp / L"safety";
    const std::string bytes = randomBytes(256 * kKB, 7);
    {
        AudioCache c(dir);
        c.setFreeSpaceProbe([] { return kHugeFree; });
        c.setLimit(64ll << 20);
        for (const char* id : {"good0000000", "gone0000000", "short000000"}) CHECK(put(c, id, 140, bytes));
        c.flush();
    }
    std::error_code ec;
    fs::remove(dir / L"gone0000000-140.m4a", ec);
    fs::resize_file(dir / L"short000000-140.m4a", 100 * kKB, ec);
    std::ofstream(dir / L"left0000000-140.part", std::ios::binary) << "half a download";
    std::ofstream(dir / L"orphan.m4a", std::ios::binary) << "nobody knows me";
    {
        AudioCache c(dir);
        c.setFreeSpaceProbe([] { return kHugeFree; });
        c.setLimit(64ll << 20);
        c.load();
        CHECK(c.stats().count == 1 && c.hasVideo("good0000000"));
        CHECK(!fs::exists(dir / L"left0000000-140.part") && !fs::exists(dir / L"orphan.m4a"));
        CHECK(!fs::exists(dir / L"short000000-140.m4a"));   // its entry is gone, so is the file
        CHECK(c.find("good0000000", "wh", "").has_value());

        // Damaged after it was stored: the head, the tail, a sampled block, a truncation.
        const int64_t n = static_cast<int64_t>(bytes.size());
        const std::pair<const char*, int64_t> damage[] = {
            {"head0000000", 10}, {"tail0000000", n - 10}, {"block000000", (n - 4096) * 8 / 15 + 100}};
        for (const auto& [id, offset] : damage) {
            CHECK(put(c, id, 140, bytes));
            patchFile(dir / (std::string(id) + "-140.m4a"), offset, static_cast<char>(~bytes[static_cast<size_t>(offset)]));
            CHECK(!c.find(id, "wh", "") && !c.hasVideo(id) && !fs::exists(dir / (std::string(id) + "-140.m4a")));
        }
        CHECK(put(c, "trunc000000", 140, bytes));
        fs::resize_file(dir / L"trunc000000-140.m4a", n - 1, ec);
        CHECK(!c.findAny("trunc000000", "", true) && !c.hasVideo("trunc000000"));
        // A byte between the samples is not noticed (documented: a cheap hash).
        CHECK(put(c, "gap00000000", 140, bytes));
        patchFile(dir / L"gap00000000-140.m4a", 80 * kKB, static_cast<char>(~bytes[80 * kKB]));   // between blocks 4 and 5
        CHECK(c.find("gap00000000", "wh", "").has_value());
        // The hash itself: memory and file agree, and the length is part of it.
        const auto h1 = AudioCache::sampledHash(n, [&](int64_t off, void* dst, size_t len) {
            std::memcpy(dst, bytes.data() + off, len);
            return true;
        });
        const auto h2 = AudioCache::sampledHash(n - 1, [&](int64_t off, void* dst, size_t len) {
            std::memcpy(dst, bytes.data() + off, len);
            return true;
        });
        CHECK(h1 && h2 && *h1 != *h2);
        c.clear();
        CHECK(c.stats().count == 0 && filesIn(dir) == 0);
    }
}

void testSize(const fs::path& tmp) {
    std::printf("size: LRU, limits, low disk\n");
    const fs::path dir = tmp / L"size";
    std::atomic<int64_t> freeBytes{kHugeFree};
    AudioCache c(dir);
    c.setFreeSpaceProbe([&] { return freeBytes.load(); });
    c.setLimit(400 * kKB);
    const std::string s = randomBytes(90 * kKB, 3);
    for (const char* id : {"s1000000000", "s2000000000", "s3000000000", "s4000000000"}) CHECK(put(c, id, 140, s));
    CHECK(c.stats().count == 4);
    CHECK(c.find("s1000000000", "wh", ""));   // played again: the most recent now
    CHECK(put(c, "s5000000000", 140, s));     // 450 KB > 400 KB: the least recently used goes
    CHECK(!c.hasVideo("s2000000000") && c.hasVideo("s1000000000") && c.stats().count == 4);
    CHECK(c.stats().bytes <= 400 * kKB);
    // A lower limit takes effect with trim(): the two most recently used stay.
    c.setLimit(200 * kKB);
    c.trim();
    CHECK(c.stats().count == 2 && c.hasVideo("s1000000000") && c.hasVideo("s5000000000"));
    CHECK(filesIn(dir) == 2);
    // A stream larger than a quarter of the limit is not kept.
    CHECK(!put(c, "big00000000", 140, randomBytes(60 * kKB, 4)));
    c.setLimit(400 * kKB);
    // The disk is nearly full: the cache gives space back first, so the new song still fits next to one old one...
    freeBytes = AudioCache::kMinFreeBytes + 50 * kKB;
    CHECK(put(c, "s6000000000", 140, s));
    CHECK(c.stats().count == 2 && !c.hasVideo("s1000000000"));
    // ... and below the floor it empties itself and stores nothing.
    freeBytes = AudioCache::kMinFreeBytes - 200 * kKB;
    CHECK(!put(c, "s7000000000", 140, s));
    CHECK(c.stats().count == 0 && filesIn(dir) == 0);
    freeBytes = kHugeFree;
    // Off: nothing stored, nothing served.
    CHECK(put(c, "s8000000000", 140, s));
    c.setLimit(0);
    CHECK(!c.find("s8000000000", "wh", ""));
    CHECK(!put(c, "s9000000000", 140, s));
    c.trim();   // what the Settings row does when it is turned off
    CHECK(c.stats().count == 0 && filesIn(dir) == 0);
}

// ---- 4: an HTTP server answering ranged requests -------------------------------------------------------------------

class HttpServer {
public:
    explicit HttpServer(std::string body, size_t bytesPerSecond = 0) : body_(std::move(body)), rate_(bytesPerSecond) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listen_, 16);
        int len = sizeof a;
        getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { acceptLoop(); });
    }
    ~HttpServer() {
        stop_ = true;
        closesocket(listen_);
        acceptor_.join();
        {
            std::lock_guard lock(mutex_);
            for (SOCKET s : open_) shutdown(s, SD_BOTH);
        }
        for (auto& t : connections_) t.join();
        for (SOCKET s : open_) closesocket(s);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/stream"; }
    int requests() const { return requests_.load(); }

private:
    void acceptLoop() {
        for (;;) {
            const SOCKET c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::lock_guard lock(mutex_);
            open_.push_back(c);
            connections_.emplace_back([this, c] { serve(c); });
        }
    }
    bool sendAll(SOCKET c, const char* p, size_t n) {
        while (n > 0) {
            const int sent = send(c, p, static_cast<int>(std::min<size_t>(n, 16 * 1024)), 0);
            if (sent <= 0) return false;
            p += sent;
            n -= static_cast<size_t>(sent);
            if (rate_ > 0) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(sent) * 1'000'000 / static_cast<int64_t>(rate_)));
            if (stop_) return false;
        }
        return true;
    }
    void serve(SOCKET c) {
        std::string buf;
        char tmp[4096];
        while (!stop_) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                const int n = recv(c, tmp, sizeof tmp, 0);
                if (n <= 0) return;
                buf.append(tmp, static_cast<size_t>(n));
            }
            const std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);
            ++requests_;
            const int64_t total = static_cast<int64_t>(body_.size());
            int64_t first = 0, last = total - 1;
            const auto r = head.find("Range: bytes=");
            const bool ranged = r != std::string::npos;
            if (ranged) {
                first = std::stoll(head.substr(r + 13));
                const auto dash = head.find('-', r + 13);
                if (dash != std::string::npos && dash + 1 < head.size() && isdigit(static_cast<unsigned char>(head[dash + 1])))
                    last = std::min(total - 1, std::stoll(head.substr(dash + 1)));
            }
            std::string resp = ranged ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(first) + "-" +
                                            std::to_string(last) + "/" + std::to_string(total) + "\r\n"
                                      : std::string("HTTP/1.1 200 OK\r\n");
            resp += "Content-Type: application/octet-stream\r\nContent-Length: " + std::to_string(last - first + 1) +
                    "\r\nConnection: keep-alive\r\n\r\n";
            if (!sendAll(c, resp.data(), resp.size())) return;
            if (!sendAll(c, body_.data() + first, static_cast<size_t>(last - first + 1))) return;
        }
    }

    std::string body_;
    size_t rate_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::mutex mutex_;
    std::vector<SOCKET> open_;
    std::vector<std::thread> connections_;
    std::thread acceptor_;
};

template <class F>
bool waitFor(F done, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (!done()) {
        if (GetTickCount64() - start > timeoutMs) return false;
        Sleep(10);
    }
    return true;
}

void testBufferHook() {
    std::printf("buffer: the completion hook\n");
    using st::audio::ProgressiveBuffer;
    const std::string body = randomBytes(3 * 1024 * kKB + 12345, 11);   // > 1 MB: a mapped temporary file, 4 ranges
    struct Seen {
        std::mutex m;
        int calls = 0;
        std::string bytes;
    };
    auto hook = [](Seen& s) {
        return [&s](const uint8_t* data, int64_t n) {
            std::lock_guard lock(s.m);
            ++s.calls;
            s.bytes.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(n));
        };
    };
    {   // A plain download.
        HttpServer server(body);
        Seen seen;
        ProgressiveBuffer buf(server.url(), L"", static_cast<int64_t>(body.size()), nullptr);
        buf.setOnComplete(hook(seen));
        buf.start();
        CHECK(buf.waitForLength() == static_cast<int64_t>(body.size()));
        CHECK(waitFor([&] { return buf.finished(); }, 20000));
        std::lock_guard lock(seen.m);
        CHECK(seen.calls == 1 && seen.bytes == body);
        CHECK(server.requests() >= 4);   // ranged: the head, the parallel tail, the rest
    }
    {   // A far seek right away (the download restarts there and fills the gap afterwards).
        HttpServer server(body, 2 * 1024 * 1024);
        Seen seen;
        ProgressiveBuffer buf(server.url(), L"", static_cast<int64_t>(body.size()), nullptr);
        buf.setOnComplete(hook(seen));
        buf.start();
        std::vector<char> dst(4096);
        CHECK(buf.read(static_cast<int64_t>(body.size()) - 200000, dst.data(), dst.size()) == ProgressiveBuffer::ReadStatus::Ok);
        CHECK(std::memcmp(dst.data(), body.data() + body.size() - 200000, dst.size()) == 0);
        CHECK(waitFor([&] { return buf.finished(); }, 30000));
        std::lock_guard lock(seen.m);
        CHECK(seen.calls == 1 && seen.bytes == body);
    }
    {   // Cancelled half way: never handed over.
        HttpServer server(body, 256 * 1024);
        Seen seen;
        auto buf = std::make_unique<ProgressiveBuffer>(server.url(), L"", static_cast<int64_t>(body.size()), nullptr);
        buf->setOnComplete(hook(seen));
        buf->start();
        Sleep(600);
        CHECK(buf->fraction() > 0.f && buf->fraction() < 1.f);
        buf->cancel();
        buf->release();
        buf.reset();
        std::lock_guard lock(seen.m);
        CHECK(seen.calls == 0);
    }
}

// ---- 5-6: the engine and the player --------------------------------------------------------------------------------

// A 16-bit stereo 44.1 kHz WAV of a 440 Hz tone.
std::string wav(uint32_t ms) {
    constexpr uint32_t rate = 44100, channels = 2;
    const uint32_t frames = rate * ms / 1000;
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(6000 * std::sin(2 * 3.14159265358979 * 440 * i / rate));
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    std::string out;
    auto raw = [&](const void* p, size_t n) { out.append(static_cast<const char*>(p), n); };
    auto u32 = [&](uint32_t v) { raw(&v, 4); };
    auto u16 = [&](uint16_t v) { raw(&v, 2); };
    raw("RIFF", 4);
    u32(36 + data);
    raw("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(channels * 2);
    u16(16);
    raw("data", 4);
    u32(data);
    raw(pcm.data(), data);
    return out;
}

void testEngine(const fs::path& tmp) {
    std::printf("engine: a streamed song lands in the cache, the cached file plays\n");
    using namespace st::audio;
    const std::string body = wav(2000);
    HttpServer server(body);
    AudioCache cache(tmp / L"engine");
    cache.setFreeSpaceProbe([] { return kHugeFree; });
    cache.setLimit(64ll << 20);
    std::atomic<int> state{static_cast<int>(State::Idle)};
    std::atomic<uint64_t> stateTag{0};
    EngineEvents ev;
    ev.onState = [&](State s, uint64_t tag) {
        stateTag = tag;
        state = static_cast<int>(s);
    };
    AudioEngine engine(std::move(ev));
    engine.setVolume(0.f);
    StreamSource s;
    s.url = server.url();
    s.contentLength = static_cast<int64_t>(body.size());
    s.mimeType = "audio/wav";
    s.tag = 1;
    auto song = AudioCache::songFor(resolved("engine00001", 140, "audio/wav"), "wh", "t:engine");
    CHECK(song.has_value());
    s.onComplete = [&cache, song = *song](const uint8_t* data, int64_t n) { cache.store(song, data, n); };
    ULONGLONG t0 = GetTickCount64();
    engine.open(s);
    CHECK(waitFor([&] { return stateTag == 1 && state == static_cast<int>(State::Playing); }, 10000));
    std::printf("        over HTTP: playing after %llu ms\n", GetTickCount64() - t0);
    CHECK(waitFor([&] { return cache.stats().count == 1; }, 10000));
    const auto hit = cache.find("engine00001", "wh", "");
    CHECK(hit && fs::file_size(hit->path) == body.size());
    if (!hit) return;
    StreamSource local;
    local.localPath = hit->path;
    local.mimeType = hit->song.mimeType;
    local.tag = 2;
    t0 = GetTickCount64();
    engine.open(local);
    CHECK(waitFor([&] { return stateTag == 2 && state == static_cast<int>(State::Playing); }, 10000));
    std::printf("        from the cache: playing after %llu ms\n", GetTickCount64() - t0);
    engine.stop();
}

// Pumps the UI thread (Dispatcher posts, the player's WM_TIMER prefetch tick) until `done()` or the timeout.
template <class F>
bool pumpUntil(F done, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (!done()) {
        if (GetTickCount64() - start > timeoutMs) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return true;
}

void testPlayer(const fs::path& tmp) {
    std::printf("player: songs from the cache with YouTube unreachable (SHADETUBE_OFFLINE)\n");
    using st::player::Status;
    using PS = st::player::Player::PreviewState;
    st::Dispatcher::init();
    st::ThreadPool pool(4);   // the player resolves on workers
    st::ThreadPool::setShared(&pool);
    AudioCache cache(tmp / L"player");
    cache.setFreeSpaceProbe([] { return kHugeFree; });
    cache.setLimit(64ll << 20);
    // The app's shared transport, which SHADETUBE_OFFLINE cuts off (a default YoutubeClient would have its own).
    st::youtube::MatchService matcher{YoutubeExplode::YoutubeClient(st::http::client())};
    CHECK(st::http::simulatedOffline());
    // The stream choice of the player's default quality (High): Opus when this machine decodes it.
    const std::string profile = AudioCache::profile(st::audio::AudioEngine::supportsWebm(), false);
    const std::map<std::string, std::string> videos{{"song:a", "aaaaaaaaaaa"}, {"song:b", "bbbbbbbbbbb"}, {"song:c", "ccccccccccc"}};
    const std::string body = wav(3000);
    for (const auto& [id, video] : videos) {
        auto r = resolved(video, 140, "audio/wav");
        matcher.pin(id, r.match);
        CHECK(put(cache, video, 140, body, profile, id, "audio/wav"));
    }
    auto T = [](const std::string& id) {
        st::catalog::Track t;
        t.id = id;
        t.name = "Song " + id.substr(5);
        t.artists = {{"", "Tester"}};
        t.durationMs = 3000;
        return t;
    };
    {
        st::player::Player player(matcher);
        player.setVolume(0.f);
        player.setCache(&cache);
        std::vector<std::string> started;
        player.onTrackChanged = [&](const st::catalog::Track& t) { started.push_back(t.id); };
        std::wstring error;
        player.onError = [&](const std::wstring& m) { error = m; };
        // What the player was doing at each notification: (item, status, from the cache, its video).
        struct Seen {
            std::string id;
            Status status;
            bool cached;
            std::string video;
        };
        std::vector<Seen> seen;
        player.onChanged = [&] {
            if (const auto* t = player.current()) {
                const auto m = player.currentMatch();
                seen.push_back({t->id, player.status(), player.currentFromCache(), m ? m->videoId : std::string{}});
            }
        };
        auto playingFromCache = [&](const std::string& id, const std::string& video) {
            return std::any_of(seen.begin(), seen.end(), [&](const Seen& s) {
                return s.id == id && s.status == Status::Playing && s.cached && s.video == video;
            });
        };

        const ULONGLONG t0 = GetTickCount64();
        player.playContext({T("song:a"), T("song:b")}, 0, {"test", L"Test"});
        CHECK(pumpUntil([&] { return player.status() == Status::Playing; }, 10000));
        std::printf("        first song playing after %llu ms\n", GetTickCount64() - t0);
        CHECK(pumpUntil([&] { return started.size() >= 2 && player.status() == Status::Idle; }, 20000));
        CHECK((started == std::vector<std::string>{"song:a", "song:b"}));
        CHECK(playingFromCache("song:a", "aaaaaaaaaaa"));
        CHECK(playingFromCache("song:b", "bbbbbbbbbbb"));
        // b was preloaded from the cache and took over gaplessly: it never went through a resolve.
        CHECK(std::none_of(seen.begin(), seen.end(), [](const Seen& s) { return s.id == "song:b" && s.status == Status::Resolving; }));
        CHECK(error.empty());

        // Crossfade between two cached songs: b takes over 2 s before a ends (3 s songs: ~1 s in).
        {
            std::vector<std::pair<std::string, ULONGLONG>> changes;
            player.onTrackChanged = [&](const st::catalog::Track& t) { changes.push_back({t.id, GetTickCount64()}); };
            st::Settings::get().crossfadeSec = 2;
            player.playContext({T("song:a"), T("song:b")}, 0, {"xfade", L"Test"});
            const bool handed = pumpUntil([&] { return changes.size() >= 2; }, 15000);
            const long fade = handed ? static_cast<long>(changes[1].second - changes[0].second) : -1;
            std::printf("        crossfade: b audible %ld ms after a started\n", fade);
            CHECK(handed && fade > 700 && fade < 2600);
            pumpUntil([&] { return player.status() == Status::Idle; }, 10000);
            st::Settings::get().crossfadeSec = 0;
            player.onTrackChanged = [&](const st::catalog::Track& t) { started.push_back(t.id); };
        }

        // A preview reads the cache too.
        std::vector<PS> previews;
        player.onPreview = [&](uint64_t, PS s) { previews.push_back(s); };
        CHECK(player.startPreview(T("song:c"), 500) != 0);
        CHECK(pumpUntil([&] { return std::find(previews.begin(), previews.end(), PS::Playing) != previews.end(); }, 10000));
        player.stopPreview();
        CHECK(std::find(previews.begin(), previews.end(), PS::Failed) == previews.end());

        // Another quality setting picks another format; offline, the cached one still plays.
        st::Settings::get().quality = st::AudioQuality::Normal;
        seen.clear();
        error.clear();
        const ULONGLONG t1 = GetTickCount64();
        player.playContext({T("song:a")}, 0, {"test2", L"Test"});
        CHECK(pumpUntil([&] { return player.status() == Status::Playing || player.status() == Status::Error; }, 60000));
        std::printf("        other quality, resolve failed: playing after %llu ms\n", GetTickCount64() - t1);
        CHECK(playingFromCache("song:a", "aaaaaaaaaaa"));
        player.pause();
        st::Settings::get().quality = st::AudioQuality::High;

        // A damaged file is dropped, never played (and the song is unavailable offline).
        patchFile(tmp / L"player" / L"ccccccccccc-140.bin", 50, 0x5A);
        seen.clear();
        error.clear();
        player.playContext({T("song:c")}, 0, {"test3", L"Test"});
        CHECK(pumpUntil([&] { return player.status() == Status::Error; }, 60000));
        CHECK(!cache.hasVideo("ccccccccccc") && !fs::exists(tmp / L"player" / L"ccccccccccc-140.bin"));
        CHECK(std::none_of(seen.begin(), seen.end(), [](const Seen& s) { return s.status == Status::Playing; }));
        CHECK(!error.empty());
        pumpUntil([] { return false; }, 300);
    }
    st::ThreadPool::setShared(nullptr);
    st::Dispatcher::shutdown();
}

} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // progress shows up as it happens, also when redirected
    // Per process: parallel runs (several checkouts) must not delete each other's files.
    const fs::path tmp = fs::temp_directory_path() / (L"shadetube_audiocache_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (tmp / L"profile").c_str());   // never the user's profile
    SetEnvironmentVariableW(L"SHADETUBE_OFFLINE", L"1");                         // YouTube unreachable (part 6)
    testStore(tmp);
    testSafety(tmp);
    testSize(tmp);
    testBufferHook();
    testEngine(tmp);
    testPlayer(tmp);
    fs::remove_all(tmp, ec);
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
