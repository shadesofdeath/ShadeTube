// Developer test for app/SponsorBlock: SHA-256 prefix, response parsing, normalisation, the check()
// skip/re-arm state machine with injected segments, and live lookups against sponsor.ajay.app.
//   sponsorblock_test            all tests (needs network)
//   sponsorblock_test --offline  unit tests only
#include "app/SponsorBlock.h"
#include "core/Dispatcher.h"
#include "core/ThreadPool.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using st::app::SponsorBlock;
using Seg = SponsorBlock::Segment;
using State = SponsorBlock::State;

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

std::string str(std::optional<int64_t> v) { return v ? std::to_string(*v) : "none"; }

void expectSkip(SponsorBlock& sb, int64_t pos, std::optional<int64_t> want, const char* what) {
    const auto got = sb.check(pos);
    const bool ok = got == want;
    std::printf("  %s check(%lld) -> %s (want %s)  %s\n", ok ? "ok   " : "FAIL ", static_cast<long long>(pos),
                str(got).c_str(), str(want).c_str(), what);
    if (!ok) ++g_failures;
}

const char* stateName(State s) {
    switch (s) {
    case State::Idle: return "Idle";
    case State::Loading: return "Loading";
    case State::Ready: return "Ready";
    default: return "Failed";
    }
}

void printSegments(const std::vector<Seg>& segs) {
    for (const auto& s : segs)
        std::printf("        [%lld, %lld] %s (videoDuration %lld)\n", static_cast<long long>(s.startMs),
                    static_cast<long long>(s.endMs), s.category.c_str(), static_cast<long long>(s.videoDurationMs));
}

// Runs the UI message loop (the Dispatcher's message-only window lives on this thread) until done() or timeout.
bool pump(const std::function<bool()>& done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
}

bool waitLoaded(SponsorBlock& sb) {
    return pump([&] { return sb.state() != State::Loading; }, 20000);
}

// --- unit tests ------------------------------------------------------------------------------------------

