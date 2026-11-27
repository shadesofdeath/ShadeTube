#include "audio/Spectrum.h"

#include "audio/Fft.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace st::audio {

namespace {

constexpr size_t N = Spectrum::kSize;
static_assert(N == fft::kSize);

} // namespace

// Seqlock writer (the engine thread is the only one): odd sequence while the samples change.
void Spectrum::push(const float* interleaved, size_t frames, uint32_t channels) {
    if (channels == 0 || frames == 0) return;
    const uint64_t total = frames;
    // Only the most recent kHistory frames matter.
    if (frames > kHistory) {
        interleaved += (frames - kHistory) * channels;
        frames = kHistory;
    }
    const float inv = 1.0f / static_cast<float>(channels);
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const uint64_t w = written_.load(std::memory_order_relaxed) + (total - frames);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0;
        for (uint32_t c = 0; c < channels; ++c) sum += interleaved[f * channels + c];
        history_[(w + f) & (kHistory - 1)].store(sum * inv, std::memory_order_relaxed);
    }
    written_.store(w + frames, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
}

void Spectrum::pushSilence(size_t frames) {
    if (frames == 0) return;
    const uint64_t total = frames;
    frames = std::min(frames, kHistory);
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const uint64_t w = written_.load(std::memory_order_relaxed) + (total - frames);
    for (size_t f = 0; f < frames; ++f) history_[(w + f) & (kHistory - 1)].store(0.f, std::memory_order_relaxed);
    written_.store(w + frames, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
}

// Seqlock reader: copy, then check that no push started or ended meanwhile. A push takes a few microseconds every
// 10-20 ms, so a retry is rare; after a few the copy is used as it is (a torn window only costs a visualiser frame).
uint64_t Spectrum::snapshot(float* out, size_t count) const {
    count = std::min(count, kHistory);
    uint64_t w = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s0 = seq_.load(std::memory_order_acquire);
        if (s0 & 1) {
            std::this_thread::yield();
            continue;
        }
        w = written_.load(std::memory_order_relaxed);
        const uint64_t first = w - count;   // wraps like the ring: only the low bits are used
        for (size_t i = 0; i < count; ++i) out[i] = history_[(first + i) & (kHistory - 1)].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == s0) break;
    }
    return w;
}

void Spectrum::resetSmoothing() const {
    std::lock_guard lock(computeMutex_);
    seed_ = true;
}

void Spectrum::compute(float* bands, int count) const {
    if (!bands || count <= 0) return;
    std::lock_guard lock(computeMutex_);
    snapshot(re_.data(), N);
    const float* window = fft::hann();
    for (size_t i = 0; i < N; ++i) re_[i] *= window[i];
    std::fill(im_.begin(), im_.end(), 0.f);
    fft::forward(re_.data(), im_.data());

    const float rate = static_cast<float>(rate_.load(std::memory_order_relaxed));
    const float binHz = rate / N;
    const float fMin = 30.f, fMax = std::min(16000.f, rate * 0.5f);
    // Full-scale sine through a Hann window peaks at N/4.
    constexpr float kNorm = 4.0f / N;
    constexpr float kFloorDb = -70.f, kCeilDb = -10.f;

    const auto now = std::chrono::steady_clock::now();
    float dt = last_.time_since_epoch().count() == 0 ? 1.f / 60 : std::chrono::duration<float>(now - last_).count();
    dt = std::clamp(dt, 0.001f, 0.1f);
    last_ = now;
    const float attack = 1.f - std::exp(-dt / 0.025f);   // ~25 ms rise
    const float release = 1.f - std::exp(-dt / 0.180f);  // ~180 ms fall
    // (Re)initialised state (new band count / after a reset) starts from the raw values.
    const bool seed = smooth_.size() != static_cast<size_t>(count) || seed_;
    if (smooth_.size() != static_cast<size_t>(count)) smooth_.assign(static_cast<size_t>(count), 0.f);
    seed_ = false;

    const float ratio = fMax / fMin;
    for (int b = 0; b < count; ++b) {
        const float f0 = fMin * std::pow(ratio, static_cast<float>(b) / count);
        const float f1 = fMin * std::pow(ratio, static_cast<float>(b + 1) / count);
        size_t k0 = static_cast<size_t>(std::floor(f0 / binHz));
        size_t k1 = static_cast<size_t>(std::ceil(f1 / binHz));
        k0 = std::clamp<size_t>(k0, 1, N / 2 - 1);
        k1 = std::clamp<size_t>(k1, k0 + 1, N / 2);
        float peak = 0;
        for (size_t k = k0; k < k1; ++k) peak = std::max(peak, re_[k] * re_[k] + im_[k] * im_[k]);
        const float db = 10.f * std::log10(peak * kNorm * kNorm + 1e-20f);
        const float v = std::clamp((db - kFloorDb) / (kCeilDb - kFloorDb), 0.f, 1.f);
        float& s = smooth_[static_cast<size_t>(b)];
        s = seed ? v : s + (v - s) * (v > s ? attack : release);
        bands[b] = s;
    }
}

} // namespace st::audio
