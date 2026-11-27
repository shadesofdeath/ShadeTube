#pragma once
// Spectrum analyser and waveform tap for visualisers (internal to st_audio).
//
// The engine thread pushes the mono mix of every block it writes to the device (before the user volume, so the
// visualiser does not shrink with the volume knob; shared and exclusive output alike). The history of the latest 4096
// samples is a seqlock: the engine thread never waits and never allocates, a reader copies it and retries (a few
// times) when a push landed meanwhile. compute() (UI thread, up to 60x/s) applies a Hann window + radix-2 FFT
// (audio/Fft) and maps the magnitudes to `count` log-spaced bands in 0..1 (-70..-10 dBFS), with attack/release
// smoothing; snapshot() hands out the samples themselves (the visualizer's own analysis, audio/VisualAnalysis).
// Cost per compute: one 2048-point complex FFT (~50 us).
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace st::audio {

class Spectrum {
public:
    static constexpr size_t kSize = 2048;      // compute()'s FFT
    static constexpr size_t kHistory = 4096;   // samples kept (the visualizer's beat detector reads ~40 ms back)

    void setSampleRate(uint32_t rate) { rate_.store(rate, std::memory_order_relaxed); }
    uint32_t sampleRate() const { return rate_.load(std::memory_order_relaxed); }
    // Engine thread.
    void push(const float* interleaved, size_t frames, uint32_t channels);
    void pushSilence(size_t frames);
    // Any thread (serialised internally).
    void compute(float* bands, int count) const;
    void resetSmoothing() const;
    // Any thread, lock-free: the latest `count` (<= kHistory) mono samples, oldest first. Returns how many samples were
    // pushed in all up to the newest one copied (it only grows; the same value = no new audio since the last copy).
    uint64_t snapshot(float* out, size_t count) const;

private:
    std::array<std::atomic<float>, kHistory> history_{};
    std::atomic<uint32_t> seq_{0};       // odd while a push is writing
    std::atomic<uint64_t> written_{0};   // samples pushed so far; the next one goes to written_ % kHistory
    std::atomic<uint32_t> rate_{48000};

    mutable std::mutex computeMutex_;
    mutable std::vector<float> smooth_;
    mutable bool seed_ = true;
    mutable std::chrono::steady_clock::time_point last_{};
    mutable std::array<float, kSize> re_{}, im_{};
};

} // namespace st::audio
