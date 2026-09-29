// AudioEngine: command queue + render loop on ONE engine thread.
//
// Threads involved in playback:
//   - caller threads (UI): public methods only enqueue commands / store atomics; getters read
//     atomics (lock-free) or take a tiny mutex (bufferedFraction, spectrum).
//   - engine thread ("st-audio-engine", MMCSS "Audio"): owns every Track, the WASAPI client and
//     all playback state. It sleeps in WaitForMultipleObjects on {wake event, WASAPI event}:
//     the WASAPI event drives rendering (~every 10 ms while the client runs); the wake event is
//     signalled by commands, tracks (format known / data / end / error) and device notifications.
//     It never blocks on the network or on Media Foundation: tracks decode on their own threads
//     and are destroyed only after their threads have exited (retire -> reap).
//     When paused/idle the WASAPI client is stopped and the thread waits with no timeout (0% CPU).
//   - per Track: a download thread (ProgressiveBuffer) and a decode thread (Track).
//
// Discontinuities never click: every pause / seek / open / stop first moves ~10 ms of the current
// audio into a "tail" with a fade-out applied (the action itself then happens immediately); the
// render loop plays the tail and fades new audio in over 10 ms. Volume changes slew over 30 ms.
//
// Audible position: every block written to the device is recorded as a Run (track tag + track
// frame position). played = framesWritten - GetCurrentPadding(); the run containing `played` is
// what is audible now. Gapless transitions (onEnded(finished, next)) and the natural end
// (onEnded(tag, 0)) are fired when they become audible, not when they are decoded.
//
// Crossfade: when the preloaded track asks for one (StreamSource::crossfadeMs) and the current track has that much
// left, the preloaded track becomes `cur` and the old one keeps playing as `fading`, mixed under it with equal-power
// curves until it ends; positions and the transition event follow the incoming track from the first mixed block. A
// pause / seek / open / stop during the mix captures both into the tail and drops the outgoing track.
//
// DSP order per block: crossfade mix -> equalizer -> spectrum -> volume / fades / loudness gain; then the whole device
// block through the look-ahead limiter (audio/Limiter: -0.3 dBFS ceiling, 5 ms late) instead of hard clipping.
//
// Live tracks: (re)starting waits for a jitter margin (kLivePrebufferMs, more after a stall) instead of 0.25 s;
// pause / play tell the track (it keeps only the newest audio while paused and flushes what went stale), seek is
// ignored, and stream titles are fired (onTitle) once the audio they start with is audible.
#include "audio/AudioEngine.h"

#include "audio/Limiter.h"
#include "audio/LiveStream.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/Spectrum.h"
#include "audio/TimeStretch.h"
#include "audio/Track.h"
#include "audio/WasapiOutput.h"
#include "core/Log.h"

#include <windows.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace st::audio {

namespace {

constexpr float kFadeSeconds = 0.010f;        // pause / resume / seek / open fades
constexpr float kVolumeRampSeconds = 0.030f;  // full-scale volume slew time
constexpr ULONGLONG kLoadingDelayMs = 150;    // stalls / seeks shorter than this don't flash "Loading"
constexpr double kPrebufferSeconds = 0.25;    // decoded audio needed before (re)starting after open/seek
constexpr double kRebufferSeconds = 0.75;     // ... after a mid-track stall (avoid stutter)
constexpr int64_t kLivePrebufferMs = 2000;    // live: audio buffered ahead before starting (jitter margin)
constexpr int64_t kLiveRebufferMs = 3000;     // ... after a stall (the network just proved to be slow)
constexpr ULONGLONG kLiveMaxWaitMs = 10000;   // ... but never wait longer than this once something is decoded
// Smart crossfade: under this peak level (-50 dBFS) audio counts as silence. A song whose last seconds went silent hands
// over at once (after kSilentTailMs of it, within kSilentTailWindowMs of its end), and the incoming song starts at its
// first sound (up to kSkipLeadMs of leading silence dropped).
constexpr float kSilence = 0.00316f;
constexpr int64_t kSilentTailMs = 1500;
constexpr int64_t kSilentTailWindowMs = 20000;
constexpr int64_t kSkipLeadMs = 5000;

// MFAudioFormat_Opus (WAVE_FORMAT_OPUS = 0x704F); defined locally for older SDKs.
constexpr GUID kAudioFormatOpus = {0x0000704F, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Perceptual volume curve: cubic (~ -18 dB at 50%, close to perceived loudness).
float perceptualGain(float linear) {
    const float v = std::clamp(linear, 0.f, 1.f);
    return v * v * v;
}

float dbToGain(float db) { return db == 0.f ? 1.f : std::pow(10.f, std::clamp(db, -30.f, 15.f) / 20.f); }

struct Command {
    enum class Type { Open, Preload, ClearPreload, Play, Pause, Toggle, Stop, Seek, SetDevice, SetSpeed } type;
    StreamSource source;
    int64_t ms = 0;
    bool autoplay = true;
    std::wstring device;   // SetDevice
    float speed = 1;       // SetSpeed
};

// A contiguous block of frames written to the device.
struct Run {
    uint64_t start = 0;        // device frame index (since the client was last started)
    uint32_t frames = 0;
    uint64_t tag = 0;
    int64_t pos = -1;          // track frame of the first frame; -1 = silence
    uint32_t rate = 0;
    uint32_t serial = 0;       // position epoch (bumped by seek/open/stop)
    uint64_t finishedTag = 0;  // != 0: first run after a natural transition from this tag
    float speed = 1;           // track frames per device frame (playback speed)
};

enum class Phase { None, Active, Ended, Failed };
enum class Rebuffer { None, Open, Seek, Stall };

} // namespace

struct AudioEngine::Impl {
    explicit Impl(EngineEvents e) : events(std::move(e)) {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = std::thread([this] { threadMain(); });
    }
    ~Impl() {
        quit.store(true);
        SetEvent(wake);
        if (thread.joinable()) thread.join();
        CloseHandle(wake);
    }

