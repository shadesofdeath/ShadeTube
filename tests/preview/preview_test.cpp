// Developer test for the song preview (offline, no network):
//   1. ui::HoldGesture stepped with explicit times: a press that rests 350 ms is due, a move past the slop or an early
//      release cancels it (a click / drag as before), a taken hold swallows its release, Esc (finish) still does.
//   2. preview::startPoint: unknown / short songs from the top, about 35 % in otherwise, past a SponsorBlock segment,
//      never so late that the 20 s don't fit.
//   3. preview::Session with a fake player and clock: modes, hold timing, release before the audio (cancelled, never
//      audible), the 20 s limit, Esc, the key's toggle, the song that already plays (a hint, no start), collections
//      (lookup, cache, nothing found), failures and timeouts, rapid repeated holds (every start stopped exactly once),
//      stale ids ignored.
//   4. A real player::Player (AudioEngine + WASAPI at volume 0) on generated WAVs: the current track holds its position
//      and state while a preview plays and continues from there afterwards; a paused one stays paused; with nothing
//      playing nothing plays afterwards; a preview stopped before its audio never holds the current track; rapid
//      previews leave no streams behind; a command ends the preview first; a preview that reaches its end hands back
//      by itself; keepMainRunning keeps the current track running muted (listen together); the queue, the current item
//      and onTrackChanged are untouched throughout.
//   preview_test
// Runs in a temporary profile (SHADETUBE_DATA_DIR) and needs an audio output device for part 4.
#include "app/PreviewSession.h"
#include "core/Dispatcher.h"
#include "player/Player.h"
#include "ui/HoldGesture.h"
#include "youtube/MatchService.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using st::catalog::Track;
namespace pv = st::app::preview;

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

Track T(const std::string& id, int durationMs = 200000) {
    Track t;
    t.id = id;
    t.name = "Song " + id;
    t.artists = {{"", "Tester"}};
    t.durationMs = durationMs;
    return t;
}

// ---- 1. Hold gesture ---------------------------------------------------------------------------------------------------
void testGesture() {
    std::printf("hold gesture\n");
    using st::ui::HoldGesture;
    HoldGesture g;
    g.delayMs = 350;
    g.slop = 6;
    g.press({100, 100}, 1000);
    CHECK(g.pending() && !g.due(1349) && g.due(1350));
    CHECK(g.remainingMs(1200) == 150);
    CHECK(g.move({104, 103}));               // 5 DIPs: still a hold
    CHECK(g.pending());
    g.take();
    CHECK(g.taken() && g.owned());
    CHECK(g.release());                      // the release is the hold's: no click
    CHECK(g.state() == HoldGesture::State::Idle);

    g.press({100, 100}, 0);
    CHECK(!g.move({107, 100}));              // past the slop: a drag starts as before
    CHECK(!g.due(1000));
    CHECK(!g.release());                     // an ordinary release (click / drop)

    g.press({100, 100}, 0);
    CHECK(!g.release());                     // released early: a click
    CHECK(!g.due(500));

    g.press({0, 0}, 0);
    g.take();
    g.finish();                              // Esc: ended, but the release is still swallowed
    CHECK(!g.taken() && g.owned());
    CHECK(!g.move({50, 50}));                // moves don't revive it
    CHECK(g.release());

    g.press({0, 0}, 0);
    g.cancel();                              // the app declined / another button / capture lost
    CHECK(!g.pending() && !g.release());
    g.take();                                // taking an idle gesture does nothing
    CHECK(!g.owned());
}

