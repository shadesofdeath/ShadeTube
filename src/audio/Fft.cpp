#include "audio/Fft.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace st::audio::fft {

namespace {

constexpr size_t N = kSize;
constexpr int kLog2N = 11;
static_assert((size_t(1) << kLog2N) == N);

struct Tables {
    std::array<float, N> window;
    std::array<float, N / 2> cosT, sinT;
    std::array<uint16_t, N> reverse;
    Tables() {
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

const Tables& tables() {
    static const Tables t;
    return t;
}

} // namespace

const float* hann() { return tables().window.data(); }

void forward(float* re, float* im) {
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

} // namespace st::audio::fft