    void post(Command c) {
        {
            std::lock_guard lock(cmdMutex);
            commands.push_back(std::move(c));
        }
        SetEvent(wake);
    }

    // ---- shared (any thread) -------------------------------------------------------------------
    EngineEvents events;
    HANDLE wake = nullptr;
    std::mutex cmdMutex;
    std::deque<Command> commands;
    std::atomic<bool> quit{false};

    std::atomic<float> volume{1.f};
    std::atomic<State> state{State::Idle};
    std::atomic<uint64_t> tag{0};
    std::atomic<int64_t> posMs{0};
    std::atomic<int64_t> durMs{0};
    std::atomic<int64_t> memBytes{0};
    std::mutex eqMutex;
    EqSettings eqPending;                // guarded by eqMutex; eqVersion bumps on every change
    std::atomic<uint32_t> eqVersion{0};
    mutable std::mutex bufferMutex;
    std::shared_ptr<ProgressiveBuffer> audibleBuffer;
    std::shared_ptr<LiveStream> audibleLive;
    Spectrum spectrum;
    std::thread thread;

    // ---- engine thread only --------------------------------------------------------------------
    std::unique_ptr<WasapiOutput> out;
    std::unique_ptr<Track> cur, next;
    std::vector<std::unique_ptr<Track>> retired;
    Phase phase = Phase::None;
    bool wantPlay = false;
    Rebuffer rebuffer = Rebuffer::None;
    ULONGLONG rebufferSince = 0;
    bool endPending = false;          // natural end reached in the writer; fire when audible
    uint64_t pendingFinishedTag = 0;  // natural transition waiting for the next track's first run
    uint32_t serial = 0;
    float fade = 0.f, vol = 0.f;
    int64_t curEndFrame = -1;         // track frame after the last one read from cur (-1: none since open / seek)
    int64_t silentRun = 0;            // frames of cur read in a row that stayed under kSilence
    uint32_t outputRate = 0;          // the device's mix rate: every track decodes to it (0 = native rates)

    Equalizer eq;
    uint32_t eqApplied = 0, eqRate = 0, eqChannels = 0;
    Limiter limiter;
    TimeStretch stretch;              // playback speed of cur (see readCur)
    float curSpeed = 1;               // cur's speed: its StreamSource::speed, then setSpeed()

    // Crossfade (see the header comment).
    std::unique_ptr<Track> fading;    // the outgoing track, mixed under cur
    bool xfading = false;             // a mix is running (fading, or the incoming track still rising after it ended)
    float fadingGain = 1.f;           // loudness gain of the outgoing track
    float xfadeIn = 1.f;              // current curve value of the incoming track
    int64_t xfadeTotal = 0, xfadeDone = 0;   // frames
    std::vector<float> mixBuf;

    std::vector<float> tail;  // faded-out audio of the previous state (see header comment)
    size_t tailDone = 0;
    uint64_t tailTag = 0;
    int64_t tailPos = 0;
    float tailSpeed = 1;
    uint32_t tailSerial = 0;

    uint64_t written = 0;      // frames written since the client was started
    uint64_t lastRealEnd = 0;  // `written` index just after the last non-silent frame
    std::deque<Run> runs;

    bool deviceLost = false;
    bool deviceErrorReported = false;
    ULONGLONG deviceRetryAt = 0;

    State published = State::Idle;
    uint64_t publishedTag = 0;

    // ---------------------------------------------------------------------------------------------
    void threadMain() {
        SetThreadDescription(GetCurrentThread(), L"st-audio-engine");
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mf)) ST_LOG_ERROR("audio", "MFStartup failed (hr=0x{:08X})", static_cast<uint32_t>(mf));
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        out = std::make_unique<WasapiOutput>(wake);
        vol = perceptualGain(volume.load());
        // Tracks decode to the device's mix rate (Decoder: the Source Reader's resampler), so songs of different
        // rates never need a device reopen between them: gapless handoffs and crossfades always line up.
        outputRate = out->mixRate();
        // Warm-up: the first IAudioClient initialisation in a process can take ~0.5-1 s (audio
        // engine / APO loading). Do it now, while nothing plays, in the format tracks will have. The client stays
        // initialised but stopped (no CPU); another format simply reopens it, which is fast once warm. Failures are
        // ignored here.
        if (FAILED(out->open(outputRate ? outputRate : 44100, 2))) out->close();

        while (!quit.load()) {
            HANDLE handles[2] = {wake, static_cast<HANDLE>(out->event())};
            WaitForMultipleObjects(2, handles, FALSE, waitTimeout());
            if (quit.load()) break;
            step();
        }

