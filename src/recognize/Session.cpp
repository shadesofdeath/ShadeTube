#include "recognize/Session.h"

#include "core/Log.h"

#include <windows.h>

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace st::recognize {

namespace {

constexpr char kTag[] = "recognize";

using Clock = std::chrono::steady_clock;

Failure failureOf(CaptureError e) {
    switch (e) {
    case CaptureError::NoDevice: return Failure::NoDevice;
    case CaptureError::DeviceBusy: return Failure::DeviceBusy;
    case CaptureError::AccessDenied: return Failure::AccessDenied;
    case CaptureError::None:
    case CaptureError::Failed: break;
    }
    return Failure::Capture;
}

// One request out: answered on its own thread, read by the session's.
struct Pending {
    std::mutex m;
    bool done = false;
    std::optional<Answer> answer;
    Failure failure = Failure::None;
    int status = 0;
    YoutubeExplode::CancellationTokenSource cts;
};

} // namespace

const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Listening: return "listening";
    case Phase::Identifying: return "identifying";
    case Phase::Matched: return "matched";
    case Phase::NotFound: return "not found";
    case Phase::Silent: return "silent";
    case Phase::Failed: return "failed";
    }
    return "?";
}

struct Session::Shared {
    std::mutex m;
    std::function<void(const Event&)> onEvent;
    std::shared_ptr<Pending> pending;

    void emit(const Event& e) {
        std::lock_guard lock(m);
        if (onEvent) onEvent(e);
    }
};

Session::Session(Options options, std::function<void(const Event&)> onEvent)
    : shared_(std::make_shared<Shared>()), options_(std::move(options)) {
    shared_->onEvent = std::move(onEvent);
    if (!options_.testAudio) capture_ = std::make_unique<Capture>();
    thread_ = std::jthread([this](std::stop_token st) {
        SetThreadDescription(GetCurrentThread(), L"st-recognize");
        run(st);
    });
}

Session::~Session() {
    stop();
    if (thread_.joinable()) thread_.join();
    if (capture_) capture_->stop();
}

void Session::stop() {
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lock(shared_->m);
        shared_->onEvent = nullptr;
        pending = shared_->pending;
    }
    if (pending) pending->cts.cancel();
    thread_.request_stop();
}

float Session::level() const { return capture_ ? capture_->level() : testLevel_.load(); }

