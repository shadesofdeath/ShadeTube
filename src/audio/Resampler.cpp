#include "audio/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace st::audio {

namespace {

constexpr int kBaseHalf = 56;      // taps per side at 1:1 (~2.2 kHz transition band at 44.1 / 48 kHz, ~80 dB)
constexpr int kMaxHalf = 384;      // 192 kHz -> 22.05 kHz would want more; the stop band just gets a little shallower
constexpr double kBeta = 7.86;     // Kaiser beta for ~80 dB
constexpr double kCutoff = 0.95;   // -6 dB point, as a fraction of the lower Nyquist frequency

// Zeroth-order modified Bessel function of the first kind (power series; converges fast for the betas used here).
double besselI0(double x) {
    double sum = 1, term = 1;
    const double q = x * x / 4;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

} // namespace

void Resampler::configure(uint32_t inRate, uint32_t outRate, uint32_t channels) {
    if (inRate == in_ && outRate == out_ && channels == ch_) return;
    in_ = inRate;
    out_ = outRate;
    ch_ = channels;
    kernel_.clear();
    half_ = 0;
    if (in_ && out_ && in_ != out_) {
        const double ratio = std::max(1.0, double(in_) / out_);
        half_ = std::min(kMaxHalf, static_cast<int>(std::ceil(kBaseHalf * ratio)));
        const double fc = kCutoff * std::min(in_, out_) / in_;   // in input Nyquist units
        const double i0b = besselI0(kBeta);
        const int n = 2 * half_ * kTable + 2;
        kernel_.resize(n);
        for (int u = 0; u < n; ++u) {
            const double x = double(u) / kTable - half_;   // input frames from the interpolation point
            const double r = x / half_;
            if (std::fabs(r) >= 1) {
                kernel_[u] = 0;
                continue;
            }
            const double t = std::numbers::pi * fc * x;
            const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(t) / t;
            kernel_[u] = static_cast<float>(fc * sinc * besselI0(kBeta * std::sqrt(1 - r * r)) / i0b);
        }
        coef_.resize(2 * size_t(half_));
    }
    reset();
}

void Resampler::reset(int64_t startFrame) {
    buf_.assign(size_t(half_) * ch_, 0.f);   // the silent past before the first frame
    bufStart_ = -half_;
    received_ = 0;
    next_ = 0;
    startFrame = std::max<int64_t>(0, startFrame);
    if (passthrough() || !in_ || !out_) {
        outStart_ = startFrame;
        phase_ = 0;
        return;
    }
    const uint64_t s = static_cast<uint64_t>(startFrame);
    outStart_ = static_cast<int64_t>((s * out_ + in_ - 1) / in_);
    phase_ = static_cast<uint64_t>(outStart_) * in_ - s * out_;   // 0 .. in_ - 1
}

int64_t Resampler::toOutput(int64_t inFrames) const {
    if (in_ == out_ || !in_) return inFrames;
    return static_cast<int64_t>((static_cast<uint64_t>(std::max<int64_t>(0, inFrames)) * out_ + in_ - 1) / in_);
}

int64_t Resampler::toInput(int64_t outFrames) const {
    if (in_ == out_ || !out_) return outFrames;
    return static_cast<int64_t>(static_cast<uint64_t>(std::max<int64_t>(0, outFrames)) * in_ / out_);
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    if (frames == 0 || ch_ == 0) return;
    if (passthrough()) {
        out.insert(out.end(), in, in + frames * ch_);
        return;
    }
    buf_.insert(buf_.end(), in, in + frames * ch_);
    received_ += static_cast<int64_t>(frames);
    produce(out, false);
}

void Resampler::flush(std::vector<float>& out) {
    if (passthrough() || ch_ == 0) return;
    buf_.insert(buf_.end(), size_t(half_) * ch_, 0.f);   // the silent future after the last frame
    produce(out, true);
    buf_.resize(buf_.size() - size_t(half_) * ch_);
}

void Resampler::produce(std::vector<float>& out, bool final) {
    const size_t ch = ch_;
    const int taps = 2 * half_;
    const int64_t bufEnd = bufStart_ + static_cast<int64_t>(buf_.size() / ch);
    for (;;) {
        const uint64_t num = next_ * in_ + phase_;
        const int64_t n = static_cast<int64_t>(num / out_);
        const uint64_t frac = num % out_;
        if (final ? n >= received_ : n + half_ >= bufEnd) break;
        // Kernel taps for input frames n - half_ + 1 .. n + half_ at fractional offset f = frac / out_.
        const double q = (1.0 - double(frac) / out_) * kTable;
        int q0 = static_cast<int>(q);
        float w = static_cast<float>(q - q0);
        if (q0 >= kTable) {   // f == 0 exactly
            q0 = kTable;
            w = 0;
        }
        for (int i = 0; i < taps; ++i) {
            const float* k = kernel_.data() + size_t(i) * kTable + q0;
            coef_[i] = k[0] + w * (k[1] - k[0]);
        }
        const float* src = buf_.data() + size_t(n - half_ + 1 - bufStart_) * ch;
        const size_t at = out.size();
        out.resize(at + ch);
        float* dst = out.data() + at;
        if (ch == 2) {
            float l = 0, r = 0;
            for (int i = 0; i < taps; ++i) {
                l += src[2 * i] * coef_[i];
                r += src[2 * i + 1] * coef_[i];
            }
            dst[0] = l;
            dst[1] = r;
        } else {
            for (size_t c = 0; c < ch; ++c) {
                float s = 0;
                for (int i = 0; i < taps; ++i) s += src[size_t(i) * ch + c] * coef_[i];
                dst[c] = s;
            }
        }
        ++next_;
    }
    // Drop input no later output frame needs (in blocks, so the vector isn't shifted for every call).
    const int64_t need = static_cast<int64_t>((next_ * in_ + phase_) / out_) - half_ + 1;
    if (const int64_t drop = need - bufStart_; drop > 4096) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(drop * ch));
        bufStart_ += drop;
    }
}

} // namespace st::audio
