// scrobble_test: checks for app/Scrobbler (Last.fm + ListenBrainz) and app/DiscordRpc.
//
//   offline : MD5 + Last.fm api_sig known answers (expected values computed independently with .NET MD5 over
//             the same UTF-8 byte string), the listened-time rule with injected progress, DPAPI store round
//             trip, Discord IPC framing against an in-process fake Discord pipe server, and Discord failing
//             fast + silently when no pipe exists.
//   live    : ListenBrainz validate-token with a bogus token -> ok=false; Last.fm auth.getToken with a bogus
//             API key -> a graceful error. (Both also pass offline: the failure path is what is verified.)
//   optional: set SHADETUBE_DISCORD_APPID=<application id> to show a 5 s presence on the real Discord client.
//
// No credential is ever printed.
#include "app/DiscordRpc.h"
#include "app/Scrobbler.h"
#include "core/Dispatcher.h"
#include "core/ThreadPool.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::app;
using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static int64_t msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

// Runs the UI-thread dispatcher (st::async continuations) until pred() or the timeout.
template <class Pred>
static bool pumpUntil(Pred pred, int timeoutMs) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    MSG msg;
    while (!pred()) {
        if (GetTickCount64() >= end) return false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred()) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    return true;
}

static std::filesystem::path tempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::filesystem::path(dir) / name;
}