        // Shutdown: stop output, cancel every track, join their threads, then tear down MF.
        out->close();
        retire(cur);
        retire(next);
        retire(fading);
        retired.clear();  // ~Track joins (quick: everything is cancelled)
        out.reset();
        {
            std::lock_guard lock(bufferMutex);
            audibleBuffer.reset();
            audibleLive.reset();
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    DWORD waitTimeout() const {
        DWORD t = INFINITE;
        if (out->running()) t = 100;  // safety net; the WASAPI event normally fires every ~10 ms
        if (phase == Phase::Active && rebuffer != Rebuffer::None) t = std::min<DWORD>(t, 40);
        if (!retired.empty()) t = std::min<DWORD>(t, 15);
        if (deviceLost) t = std::min<DWORD>(t, 500);
        return t;
    }

    void step() {
        processCommands();
        handleDeviceChange();
        checkTracks();
        if (out->running()) renderAvailable();
        maybeStartOrStop();
        reap();
        if (cur && cur->tag() == tag.load()) {
            durMs.store(cur->durationMs());
            if (cur->live() && events.onTitle) {
                if (auto title = cur->takeLiveTitle(posMs.load())) events.onTitle(*title, cur->tag());
            }
        }
        publishState();
    }

    // ---- commands ------------------------------------------------------------------------------
    void processCommands() {
        std::deque<Command> batch;
        {
            std::lock_guard lock(cmdMutex);
            batch.swap(commands);
        }
        for (auto& c : batch) {
            switch (c.type) {
            case Command::Type::Open: cmdOpen(c.source, c.ms, c.autoplay); break;
            case Command::Type::Preload:
                retire(next);
                next = makeTrack(c.source, 0);
                break;
            case Command::Type::ClearPreload: retire(next); break;
            case Command::Type::Play: cmdPlay(); break;
            case Command::Type::Pause: cmdPause(); break;
            case Command::Type::Toggle:
                if (phase == Phase::Active && wantPlay) cmdPause();
                else cmdPlay();
                break;
            case Command::Type::Stop: cmdStop(); break;
            case Command::Type::Seek: cmdSeek(c.ms); break;
            case Command::Type::SetDevice: cmdSetDevice(c.device); break;
            case Command::Type::SetSpeed: curSpeed = std::clamp(c.speed, 0.5f, 3.f); break;
            }
        }
    }

    std::unique_ptr<Track> makeTrack(const StreamSource& s, int64_t startMs) {
        return std::make_unique<Track>(s, startMs, &memBytes, wake, outputRate);
    }

    void cmdOpen(const StreamSource& s, int64_t startMs, bool autoplay) {
        captureTail();
        endCrossfade();
        ++serial;
        std::unique_ptr<Track> track;
        // Promote the preloaded track if the caller opens the same item (e.g. "next" pressed early).
        if (next && s.tag != 0 && next->tag() == s.tag && next->source().url == s.url &&
            next->source().localPath == s.localPath && !next->failed()) {
            track = std::move(next);
            if (startMs > 0) track->seek(startMs);
        } else {
            track = makeTrack(s, startMs);
        }
        retire(cur);
        cur = std::move(track);
        phase = Phase::Active;
        wantPlay = autoplay;
        if (cur->live() && !autoplay) cur->setLivePaused(true);
        endPending = false;
        clearTransitions();
        curEndFrame = -1;
        silentRun = 0;
        stretch.clear();
        curSpeed = cur->live() ? 1.f : std::clamp(cur->source().speed, 0.5f, 3.f);
        beginRebuffer(Rebuffer::Open);
        publishTrack(s.tag, std::max<int64_t>(0, startMs), cur->durationMs(), cur.get());
    }

    void cmdPlay() {
        if (phase == Phase::Ended && cur) {  // replay from the start
            cmdSeek(0);
            wantPlay = true;
            return;
        }
        if (phase == Phase::Active && !wantPlay) {
            wantPlay = true;
            fade = 0.f;  // fade in
            // Live: what was kept while paused may be stale (dropped audio, closed connection): rebuffer live.
            if (cur && cur->live() && cur->setLivePaused(false)) beginRebuffer(Rebuffer::Open);
        }
    }

    void cmdPause() {
        if (phase != Phase::Active || !wantPlay) return;
        captureTail();
        endCrossfade();
        wantPlay = false;
        if (cur && cur->live()) cur->setLivePaused(true);
    }

    void cmdStop() {
        captureTail();
        endCrossfade();
        stretch.clear();
        ++serial;
        retire(cur);
        retire(next);
        phase = Phase::None;
        wantPlay = false;
        endPending = false;
        clearTransitions();
        rebuffer = Rebuffer::None;
        publishTrack(0, 0, 0, nullptr);
    }

    void cmdSeek(int64_t ms) {
        if (!cur || (phase != Phase::Active && phase != Phase::Ended) || cur->live()) return;
        const int64_t dur = cur->durationMs();
        ms = std::max<int64_t>(0, dur > 0 ? std::min(ms, dur) : ms);
        captureTail();
        endCrossfade();
        ++serial;
        cur->seek(ms);
        curEndFrame = -1;
        silentRun = 0;
        stretch.clear();
        phase = Phase::Active;
        endPending = false;
        beginRebuffer(Rebuffer::Seek);
        posMs.store(ms);
    }

    void cmdSetDevice(const std::wstring& id) {
        if (id == out->preferredDevice()) return;
        out->setPreferredDevice(id);
        deviceLost = false;
        deviceErrorReported = false;
        if (!out->isOpen()) return;   // opened on the new device when something plays
        ST_LOG_INFO("audio", "output device changed: reopening");
        finalizeOutput();
        tail.clear();
        out->close();
        fade = 0.f;
        outputRate = out->mixRate();   // tracks opened from now on decode to the new device's rate
    }

    void publishTrack(uint64_t t, int64_t pos, int64_t dur, const Track* track) {
        tag.store(t);
        posMs.store(pos);
        durMs.store(dur);
        std::lock_guard lock(bufferMutex);
        audibleBuffer = track ? track->buffer() : nullptr;
        audibleLive = track ? track->liveStream() : nullptr;
    }

    void clearTransitions() {
        pendingFinishedTag = 0;
        for (auto& r : runs) r.finishedTag = 0;
    }

    void beginRebuffer(Rebuffer kind) {
        rebuffer = kind;
        rebufferSince = GetTickCount64();
        fade = 0.f;
        if (cur) cur->requestWake();
    }

    void retire(std::unique_ptr<Track>& t) {
        if (!t) return;
        t->cancel();
        retired.push_back(std::move(t));
    }

    void reap() {
        std::erase_if(retired, [](const std::unique_ptr<Track>& t) { return t->finished(); });
    }

    // ---- tracks --------------------------------------------------------------------------------
    bool formatMatches(const Track& t) const {
        return out->isOpen() && out->sampleRate() == t.sampleRate() && out->channels() == t.channels();
    }

    // The user wants sound and the current track has data ready.
    bool wantOutput() const {
        return phase == Phase::Active && wantPlay && rebuffer == Rebuffer::None && cur && cur->formatReady();
    }
    bool feedingNow() const { return wantOutput() && formatMatches(*cur); }

    void checkTracks() {
        if (next && next->failed()) {
            ST_LOG_WARN("audio", "preload {} failed: {}", next->tag(), next->errorMessage());
            retire(next);
        }
        if (phase != Phase::Active || !cur) return;
        if (cur->failed() && cur->available() == 0) {
            const ErrorKind kind = cur->errorKind();
            const std::string message = cur->errorMessage();
            const uint64_t t = cur->tag();
            endCrossfade();
            retire(cur);  // frees its memory; the tag stays published
            phase = Phase::Failed;
            rebuffer = Rebuffer::None;
            publishState();
            if (events.onError) events.onError(kind, message, t);
            return;
        }
        if (rebuffer != Rebuffer::None && cur->formatReady()) {
            const double seconds = rebuffer == Rebuffer::Stall ? kRebufferSeconds : kPrebufferSeconds;
            const size_t need = std::min(static_cast<size_t>(cur->sampleRate() * seconds), cur->capacity() * 9 / 10);
            bool ready = cur->available() >= need || cur->decoderEnded() || cur->failed();
            if (ready && cur->live() && !cur->failed()) {
                // A live stream arrives in real time: start only with a margin, or it stalls again right away.
                const int64_t margin = rebuffer == Rebuffer::Stall ? kLiveRebufferMs : kLivePrebufferMs;
                ready = cur->bufferedAheadMs() >= margin || GetTickCount64() - rebufferSince >= kLiveMaxWaitMs;
            }
            if (ready) {
                if (out->isOpen() && !formatMatches(*cur)) {
                    // New sample rate / channel count: let the previous audio finish, then reopen.
                    if (out->running()) {
                        if (!tail.empty()) return;
                        uint32_t pad = 0;
                        if (const HRESULT hr = out->padding(pad); FAILED(hr)) {
                            deviceFailure(hr);
                            return;
                        }
                        if (written - pad < lastRealEnd) return;
                    }
                    finalizeOutput();
                    out->close();
                }
                rebuffer = Rebuffer::None;
            } else {
                cur->requestWake();
            }
        }
    }

    // Current track reached its end while feeding: continue with the preloaded one if possible.
    bool advance() {
        const uint64_t finished = cur->tag();
        endCrossfade();   // the incoming track of a mix ended already (shorter than the fade)
        curEndFrame = -1;
        silentRun = 0;
        if (next && next->failed()) retire(next);
        if (!next) {
            phase = Phase::Ended;
            endPending = true;
            return false;
        }
        retire(cur);
        cur = std::move(next);
        stretch.clear();   // the finished track's input has all been played (readCur flushed it)
        curSpeed = std::clamp(cur->source().speed, 0.5f, 3.f);
        pendingFinishedTag = finished;
        if (cur->formatReady() && formatMatches(*cur)) return true;  // gapless, same format
        beginRebuffer(Rebuffer::Seek);  // not decoded yet, or needs a device reopen
        return false;
    }

    // ---- device --------------------------------------------------------------------------------
    bool openDevice(uint32_t rate, uint32_t channels) {
        const ULONGLONG now = GetTickCount64();
        if (deviceLost && now < deviceRetryAt) return false;
        const HRESULT hr = out->open(rate, channels);
        if (FAILED(hr)) {
            deviceLost = true;
            deviceRetryAt = now + 1000;
            if (!deviceErrorReported) {
                deviceErrorReported = true;
                const std::string msg = std::format("cannot open audio device (hr=0x{:08X})", static_cast<uint32_t>(hr));
                ST_LOG_WARN("audio", "{}", msg);
                if (events.onError) events.onError(ErrorKind::Device, msg, tag.load());
            }
            return false;
        }
        deviceLost = false;
        deviceErrorReported = false;
        spectrum.setSampleRate(rate);
        return true;
    }

    void deviceFailure(HRESULT hr) {
        ST_LOG_WARN("audio", "audio device failure (hr=0x{:08X}), reopening", static_cast<uint32_t>(hr));
        finalizeOutput();
        tail.clear();
        out->close();
        fade = 0.f;
        deviceLost = true;
        deviceRetryAt = GetTickCount64();  // retry right away (the default device may have changed)
    }

    void handleDeviceChange() {
        if (!out->takeDeviceChanged() || !out->isOpen()) return;
        ST_LOG_INFO("audio", "audio device changed: reopening");
        finalizeOutput();
        tail.clear();
        out->close();  // reopened lazily by maybeStartOrStop() on the new default device
        fade = 0.f;
        deviceLost = false;
        outputRate = out->mixRate();
    }

    // Treats everything written as heard (fires pending transitions) and forgets the runs.
    void finalizeOutput() {
        updateAudible(written);
        runs.clear();
        written = lastRealEnd = 0;
    }

    void maybeStartOrStop() {
        if (wantOutput()) {
            if (!out->isOpen() && !openDevice(cur->sampleRate(), cur->channels())) return;
            if (!formatMatches(*cur)) return;  // checkTracks() drains and reopens
            if (!out->running()) startOutput();
        } else if (out->running() && tail.empty()) {
            uint32_t pad = 0;
            if (const HRESULT hr = out->padding(pad); FAILED(hr)) {
                deviceFailure(hr);
                return;
            }
            if (written - pad >= lastRealEnd) {  // everything real has been heard: stop (0% CPU)
                finalizeOutput();
                if (const HRESULT hr = out->stop(); FAILED(hr)) deviceFailure(hr);
            }
        }
    }

    void startOutput() {
        runs.clear();
        written = lastRealEnd = 0;
        limiter.configure(out->sampleRate(), out->channels());
        limiter.reset();   // what it still delays belongs to the previous run
        uint32_t pad = 0;
        HRESULT hr = out->padding(pad);
        const uint32_t n = out->bufferFrames() - std::min(pad, out->bufferFrames());
        float* data = nullptr;
        if (SUCCEEDED(hr) && n > 0) {
            hr = out->getBuffer(n, data);
            if (SUCCEEDED(hr)) {
                render(data, n);  // prefill (starts with a fade-in)
                hr = out->releaseBuffer(n);
            }
        }
        if (SUCCEEDED(hr)) hr = out->start();
        if (FAILED(hr)) deviceFailure(hr);
    }

    void renderAvailable() {
        uint32_t pad = 0;
        HRESULT hr = out->padding(pad);
        if (FAILED(hr)) return deviceFailure(hr);
        updateAudible(written - pad);
        const uint32_t n = out->bufferFrames() - std::min(pad, out->bufferFrames());
        if (n == 0) return;
        float* data = nullptr;
        hr = out->getBuffer(n, data);
        if (FAILED(hr)) return deviceFailure(hr);
        render(data, n);
        hr = out->releaseBuffer(n);
        if (FAILED(hr)) deviceFailure(hr);
    }

    // ---- rendering -----------------------------------------------------------------------------
    // Moves ~10 ms of the current audio into `tail` with a fade-out, so the caller can change
    // state immediately without an audible click.
    void captureTail() {
        if (!out->running() || !tail.empty() || !feedingNow() || fade <= 0.f) return;
        const uint32_t ch = out->channels();
        const size_t n = static_cast<size_t>(out->sampleRate() * kFadeSeconds);
        tail.resize(n * ch);
        int64_t pos = 0;
        const float speed = stretching() ? curSpeed : 1.f;
        const size_t got = readCur(tail.data(), n, pos, ch, out->sampleRate());
        tail.resize(got * ch);
        if (got == 0) return;
        tailSpeed = speed;
        float gain = gainOf(*cur);
        if (xfading) {   // the outgoing track of a running mix fades out with it
            mixCrossfade(tail.data(), got, ch, out->sampleRate());
            gain = 1.f;
        }
        syncEq();
        eq.process(tail.data(), got);
        for (size_t i = 0; i < got; ++i) {
            const float g = fade * gain * (1.f - static_cast<float>(i + 1) / static_cast<float>(got));
            for (uint32_t c = 0; c < ch; ++c) tail[i * ch + c] *= g;
        }
        tailDone = 0;
        tailTag = cur->tag();
        tailPos = pos;
        tailSerial = serial;
        fade = 0.f;
    }

    void addRun(uint64_t start, size_t frames, uint64_t runTag, int64_t pos, uint32_t rate, uint32_t runSerial,
                uint64_t finishedTag, float speed = 1.f) {
        if (pos < 0 && !runs.empty()) {  // merge consecutive silence
            Run& last = runs.back();
            if (last.pos < 0 && last.start + last.frames == start && last.serial == runSerial) {
                last.frames += static_cast<uint32_t>(frames);
                return;
            }
        }
        runs.push_back({start, static_cast<uint32_t>(frames), runTag, pos, rate, runSerial, finishedTag, speed});
    }

    void applyGain(float* p, size_t frames, uint32_t ch, uint32_t rate, float volTarget, bool withFade, float gain) {
        const float volStep = 1.f / (kVolumeRampSeconds * static_cast<float>(rate));
        const float fadeStep = 1.f / (kFadeSeconds * static_cast<float>(rate));
        size_t f = 0;
        for (; f < frames && (vol != volTarget || (withFade && fade < 1.f)); ++f) {
            if (vol < volTarget) vol = std::min(volTarget, vol + volStep);
            else if (vol > volTarget) vol = std::max(volTarget, vol - volStep);
            float g = vol * gain;
            if (withFade) {
                fade = std::min(1.f, fade + fadeStep);
                g *= fade;
            }
            for (uint32_t c = 0; c < ch; ++c) p[f * ch + c] *= g;
        }
        const float g = vol * gain;  // steady state; peaks are the limiter's job (end of render())
        if (g != 1.f)
            for (size_t i = f * ch; i < frames * ch; ++i) p[i] *= g;
    }

    float gainOf(const Track& t) const { return dbToGain(t.gainDb()); }

    // cur plays through the time-stretcher: at another speed than 1, or while it still holds audio after a change back.
    bool stretching() const { return cur && !cur->live() && (curSpeed != 1.f || !stretch.empty()); }

    // Reads cur's next frames, at its speed. Back at 1x the stretcher first hands out what it still holds, then reads go
    // straight to the track again (seamless: the stretcher only ever buffers the track's next frames).
    size_t readCur(float* dst, size_t n, int64_t& pos, uint32_t ch, uint32_t rate) {
        Track* t = cur.get();
        if (!stretching()) return t->read(dst, n, pos, ch, rate);
        stretch.configure(rate, ch);
        if (curSpeed != 1.f) {
            stretch.setTempo(curSpeed);
            return stretch.process(dst, n, pos,
                                   [t, ch, rate](float* d, size_t k, int64_t& fp) { return t->read(d, k, fp, ch, rate); },
                                   [t] { return t->decoderEnded(); });
        }
        size_t got = stretch.process(dst, n, pos, nullptr, [] { return true; });
        if (got < n && stretch.empty()) {
            int64_t p2 = 0;
            const size_t more = t->read(dst + got * ch, n - got, p2, ch, rate);
            if (got == 0) pos = p2;
            got += more;
        }
        return got;
    }

    // Picks up equalizer changes and output format changes (engine thread, before processing a block).
    void syncEq() {
        if (!out->isOpen()) return;
        const uint32_t v = eqVersion.load(std::memory_order_acquire);
        if (v == eqApplied && out->sampleRate() == eqRate && out->channels() == eqChannels) return;
        EqSettings s;
        {
            std::lock_guard lock(eqMutex);
            s = eqPending;
        }
        eq.configure(s, out->sampleRate(), out->channels());
        eqApplied = v;
        eqRate = out->sampleRate();
        eqChannels = out->channels();
    }

    // Starts a crossfade into the preloaded track once the current one is within its fade length of the end.
    void maybeStartCrossfade(uint32_t rate) {
        if (xfading || !next || !cur || cur->live() || next->live() || next->failed()) return;
        if (curSpeed != 1.f || !stretch.empty() || next->source().speed != 1.f) return;
        const int ms = next->source().crossfadeMs;
        if (ms <= 0 || !next->formatReady() || !formatMatches(*next)) return;
        const int64_t dur = cur->durationMs();
        if (dur <= 0 || curEndFrame < 0) return;
        const int64_t remaining = dur * rate / 1000 - curEndFrame;
        int64_t length = int64_t(ms) * rate / 1000;
        // A silent ending (a fade-out that is over, a video's quiet outro) needn't be sat through: hand over now.
        const bool silentEnd = silentRun >= kSilentTailMs * rate / 1000 && remaining <= kSilentTailWindowMs * rate / 1000;
        if (silentEnd) length = std::min<int64_t>(remaining, rate / 2);
        else if (remaining > length || remaining < rate / 5) return;   // not yet, or too late to be worth it: gapless
        // The incoming track must be decoded ahead, or the mix would stall right away.
        if (next->available() < std::min<size_t>(next->capacity() / 2, rate / 4) && !next->decoderEnded()) return;
        fading = std::move(cur);
        fadingGain = gainOf(*fading);
        cur = std::move(next);
        cur->skipSilence(kSilence, static_cast<size_t>(kSkipLeadMs * rate / 1000));   // start at its first sound
        silentRun = 0;
        xfading = true;
        xfadeTotal = silentEnd ? length : remaining;
        xfadeDone = 0;
        xfadeIn = 0.f;
        pendingFinishedTag = fading->tag();
        curEndFrame = -1;
        ST_LOG_DEBUG("audio", "crossfade {} -> {} over {} ms{}", fading->tag(), cur->tag(), xfadeTotal * 1000 / rate,
                     silentEnd ? " (silent ending)" : "");
    }

    // Mixes the outgoing track under `p` (frames of the incoming one) with equal-power curves, both loudness gains
    // included. When the outgoing track runs out (or stalls) early, the incoming one rises to full level in 50 ms.
    void mixCrossfade(float* p, size_t frames, uint32_t ch, uint32_t rate) {
        size_t have = 0;
        if (fading) {
            mixBuf.resize(frames * ch);
            int64_t unused = 0;
            have = fading->read(mixBuf.data(), frames, unused, ch, rate);
        }
        const float gainIn = gainOf(*cur);
        const float ramp = 1.f / (0.05f * static_cast<float>(rate));
        constexpr double kHalfPi = std::numbers::pi / 2;
        const double total = static_cast<double>(std::max<int64_t>(1, xfadeTotal));
        for (size_t i = 0; i < frames; ++i) {
            float gOut = 0.f;
            if (i < have) {
                const double t = std::min(1.0, static_cast<double>(xfadeDone + static_cast<int64_t>(i)) / total);
                xfadeIn = static_cast<float>(std::sin(t * kHalfPi));
                gOut = static_cast<float>(std::cos(t * kHalfPi)) * fadingGain;
            } else {
                xfadeIn = std::min(1.f, xfadeIn + ramp);
            }
            const float gIn = xfadeIn * gainIn;
            float* s = p + i * ch;
            if (i < have) {
                const float* o = mixBuf.data() + i * ch;
                for (uint32_t c = 0; c < ch; ++c) s[c] = s[c] * gIn + o[c] * gOut;
            } else {
                for (uint32_t c = 0; c < ch; ++c) s[c] *= gIn;
            }
        }
        xfadeDone += static_cast<int64_t>(frames);
        if (fading && (have < frames || xfadeDone >= xfadeTotal)) retire(fading);
        if (!fading && xfadeIn >= 1.f) xfading = false;
    }

    void endCrossfade() {
        retire(fading);
        xfading = false;
        xfadeIn = 1.f;
    }

    void render(float* dst, uint32_t n) {
        const uint32_t ch = out->channels(), rate = out->sampleRate();
        const float volTarget = perceptualGain(volume.load(std::memory_order_relaxed));
        uint32_t done = 0;
        syncEq();
        limiter.configure(rate, ch);

        if (!tail.empty()) {
            const size_t tailFrames = tail.size() / ch;
            const uint32_t k = static_cast<uint32_t>(std::min<size_t>(n, tailFrames - tailDone));
            std::memcpy(dst, tail.data() + tailDone * ch, size_t(k) * ch * sizeof(float));
            spectrum.push(dst, k, ch);
            applyGain(dst, k, ch, rate, volTarget, false, 1.f);
            addRun(written, k, tailTag, tailPos + static_cast<int64_t>(tailDone * tailSpeed), rate, tailSerial, 0, tailSpeed);
            tailDone += k;
            done += k;
            lastRealEnd = written + done + limiter.latencyFrames();   // it leaves the limiter that much later
            if (tailDone >= tailFrames) {
                tail.clear();
                tailDone = 0;
            }
        }

        while (done < n && feedingNow()) {
            maybeStartCrossfade(rate);
            Track& t = *cur;
            float* p = dst + size_t(done) * ch;
            int64_t pos = 0;
            const float speed = stretching() ? curSpeed : 1.f;
            const size_t got = readCur(p, n - done, pos, ch, rate);
            if (got > 0) {
                curEndFrame = pos + static_cast<int64_t>(got);
                float peak = 0;
                for (size_t i = 0; i < got * ch; ++i) peak = std::max(peak, std::fabs(p[i]));
                silentRun = peak < kSilence ? silentRun + static_cast<int64_t>(got) : 0;
                float gain = gainOf(t);
                if (xfading) {
                    mixCrossfade(p, got, ch, rate);
                    gain = 1.f;
                }
                eq.process(p, got);
                spectrum.push(p, got, ch);
                applyGain(p, got, ch, rate, volTarget, true, gain);
                addRun(written + done, got, t.tag(), pos, rate, serial, pendingFinishedTag, speed);
                pendingFinishedTag = 0;
                done += static_cast<uint32_t>(got);
                lastRealEnd = written + done + limiter.latencyFrames();   // it leaves the limiter that much later
                continue;
            }
            if (t.ended() && stretch.empty()) {
                if (advance()) continue;
                break;
            }
            beginRebuffer(Rebuffer::Stall);  // decoder/network can't keep up
            break;
        }

        if (done < n) {
            std::memset(dst + size_t(done) * ch, 0, size_t(n - done) * ch * sizeof(float));
            spectrum.pushSilence(n - done);
            addRun(written + done, n - done, 0, -1, rate, serial, 0);
        }
        limiter.process(dst, n);
        written += n;
    }

    // `played` = device frames already heard. Updates the position, fires audible transitions.
    void updateAudible(uint64_t played) {
        while (!runs.empty()) {
            Run& r = runs.front();
            if (played <= r.start) break;
            if (r.finishedTag) {
                const uint64_t finished = r.finishedTag;
                r.finishedTag = 0;
                const bool isCur = cur && cur->tag() == r.tag;
                publishTrack(r.tag, r.pos * 1000 / r.rate, isCur ? cur->durationMs() : 0, isCur ? cur.get() : nullptr);
                if (events.onEnded) events.onEnded(finished, r.tag);
            }
            if (r.pos >= 0 && r.serial == serial) {
                const uint64_t into = std::min<uint64_t>(played - r.start, r.frames);
                posMs.store((r.pos + static_cast<int64_t>(static_cast<double>(into) * r.speed)) * 1000 / r.rate);
            }
            if (played >= r.start + r.frames) runs.pop_front();
            else break;
        }
        if (endPending && played >= lastRealEnd) {
            endPending = false;
            if (durMs.load() > 0) posMs.store(durMs.load());
            publishState();
            if (events.onEnded) events.onEnded(tag.load(), 0);
        }
    }

    void publishState() {
        State s = State::Idle;
        switch (phase) {
        case Phase::None: s = State::Idle; break;
        case Phase::Failed: s = State::Error; break;
        case Phase::Ended: s = endPending ? State::Playing : State::Ended; break;
        case Phase::Active:
            if (!cur || !cur->formatReady()) s = State::Loading;
            else if (!wantPlay) s = State::Paused;
            else if (rebuffer == Rebuffer::None) s = State::Playing;
            else if (rebuffer == Rebuffer::Open || GetTickCount64() - rebufferSince >= kLoadingDelayMs) s = State::Loading;
            else s = published == State::Loading ? State::Loading : State::Playing;
            break;
        }
        const uint64_t t = tag.load();
        if (s == published && t == publishedTag) return;
        published = s;
        publishedTag = t;
        state.store(s);
        if (events.onState) events.onState(s, t);
    }
};

// ---- public API ---------------------------------------------------------------------------------

AudioEngine::AudioEngine(EngineEvents events) : impl_(std::make_unique<Impl>(std::move(events))) {}
AudioEngine::~AudioEngine() = default;

void AudioEngine::open(const StreamSource& source, int64_t startMs, bool autoplay) {
    impl_->post({Command::Type::Open, source, startMs, autoplay});
}
void AudioEngine::preload(const StreamSource& source) { impl_->post({Command::Type::Preload, source}); }
void AudioEngine::clearPreload() { impl_->post({Command::Type::ClearPreload, {}}); }
void AudioEngine::play() { impl_->post({Command::Type::Play, {}}); }
void AudioEngine::pause() { impl_->post({Command::Type::Pause, {}}); }
void AudioEngine::togglePause() { impl_->post({Command::Type::Toggle, {}}); }
void AudioEngine::stop() { impl_->post({Command::Type::Stop, {}}); }
void AudioEngine::seek(int64_t ms) { impl_->post({Command::Type::Seek, {}, ms}); }

void AudioEngine::setVolume(float linear) {
    impl_->volume.store(std::clamp(linear, 0.f, 1.f));
    SetEvent(impl_->wake);  // not strictly needed while playing; keeps paused state consistent
}
float AudioEngine::volume() const { return impl_->volume.load(); }

void AudioEngine::setEqualizer(const EqSettings& eq) {
    {
        std::lock_guard lock(impl_->eqMutex);
        impl_->eqPending = eq;
    }
    impl_->eqVersion.fetch_add(1, std::memory_order_release);
    SetEvent(impl_->wake);
}

void AudioEngine::setSpeed(float speed) {
    Command c{Command::Type::SetSpeed, {}};
    c.speed = speed;
    impl_->post(std::move(c));
}

void AudioEngine::setOutputDevice(const std::wstring& id) {
    Command c{Command::Type::SetDevice, {}};
    c.device = id;
    impl_->post(std::move(c));
}

std::vector<OutputDevice> AudioEngine::outputDevices() {
    std::wstring defaultId;
    std::vector<OutputDevice> devices;
    for (auto& e : WasapiOutput::endpoints(defaultId)) devices.push_back({e.id, e.name, e.id == defaultId});
    return devices;
}

State AudioEngine::state() const { return impl_->state.load(std::memory_order_relaxed); }
uint64_t AudioEngine::currentTag() const { return impl_->tag.load(std::memory_order_relaxed); }
int64_t AudioEngine::positionMs() const { return impl_->posMs.load(std::memory_order_relaxed); }
int64_t AudioEngine::durationMs() const { return impl_->durMs.load(std::memory_order_relaxed); }

float AudioEngine::bufferedFraction() const {
    std::lock_guard lock(impl_->bufferMutex);
    return impl_->audibleBuffer ? impl_->audibleBuffer->fraction() : 0.f;
}

bool AudioEngine::spectrum(float* bands, int count) const {
    if (!bands || count <= 0) return false;
    if (state() != State::Playing) {
        std::fill(bands, bands + count, 0.f);
        impl_->spectrum.resetSmoothing();
        return false;
    }
    impl_->spectrum.compute(bands, count);
    return true;
}

size_t AudioEngine::bufferedBytes() const { return static_cast<size_t>(std::max<int64_t>(0, impl_->memBytes.load())); }

LiveInfo AudioEngine::liveInfo() const {
    std::shared_ptr<LiveStream> stream;
    {
        std::lock_guard lock(impl_->bufferMutex);
        stream = impl_->audibleLive;
    }
    return stream ? stream->info() : LiveInfo{};
}

namespace {
// Whether Media Foundation has an audio decoder MFT for `subtype` (any thread; COM and MF are started for the call).
bool hasAudioDecoder(const GUID& subtype) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool decoder = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, subtype};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                    MFT_ENUM_FLAG_SORTANDFILTER,
                                &input, nullptr, &activates, &count))) {
            decoder = count > 0;
            for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        MFShutdown();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return decoder;
}
} // namespace