void Session::run(std::stop_token st) {
    Event ev;
    auto emit = [&](Phase phase) {
        ev.phase = phase;
        ev.heardSec = heard_.load();
        shared_->emit(ev);
    };
    if (capture_) {
        if (!capture_->start(options_.source)) {
            ev.failure = failureOf(capture_->error());
            emit(Phase::Failed);
            return;
        }
        ev.ownSoundIncluded = capture_->ownSoundIncluded();
    }
    emit(Phase::Listening);

    SignatureGenerator gen;
    std::vector<float> buf;
    const size_t maxSamples = static_cast<size_t>(options_.maxSeconds * kSampleRate);
    size_t next = 0, testPos = 0;
    float loudest = 0;
    const auto t0 = Clock::now();
    std::shared_ptr<Pending> pending;
    std::optional<Song> candidate;   // an answer waiting for its confirmation
    auto finish = [&](Phase phase) {
        emit(phase);
        if (capture_) capture_->stop();
    };

    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (st.stop_requested()) break;
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        buf.clear();
        if (options_.testAudio) {
            const auto& a = *options_.testAudio;
            const size_t due = std::min(a.size(), static_cast<size_t>(wall * options_.testSpeed * kSampleRate));
            if (due > testPos) buf.assign(a.begin() + static_cast<ptrdiff_t>(testPos), a.begin() + static_cast<ptrdiff_t>(due));
            testPos = std::max(testPos, due);
            float peak = 0;
            for (float s : buf) peak = std::max(peak, std::abs(s));
            testLevel_ = std::min(1.f, peak);
        } else {
            capture_->take(buf);
            if (const CaptureError e = capture_->error(); e != CaptureError::None) {
                ev.failure = failureOf(e);
                finish(Phase::Failed);
                return;
            }
        }
        const size_t room = maxSamples - std::min<size_t>(maxSamples, gen.samplesFed());
        const size_t n = std::min(room, buf.size());
        if (n) {
            gen.feed(buf.data(), n);
            for (size_t i = 0; i < n; ++i) loudest = std::max(loudest, std::abs(buf[i]));
        }
        heard_ = static_cast<double>(gen.samplesFed()) / kSampleRate;

        // Nothing to hear: no request at all.
        if (!pending && ev.requests == 0 && wall >= options_.silenceWindowSec && loudest < options_.silenceLevel) {
            ST_LOG_INFO(kTag, "nothing heard in {:.1f} s (peak {:.4f})", wall, loudest);
            // The output held in exclusive mode by some app: its sound never reaches the mix (nor the loopback).
            if (capture_ && options_.source == Source::System && Capture::outputHeldExclusively()) {
                ST_LOG_INFO(kTag, "the default output is held in exclusive mode");
                ev.failure = Failure::DeviceBusy;
                finish(Phase::Failed);
                return;
            }
            finish(Phase::Silent);
            return;
        }

        if (pending) {
            std::unique_lock lock(pending->m);
            if (!pending->done) continue;
            const auto answer = pending->answer;
            const Failure failure = pending->failure;
            const int status = pending->status;
            lock.unlock();
            pending.reset();
            {
                std::lock_guard l(shared_->m);
                shared_->pending.reset();
            }
            if (answer && answer->song) {
                const Song& s = *answer->song;
                const double skew = std::max(std::abs(s.timeSkew), std::abs(s.frequencySkew));
                ST_LOG_INFO(kTag, "matched after {:.1f} s of audio, request {} (offset {:.1f} s, skew {:.5f} / {:.5f})",
                            heard_.load(), ev.requests, s.offsetSec, s.timeSkew, s.frequencySkew);
                // A short signature's answer with a large skew is confirmed by the next, longer one (a later answer
                // wins); the last checkpoint's answer, and a second one, are taken as they are.
                if (skew <= options_.confidentSkew || ev.confirming || next >= options_.checkpoints.size()) {
                    ev.song = answer->song;
                    finish(Phase::Matched);
                    return;
                }
                ST_LOG_INFO(kTag, "not sure yet (skew {:.5f}): listening on to confirm", skew);
                candidate = answer->song;
                ev.confirming = true;
                emit(Phase::Listening);
            } else if (!answer) {
                if (failure == Failure::None) return;   // cancelled: stop() is on its way
                ev.failure = failure;
                ev.status = status;
                finish(Phase::Failed);
                return;
            } else {
                ST_LOG_INFO(kTag, "no match with {:.1f} s (request {})", heard_.load(), ev.requests);
                if (next >= options_.checkpoints.size()) {
                    if (candidate) {   // the earlier, unconfirmed answer is still better than none
                        ev.song = candidate;
                        finish(Phase::Matched);
                    } else {
                        finish(Phase::NotFound);
                    }
                    return;
                }
                emit(Phase::Listening);
            }
        }

        // The next checkpoint (the last one also once the signature is full).
        const bool full = gen.samplesFed() >= maxSamples;
        if (next < options_.checkpoints.size() && (heard_.load() >= options_.checkpoints[next] || full)) {
            ++next;
            if (full) next = options_.checkpoints.size();
            if (loudest < options_.silenceLevel) {
                if (next >= options_.checkpoints.size()) {
                    finish(Phase::Silent);
                    return;
                }
                continue;
            }
            Signature sig = gen.signature();
            ++ev.requests;
            ST_LOG_INFO(kTag, "request {}: {:.1f} s, {} peaks", ev.requests, sig.seconds(), sig.peakCount());
            pending = std::make_shared<Pending>();
            {
                std::lock_guard l(shared_->m);
                shared_->pending = pending;
            }
            std::thread([pending, sig = std::move(sig), service = options_.service] {
                SetThreadDescription(GetCurrentThread(), L"st-recognize-request");
                std::optional<Answer> answer;
                Failure failure = Failure::None;
                int status = 0;
                try {
                    answer = service ? service(sig, pending->cts.token()) : identify(sig, pending->cts.token());
                } catch (const ServiceError& e) {
                    status = e.status;
                    failure = e.status == 429 ? Failure::RateLimited : e.status == 0 ? Failure::Network : Failure::Service;
                    ST_LOG_WARN(kTag, "request failed: {}", e.what());
                } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
                } catch (const std::exception& e) {
                    failure = Failure::Service;
                    ST_LOG_WARN(kTag, "request failed: {}", e.what());
                }
                std::lock_guard lock(pending->m);
                pending->answer = std::move(answer);
                pending->failure = pending->cts.isCancellationRequested() ? Failure::None : failure;
                pending->status = status;
                pending->done = true;
            }).detach();
            emit(Phase::Identifying);
        }
    }
    if (capture_) capture_->stop();
}

} // namespace st::recognize
