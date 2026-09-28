#include "audio/Spectrum.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace st::audio {

namespace {

constexpr size_t N = Spectrum::kSize;
constexpr int kLog2N = 11;
static_assert((size_t(1) << kLog2N) == N);

struct FftTables {
    std::array<float, N> window;
    std::array<float, N / 2> cosT, sinT;
    std::array<uint16_t, N> reverse;
    FftTables() {
        for (size_t i = 0; i < N; ++i) {
            window[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / (N - 1));
            uint32_t r = 0;
            for (int b = 0; b < kLog2N; ++b)
                if (i & (size_t(1) << b)) r |= 1u << (kLog2N - 1 - b);
            reverse[i] = static_cast<uint16_t>(r);
        }
        for (size_t k = 0; k < N / 2; ++k) {
            const double a = 2.0 * std::numbers::pi * static_cast<double>(k) / N;
            cosT[k] = static_cast<float>(std::cos(a));
            sinT[k] = static_cast<float>(std::sin(a));
        }
    }
};

const FftTables& tables() {
    static const FftTables t;  // thread-safe static init
    return t;
}

// In-place iterative radix-2 decimation-in-time FFT.
void fft(float* re, float* im) {
    const auto& t = tables();
    for (size_t i = 0; i < N; ++i) {
        const size_t j = t.reverse[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t size = 2; size <= N; size <<= 1) {
        const size_t half = size >> 1, step = N / size;
        for (size_t i = 0; i < N; i += size) {
            for (size_t j = 0; j < half; ++j) {
                const float wr = t.cosT[j * step], wi = -t.sinT[j * step];
                const size_t a = i + j, b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

} // namespace

void Spectrum::push(const float* interleaved, size_t frames, uint32_t channels) {
    if (channels == 0) return;
    // Only the most recent kSize frames matter.
    if (frames > kSize) {
        interleaved += (frames - kSize) * channels;
        frames = kSize;
    }
    const float inv = 1.0f / static_cast<float>(channels);
    std::lock_guard lock(historyMutex_);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0;
        for (uint32_t c = 0; c < channels; ++c) sum += interleaved[f * channels + c];
        history_[write_] = sum * inv;
        write_ = (write_ + 1) & (kSize - 1);
    }
}

void Spectrum::pushSilence(size_t frames) {
    frames = std::min(frames, kSize);
    std::lock_guard lock(historyMutex_);
    for (size_t f = 0; f < frames; ++f) {
        history_[write_] = 0.f;
        write_ = (write_ + 1) & (kSize - 1);
    }
}

void Spectrum::resetSmoothing() const {
    std::lock_guard lock(computeMutex_);
    seed_ = true;
}

void Spectrum::compute(float* bands, int count) const {
    if (!bands || count <= 0) return;
    std::lock_guard lock(computeMutex_);
    const auto& t = tables();
    {
        std::lock_guard h(historyMutex_);
        for (size_t i = 0; i < N; ++i) re_[i] = history_[(write_ + i) & (N - 1)] * t.window[i];
    }
    std::fill(im_.begin(), im_.end(), 0.f);
    fft(re_.data(), im_.data());

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
