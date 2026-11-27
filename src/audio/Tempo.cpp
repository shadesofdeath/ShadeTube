#include "audio/Tempo.h"

#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"

#include <windows.h>

#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>

namespace st::audio::tempo {
namespace {

constexpr int kBands = 20;
constexpr double kLoHz = 40, kHiHz = 5000;
constexpr double kCombW[4] = {1.0, 0.8, 0.6, 0.4};
constexpr double kCombSum = 2.8;
constexpr double kPriorCenter = 120, kPriorOctaves = 1.4;
constexpr double kUpRatio = 0.78;     // ACF(P/2) / ACF(P) from which the half period is the beat...
constexpr double kUpRatioFast = 0.92; // ...when that makes it faster than kFastBpm (strong eighths of a mid-tempo song
constexpr double kFastBpm = 180;      // are not its beat: a song above 180 BPM needs events nearly as strong between)
constexpr double kDownRatio = 0.55;   // ACF(P) / ACF(2P) under which P is a subdivision
constexpr int kMaxHarmonics = 8;      // peaks used to refine the period
constexpr double kMinOnsets = 0.15;   // mean envelope below this: no onsets to speak of (a held tone, a drone)

size_t pow2AtLeast(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place iterative radix-2 complex FFT with cached twiddles and bit reversal.
class Fft {
public:
    explicit Fft(size_t n) : n_(n), cos_(n / 2), sin_(n / 2), rev_(n) {
        for (size_t i = 0; i < n / 2; ++i) {
            const double a = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
            cos_[i] = static_cast<float>(std::cos(a));
            sin_[i] = static_cast<float>(std::sin(a));
        }
        size_t bits = 0;
        while ((size_t{1} << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    void run(float* re, float* im) const {
        for (size_t i = 0; i < n_; ++i)
            if (rev_[i] > i) {
                std::swap(re[i], re[rev_[i]]);
                std::swap(im[i], im[rev_[i]]);
            }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len / 2, step = n_ / len;
            for (size_t i = 0; i < n_; i += len)
                for (size_t j = 0; j < half; ++j) {
                    const float wr = cos_[j * step], wi = sin_[j * step];
                    float* ar = re + i + j;
                    float* ai = im + i + j;
                    float* br = re + i + j + half;
                    float* bi = im + i + j + half;
                    const float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                    *br = *ar - tr;
                    *bi = *ai - ti;
                    *ar += tr;
                    *ai += ti;
                }
        }
    }

private:
    size_t n_;
    std::vector<float> cos_, sin_;
    std::vector<size_t> rev_;
};

double prior(double bpm) {
    const double o = std::log2(bpm / kPriorCenter) / kPriorOctaves;
    return std::exp(-0.5 * o * o);
}

} // namespace

std::vector<float> onsetEnvelope(const float* x, size_t frames, uint32_t rate, double& frameRate) {
    frameRate = 0;
    if (!x || rate < 4000) return {};
    const size_t n = pow2AtLeast(static_cast<size_t>(std::lround(rate * 0.0464)));
    const size_t hop = n / 4;
    frameRate = static_cast<double>(rate) / static_cast<double>(hop);
    if (frames < n) return {};
    const size_t count = (frames - n) / hop + 1;
    // The level is normalized first (RMS 0.1), so the log compression below doesn't depend on the master's loudness.
    double sum2 = 0;
    for (size_t i = 0; i < frames; ++i) sum2 += static_cast<double>(x[i]) * x[i];
    const double rms = std::sqrt(sum2 / static_cast<double>(frames));
    if (rms < 1e-6) return std::vector<float>(count, 0.f);
    const float gain = static_cast<float>(0.1 / rms);

    std::vector<float> window(n);
    for (size_t i = 0; i < n; ++i)
        window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n)));
    // Band edges in bins, log-spaced; every band at least one bin wide.
    const double hi = std::min(kHiHz, rate * 0.45);
    std::vector<size_t> edge(kBands + 1);
    for (int b = 0; b <= kBands; ++b) {
        const double f = kLoHz * std::pow(hi / kLoHz, static_cast<double>(b) / kBands);
        edge[b] = std::clamp<size_t>(static_cast<size_t>(std::lround(f * static_cast<double>(n) / rate)), 1, n / 2);
        if (b > 0 && edge[b] <= edge[b - 1]) edge[b] = std::min(n / 2, edge[b - 1] + 1);
    }

