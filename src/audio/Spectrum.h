#pragma once
// Spectrum analyser for visualisers (internal to st_audio).
//
// The engine thread pushes the mono mix of every block it writes to the device (before the user
// volume, so the visualiser does not shrink with the volume knob). compute() (UI thread, up to
// 60x/s) copies the latest 2048 samples, applies a Hann window + radix-2 FFT and maps the
// magnitudes to `count` log-spaced bands in 0..1 (-70..-10 dBFS), with attack/release smoothing.
// Cost per call: one 2048-point complex FFT (~50 us).
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
    static constexpr size_t kSize = 2048;

    void setSampleRate(uint32_t rate) { rate_.store(rate, std::memory_order_relaxed); }
    // Engine thread.
    void push(const float* interleaved, size_t frames, uint32_t channels);
    void pushSilence(size_t frames);
    // Any thread (serialised internally).
    void compute(float* bands, int count) const;
    void resetSmoothing() const;

private:
    mutable std::mutex historyMutex_;
    std::array<float, kSize> history_{};
    size_t write_ = 0;
    std::atomic<uint32_t> rate_{48000};

    mutable std::mutex computeMutex_;
    mutable std::vector<float> smooth_;
    mutable bool seed_ = true;
    mutable std::chrono::steady_clock::time_point last_{};
    mutable std::array<float, kSize> re_{}, im_{};
};

} // namespace st::audio
