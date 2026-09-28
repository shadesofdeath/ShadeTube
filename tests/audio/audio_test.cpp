// Developer test for st::audio::AudioEngine against real YouTube streams.
// You cannot hear anything from CI: the program verifies behaviour through positions, states,
// events and memory accounting, and exits non-zero on detected failures.
//
//   audio_test.exe [videoA] [videoB]
#include "audio/AudioEngine.h"
#include "core/Log.h"

#include <YoutubeExplode.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using st::audio::AudioEngine;
using st::audio::State;
using st::audio::StreamSource;

namespace {

std::mutex g_print;
int g_failures = 0;

template <class... A>
void say(std::format_string<A...> fmt, A&&... args) {
    std::lock_guard lock(g_print);
    std::fputs(std::format(fmt, std::forward<A>(args)...).c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void check(bool ok, const std::string& what) {
    say("  [{}] {}", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

const char* name(State s) {
    switch (s) {
    case State::Idle: return "Idle";
    case State::Loading: return "Loading";
    case State::Playing: return "Playing";
    case State::Paused: return "Paused";
    case State::Ended: return "Ended";
    case State::Error: return "Error";
    }
    return "?";
}

std::string clock(int64_t ms) { return std::format("{}:{:02}.{:01}", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10); }

struct Events {
    std::mutex m;
    std::vector<std::pair<uint64_t, uint64_t>> ended;
    std::vector<std::string> errors;
};

std::optional<StreamSource> resolve(YoutubeExplode::YoutubeClient& yt, const std::string& id, const std::string& container,
                                    uint64_t tag) {
    try {
        const auto t0 = std::chrono::steady_clock::now();
        auto manifest = yt.videos().streams().getManifest(id);
        auto audio = manifest.tryGetBestAudioOnlyStream(container);
        if (!audio) {
            say("  no {} audio stream for {}", container, id);
            return std::nullopt;
        }
        StreamSource s;
        s.url = audio->url();
        s.contentLength = audio->size().bytes();
        s.mimeType = "audio/" + container;
        s.tag = tag;
        say("  {} [{}] {} {} bytes, codec {} (manifest {} ms)", id, tag, audio->mimeType(), s.contentLength,
            audio->audioCodec(),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        return s;
    } catch (const std::exception& e) {
        say("  !! YouTube request for {} failed: {}", id, e.what());
        return std::nullopt;
    }
}

class Runner {
public:
    explicit Runner(AudioEngine& e) : engine(e) {}
    AudioEngine& engine;

    void status() {
        float bands[16]{};
        const bool live = engine.spectrum(bands, 16);
        std::string bar;
        static const char* levels = " .:-=+*#%@";
        for (float b : bands) bar.push_back(live ? levels[std::min(9, static_cast<int>(b * 9.99f))] : ' ');
        say("    {:<8} tag {} {} / {}  buf {:5.1f}%  mem {:7.1f} KB  vol {:.2f}  |{}|", name(engine.state()),
            engine.currentTag(), clock(engine.positionMs()), clock(engine.durationMs()),
            engine.bufferedFraction() * 100.f, engine.bufferedBytes() / 1024.0, engine.volume(), bar);
    }

    // Waits (printing status every 500 ms) until `done` or the timeout; returns done().
    bool wait(std::chrono::milliseconds timeout, const std::function<bool()>& done = {}) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        auto nextPrint = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < end) {
            if (done && done()) {
                status();
                return true;
            }
            if (std::chrono::steady_clock::now() >= nextPrint) {
                status();
                nextPrint += 500ms;
            }
            std::this_thread::sleep_for(16ms);  // "UI" polling at ~60 fps
        }
        status();
        return done ? done() : true;
    }
};

bool playsFor(Runner& r, std::chrono::milliseconds span, int64_t minAdvanceMs) {
    const int64_t p0 = r.engine.positionMs();
    const auto t0 = std::chrono::steady_clock::now();
    r.wait(span);
    const int64_t advance = r.engine.positionMs() - p0;
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    say("    position advanced {} ms in {} ms wall time", advance, wall);
    return advance >= minAdvanceMs && advance <= wall + 150;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    st::log::init();
    const std::string idA = argc > 1 ? argv[1] : "dQw4w9WgXcQ";
    const std::string idB = argc > 2 ? argv[2] : "kJQP7kiw5Fk";

    say("supportsWebm() = {}", AudioEngine::supportsWebm());

    Events ev;
    st::audio::EngineEvents callbacks;
    callbacks.onState = [](State s, uint64_t tag) { say("  <event> state {} (tag {})", name(s), tag); };
    callbacks.onEnded = [&](uint64_t finished, uint64_t next) {
        say("  <event> ended {} -> next {}", finished, next);
        std::lock_guard lock(ev.m);
        ev.ended.emplace_back(finished, next);
    };
    callbacks.onError = [&](st::audio::ErrorKind kind, const std::string& msg, uint64_t tag) {
        say("  <event> ERROR kind {} tag {}: {}", static_cast<int>(kind), tag, msg);
        std::lock_guard lock(ev.m);
        ev.errors.push_back(msg);
    };

    // Created first, like the app does at startup (the engine warms up the audio device while
    // the streams are being resolved).
    auto enginePtr = std::make_unique<AudioEngine>(callbacks);

    YoutubeExplode::YoutubeClient yt;
    say("Resolving streams...");
    auto a = resolve(yt, idA, "mp4", 1);
    auto b = resolve(yt, idB, "mp4", 2);
    if (!a || !b) {
        say("!! Could not resolve YouTube streams (blocked or offline?) - cannot run the engine test.");
        return 2;
    }

    {
        AudioEngine& engine = *enginePtr;
        Runner r(engine);
        engine.setVolume(0.8f);

        // 1. open + play ------------------------------------------------------------------------
        say("\n== 1. open {} and play", idA);
        const auto t0 = std::chrono::steady_clock::now();
        engine.open(*a);
        const bool started = r.wait(20s, [&] { return engine.state() == State::Playing; });
        check(started, std::format("playback started ({} ms after open)",
                                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
        if (!started) {
            say("!! aborting: playback never started");
            return 1;
        }
        check(playsFor(r, 2500ms, 2000), "position advances in real time");
        check(engine.durationMs() > 60'000, std::format("duration known ({})", clock(engine.durationMs())));

        // 2. seek to 70% (far beyond the download frontier right after start) -------------------
        const int64_t dur = engine.durationMs();
        const int64_t target = dur * 7 / 10;
        say("\n== 2. seek to 70% ({})", clock(target));
        engine.seek(target);
        std::this_thread::sleep_for(50ms);
        check(std::llabs(engine.positionMs() - target) < 300, std::format("position jumped to {}", clock(engine.positionMs())));
        const bool resumed = r.wait(15s, [&] { return engine.state() == State::Playing && engine.positionMs() > target + 300; });
        check(resumed, "playback continues after the seek");
        check(playsFor(r, 5000ms, 4300), "position advances after the seek");
        check(engine.positionMs() >= target && engine.positionMs() < target + 8000, "position is near the seek target");

        // 2b. seek back into the not-yet-downloaded gap
        say("\n== 2b. seek back to 15%");
        engine.seek(dur * 15 / 100);
        r.wait(10s, [&] { return engine.state() == State::Playing && engine.positionMs() > dur * 15 / 100 + 300; });
        check(playsFor(r, 2000ms, 1500), "plays after seeking back");

        // 2c. re-open and seek to 70% immediately (random access beyond the download frontier)
        say("\n== 2c. re-open and seek to 70% after 120 ms (before the download reaches it)");
        engine.open(*a);
        std::this_thread::sleep_for(120ms);
        engine.seek(target);
        const bool farSeek = r.wait(15s, [&] { return engine.state() == State::Playing && engine.positionMs() > target + 300; });
        check(farSeek, "plays at 70% right after open");
        say("    buffered {:.0f}% when playback resumed", engine.bufferedFraction() * 100);
        check(playsFor(r, 2000ms, 1500), "position advances after the far seek");
        engine.seek(dur * 15 / 100);
        r.wait(10s, [&] { return engine.state() == State::Playing && engine.positionMs() > dur * 15 / 100 + 300; });
        check(playsFor(r, 1500ms, 1000), "plays after seeking back into the gap");

        // 3. pause ------------------------------------------------------------------------------
        say("\n== 3. pause 2 s");
        engine.pause();
        r.wait(2s, [&] { return engine.state() == State::Paused; });
        std::this_thread::sleep_for(300ms);  // let the ~50 ms fade/device tail finish
        const int64_t pausedAt = engine.positionMs();
        r.wait(2000ms);
        check(engine.state() == State::Paused, "state is Paused");
        check(std::llabs(engine.positionMs() - pausedAt) <= 5, std::format("position frozen while paused ({} -> {})",
                                                                          pausedAt, engine.positionMs()));
        float bands[32];
        check(!engine.spectrum(bands, 32), "spectrum() returns false while paused");

        // 4. resume -----------------------------------------------------------------------------
        say("\n== 4. resume");
        engine.play();
        r.wait(3s, [&] { return engine.state() == State::Playing; });
        check(playsFor(r, 2000ms, 1500), "position advances after resume");
        check(std::llabs(engine.positionMs() - pausedAt) < 3000, "resumed where it paused");
        bool anyBand = false;
        const bool live = engine.spectrum(bands, 32);
        std::string values;
        for (float v : bands) {
            anyBand |= live && v > 0.05f;
            values += std::format("{:.2f} ", v);
        }
        say("    spectrum(32) live={} {}", live, values);
        check(anyBand, "spectrum shows energy while playing");

        // 5. volume ramp ------------------------------------------------------------------------
        say("\n== 5. volume ramp 0.8 -> 0.1 -> 1.0");
        for (int i = 0; i <= 10; ++i) {
            engine.setVolume(0.8f - 0.07f * i);
            std::this_thread::sleep_for(60ms);
        }
        r.status();
        for (int i = 0; i <= 10; ++i) {
            engine.setVolume(0.1f + 0.09f * i);
            std::this_thread::sleep_for(60ms);
        }
        r.status();
        check(std::abs(engine.volume() - 1.0f) < 0.01f && engine.state() == State::Playing, "volume ramp done, still playing");

        // 6. gapless transition -----------------------------------------------------------------
        say("\n== 6. preload {} and seek near the end of {}", idB, idA);
        engine.preload(*b);
        r.wait(3000ms, [&] { return engine.bufferedBytes() > static_cast<size_t>(a->contentLength); });
        check(engine.bufferedBytes() > static_cast<size_t>(a->contentLength), "preload holds a second buffer");
        engine.seek(engine.durationMs() - 4000);
        const bool switched = r.wait(20s, [&] {
            std::lock_guard lock(ev.m);
            return !ev.ended.empty();
        });
        {
            std::lock_guard lock(ev.m);
            check(switched && ev.ended.size() == 1 && ev.ended[0] == std::pair<uint64_t, uint64_t>{1, 2},
                  "onEnded(1, 2) fired for the gapless transition");
        }
        check(engine.currentTag() == 2, "current tag is the preloaded track");
        check(engine.positionMs() < 2000, std::format("position restarted ({})", clock(engine.positionMs())));
        check(engine.state() == State::Playing, "still playing after the transition");
        check(playsFor(r, 3000ms, 2500), "second track plays");
        check(engine.bufferedBytes() <= static_cast<size_t>(b->contentLength),
              std::format("first track's buffer released ({} bytes held)", engine.bufferedBytes()));

        // 7. stop -------------------------------------------------------------------------------
        say("\n== 7. stop");
        engine.stop();
        r.wait(2s, [&] { return engine.state() == State::Idle && engine.bufferedBytes() == 0; });
        check(engine.state() == State::Idle, "state is Idle");
        check(engine.bufferedBytes() == 0, std::format("bufferedBytes() == 0 ({})", engine.bufferedBytes()));

        // 8. WebM / Opus --------------------------------------------------------------------------
        if (AudioEngine::supportsWebm()) {
            say("\n== 8. WebM/Opus");
            if (auto w = resolve(yt, idA, "webm", 3)) {
                engine.open(*w, 60'000);
                const bool ok = r.wait(20s, [&] { return engine.state() == State::Playing; });
                check(ok, "webm playback started (at 1:00)");
                if (ok) check(playsFor(r, 4000ms, 3300), "webm position advances");
                check(engine.positionMs() >= 60'000, "webm started at the requested position");
                engine.stop();
                r.wait(2s, [&] { return engine.bufferedBytes() == 0; });
            }
        } else {
            say("\n== 8. WebM/Opus not supported on this machine - skipped");
        }

        {
            std::lock_guard lock(ev.m);
            check(ev.errors.empty(), std::format("no engine errors ({})", ev.errors.size()));
        }
        say("\n== destroying engine");
    }
    enginePtr.reset();  // joins every thread; MFShutdown
    say("\n{} failure(s)", g_failures);
    st::log::shutdown();
    return g_failures ? 1 : 0;
}
