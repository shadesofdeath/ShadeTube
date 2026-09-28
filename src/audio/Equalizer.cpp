#include "audio/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace st::audio {

namespace {

constexpr double kQ = 1.41;   // ~1 octave between the -3 dB points: neighbouring bands blend smoothly

struct Biquad {
    double b0, b1, b2, a1, a2;   // normalised (a0 = 1)
};

Biquad peaking(double freq, double gainDb, double rate) {
    const double f = std::min(freq, rate * 0.45);   // stay clear of Nyquist (16 kHz at 32 kHz output)
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2 * std::numbers::pi * f / rate;
    const double alpha = std::sin(w0) / (2 * kQ);
    const double cosw = std::cos(w0);
    const double a0 = 1 + alpha / a;
    return {(1 + alpha * a) / a0, (-2 * cosw) / a0, (1 - alpha * a) / a0, (-2 * cosw) / a0, (1 - alpha / a) / a0};
}

double magnitudeDb(const Biquad& q, double freq, double rate) {
    const double w = 2 * std::numbers::pi * freq / rate;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    const std::complex<double> h = (q.b0 + q.b1 * z1 + q.b2 * z2) / (1.0 + q.a1 * z1 + q.a2 * z2);
    return 20 * std::log10(std::max(std::abs(h), 1e-9));
}

} // namespace

bool EqSettings::flat() const {
    return preampDb == 0 && std::all_of(gainsDb.begin(), gainsDb.end(), [](float g) { return g == 0; });
}

const std::vector<EqPreset>& eqPresets() {
    //                                   31   62  125  250  500   1k   2k   4k   8k  16k
    static const std::vector<EqPreset> presets = {
        {"flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"bass", {6, 5, 4, 2, 0.5f, 0, 0, 0, 0, 0}},
        {"bass-cut", {-6, -5, -4, -2, -0.5f, 0, 0, 0, 0, 0}},
        {"treble", {0, 0, 0, 0, 0, 0.5f, 1.5f, 3, 5, 6}},
        {"treble-cut", {0, 0, 0, 0, 0, -0.5f, -1.5f, -3, -5, -6}},
        {"vocal", {-2, -2, -1, 1, 3, 4, 3.5f, 2.5f, 1, 0}},
        {"pop", {-1, 0, 1.5f, 3, 3.5f, 2, 0, -0.5f, -0.5f, -1}},
        {"rock", {4.5f, 3.5f, 2, 0, -1, -1, 0.5f, 2, 3, 3.5f}},
        {"hiphop", {5, 4.5f, 2, 3, -0.5f, -1, 1, -0.5f, 2, 3}},
        {"electronic", {5, 4, 1.5f, 0, -2, 1.5f, 0.5f, 1.5f, 4, 5}},
        {"jazz", {3, 2, 1, 2, -1.5f, -1.5f, 0, 1, 2, 3}},
        {"classical", {3, 2.5f, 1.5f, 1, 0, 0, 0, 1, 2, 2.5f}},
        {"acoustic", {3, 3, 2, 1, 1.5f, 1, 2, 2.5f, 2, 1}},
        {"loudness", {4, 3, 1, 0, -1, -0.5f, 0, 1, 3, 3.5f}},
        {"speech", {-4, -3, -1, 1, 3, 4, 4, 3, 1, -1}},
    };
    return presets;
}

const EqPreset* findEqPreset(std::string_view id) {
    for (const auto& p : eqPresets())
        if (id == p.id) return &p;
    return nullptr;
}

double eqResponseDb(const std::array<float, kEqBands>& gainsDb, double freqHz, double sampleRate) {
    double db = 0;
    for (int b = 0; b < kEqBands; ++b)
        if (gainsDb[b] != 0) db += magnitudeDb(peaking(kEqFrequencies[b], gainsDb[b], sampleRate), freqHz, sampleRate);
    return db;
}

double eqHeadroomDb(const std::array<float, kEqBands>& gainsDb, double sampleRate) {
    double peak = 0;
    // 1/12-octave grid from 20 Hz to 20 kHz (or Nyquist): fine enough to catch the summit of two neighbouring boosts.
    const double top = std::min(20000.0, sampleRate * 0.49);
    for (double f = 20; f <= top; f *= 1.0594630943592953) peak = std::max(peak, eqResponseDb(gainsDb, f, sampleRate));
    return peak;
}

void Equalizer::configure(const EqSettings& s, uint32_t sampleRate, uint32_t channels) {
    const bool formatChanged = sampleRate != rate_ || channels != channels_;
    rate_ = sampleRate;
    channels_ = channels;
    active_ = s.enabled && !s.flat() && sampleRate > 0 && channels > 0;
    if (!active_) {
        reset();
        return;
    }
    const bool fresh = formatChanged || state_.size() != size_t(kEqBands) * channels_;
    if (fresh) state_.assign(size_t(kEqBands) * channels_, {});
    for (int b = 0; b < kEqBands; ++b) {
        const float g = std::clamp(s.gainsDb[b], -kEqMaxGainDb, kEqMaxGainDb);
        const bool on = g != 0;
        if (!on && bandOn_[b] && !fresh)   // a band switched off: forget its memory
            for (uint32_t c = 0; c < channels_; ++c) state_[size_t(b) * channels_ + c] = {};
        bandOn_[b] = on;
        const Biquad q = peaking(kEqFrequencies[b], g, sampleRate);
        coeffs_[b] = {q.b0, q.b1, q.b2, q.a1, q.a2};
    }
    const double headroom = eqHeadroomDb(s.gainsDb, sampleRate);
    gain_ = static_cast<float>(std::pow(10.0, (std::clamp(s.preampDb, -kEqMaxGainDb, kEqMaxGainDb) - headroom) / 20.0));
}

void Equalizer::reset() {
    for (auto& st : state_) st = {};
}

void Equalizer::process(float* p, size_t frames) {
    if (!active_ || frames == 0) return;
    const uint32_t ch = channels_;
    for (int b = 0; b < kEqBands; ++b) {
        if (!bandOn_[b]) continue;
        const Coeffs k = coeffs_[b];
        for (uint32_t c = 0; c < ch; ++c) {
            State s = state_[size_t(b) * ch + c];
            float* x = p + c;
            for (size_t i = 0; i < frames; ++i, x += ch) {
                const double in = *x;
                const double out = k.b0 * in + s.z1;
                s.z1 = k.b1 * in - k.a1 * out + s.z2;
                s.z2 = k.b2 * in - k.a2 * out;
                *x = static_cast<float>(out);
            }
            // Denormals after long silence cost a lot of CPU on x64: flush the tiny tail.
            if (std::abs(s.z1) < 1e-20) s.z1 = 0;
            if (std::abs(s.z2) < 1e-20) s.z2 = 0;
            state_[size_t(b) * ch + c] = s;
        }
    }
    if (gain_ != 1.f)
        for (size_t i = 0; i < frames * ch; ++i) p[i] *= gain_;
}

} // namespace st::audio