// ---- 2. Start point -----------------------------------------------------------------------------------------------------
void testStartPoint() {
    std::printf("start point\n");
    CHECK(pv::startPoint(0) == 0);
    CHECK(pv::startPoint(-5) == 0);
    CHECK(pv::startPoint(25'000) == 0);                       // shorter than the preview + 10 s: the top
    CHECK(pv::startPoint(200'000) == 70'000);                 // 35 %
    CHECK(pv::startPoint(32'000) == 11'200);                  // 35 %, and 20 s still fit
    CHECK(pv::startPoint(40'000) + 20'000 <= 40'000);
    // Inside a segment (or just before it): after it.
    CHECK(pv::startPoint(200'000, {{60'000, 80'000}}) == 80'500);
    CHECK(pv::startPoint(200'000, {{70'500, 75'000}}) == 75'500);    // starts 500 ms after 35 %: counts as inside
    // Back-to-back segments.
    CHECK(pv::startPoint(200'000, {{85'000, 95'000}, {60'000, 85'000}}) == 95'500);
    // Pushed past the end: the music before the segment instead.
    const int64_t s = pv::startPoint(100'000, {{30'000, 95'000}});
    CHECK(s == 10'000);
    CHECK(pv::startPoint(100'000, {{0, 100'000}}) >= 0);      // all of it skipped: still a valid position
    CHECK(pv::startPoint(200'000, {{80'000, 60'000}}) == 70'000);    // an invalid segment is ignored
}

// ---- 3. Session ---------------------------------------------------------------------------------------------------------
struct FakePort : pv::Port {
    std::string current;
    int starts = 0, stops = 0, lookups = 0;
    uint64_t startedId = 0, lookupId = 0;
    std::string startedTrack;
    pv::Collection lastLookup;
    bool refuseStart = false, refuseLookup = false;

    std::string currentTrackId() const override { return current; }
    bool canPreview(const Track& t) const override { return t.playable && t.id.rfind("radio:", 0) != 0; }
    bool lookup(uint64_t id, const pv::Collection& c) override {
        ++lookups;
        lookupId = id;
        lastLookup = c;
        return !refuseLookup;
    }
    bool start(uint64_t id, const Track& t) override {
        if (refuseStart) return false;
        ++starts;
        startedId = id;
        startedTrack = t.id;
        return true;
    }
    void stop(uint64_t) override { ++stops; }
};

pv::Target album(const std::string& id) {
    pv::Target t;
    t.collection = {pv::Collection::Kind::Album, id};
    t.title = L"Album";
    return t;
}

void testSession() {
    std::printf("session\n");
    using pv::Phase;
    using pv::Trigger;
    FakePort port;
    pv::Session s(port);
    double now = 0;

    // Modes: off refuses everything; "menu" refuses holds but takes the menu and the key.
    s.mode = pv::Mode::Off;
    CHECK(!s.begin(pv::songTarget(T("a")), Trigger::Menu, now));
    s.mode = pv::Mode::Menu;
    CHECK(!s.begin(pv::songTarget(T("a")), Trigger::Hold, now));
    CHECK(port.starts == 0);
    CHECK(s.begin(pv::songTarget(T("a")), Trigger::Menu, now));
    CHECK(port.starts == 1 && s.phase() == Phase::Resolving);
    CHECK(s.escape());
    CHECK(port.stops == 1 && !s.active());
    CHECK(!s.escape());                                       // nothing left to stop
    s.mode = pv::Mode::Hold;
    CHECK(pv::parseMode("menu") == pv::Mode::Menu && pv::parseMode("off") == pv::Mode::Off);
    CHECK(pv::parseMode("") == pv::Mode::Hold && pv::parseMode("x") == pv::Mode::Hold);
    CHECK(std::string(pv::modeName(pv::Mode::Menu)) == "menu");

    // Not a song: a station is not taken (the press stays a click), nor is an empty target.
    Track station = T("radio:1");
    CHECK(!s.begin(pv::songTarget(station), Trigger::Hold, now));
    CHECK(!s.begin(pv::Target{}, Trigger::Hold, now));
    Track unplayable = T("u");
    unplayable.playable = false;
    CHECK(!s.begin(pv::songTarget(unplayable), Trigger::Hold, now));

    // Hold released before the audio: cancelled, never audible; a late audio start is ignored.
    port = FakePort{};
    CHECK(s.begin(pv::songTarget(T("b")), Trigger::Hold, now = 1000));
    const uint64_t id1 = s.id();
    CHECK(s.held() && s.phase() == Phase::Resolving && port.startedTrack == "b");
    s.release(now += 400);
    CHECK(!s.active() && port.stops == 1);
    s.audioStarted(id1, now += 10);
    CHECK(!s.active());                                       // late: dropped
    s.release(now);                                           // a second release does nothing
    CHECK(port.stops == 1);

    // Hold: audio starts, progress runs, release stops it once.
    port = FakePort{};
    CHECK(s.begin(pv::songTarget(T("c")), Trigger::Hold, now = 10'000));
    s.audioStarted(s.id(), now += 600);
    CHECK(s.phase() == Phase::Playing);
    CHECK(std::abs(s.progress(now + 5000) - 0.25f) < 0.001f);
    s.tick(now += 5000);
    CHECK(s.phase() == Phase::Playing);
    s.release(now);
    CHECK(!s.active() && port.starts == 1 && port.stops == 1);

    // The 20 s limit counts from the audio, not from the press; the later release does nothing.
    port = FakePort{};
    CHECK(s.begin(pv::songTarget(T("d")), Trigger::Hold, now = 50'000));
    s.tick(now += 5000);                                      // still resolving after 5 s: fine
    CHECK(s.phase() == Phase::Resolving);
    s.audioStarted(s.id(), now);
    s.tick(now + pv::kLimitMs - 1);
    CHECK(s.phase() == Phase::Playing);
    s.tick(now + pv::kLimitMs);
    CHECK(!s.active() && port.stops == 1);
    s.release(now + pv::kLimitMs + 3000);
    CHECK(port.stops == 1);

    // Esc during a hold.
    port = FakePort{};
    s.begin(pv::songTarget(T("e")), Trigger::Hold, now = 0);
    s.audioStarted(s.id(), now += 300);
    CHECK(s.escape() && port.stops == 1);
    s.release(now += 100);
    CHECK(port.stops == 1);

    // The key toggles: a second press stops; a key preview runs without a held button.
    port = FakePort{};
    CHECK(s.toggle(pv::songTarget(T("f")), now = 0));
    CHECK(s.trigger() == Trigger::Key && !s.held() && port.starts == 1);
    s.release(now);                                           // no button: no effect
    CHECK(s.active());
    CHECK(s.toggle(pv::songTarget(T("g")), now += 10));
    CHECK(!s.active() && port.stops == 1 && port.starts == 1);

    // The song that already plays: a hint, no start; a hold keeps it until the release, the menu for kHintMs.
    port = FakePort{};
    port.current = "h";
    CHECK(s.begin(pv::songTarget(T("h")), Trigger::Hold, now = 0));
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::AlreadyPlaying && port.starts == 0);
    s.tick(now += pv::kHintMs * 3);
    CHECK(s.phase() == Phase::Hint);                          // held: stays
    s.release(now);
    CHECK(!s.active() && port.stops == 0);
    CHECK(s.begin(pv::songTarget(T("h")), Trigger::Menu, now = 0));
    s.tick(now += pv::kHintMs - 1);
    CHECK(s.phase() == Phase::Hint);
    s.tick(now += 1);
    CHECK(!s.active());
    port.current.clear();

    // Collections: looked up first, then cached for the run (no second lookup).
    port = FakePort{};
    CHECK(s.begin(album("x"), Trigger::Hold, now = 0));
    CHECK(s.phase() == Phase::Looking && port.lookups == 1 && port.lastLookup.key() == "album:x");
    s.found(port.lookupId, T("x1"), now += 300);
    CHECK(s.phase() == Phase::Resolving && port.startedTrack == "x1" && s.song() && s.song()->id == "x1");
    s.release(now += 100);
    CHECK(port.stops == 1);
    CHECK(s.begin(album("x"), Trigger::Hold, now += 1000));
    CHECK(port.lookups == 1 && port.starts == 2 && s.phase() == Phase::Resolving);
    s.release(now);
    // Released while looking up: nothing starts when the answer comes.
    CHECK(s.begin(album("y"), Trigger::Hold, now += 1000));
    const uint64_t lookY = port.lookupId;
    s.release(now += 50);
    s.found(lookY, T("y1"), now += 500);
    CHECK(!s.active() && port.starts == 2);
    // ... and the next time it is looked up again (nothing was cached).
    CHECK(s.begin(album("y"), Trigger::Menu, now += 10));
    CHECK(port.lookups == 3);
    // Nothing found: a hint.
    s.notFound(port.lookupId, now += 100);
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::NoSong);
    s.escape();
    // A collection whose song is the one playing.
    port.current = "x1";
    CHECK(s.begin(album("x"), Trigger::Hold, now += 10));
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::AlreadyPlaying);
    s.release(now);
    port.current.clear();
    // A lookup that can't run.
    port.refuseLookup = true;
    CHECK(s.begin(album("z"), Trigger::Menu, now += 10));
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::NoSong);
    s.escape();
    port.refuseLookup = false;

    // Failures: the player's failure stops it (back to idle) and shows a hint; a refused start too.
    port = FakePort{};
    s.begin(pv::songTarget(T("k")), Trigger::Hold, now = 0);
    s.failed(s.id(), now += 800);
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::Failed && port.stops == 1);
    s.release(now);
    CHECK(port.stops == 1 && !s.active());
    port.refuseStart = true;
    CHECK(s.begin(pv::songTarget(T("k")), Trigger::Menu, now));
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::Failed);
    s.escape();
    port.refuseStart = false;
    // No audio within the timeout: given up.
    port = FakePort{};
    s.begin(pv::songTarget(T("t")), Trigger::Hold, now = 0);
    s.tick(now + pv::kResolveTimeoutMs);
    CHECK(s.phase() == Phase::Hint && s.hint() == pv::Hint::Failed && port.stops == 1);
    s.release(now);
    CHECK(port.stops == 1);
    // The player ended it itself (its end, a playback command): no stop owed, the release does nothing.
    port = FakePort{};
    s.begin(pv::songTarget(T("n")), Trigger::Hold, now = 0);
    s.audioStarted(s.id(), now += 100);
    s.ended(s.id());
    CHECK(!s.active());
    s.release(now += 100);
    CHECK(port.stops == 0);

    // Rapid repeated holds: each new one stops the last first; every start is stopped exactly once in the end.
    port = FakePort{};
    for (int i = 0; i < 25; ++i) {
        s.begin(pv::songTarget(T("r" + std::to_string(i % 3))), i % 4 ? Trigger::Hold : Trigger::Menu, now += 37);
        if (i % 2) s.audioStarted(s.id(), now += 5);
        if (i % 3 == 0) s.release(now += 3);
        CHECK(port.starts - port.stops == (s.active() && s.phase() != Phase::Hint ? 1 : 0));
    }
    s.escape();
    CHECK(port.starts == port.stops && port.starts == 25);
    // Stale ids are ignored.
    s.begin(pv::songTarget(T("s1")), Trigger::Hold, now += 10);
    const uint64_t old = s.id();
    s.begin(pv::songTarget(T("s2")), Trigger::Hold, now += 10);
    s.audioStarted(old, now);
    s.failed(old, now);
    s.ended(old);
    CHECK(s.phase() == Phase::Resolving && port.startedTrack == "s2");
    s.release(now);
    CHECK(port.starts == port.stops);
}

// ---- 4. A real player ---------------------------------------------------------------------------------------------------
// A tone of `ms` (16-bit stereo, 44.1 kHz).
void writeWav(const fs::path& path, uint32_t ms, double hz) {
    constexpr uint32_t rate = 44100, channels = 2;
    const uint32_t frames = rate * ms / 1000;
    std::vector<int16_t> pcm(size_t(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(6000 * std::sin(2 * 3.14159265358979 * hz * i / rate));
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + data);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(channels * 2);
    u16(16);
    f.write("data", 4);
    u32(data);
    f.write(reinterpret_cast<const char*>(pcm.data()), data);
}

template <class F>
bool pumpUntil(F done, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (!done()) {
        if (GetTickCount() - start > timeoutMs) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return true;
}
void pumpFor(DWORD ms) { pumpUntil([] { return false; }, ms); }

void testPlayer(const fs::path& tmp) {
    std::printf("player: preview voice (local WAVs, volume 0)\n");
    using st::player::Player;
    using st::player::Status;
    using PS = Player::PreviewState;
    st::Dispatcher::init();
    std::map<std::string, fs::path> files;
    const std::map<std::string, std::pair<uint32_t, double>> specs{
        {"m", {30000, 440}}, {"n", {30000, 550}}, {"p", {40000, 660}}, {"s", {1500, 880}}};
    for (const auto& [id, spec] : specs) {
        files[id] = tmp / (id + ".wav");
        writeWav(files[id], spec.first, spec.second);
    }
    st::youtube::MatchService matcher{YoutubeExplode::YoutubeClient()};
    const Track main = T("m", 30000), main2 = T("n", 30000), sample = T("p", 40000), shortSample = T("s", 1500);
    {
        Player player(matcher);
        player.setVolume(0.f);
        player.localFileFor = [&](const std::string& id) {
            const auto it = files.find(id);
            return it == files.end() ? std::wstring{} : it->second.wstring();
        };
        std::vector<std::string> started;
        player.onTrackChanged = [&](const Track& t) { started.push_back(t.id); };
        std::vector<PS> states;
        player.onPreview = [&](uint64_t, PS s) { states.push_back(s); };

        // a) The current track holds while a preview plays and continues from there.
        player.playContext({main, main2}, 0, {"test", L"Test"});
        CHECK(pumpUntil([&] { return player.status() == Status::Playing && player.positionMs() > 1200; }, 8000));
        CHECK(player.startPreview(sample) != 0);
        CHECK(player.previewState() == PS::Resolving);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        pumpFor(500);   // the 250 ms fade-out, then held
        const int64_t held = player.positionMs();
        pumpFor(1200);
        const int64_t heldLater = player.positionMs();
        std::printf("        held at %lld ms, %lld ms 1.2 s later; preview at %lld ms\n", static_cast<long long>(held),
                    static_cast<long long>(heldLater), static_cast<long long>(player.previewPositionMs()));
        CHECK(std::llabs(heldLater - held) < 40);
        CHECK(player.status() == Status::Playing);           // its state is untouched
        CHECK(player.current() && player.current()->id == "m");
        CHECK(player.items().size() == 2 && player.currentOrderIndex() == 0);
        CHECK(started.size() == 1);
        CHECK(player.previewPositionMs() > 14000 + 1000);   // started 35 % in (14 s of 40 s) and running
        player.stopPreview();
        CHECK(player.previewState() == PS::Idle);
        pumpFor(1000);
        const int64_t resumed = player.positionMs();
        std::printf("        1 s after the stop: %lld ms\n", static_cast<long long>(resumed));
        CHECK(resumed > held + 600 && resumed < held + 1300);   // continues from where it was held
        CHECK(player.status() == Status::Playing);

        // b) A paused track stays paused, at its position.
        player.pause();
        CHECK(pumpUntil([&] { return player.status() == Status::Paused; }, 3000));
        pumpFor(200);
        const int64_t pausedAt = player.positionMs();
        player.startPreview(sample);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        pumpFor(600);
        player.stopPreview();
        pumpFor(700);
        CHECK(player.status() == Status::Paused);
        CHECK(std::llabs(player.positionMs() - pausedAt) < 40);

        // c) Stopped before its audio: the current track never holds (no blip, no lost position).
        player.play();
        CHECK(pumpUntil([&] { return player.status() == Status::Playing; }, 3000));
        pumpFor(300);
        states.clear();
        player.startPreview(sample);
        player.stopPreview();
        const int64_t before = player.positionMs();
        pumpFor(800);
        CHECK(std::find(states.begin(), states.end(), PS::Playing) == states.end());
        CHECK(player.positionMs() > before + 500);

        // d) Rapid repeated previews: nothing is left behind and the current track runs on.
        pumpFor(300);
        const size_t baseBytes = player.bufferedBytes();
        for (int i = 0; i < 6; ++i) {
            player.startPreview(i % 2 ? sample : shortSample, i % 2 ? -1 : 0);
            pumpFor(i % 3 == 0 ? 700 : 90);   // some become audible, some don't
        }
        player.stopPreview();
        pumpFor(1500);
        const int64_t p1 = player.positionMs();
        pumpFor(500);
        std::printf("        after rapid previews: %zu bytes buffered (%zu before), position %lld -> %lld\n",
                    player.bufferedBytes(), baseBytes, static_cast<long long>(p1), static_cast<long long>(player.positionMs()));
        CHECK(player.positionMs() > p1 + 300);
        CHECK(player.bufferedBytes() <= baseBytes + 64 * 1024);
        CHECK(player.status() == Status::Playing);

        // e) A queue edit leaves the preview playing; a command ends it first, then applies (pause), and the current
        // track comes back paused.
        player.startPreview(sample);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        player.enqueue({main2});
        CHECK(player.previewState() == PS::Playing && player.items().size() == 3);
        player.removeAt(2);
        CHECK(player.previewState() == PS::Playing && player.items().size() == 3 && player.order().size() == 2);
        player.togglePause();
        CHECK(player.previewState() == PS::Idle);
        CHECK(pumpUntil([&] { return player.status() == Status::Paused; }, 3000));
        player.play();
        CHECK(pumpUntil([&] { return player.status() == Status::Playing; }, 3000));

        // f) A preview that reaches its end hands back by itself.
        states.clear();
        player.startPreview(shortSample, 0);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        CHECK(pumpUntil([&] { return player.previewState() == PS::Idle; }, 5000));
        pumpFor(800);
        const int64_t after = player.positionMs();
        pumpFor(500);
        CHECK(player.positionMs() > after + 300);

        // g) keepMainRunning (listen together): the current track keeps running muted under the preview.
        player.startPreview(sample, -1, true);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        const int64_t g0 = player.positionMs();
        pumpFor(1000);
        CHECK(player.positionMs() > g0 + 700);
        player.stopPreview();

        // i) At another speed (1.5x through the time stretcher) the current track holds and resumes the same way, and the
        // preview plays at 1x.
        player.setSpeed(1.5f);
        pumpFor(500);
        player.startPreview(sample);
        CHECK(pumpUntil([&] { return player.previewState() == PS::Playing; }, 5000));
        pumpFor(500);
        const int64_t s0 = player.positionMs(), pv0 = player.previewPositionMs();
        pumpFor(1000);
        const int64_t pvAdvance = player.previewPositionMs() - pv0;
        CHECK(std::llabs(player.positionMs() - s0) < 40);
        CHECK(pvAdvance > 700 && pvAdvance < 1300);   // 1x
        player.stopPreview();
        pumpFor(1000);
        const int64_t s1 = player.positionMs() - s0;
        std::printf("        1.5x: %lld ms of track in 1 s after the stop\n", static_cast<long long>(s1));
        CHECK(s1 > 1000 && s1 < 1900);
        player.setSpeed(1.f);

        // A station / an episode is refused; nothing of the queue changed through all of it.
        player.liveStreamFor = [](const Track& t) -> std::optional<Player::LiveStream> {
            if (t.id == "radio:x") return Player::LiveStream{"http://127.0.0.1/none", {}, true};
            return std::nullopt;
        };
        CHECK(player.startPreview(T("radio:x")) == 0 && player.previewState() == PS::Idle);
        player.liveStreamFor = nullptr;
        CHECK(started == std::vector<std::string>{"m"});
        CHECK(player.order().size() == 2 && player.current() && player.current()->id == "m");
        player.pause();
        pumpFor(300);
    }
    {
        // h) Nothing playing: the preview plays, and nothing plays afterwards.
        Player idle(matcher);
        idle.setVolume(0.f);
        idle.localFileFor = [&](const std::string& id) {
            const auto it = files.find(id);
            return it == files.end() ? std::wstring{} : it->second.wstring();
        };
        CHECK(idle.status() == Status::Idle && !idle.current());
        idle.startPreview(sample);
        CHECK(pumpUntil([&] { return idle.previewState() == Player::PreviewState::Playing; }, 5000));
        pumpFor(500);
        idle.stopPreview();
        pumpFor(600);
        CHECK(idle.status() == Status::Idle && !idle.current());
    }
    st::Dispatcher::shutdown();
}

} // namespace

int wmain() {
    // Per process: parallel runs (several checkouts) must not delete each other's files.
    const fs::path tmp = fs::temp_directory_path() / (L"shadetube_preview_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (tmp / L"profile").c_str());   // never the user's profile
    testGesture();
    testStartPoint();
    testSession();
    testPlayer(tmp);
    fs::remove_all(tmp, ec);
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