bool AudioEngine::supportsLiveCodec(std::string_view codec) {
    std::string c;
    for (const char ch : codec)
        if (ch != ' ') c.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    auto has = [&](const char* s) { return c.find(s) != std::string::npos; };
    if (c.empty() || c == "unknown" || has("mpegurl") || has("m3u") || has("scpls") || c == "hls") return true;
    if (has("mp3") || has("mpeg") || has("mpga") || c == "mp2" || c == "mp1") return true;
    if (has("aac") || has("mp4a")) return true;   // AAC, AAC+, HE-AAC, audio/aacp, audio/x-aac, "AAC,H.264"...
    if (has("opus")) {   // Ogg Opus (a plain "OGG" is usually Vorbis, which has no decoder here)
        static const bool opus = hasAudioDecoder(kAudioFormatOpus);
        return opus;
    }
    return false;   // Ogg Vorbis, FLAC, WMA, AC-3...
}

bool AudioEngine::supportsWebm() {
    static const bool supported = [] {
        const bool decoder = hasAudioDecoder(kAudioFormatOpus);
        bool handler = false;
        for (const wchar_t* key : {L".webm", L"audio/webm", L"video/webm"}) {
            const std::wstring path = std::wstring(L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\") + key;
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
                RegCloseKey(h);
                handler = true;
                break;
            }
        }
        ST_LOG_INFO("audio", "WebM/Opus support: decoder={} handler={}", decoder, handler);
        return decoder && handler;
    }();
    return supported;
}

} // namespace st::audio