    const Fft fft(n);
    std::vector<float> re(n), im(n), flux(count, 0.f), prev(kBands, 0.f), cur(kBands, 0.f);
    for (size_t f = 0; f < count; ++f) {
        const float* s = x + f * hop;
        for (size_t i = 0; i < n; ++i) {
            re[i] = s[i] * window[i] * gain;
            im[i] = 0;
        }
        fft.run(re.data(), im.data());
        float d = 0;
        for (int b = 0; b < kBands; ++b) {
            float e = 0;
            for (size_t k = edge[b]; k < edge[b + 1]; ++k) e += re[k] * re[k] + im[k] * im[k];
            const float mag = std::sqrt(e / static_cast<float>(edge[b + 1] - edge[b]));
            cur[b] = std::log1p(100.f * mag);
            if (f > 0) d += std::max(0.f, cur[b] - prev[b]);
        }
        flux[f] = d;
        std::swap(prev, cur);
    }
    // High-pass: minus the moving average over ~1 s, rectified (what is left are the onsets standing out).
    std::vector<double> prefix(count + 1, 0.0);
    for (size_t i = 0; i < count; ++i) prefix[i + 1] = prefix[i] + flux[i];
    const size_t half = static_cast<size_t>(std::lround(frameRate * 0.5));
    std::vector<float> hp(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t a = i > half ? i - half : 0, b = std::min(count, i + half + 1);
        const double mean = (prefix[b] - prefix[a]) / static_cast<double>(b - a);
        hp[i] = static_cast<float>(std::max(0.0, flux[i] - mean));
    }
    // A short triangular smoothing (5 frames, ~58 ms): an onset lands on one frame or the next as the beat period is
    // rarely a whole number of frames; smoothed, the autocorrelation at a fractional period no longer loses half of
    // its peak to that rounding.
    std::vector<float> env(count, 0.f);
    static constexpr float kTri[5] = {1.f / 9, 2.f / 9, 3.f / 9, 2.f / 9, 1.f / 9};
    for (size_t i = 0; i < count; ++i) {
        float s = 0;
        for (int k = -2; k <= 2; ++k) {
            const long j = static_cast<long>(i) + k;
            if (j >= 0 && j < static_cast<long>(count)) s += kTri[k + 2] * hp[static_cast<size_t>(j)];
        }
        env[i] = s;
    }
    return env;
}

Estimate fromEnvelope(const std::vector<float>& env, double fr) {
    Estimate out;
    const size_t n = env.size();
    if (fr <= 0 || static_cast<double>(n) < fr * 6) return out;
    double mean = 0;
    for (float v : env) mean += v;
    mean /= static_cast<double>(n);
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = env[i] - mean;

    const double lagMin = fr * 60 / kMaxBpm, lagMax = fr * 60 / kMinBpm;
    // Lags far enough for the comb (4 P) and the refinement's harmonics, within a third of the envelope.
    const size_t maxLag = std::min(n / 3 * 2, static_cast<size_t>(std::ceil(lagMax * 4)) + 4);
    std::vector<double> acf(maxLag + 1, 0.0);
    for (size_t lag = 0; lag <= maxLag; ++lag) {
        double s = 0;
        const double* a = x.data();
        const double* b = x.data() + lag;
        const size_t m = n - lag;
        for (size_t i = 0; i < m; ++i) s += a[i] * b[i];
        acf[lag] = s / static_cast<double>(m);
    }
    if (acf[0] <= 1e-12) return out;
    for (size_t lag = maxLag + 1; lag-- > 0;) acf[lag] /= acf[0];

    auto A = [&](double lag) {
        if (lag < 0 || lag >= static_cast<double>(maxLag)) return 0.0;
        const size_t i = static_cast<size_t>(lag);
        const double t = lag - static_cast<double>(i);
        return acf[i] * (1 - t) + acf[i + 1] * t;
    };
    auto comb = [&](double p) {
        double s = 0;
        for (int k = 0; k < 4; ++k) s += kCombW[k] * A(p * (k + 1));
        return s / kCombSum;
    };
    // The local maximum of the autocorrelation within +-radius frames of `lag` (parabolic interpolation).
    auto peakNear = [&](double lag, double radius) {
        const long lo = std::max<long>(1, static_cast<long>(std::floor(lag - radius)));
        const long hi = std::min<long>(static_cast<long>(maxLag) - 1, static_cast<long>(std::ceil(lag + radius)));
        if (lo > hi) return lag;
        long best = lo;
        for (long i = lo; i <= hi; ++i)
            if (acf[static_cast<size_t>(i)] > acf[static_cast<size_t>(best)]) best = i;
        const double y0 = acf[static_cast<size_t>(best - 1)], y1 = acf[static_cast<size_t>(best)],
                     y2 = acf[static_cast<size_t>(best + 1)];
        const double den = y0 - 2 * y1 + y2;
        const double d = std::abs(den) > 1e-12 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        return static_cast<double>(best) + d;
    };

    // 3. The best period by comb score x prior.
    double bestP = 0, bestScore = -1e9;
    for (double p = lagMin; p <= lagMax; p += 0.125) {
        const double s = comb(p) * prior(60 * fr / p);
        if (s > bestScore) {
            bestScore = s;
            bestP = p;
        }
    }
    if (bestP <= 0) return out;
    double p = peakNear(bestP, 1.0);
    if (std::getenv("SHADETUBE_TEMPO_DEBUG")) {   // dev: the candidates (tempo_test file)
        std::fprintf(stderr, "  best %.1f BPM  A(P) %.3f  A(P/2) %.3f  A(2P) %.3f  A(3P/2) %.3f  A(2P/3) %.3f\n", 60 * fr / p, A(p),
                     A(peakNear(p / 2, 1)), A(peakNear(2 * p, 2)), A(peakNear(1.5 * p, 1.5)), A(peakNear(p / 1.5, 1)));
        for (double q = lagMin; q <= lagMax; q += 0.125) {
            const double s = comb(q) * prior(60 * fr / q);
            const double l = comb(q - 0.125) * prior(60 * fr / (q - 0.125)), r = comb(q + 0.125) * prior(60 * fr / (q + 0.125));
            if (s >= l && s > r && s > 0.25 * bestScore) std::fprintf(stderr, "    peak %.1f BPM score %.3f comb %.3f A %.3f\n", 60 * fr / q, s, comb(q), A(q));
        }
    }
    // 4. Octave check.
    bool doubled = false;
    if (60 * fr / (p / 2) <= kMaxBpm + 0.5) {
        const double ph = peakNear(p / 2, 1.0);
        // Past kFastBpm the comb (the harmonics' support) must agree too: a mid-tempo song's strong eighths can repeat
        // more than its beat at one lag, rarely over four.
        const bool fast = 60 * fr / ph > kFastBpm;
        if (A(ph) >= (fast ? kUpRatioFast : kUpRatio) * A(p) && (!fast || comb(ph) >= kUpRatioFast * comb(p))) {
            p = ph;
            doubled = true;
        }
    }
    if (!doubled && 60 * fr / (p * 2) >= kMinBpm - 0.5 && 2 * p + 3 < static_cast<double>(maxLag)) {
        const double p2 = peakNear(p * 2, 2.0);
        if (A(p) < kDownRatio * A(p2)) p = p2;
    }
    // 5. Refinement: the harmonics' peaks fitted through the origin (least squares), as far as the lags reach.
    double num = 0, den = 0;
    for (int k = 1; k <= kMaxHarmonics; ++k) {
        const double target = p * k;
        if (target + 2 >= static_cast<double>(maxLag)) break;
        const double pk = peakNear(target, std::max(1.0, 0.5 * k));
        if (A(pk) < 0.5 * A(p) && k > 1) continue;   // no clear peak there (a tempo drift, a fill)
        num += k * pk;
        den += static_cast<double>(k) * k;
    }
    if (den > 0) p = num / den;
    out.confidence = std::clamp(A(p), 0.0, 1.0);
    if (mean < kMinOnsets) return {0, 0};
    if (out.confidence < kMinConfidence) return {0, out.confidence};
    out.bpm = std::round(600 * fr / p) / 10;
    return out;
}

Estimate estimate(const float* mono, size_t frames, uint32_t rate) {
    double fr = 0;
    const auto env = onsetEnvelope(mono, frames, rate, fr);
    return fromEnvelope(env, fr);
}

namespace {
// COM + Media Foundation for one call on a worker thread.
struct MfScope {
    bool ok = false;
    HRESULT co = E_FAIL;
    MfScope() {
        co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE on an STA thread: not ours
        ok = SUCCEEDED(MFStartup(MF_VERSION));
    }
    ~MfScope() {
        if (ok) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }
};
} // namespace

FileResult analyzeFile(const std::wstring& path, const std::string& mimeType, double maxSeconds,
                       const std::atomic<bool>* cancel) {
    using clock = std::chrono::steady_clock;
    FileResult r;
    const auto t0 = clock::now();
    MfScope mf;
    if (!mf.ok) {
        r.error = "Media Foundation unavailable";
        return r;
    }
    auto buffer = std::make_shared<ProgressiveBuffer>(std::string(), path, 0, nullptr);
    buffer->start();
    auto done = [&] {
        buffer->cancel();
        buffer->release();
    };
    if (buffer->waitForLength() < 0) {
        r.error = "cannot read the file";
        done();
        return r;
    }
    std::vector<float> mono;
    {
        Decoder dec;
        bool unsupported = false;
        if (FAILED(dec.open(buffer, mimeType, unsupported, RatePolicy{kRate}))) {
            r.error = unsupported ? "unsupported format" : "cannot decode the file";
            done();
            return r;
        }
        uint32_t rate = dec.sampleRate() ? dec.sampleRate() : kRate;
        uint32_t channels = dec.channels() ? dec.channels() : 2;
        const double duration = static_cast<double>(dec.durationHns()) / 1e7;
        int64_t startHns = 0;
        if (duration > maxSeconds + 10) {
            const double start = std::min(duration * 0.2, duration - maxSeconds);
            startHns = static_cast<int64_t>(start * 1e7);
            if (FAILED(dec.seek(startHns))) startHns = 0;
        }
        const size_t want = static_cast<size_t>(maxSeconds * rate);
        mono.reserve(want);
        std::vector<float> scratch;
        for (int guard = 0; guard < 1'000'000 && mono.size() < want; ++guard) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                r.error = "cancelled";
                done();
                return r;
            }
            scratch.clear();
            int64_t ts = -1;
            bool changed = false;
            HRESULT hr = S_OK;
            const Decoder::Status s = dec.read(scratch, ts, changed, hr);
            if (s == Decoder::Status::Error) break;
            if (changed) {
                if (dec.sampleRate() != rate && !mono.empty()) break;   // a rate change mid-way: keep what came
                rate = dec.sampleRate() ? dec.sampleRate() : rate;
                channels = dec.channels() ? dec.channels() : channels;
            }
            const size_t fcount = scratch.size() / channels;
            size_t skip = 0;   // a seek lands at a packet before the target: drop what lies before it
            if (startHns > 0 && ts >= 0 && ts < startHns && mono.empty())
                skip = std::min(fcount, static_cast<size_t>((startHns - ts) * rate / 10'000'000));
            for (size_t i = skip; i < fcount; ++i) {
                float v = 0;
                for (uint32_t c = 0; c < channels; ++c) v += scratch[i * channels + c];
                mono.push_back(v / static_cast<float>(channels));
            }
            if (s == Decoder::Status::EndOfStream) break;
        }
        r.seconds = static_cast<double>(mono.size()) / rate;
        done();
        const auto t1 = clock::now();
        r.decodeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (mono.size() < static_cast<size_t>(rate) * 8) {
            r.error = "too short";
            return r;
        }
        r.tempo = estimate(mono.data(), mono.size(), rate);
        r.analyzeMs = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
    }
    r.ok = true;
    return r;
}

} // namespace st::audio::tempo