void testSha256() {
    std::printf("[sha256]\n");
    CHECK(SponsorBlock::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(SponsorBlock::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(SponsorBlock::sha256Hex("kJQP7kiw5Fk").substr(0, 4) == "f1d9");   // verified with sha256sum
    CHECK(SponsorBlock::sha256Hex("dQw4w9WgXcQ").substr(0, 4) == "5f6b");
}

void testParse() {
    std::printf("[parse]\n");
    const char* direct = R"([
        {"category":"music_offtopic","actionType":"skip","segment":[0,21.808434],"UUID":"a","videoDuration":282},
        {"category":"sponsor","actionType":"mute","segment":[30,40],"UUID":"b"},
        {"category":"poi_highlight","actionType":"poi","segment":[50,50],"UUID":"c"},
        {"category":"selfpromo","segment":[249.38,281.521],"UUID":"d","videoDuration":0},
        {"category":"sponsor","actionType":"skip","segment":["x",2],"UUID":"e"},
        {"category":"sponsor","actionType":"skip","UUID":"f"}
    ])";
    const auto d = SponsorBlock::parseResponse(direct, "ignored");
    printSegments(d);
    CHECK(d.size() == 2);
    if (d.size() == 2) {
        CHECK(d[0].startMs == 0 && d[0].endMs == 21808 && d[0].category == "music_offtopic");
        CHECK(d[0].videoDurationMs == 282000);
        CHECK(d[1].startMs == 249380 && d[1].endMs == 281521 && d[1].videoDurationMs == 0);
    }

    const char* hashed = R"([
        {"videoID":"other","segments":[{"category":"sponsor","actionType":"skip","segment":[1,5]}]},
        {"videoID":"kJQP7kiw5Fk","hash":"f1d9","segments":[
            {"category":"music_offtopic","actionType":"skip","segment":[0,21.8]},
            {"category":"exclusive_access","actionType":"full","segment":[0,0]}]}
    ])";
    const auto h = SponsorBlock::parseResponse(hashed, "kJQP7kiw5Fk");
    CHECK(h.size() == 1 && h[0].endMs == 21800);
    CHECK(SponsorBlock::parseResponse(hashed, "notInBucket").empty());
    CHECK(SponsorBlock::parseResponse("[]", "x").empty());

    bool threw = false;
    try { SponsorBlock::parseResponse("Not Found", "x"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { SponsorBlock::parseResponse(R"({"error":1})", "x"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

void testNormalize() {
    std::printf("[normalize]\n");
    const auto n = SponsorBlock::normalize({
        {60000, 70000, "sponsor", 0},
        {0, 20000, "music_offtopic", 0},
        {19800, 25000, "selfpromo", 0},      // overlaps -> merged
        {25400, 30000, "interaction", 0},    // 400 ms gap -> merged
        {40000, 40800, "sponsor", 0},        // 0.8 s -> dropped
        {50000, 49000, "sponsor", 0},        // invalid -> dropped
        {70600, 80000, "selfpromo", 0},      // 600 ms gap -> separate
    });
    printSegments(n);
    CHECK(n.size() == 3);
    if (n.size() == 3) {
        CHECK(n[0].startMs == 0 && n[0].endMs == 30000 && n[0].category == "music_offtopic");
        CHECK(n[1].startMs == 60000 && n[1].endMs == 70000);
        CHECK(n[2].startMs == 70600 && n[2].endMs == 80000);
    }
    const auto c = SponsorBlock::normalize({{-1000, 5000, "music_offtopic", 0}});
    CHECK(c.size() == 1 && c[0].startMs == 0 && c[0].endMs == 5000);

    // Outdated submissions (video re-edited): dropped when the real length is known.
    const std::vector<Seg> withDur{{0, 20000, "a", 282000}, {30000, 40000, "b", 0}, {50000, 60000, "c", 200000}};
    CHECK(SponsorBlock::normalize(withDur, 281521).size() == 2);
    CHECK(SponsorBlock::normalize(withDur, 0).size() == 3);
}

void testCheck() {
    std::printf("[check: skip once, no loop, re-arm]\n");
    SponsorBlock sb;
    sb.setSegments({{0, 20000, "music_offtopic", 0}, {60000, 90000, "sponsor", 0}, {200000, 240000, "music_offtopic", 0}});
    CHECK(sb.segmentCount() == 3);
    CHECK(sb.state() == State::Ready);
    expectSkip(sb, 0, std::nullopt, "position 0 (track switching) never acts");
    expectSkip(sb, 200, 20000, "intro skipped at the first real tick");
    CHECK(sb.lastSkipped() && sb.lastSkipped()->category == "music_offtopic");
    expectSkip(sb, 450, std::nullopt, "stale position while the seek is in flight: no second seek");
    expectSkip(sb, 20050, std::nullopt, "after the seek");
    expectSkip(sb, 59700, std::nullopt, "before the sponsor");
    expectSkip(sb, 60100, 90000, "sponsor entered by normal playback");
    expectSkip(sb, 60300, std::nullopt, "stale position again");
    expectSkip(sb, 90050, std::nullopt, "after the seek");
    expectSkip(sb, 70000, std::nullopt, "user seeks back INTO the skipped sponsor: it plays");
    expectSkip(sb, 75000, std::nullopt, "...and keeps playing");
    expectSkip(sb, 50000, std::nullopt, "user seeks back in front of it: re-armed");
    expectSkip(sb, 60200, 90000, "skipped again on normal entry");
    expectSkip(sb, 120000, std::nullopt, "later");
    expectSkip(sb, 0, std::nullopt, "restart (previous / repeat-one) reports 0 first");
    expectSkip(sb, 250, 20000, "intro skipped again after the restart");
    expectSkip(sb, 100000, std::nullopt, "later");
    expectSkip(sb, 400, 20000, "seek straight to 0.4 s (restart) re-arms and skips the intro");
    expectSkip(sb, 30000, std::nullopt, "music");
    expectSkip(sb, 10000, std::nullopt, "deliberate seek to the middle of the skipped intro: plays");
    expectSkip(sb, 210000, 240000, "forward seek into a not-yet-skipped segment: skipped");

    std::printf("[check: edge cases]\n");
    SponsorBlock e;
    e.setSegments({{0, 20000, "music_offtopic", 0}});
    expectSkip(e, 19700, std::nullopt, "last 0.3 s of a segment: not worth a seek");
    expectSkip(e, 19900, std::nullopt, "...and disarmed");

    SponsorBlock t;   // an old video's never-entered intro must not fire on the 0 reported at a track switch
    t.setSegments({{0, 20000, "music_offtopic", 0}});
    expectSkip(t, 150000, std::nullopt, "resumed mid-track");
    expectSkip(t, 0, std::nullopt, "track switch transient");

    SponsorBlock off;
    off.setSegments({{10000, 20000, "sponsor", 0}});
    off.enabled = false;
    expectSkip(off, 15000, std::nullopt, "disabled");
    off.enabled = true;
    expectSkip(off, 15100, 20000, "enabled again");

    SponsorBlock none;
    none.setSegments({{10000, 20000, "sponsor", 0}});
    none.setVideo("");
    CHECK(none.segmentCount() == 0);
    CHECK(none.state() == State::Idle);
    expectSkip(none, 15000, std::nullopt, "setVideo(\"\") = no video");
}

// --- live tests -------------------------------------------------------------------------------------------

// Luis Fonsi - Despacito: music_offtopic [0, 21.8 s] (talk/ambience before the song) and [249.4, 281.5 s].
constexpr const char* kWithSegments = "kJQP7kiw5Fk";
constexpr int64_t kWithSegmentsDurationMs = 281521;
// Rick Astley - Never Gonna Give You Up: no segments in these categories (direct lookup = HTTP 404).
constexpr const char* kWithout = "dQw4w9WgXcQ";

void testLiveStatic() {
    std::printf("[live: fetchSegments]\n");
    const auto cats = SponsorBlock::defaultCategories();
    try {
        const auto hashed = SponsorBlock::fetchSegments(kWithSegments, cats, true);
        std::printf("    hash-prefix lookup %s: %zu segment(s)\n", kWithSegments, hashed.size());
        printSegments(hashed);
        const auto direct = SponsorBlock::fetchSegments(kWithSegments, cats, false);
        std::printf("    direct lookup      %s: %zu segment(s)\n", kWithSegments, direct.size());
        CHECK(!hashed.empty());
        CHECK(hashed.size() == direct.size());
        bool same = hashed.size() == direct.size();
        for (size_t i = 0; same && i < hashed.size(); ++i)
            same = hashed[i].startMs == direct[i].startMs && hashed[i].endMs == direct[i].endMs;
        CHECK(same);
    } catch (const std::exception& ex) {
        std::printf("  FAIL  lookup threw: %s\n", ex.what());
        ++g_failures;
    }
    try {
        CHECK(SponsorBlock::fetchSegments(kWithout, cats, false).empty());   // 404 -> empty, no throw
        CHECK(SponsorBlock::fetchSegments(kWithout, cats, true).empty());    // bucket without it -> empty
    } catch (const std::exception& ex) {
        std::printf("  FAIL  no-segment lookup threw: %s\n", ex.what());
        ++g_failures;
    }
}

void testLiveClass() {
    std::printf("[live: SponsorBlock via st::async + Dispatcher]\n");
    SponsorBlock sb;
    sb.setVideo(kWithSegments, kWithSegmentsDurationMs);
    CHECK(sb.state() == State::Loading);
    CHECK(waitLoaded(sb));
    std::printf("    %s: state %s, %d segment(s)\n", kWithSegments, stateName(sb.state()), sb.segmentCount());
    printSegments(sb.segments());
    CHECK(sb.state() == State::Ready);
    CHECK(sb.segmentCount() > 0);
    const auto target = sb.check(1000);
    std::printf("    check(1000) -> %s\n", str(target).c_str());
    CHECK(target && *target > 15000 && *target < 30000);

    sb.setVideo(kWithout);
    CHECK(waitLoaded(sb));
    std::printf("    %s: state %s, %d segment(s)\n", kWithout, stateName(sb.state()), sb.segmentCount());
    CHECK(sb.state() == State::Ready);
    CHECK(sb.segmentCount() == 0);
    CHECK(!sb.check(1000));

    sb.setVideo(kWithSegments, kWithSegmentsDurationMs);   // replay: served from the session cache
    CHECK(sb.state() == State::Ready && sb.segmentCount() > 0);

    std::printf("[live: a superseded lookup is dropped]\n");
    SponsorBlock stale;
    stale.setVideo(kWithSegments);
    stale.setVideo(kWithout);
    CHECK(waitLoaded(stale));
    pump([] { return false; }, 2500);   // give the first lookup every chance to (wrongly) land
    CHECK(stale.videoId() == kWithout);
    CHECK(stale.segmentCount() == 0);

    std::printf("[live: disabled -> no lookup; enabling looks up lazily]\n");
    SponsorBlock lazy;
    lazy.enabled = false;
    lazy.setVideo(kWithSegments);
    CHECK(lazy.state() == State::Idle);
    CHECK(!lazy.check(1000));
    lazy.enabled = true;
    CHECK(!lazy.check(1100));   // starts the lookup
    CHECK(lazy.state() == State::Loading);
    CHECK(waitLoaded(lazy));
    CHECK(lazy.segmentCount() > 0);

    std::printf("[live: changing categories re-fetches]\n");
    lazy.categories = {"filler"};
    lazy.check(1200);
    CHECK(lazy.state() == State::Loading);
    CHECK(waitLoaded(lazy));
    std::printf("    filler only: state %s, %d segment(s)\n", stateName(lazy.state()), lazy.segmentCount());
    printSegments(lazy.segments());
    CHECK(lazy.state() == State::Ready);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool offline = argc > 1 && std::string(argv[1]) == "--offline";

    st::ThreadPool pool(2);
    st::ThreadPool::setShared(&pool);
    st::Dispatcher::init();

    testSha256();
    testParse();
    testNormalize();
    testCheck();
    if (!offline) {
        testLiveStatic();
        testLiveClass();
    }

    st::Dispatcher::shutdown();
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    const int rc = g_failures ? 1 : 0;
    // Pool is joined before the process exits; nothing may post to it afterwards.
    return rc;
}