// ---------------------------------------------------------------------------------------------------------
static void testMd5() {
    std::printf("md5 (CNG):\n");
    CHECK(scrobble::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(scrobble::md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
}

static void testSignature() {
    std::printf("last.fm api_sig:\n");
    // Deliberately unsorted, with "format" and "callback" (both excluded from the signature) and UTF-8 text:
    // artist "Barış Manço", track "Dönence", album "Sahibinden İhtiyaçtan" (escaped so the source encoding
    // cannot matter). Expected value: .NET MD5 of the identical UTF-8 string (see the report).
    const scrobble::Params p{
        {"track", "D\xC3\xB6nence"},
        {"format", "json"},
        {"method", "track.scrobble"},
        {"timestamp", "1700000000"},
        {"artist", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o"},
        {"sk", "d580d57f32848f5dcf574d1ce18d78b2"},
        {"callback", "cb"},
        {"api_key", "0123456789abcdef0123456789abcdef"},
        {"album", "Sahibinden \xC4\xB0htiya\xC3\xA7tan"},
    };
    const std::string sig = scrobble::lastfmSignature(p, "fedcba9876543210fedcba9876543210");
    std::printf("  track.scrobble  sig=%s (want 1e9e783c9054306e9da537f8a7cb685f)\n", sig.c_str());
    CHECK(sig == "1e9e783c9054306e9da537f8a7cb685f");

    const std::string sig2 = scrobble::lastfmSignature(
        {{"method", "auth.getToken"}, {"api_key", "0123456789abcdef0123456789abcdef"}}, "fedcba9876543210fedcba9876543210");
    std::printf("  auth.getToken   sig=%s (want 122db8efff0100fb11599a7629ab9106)\n", sig2.c_str());
    CHECK(sig2 == "122db8efff0100fb11599a7629ab9106");
}

// ---------------------------------------------------------------------------------------------------------
static void testListenClock() {
    std::printf("listened-time rule:\n");
    using scrobble::ListenClock;

    {   // 200 s track -> threshold 100 s; fires exactly once, at 100 s of real listening.
        ListenClock c;
        int64_t wall = 1'000'000;
        c.reset(200'000, wall);
        CHECK(c.thresholdMs() == 100'000);
        int fired = 0, firedAt = -1;
        for (int s = 1; s <= 150; ++s) {
            wall += 1000;
            if (c.update(s * 1000LL, true, wall)) {
                ++fired;
                firedAt = s;
            }
        }
        CHECK(fired == 1);
        CHECK(firedAt == 100);
    }
    {   // A forward seek is not listening: 10 s played, seek to 90 s, then it takes 90 more real seconds.
        ListenClock c;
        int64_t wall = 0, pos = 0;
        c.reset(200'000, wall);
        for (int s = 0; s < 10; ++s) c.update(pos += 1000, true, wall += 1000);
        CHECK(c.listenedMs() == 10'000);
        CHECK(!c.update(pos = 90'000, true, wall += 1000));   // seek (+80 s in 1 s of wall time)
        CHECK(c.listenedMs() == 10'000);
        int more = 0;
        while (!c.update(pos += 1000, true, wall += 1000)) ++more;
        CHECK(more + 1 == 90);
        CHECK(pos == 180'000);
    }
    {   // Backward seek: not counted itself, but re-listening counts (it is real listening time).
        ListenClock c;
        int64_t wall = 0, pos = 0;
        c.reset(200'000, wall);
        for (int s = 0; s < 50; ++s) c.update(pos += 1000, true, wall += 1000);
        c.update(pos = 0, true, wall += 1000);   // back to the start
        CHECK(c.listenedMs() == 50'000);
        bool fired = false;
        for (int s = 0; s < 50 && !fired; ++s) fired = c.update(pos += 1000, true, wall += 1000);
        CHECK(fired);
        CHECK(c.listenedMs() == 100'000);
    }
    {   // Paused: the wall clock runs, the position does not; a seek while paused is not counted.
        ListenClock c;
        int64_t wall = 0;
        c.reset(200'000, wall);
        c.update(5'000, true, wall += 5'000);
        for (int s = 0; s < 60; ++s) c.update(5'000, false, wall += 1000);
        c.update(40'000, false, wall += 60'000);   // seek while paused (wall allows it, but nothing played)
        CHECK(c.listenedMs() == 5'000);
    }
    {   // Coarse 2 s ticks (the app's housekeeping cadence) and a delayed tick still count fully.
        ListenClock c;
        int64_t wall = 0, pos = 0;
        c.reset(100'000, wall);
        for (int s = 0; s < 20; ++s) c.update(pos += 2000, true, wall += 2000);
        c.update(pos += 7000, true, wall += 7000);   // UI thread was busy for 7 s
        CHECK(c.listenedMs() == 47'000);
    }
    {   // Rule edges.
        ListenClock c;
        c.reset(30'000, 0);
        CHECK(c.thresholdMs() == -1);   // not longer than 30 s -> never
        int64_t wall = 0;
        bool fired = false;
        for (int s = 1; s <= 30; ++s) fired |= c.update(s * 1000LL, true, wall += 1000);
        CHECK(!fired);
        c.reset(31'000, 0);
        CHECK(c.thresholdMs() == 15'500);
        c.reset(20 * 60'000, 0);
        CHECK(c.thresholdMs() == 240'000);   // 4-minute cap
        c.reset(0, 0);
        CHECK(c.thresholdMs() == 240'000);   // unknown duration: the 4-minute rule alone
        c.learnDuration(100'000);
        CHECK(c.thresholdMs() == 50'000);
        c.learnDuration(300'000);              // a known duration is not overwritten
        CHECK(c.thresholdMs() == 50'000);
    }
}

// Scrobbler-level rule with an injected clock (no services connected: onScrobble still reports the play).
static void testScrobblerRule() {
    std::printf("scrobbler (injected clock, offline):\n");
    const auto file = tempFile(L"shadetube_scrobble_rule_test.dat");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    Scrobbler s(file);
    int64_t wall = 0;
    s.clock = [&wall] { return wall; };
    std::vector<std::pair<std::string, int64_t>> scrobbles;
    s.onScrobble = [&](const ScrobbleTrack& t, int64_t ts) { scrobbles.emplace_back(t.title, ts); };

    catalog::Track ct;
    ct.id = "spotify:track:1";
    ct.name = "D\xC3\xB6nence";
    ct.artists = {{"a1", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o"}, {"a2", "Kurtalan Ekspres"}};
    ct.album.name = "Sahibinden \xC4\xB0htiya\xC3\xA7tan";
    ct.durationMs = 180'000;
    const ScrobbleTrack t = scrobbleTrackFrom(ct);
    CHECK(t.artist == "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o");
    CHECK(t.artistCredit == ct.artistLine());
    CHECK(t.durationMs == 180'000);

    int64_t pos = 0;
    // One second of playback: advance the injected clock and the position together.
    auto play = [&](int seconds, int64_t playerDurationMs = 0) {
        for (int i = 0; i < seconds; ++i) {
            wall += 1000;
            pos += 1000;
            s.onProgress(pos, true, playerDurationMs);
        }
    };

    const int64_t before = static_cast<int64_t>(std::time(nullptr));
    s.onTrackStarted(t);
    play(89);
    CHECK(scrobbles.empty());
    // A re-resolve of the same track (same id) mid-play keeps the listened time.
    s.onTrackStarted(t);
    play(1);   // 90 s = 50% of 180 s
    CHECK(scrobbles.size() == 1);
    // Timestamp = when the play started: the first playing sample (position 1 s) is back-dated by its position.
    if (!scrobbles.empty()) CHECK(scrobbles[0].second >= before - 2 && scrobbles[0].second <= before + 5);
    play(60);
    CHECK(scrobbles.size() == 1);   // once per play

    // Same id after the scrobble = a repeat -> a new play that can scrobble again.
    s.onTrackStarted(t);
    pos = 0;
    play(90);
    CHECK(scrobbles.size() == 2);

    // Disabled: nothing is reported.
    s.enabled = false;
    ScrobbleTrack t2 = t;
    t2.id = "spotify:track:2";
    s.onTrackStarted(t2);
    pos = 0;
    play(120);
    CHECK(scrobbles.size() == 2);
    s.enabled = true;

    // Unknown catalog duration learned from the player; stopped plays never scrobble.
    ScrobbleTrack t3 = t;
    t3.id = "mb:3";
    t3.durationMs = 0;
    s.onTrackStarted(t3);
    pos = 0;
    play(39, 80'000);
    CHECK(scrobbles.size() == 2);
    play(1, 80'000);   // 40 s = 50% of 80 s
    CHECK(scrobbles.size() == 3);
    ScrobbleTrack t4 = t;
    t4.id = "mb:4";
    s.onTrackStarted(t4);
    s.onStopped();
    play(200);
    CHECK(scrobbles.size() == 3);

    // Long track (4-minute cap): scrobbled at 4:00. A re-resolve / "Yanlış eşleşme?" switch afterwards continues
    // at the same position -> still the same play, never a second scrobble.
    ScrobbleTrack t5 = t;
    t5.id = "mb:5";
    t5.durationMs = 20 * 60'000;
    s.onTrackStarted(t5);
    pos = 0;
    play(240);
    CHECK(scrobbles.size() == 4);
    s.onTrackStarted(t5);   // same id, continues at 4:00
    play(600);
    CHECK(scrobbles.size() == 4);

    // Same id restarted from the top before it qualified (clicked again / repeat): a new play, the earlier
    // partial listen does not carry over.
    ScrobbleTrack t6 = t;
    t6.id = "mb:6";
    s.onTrackStarted(t6);
    pos = 0;
    play(60);
    s.onTrackStarted(t6);
    pos = 0;
    play(60);
    CHECK(scrobbles.size() == 4);
    play(30);   // 90 s of the new play = 50% of 180 s
    CHECK(scrobbles.size() == 5);
    CHECK(s.pendingLastfm() == 0 && s.pendingListenbrainz() == 0);   // nothing connected -> nothing queued
    std::filesystem::remove(file, ec);
}

static void testStore() {
    std::printf("credential store (DPAPI):\n");
    const auto file = tempFile(L"shadetube_scrobble_store_test.dat");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    const std::string secret = "s3cr3t-value-that-must-not-appear";
    {
        Scrobbler s(file);
        int changes = 0;
        s.onChanged = [&] { ++changes; };
        s.lastfmSetKeys("  0123456789abcdef0123456789abcdef \r\n", " " + secret + " ");
        CHECK(changes == 1);
        CHECK(s.lastfmHasKeys());
        CHECK(!s.lastfmConnected());
        CHECK(s.lastfmApiKey() == "0123456789abcdef0123456789abcdef");
        s.lastfmSetKeys("0123456789abcdef0123456789abcdef", "");   // blank secret + same key = keep
        CHECK(changes == 1);
        CHECK(s.lastfmHasKeys());
    }
    CHECK(std::filesystem::exists(file));
    {
        std::ifstream f(file, std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(!raw.empty());
        CHECK(raw.find(secret) == std::string::npos);
        CHECK(raw.find("0123456789abcdef") == std::string::npos);
    }
    {
        Scrobbler s(file);
        s.load();
        CHECK(s.lastfmHasKeys());
        CHECK(s.lastfmHasSecret());
        CHECK(s.lastfmApiKey() == "0123456789abcdef0123456789abcdef");
        CHECK(!s.lastfmConnected());
        CHECK(!s.listenbrainzConnected());
        s.lastfmSetKeys("", "");
        CHECK(!s.lastfmHasKeys());
    }
    CHECK(!std::filesystem::exists(file));   // nothing left to store -> file removed
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "garbage, not DPAPI";
    }
    {
        Scrobbler s(file);
        s.load();   // must not throw
        CHECK(!s.lastfmHasKeys());
    }
    std::filesystem::remove(file, ec);
}

// ---------------------------------------------------------------------------------------------------------
static void testLive() {
    std::printf("live (network):\n");
    const auto file = tempFile(L"shadetube_scrobble_live_test.dat");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    Scrobbler s(file);

    bool lbDone = false, lbOk = true;
    std::string lbMsg;
    const auto t0 = std::chrono::steady_clock::now();
    s.listenbrainzSetToken("00000000-bogus-token-shadetube-test", [&](bool ok, std::string m) {
        lbDone = true;
        lbOk = ok;
        lbMsg = std::move(m);
    });
    CHECK(msSince(t0) < 50);   // never blocks the caller
    CHECK(pumpUntil([&] { return lbDone; }, 30'000));
    CHECK(lbDone && !lbOk);
    CHECK(!lbMsg.empty());
    CHECK(!s.listenbrainzConnected());
    std::printf("  listenbrainz bogus token -> ok=%d  \"%s\"\n", lbOk ? 1 : 0, lbMsg.c_str());

    bool emptyDone = false, emptyOk = true;
    s.listenbrainzSetToken("   ", [&](bool ok, std::string) {
        emptyDone = true;
        emptyOk = ok;
    });
    CHECK(!emptyDone);   // callbacks are always asynchronous (UI thread, later)
    CHECK(pumpUntil([&] { return emptyDone; }, 2'000));
    CHECK(!emptyOk);

    bool lfDone = false;
    std::string lfUrl, lfErr;
    s.lastfmSetKeys("0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
    s.lastfmBeginAuth([&](std::string url, std::string err) {
        lfDone = true;
        lfUrl = std::move(url);
        lfErr = std::move(err);
    });
    CHECK(pumpUntil([&] { return lfDone; }, 30'000));
    CHECK(lfUrl.empty());
    CHECK(!lfErr.empty());
    CHECK(!s.lastfmAuthPending());
    std::printf("  last.fm bogus api key -> \"%s\"\n", lfErr.c_str());

    bool finDone = false, finOk = true;
    s.lastfmFinishAuth([&](bool ok, std::string) {
        finDone = true;
        finOk = ok;
    });
    CHECK(pumpUntil([&] { return finDone; }, 2'000));
    CHECK(!finOk);   // no pending token
    std::filesystem::remove(file, ec);
}

// ---------------------------------------------------------------------------------------------------------
// Minimal fake Discord: one pipe instance, answers the handshake with READY and every command with a reply.
class FakeDiscord {
public:
    // reject = answer the handshake like Discord does for an unknown application id (op 2 CLOSE, code 4000).
    explicit FakeDiscord(const std::wstring& name, bool reject = false) : reject_(reject) {
        pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
                                 65536, 0, nullptr);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeDiscord() {
        for (int i = 0; i < 100 && !finished_; ++i) {
            CancelSynchronousIo(thread_.native_handle());
            Sleep(20);
        }
        thread_.join();
        CloseHandle(pipe_);
    }
    bool ok() const { return pipe_ != INVALID_HANDLE_VALUE; }
    std::vector<std::pair<int32_t, json>> frames() {
        std::lock_guard lk(m_);
        return frames_;
    }
    bool clientGone() const { return finished_; }

private:
    bool readAll(void* buf, DWORD n) {
        DWORD done = 0;
        while (done < n) {
            DWORD got = 0;
            if (!ReadFile(pipe_, static_cast<char*>(buf) + done, n - done, &got, nullptr) || got == 0) return false;
            done += got;
        }
        return true;
    }
    void writeFrame(int32_t op, const std::string& s) {
        std::string buf(8 + s.size(), '\0');
        const auto len = static_cast<int32_t>(s.size());
        memcpy(buf.data(), &op, 4);
        memcpy(buf.data() + 4, &len, 4);
        memcpy(buf.data() + 8, s.data(), s.size());
        DWORD written = 0;
        WriteFile(pipe_, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
    }
    void serve() {
        if (ConnectNamedPipe(pipe_, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            for (;;) {
                int32_t hdr[2] = {};
                if (!readAll(hdr, 8)) break;
                std::string payload(static_cast<size_t>(hdr[1]), '\0');
                if (hdr[1] > 0 && !readAll(payload.data(), static_cast<DWORD>(hdr[1]))) break;
                const json j = json::parse(payload, nullptr, false);
                {
                    std::lock_guard lk(m_);
                    frames_.emplace_back(hdr[0], j);
                }
                if (hdr[0] == 0 && reject_) {
                    writeFrame(2, R"({"code":4000,"message":"Invalid Client ID"})");
                    break;
                }
                if (hdr[0] == 0) {
                    writeFrame(1, R"({"cmd":"DISPATCH","evt":"READY","nonce":null,"data":{"v":1,"user":{"id":"1","username":"fake"}}})");
                } else if (hdr[0] == 1) {
                    json reply = {{"cmd", j.value("cmd", "")}, {"evt", nullptr}, {"data", json::object()}, {"nonce", j.value("nonce", "")}};
                    writeFrame(1, reply.dump());
                }
            }
        }
        finished_ = true;
    }

    bool reject_ = false;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::mutex m_;
    std::vector<std::pair<int32_t, json>> frames_;
    std::atomic<bool> finished_{false};
};

static void testDiscordNoPipe() {
    std::printf("discord: no pipe -> fast, silent no-op:\n");
    {
        DiscordRpc idle;   // no app id: disabled, no thread, nothing to do
        idle.setActivity("Title", "Artist", "Album", "https://example.com/a.jpg", 1, 2);
        CHECK(idle.waitIdle(100));
        CHECK(idle.stats().connectAttempts == 0);
    }
    auto t0 = std::chrono::steady_clock::now();
    auto rpc = std::make_unique<DiscordRpc>(L"\\\\.\\pipe\\shadetube-test-nopipe-");
    rpc->setAppId("123456789012345678");
    rpc->setActivity("D\xC3\xB6nence", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", "Album", "https://example.com/a.jpg", 1'700'000'000'000,
                     1'700'000'180'000);
    const int64_t callMs = msSince(t0);
    CHECK(callMs < 50);
    t0 = std::chrono::steady_clock::now();
    CHECK(rpc->waitIdle(3'000));
    const int64_t failMs = msSince(t0);
    const auto st = rpc->stats();
    CHECK(st.connectAttempts >= 1);
    CHECK(!st.connected);
    CHECK(st.framesSent == 0);
    CHECK(failMs < 1'000);
    // Now in backoff: further updates stay cheap, the destructor must still return promptly.
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) rpc->setActivity("T" + std::to_string(i), "A", "", "", 0, 0);
    rpc->clear();
    CHECK(msSince(t0) < 50);
    t0 = std::chrono::steady_clock::now();
    rpc.reset();
    const int64_t dtorMs = msSince(t0);
    CHECK(dtorMs < 1'000);
    std::printf("  calls %lld ms, connect attempt failed after %lld ms, destructor %lld ms, attempts %d\n",
                static_cast<long long>(callMs), static_cast<long long>(failMs), static_cast<long long>(dtorMs), st.connectAttempts);
}

static void testDiscordFakeServer() {
    std::printf("discord: IPC framing against a fake server:\n");
    FakeDiscord server(L"\\\\.\\pipe\\shadetube-test-ipc-0");
    CHECK(server.ok());
    if (!server.ok()) return;
    {
        DiscordRpc rpc(L"\\\\.\\pipe\\shadetube-test-ipc-");
        rpc.setAppId(" 123456789012345678 ");
        const int64_t start = 1'700'000'000'000, end = 1'700'000'180'000;
        const std::string longAlbum(300, 'x');
        rpc.setActivity("D\xC3\xB6nence", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", longAlbum, "https://i.scdn.co/image/abc", start, end);
        CHECK(rpc.waitIdle(5'000));
        auto st = rpc.stats();
        CHECK(st.connected);
        CHECK(st.framesSent == 1);

        // Same activity with timestamps 1 s off (a periodic re-sync) is coalesced away.
        rpc.setActivity("D\xC3\xB6nence", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", longAlbum, "https://i.scdn.co/image/abc", start + 1000, end + 1000);
        CHECK(rpc.waitIdle(2'000));
        CHECK(rpc.stats().framesSent == 1);

        rpc.clear();
        CHECK(rpc.waitIdle(5'000));
        CHECK(rpc.stats().framesSent == 2);

        const auto frames = server.frames();
        CHECK(frames.size() == 3);
        if (frames.size() == 3) {
            const auto& hs = frames[0];
            CHECK(hs.first == 0);
            CHECK(hs.second.value("v", 0) == 1);
            CHECK(hs.second.value("client_id", "") == "123456789012345678");

            const auto& set = frames[1];
            CHECK(set.first == 1);
            CHECK(set.second.value("cmd", "") == "SET_ACTIVITY");
            CHECK(!set.second.value("nonce", "").empty());
            const json& args = set.second["args"];
            CHECK(args.value("pid", 0LL) == static_cast<long long>(GetCurrentProcessId()));
            const json& act = args["activity"];
            CHECK(act.value("type", 0) == 2);
            CHECK(act.value("details", "") == "D\xC3\xB6nence");
            CHECK(act.value("state", "") == "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o");
            CHECK(act["timestamps"].value("start", 0LL) == start);
            CHECK(act["timestamps"].value("end", 0LL) == end);
            CHECK(act["assets"].value("large_image", "") == "https://i.scdn.co/image/abc");
            const std::string lt = act["assets"].value("large_text", "");
            CHECK(lt.size() == 127 + 3 && lt.substr(127) == "\xE2\x80\xA6");   // cut to 128 chars with "…"

            const auto& clr = frames[2];
            CHECK(clr.first == 1);
            CHECK(clr.second["args"].contains("activity") && clr.second["args"]["activity"].is_null());
        }
    }   // destructor closes the pipe
    CHECK(pumpUntil([&] { return server.clientGone(); }, 2'000));   // Discord sees the connection close

    std::printf("discord: handshake rejected (unknown application id):\n");
    FakeDiscord rejecting(L"\\\\.\\pipe\\shadetube-test-reject-0", true);
    CHECK(rejecting.ok());
    DiscordRpc rpc(L"\\\\.\\pipe\\shadetube-test-reject-");
    rpc.setAppId("1");
    rpc.setActivity("Title", "Artist", "", "", 0, 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(rpc.waitIdle(3'000));
    const auto st = rpc.stats();
    CHECK(!st.connected);
    CHECK(st.connectAttempts == 1);
    CHECK(st.framesSent == 0);
    CHECK(msSince(t0) < 1'000);
    const auto frames = rejecting.frames();
    CHECK(frames.size() == 1 && frames[0].first == 0);
}

static void testDiscordReal() {
    char* id = nullptr;
    size_t len = 0;
    if (_dupenv_s(&id, &len, "SHADETUBE_DISCORD_APPID") != 0 || !id) return;
    const std::string appId = id;
    free(id);
    std::printf("discord: live presence on the real client (5 s):\n");
    DiscordRpc rpc;
    rpc.setAppId(appId);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rpc.setActivity("D\xC3\xB6nence", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", "Sahibinden \xC4\xB0htiya\xC3\xA7tan",
                    "https://coverartarchive.org/release-group/a5f8d0ec-4f1c-3d2f-9a2b-3e0a6d0a1c9e/front-250", now, now + 180'000);
    CHECK(rpc.waitIdle(8'000));
    const auto st = rpc.stats();
    std::printf("  connected=%d framesSent=%d attempts=%d\n", st.connected ? 1 : 0, st.framesSent, st.connectAttempts);
    Sleep(5'000);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    ThreadPool pool(4);
    ThreadPool::setShared(&pool);
    Dispatcher::init();

    testMd5();
    testSignature();
    testListenClock();
    testScrobblerRule();
    testStore();
    testDiscordNoPipe();
    testDiscordFakeServer();
    testLive();
    testDiscordReal();

    Dispatcher::shutdown();
    ThreadPool::setShared(nullptr);
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
