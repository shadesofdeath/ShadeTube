#pragma once
// Audio capture for song recognition: what this PC plays (WASAPI loopback) or the microphone, as 16 kHz mono floats.
// Nothing is stored: the samples live in memory until the session takes them and are gone with it.
//
// System sound: first a process-loopback client (Windows 10 2004+ / 11) that captures everything played EXCEPT
// ShadeTube's own process tree, so the app never recognizes its own playback; where Windows has no such client, the
// default render endpoint's loopback (then ShadeTube's sound is in it: ownSoundIncluded()). Microphone: the default
// capture endpoint (eConsole). Shared mode; the device's format is converted to mono and resampled to 16 kHz.
//
// Device changes: a new default device (output for the system sound, input for the microphone) or the device going
// away reopens on the current default without stopping; when there is none, error() turns NoDevice. A loopback of an
// endpoint another app holds exclusively can't be opened (DeviceBusy); a microphone Windows' privacy settings deny is
// AccessDenied.
//
// Threading: start() / stop() on one thread (the session's); the capture loop has its own thread ("st-capture"),
// which owns COM (MTA) and every WASAPI object. take() / level() / error() are thread-safe.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st::recognize {

enum class Source { System, Microphone };

enum class CaptureError {
    None,
    NoDevice,       // no output (system sound) / no microphone
    DeviceBusy,     // used exclusively by another app (or by ShadeTube in exclusive mode)
    AccessDenied,   // the microphone is turned off for desktop apps (Windows' privacy settings)
    Failed,         // anything else (the log says what)
};

class Capture {
public:
    Capture();
    ~Capture();   // stops
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens the device and starts the capture thread. False when it couldn't be opened (error() says why).
    bool start(Source source);
    void stop();
    bool running() const { return thread_.joinable(); }

    // Appends the 16 kHz mono samples captured since the last call.
    void take(std::vector<float>& out);
    // Peak level of the last ~30 ms (0..1), for the listening animation.
    float level() const { return level_.load(std::memory_order_relaxed); }
    // A failure while running (the device went away and none came back...). None while it captures.
    CaptureError error() const { return error_.load(); }
    // The system sound includes ShadeTube's own playback (no process loopback on this Windows).
    bool ownSoundIncluded() const { return ownIncluded_.load(); }
    // How often the device was reopened (a default device change, a lost device).
    int reopened() const { return reopened_.load(); }

    // Some app holds the default output in exclusive mode (its sound then bypasses the mix, so the system sound is
    // silent). Any thread; a shared-mode client is set up on the endpoint and dropped.
    static bool outputHeldExclusively();

    struct Device;   // the open WASAPI client (Capture.cpp)

private:
    void run(std::stop_token st, std::shared_ptr<Device> device);
    void deliver(const float* interleaved, uint32_t frames, uint32_t channels, uint32_t rate, bool silent);

    Source source_ = Source::System;
    std::jthread thread_;
    std::mutex mutex_;
    std::vector<float> out_;
    std::atomic<float> level_{0};
    std::atomic<CaptureError> error_{CaptureError::None};
    std::atomic<bool> ownIncluded_{false};
    std::atomic<int> reopened_{0};
    void* stopEvent_ = nullptr;
    // conversion state (capture thread)
    struct Convert;
    std::unique_ptr<Convert> convert_;
};

const char* captureErrorName(CaptureError e);

} // namespace st::recognize
