#include "audio/VisualAnalysis.h"

#include "audio/Fft.h"

#include <algorithm>
#include <cmath>

namespace st::audio::viz {

namespace {

static_assert(kWindow == fft::kSize);

constexpr float kTiltDbPerOctave = 2.5f;   // treble lifted, bass lowered, around 1 kHz
constexpr float kTiltMin = -8.f, kTiltMax = 9.f;
constexpr float kRangeDb = 50.f;           // the bands' range below the adaptive ceiling
constexpr float kGamma = 1.5f;             // quiet bands lower, loud ones keep their height
constexpr float kWarpHz = 100.f;           // band spacing: log of (f + 100 Hz), so the bass gets less room
constexpr float kCeilMin = -32.f, kCeilMax = 6.f;
constexpr float kPeakHold = 0.35f;         // s
constexpr float kPeakGravity = 2.4f;       // 1/s^2: from the top to the floor in about 0.9 s
constexpr float kBeatDecay = 0.18f;        // s
constexpr double kBeatHop = 512.0 / 48000;   // the beat detector's step (s of audio)
constexpr float kEnergyAttack = 0.05f, kEnergyRelease = 0.30f;
constexpr float kEnergyRange = 30.f;
struct Range {
    float lo, hi;
};
constexpr Range kEnergyRanges[3] = {{40.f, 140.f}, {250.f, 2000.f}, {2500.f, 10000.f}};

float smoothStep(float s, float target, float a, float r) { return s + (target - s) * (target > s ? a : r); }
float coeff(double dt, float tau) { return tau <= 0 ? 1.f : static_cast<float>(1.0 - std::exp(-dt / tau)); }

} // namespace

// ---- spectrum and bands ------------------------------------------------------------------------------------------

void powerSpectrum(const float* samples, float* power, float* re, float* im) {
    const float* w = fft::hann();
    for (size_t i = 0; i < kWindow; ++i) {
        re[i] = samples[i] * w[i];
        im[i] = 0.f;
    }
    fft::forward(re, im);
    // A full-scale sine through the Hann window peaks at N / 4.
    constexpr float kNorm = 4.0f / kWindow;
    for (size_t k = 0; k < kBins; ++k) power[k] = (re[k] * re[k] + im[k] * im[k]) * (kNorm * kNorm);
}

float toDb(float power) { return power > 1e-12f ? 10.f * std::log10(power) : -120.f; }

std::vector<Band> makeBands(int count, float fLo, float fHi, uint32_t rate) {
    std::vector<Band> out;
    if (count <= 0 || rate == 0) return out;
    const float binHz = static_cast<float>(rate) / kWindow;
    fHi = std::min(fHi, static_cast<float>(rate) * 0.5f * 0.95f);
    fLo = std::clamp(fLo, binHz, fHi * 0.5f);
    const float a = fLo + kWarpHz, ratio = (fHi + kWarpHz) / a;
    auto edge = [&](int i) { return a * std::pow(ratio, static_cast<float>(i) / static_cast<float>(count)) - kWarpHz; };
    out.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        Band& b = out[static_cast<size_t>(i)];
        b.lo = edge(i);
        b.hi = edge(i + 1);
        b.center = std::sqrt(b.lo * b.hi);
        b.k0 = std::max(1, static_cast<int>(std::ceil(b.lo / binHz)));
        b.k1 = std::min(static_cast<int>(kBins) - 1, static_cast<int>(std::ceil(b.hi / binHz)) - 1);
        b.bin = std::clamp(b.center / binHz, 1.f, static_cast<float>(kBins - 2));
        // Widths only grow, and a band at least a bin wide always holds a bin's centre: the narrow ones are the lowest.
        b.narrow = b.hi - b.lo < binHz;
    }
    return out;
}

float bandPower(const Band& b, const float* power) {
    if (b.interpolate()) {
        // Under a bin wide: the magnitude at the band's centre, interpolated between the two bins around it.
        const int k = static_cast<int>(b.bin);
        const float f = b.bin - static_cast<float>(k);
        const float amp = std::sqrt(power[k]) * (1.f - f) + std::sqrt(power[k + 1]) * f;
        return amp * amp;
    }
    float m = 0;
    for (int k = b.k0; k <= b.k1; ++k) m = std::max(m, power[k]);
    return m;
}

float rangePower(const float* power, float fLo, float fHi, uint32_t rate) {
    if (rate == 0) return 0;
    const float binHz = static_cast<float>(rate) / kWindow;
    int k0 = std::max(1, static_cast<int>(std::ceil(fLo / binHz)));
    int k1 = std::min(static_cast<int>(kBins) - 1, static_cast<int>(std::floor(fHi / binHz)));
    if (k1 < k0) k0 = k1 = std::clamp(static_cast<int>(std::lround((fLo + fHi) * 0.5f / binHz)), 1, static_cast<int>(kBins) - 1);
    float sum = 0;
    for (int k = k0; k <= k1; ++k) sum += power[k];
    return sum;
}

float sampleAt(const std::vector<float>& v, float x) {
    if (v.empty()) return 0;
    const float pos = std::clamp(x, 0.f, 1.f) * static_cast<float>(v.size() - 1);
    const size_t i = static_cast<size_t>(pos);
    if (i + 1 >= v.size()) return v.back();
    const float f = pos - static_cast<float>(i);
    return v[i] + (v[i + 1] - v[i]) * f;
}

// ---- beats ---------------------------------------------------------------------------------------------------------

void BeatDetector::reset() { *this = BeatDetector{}; }

float BeatDetector::push(double t, const float* binsDb, int count, float gateDb) {
    count = std::clamp(count, 0, kMaxBins);
    // A gap (pause, seek, the device reopened), time going backwards or another bin layout (sample rate): no rise is
    // measured against old audio.
    if (t < lastT_ || t - lastT_ > 0.5 || count != bins_) size_ = 0;
    bins_ = count;
    const double step = lastT_ > -1e8 ? std::clamp(t - lastT_, 0.0, 0.5) : 0.0;
    Sample now;
    now.t = t;
    for (int k = 0; k < count; ++k) now.db[static_cast<size_t>(k)] = std::max(binsDb[k], kBinFloorDb);
    // The spectrum kLag ago, interpolated between the two samples around that time (newest first).
    const double want = t - kLag;
    bool have = false;
    float flux = 0;
    for (size_t i = 0; i < size_; ++i) {
        const Sample& s = ring_[(head_ + ring_.size() - 1 - i) % ring_.size()];
        if (s.t > want) continue;
        const Sample* n = i > 0 ? &ring_[(head_ + ring_.size() - i) % ring_.size()] : nullptr;
        const float f = n && n->t > s.t ? static_cast<float>((want - s.t) / (n->t - s.t)) : 0.f;
        for (int k = 0; k < count; ++k) {
            const size_t j = static_cast<size_t>(k);
            const float lagged = n ? s.db[j] + (n->db[j] - s.db[j]) * f : s.db[j];
            flux += std::max(0.f, now.db[j] - lagged);
        }
        flux /= static_cast<float>(std::max(1, count));
        have = true;
        break;
    }
    ring_[head_] = now;
    head_ = (head_ + 1) % ring_.size();
    size_ = std::min(size_ + 1, ring_.size());
    lastT_ = t;
    if (!have) {
        prevRaw_ = flux_ = 0.f;
        return 0.f;
    }
    // The onset function: this rise and the one a step before, averaged. A kick's rise lasts several steps (the window
    // is 43 ms long), the random spikes of noise rarely two in a row.
    flux_ = 0.5f * (flux + prevRaw_);
    prevRaw_ = flux;
    flux = flux_;

    const float thr = threshold();
    float strength = 0.f;
    if (armed_ && flux > thr && gateDb > kSilenceDb && t - lastBeat_ >= kMinGap) {
        strength = std::clamp(0.55f + (flux - thr) / 10.f, 0.35f, 1.f);
        lastBeat_ = t;
        ++beats_;
        armed_ = false;
    } else if (!armed_ && flux < mean_ + 0.5f * dev_ + kFloorDb * 0.5f) {
        armed_ = true;   // the rise is over: the next one may be a beat
    }
    const float a = static_cast<float>(1.0 - std::exp(-step / kStatsTau));
    mean_ += (flux - mean_) * a;
    dev_ += (std::fabs(flux - mean_) - dev_) * a;   // the mean deviation: the beats themselves barely widen it
    return strength;
}

// ---- the analyzer ----------------------------------------------------------------------------------------------------

void Analyzer::configure(int bandCount, int waveformPoints) {
    const float attack = attack_, release = release_;
    *this = Analyzer{};
    attack_ = attack;
    release_ = release;
    count_ = std::max(1, bandCount);
    target_.assign(static_cast<size_t>(count_), 0.f);
    hold_.assign(static_cast<size_t>(count_), 0.f);
    fall_.assign(static_cast<size_t>(count_), 0.f);
    out_.bands.assign(static_cast<size_t>(count_), 0.f);
    out_.peaks.assign(static_cast<size_t>(count_), 0.f);
    wave_.assign(static_cast<size_t>(std::max(0, waveformPoints)), 0.f);
    lowpass_.assign(kWindow, 0.f);
}

void Analyzer::analyse(const float* samples, uint64_t position, uint32_t rate) {
    if (rate != rate_ || bands_.size() != static_cast<size_t>(count_)) {
        rate_ = rate;
        bands_ = makeBands(count_, kLowHz, kHighHz, rate);
    }
    const double hop = position_ != ~0ull && position > position_
                           ? std::min(0.25, static_cast<double>(position - position_) / rate)
                           : 0.02;
    position_ = position;
    ++analyses_;
    powerSpectrum(samples, power_.data(), re_.data(), im_.data());

    // Bands: tilted levels under a ceiling that follows the loudest one.
    float maxDb = -120.f;
    for (size_t i = 0; i < bands_.size(); ++i) {
        const float tilt = std::clamp(kTiltDbPerOctave * std::log2(std::max(bands_[i].center, 1.f) / 1000.f), kTiltMin, kTiltMax);
        const float db = toDb(bandPower(bands_[i], power_.data())) + tilt;
        target_[i] = db;
        maxDb = std::max(maxDb, db);
    }
    ceilDb_ += (maxDb - ceilDb_) * coeff(hop, maxDb > ceilDb_ ? 0.12f : 5.0f);
    ceilDb_ = std::clamp(ceilDb_, kCeilMin, kCeilMax);
    const float top = ceilDb_ + 1.5f, floor = top - kRangeDb;
    for (float& t : target_) t = std::pow(std::clamp((t - floor) / kRangeDb, 0.f, 1.f), kGamma);

    // Energies, each in its own adaptive range.
    for (int e = 0; e < 3; ++e) {
        const float db = toDb(rangePower(power_.data(), kEnergyRanges[e].lo, kEnergyRanges[e].hi, rate));
        float& ceil = energyCeil_[static_cast<size_t>(e)];
        ceil += (db - ceil) * coeff(hop, db > ceil ? 0.1f : 6.0f);
        ceil = std::clamp(ceil, -40.f, 6.f);
        energyTarget_[static_cast<size_t>(e)] = std::pow(std::clamp((db - (ceil - kEnergyRange)) / kEnergyRange, 0.f, 1.f), 1.2f);
    }
    double sq = 0;
    for (size_t i = 0; i < kWindow; ++i) sq += static_cast<double>(samples[i]) * samples[i];
    const float rmsDb = toDb(static_cast<float>(sq / kWindow));
    levelTarget_ = std::clamp((rmsDb + 60.f) / 54.f, 0.f, 1.f);

    buildWaveform(samples, rate, hop);
}

// Beats on a fixed grid of the audio: a step every kBeatHop seconds of samples (~10.7 ms), each from the window that
// ended there (the history reaches back that far), so the onset function never depends on when frames come or how the
// engine cuts its blocks. Frames further apart than the history covers lose the steps in between.
void Analyzer::detectBeats(const float* samples, size_t count, uint64_t position, uint32_t rate) {
    const uint64_t hop = std::max<uint64_t>(64, static_cast<uint64_t>(std::lround(rate * kBeatHop)));
    const uint64_t back = count - kWindow;   // how far before `position` a window may end
    const uint64_t earliest = position > back ? position - back : 0;
    uint64_t g = grid_ != ~0ull ? grid_ + hop : position / hop * hop;
    if (g < earliest) g = (earliest + hop - 1) / hop * hop;
    const float binHz = static_cast<float>(rate) / kWindow;
    const int k0 = std::max(1, static_cast<int>(std::ceil(40.f / binHz)));
    const int k1 = std::min({static_cast<int>(kBins) - 1, static_cast<int>(std::floor(250.f / binHz)), k0 + BeatDetector::kMaxBins - 1});
    std::array<float, BeatDetector::kMaxBins> low{};
    for (; g <= position; g += hop) {
        const float* w = samples + (count - static_cast<size_t>(position - g)) - kWindow;
        powerSpectrum(w, beatPower_.data(), re_.data(), im_.data());
        for (int k = k0; k <= k1; ++k) low[static_cast<size_t>(k - k0)] = toDb(beatPower_[static_cast<size_t>(k)]);
        const float gate = toDb(rangePower(beatPower_.data(), kEnergyRanges[0].lo, kEnergyRanges[0].hi, rate));
        if (const float st = beat_.push(static_cast<double>(g) / rate, low.data(), k1 - k0 + 1, gate); st > 0)
            out_.beat = std::max(out_.beat, st);
        grid_ = g;
    }
    out_.beats = beat_.beats();
}

// The oscilloscope's view of the window: low-passed (a box of 1/2000 s), started on a rising zero crossing so a
// periodic sound stands still, decimated by averaging, auto-gained.
void Analyzer::buildWaveform(const float* samples, uint32_t rate, double hop) {
    if (wave_.empty()) return;
    const int n = static_cast<int>(kWindow);
    const int box = std::max(1, static_cast<int>(rate / 2000));
    float acc = 0;
    for (int i = 0; i < n; ++i) {
        acc += samples[i];
        if (i >= box) acc -= samples[i - box];
        lowpass_[static_cast<size_t>(i)] = acc / static_cast<float>(std::min(i + 1, box));
    }
    const int span = std::clamp(static_cast<int>(rate * 0.024), 256, n - 2 * box - 64);
    int start = n - span;
    const float hyst = std::max(0.002f, waveEnv_ * 0.04f);
    bool below = false;
    for (int i = box; i < n - span; ++i) {
        const float v = lowpass_[static_cast<size_t>(i)];
        if (v < -hyst) below = true;
        else if (below && v >= 0.f) {
            start = i;
            break;
        }
    }
    const int points = static_cast<int>(wave_.size());
    float peak = 0;
    for (int p = 0; p < points; ++p) {
        const int a = start + p * span / points, b = std::max(a + 1, start + (p + 1) * span / points);
        float sum = 0;
        for (int i = a; i < b; ++i) sum += lowpass_[static_cast<size_t>(i)];
        const float v = sum / static_cast<float>(b - a);
        wave_[static_cast<size_t>(p)] = v;
        peak = std::max(peak, std::fabs(v));
    }
    // Fast up, slow down (about 1.5 s), never so low that the hiss of a quiet passage fills the screen.
    waveEnv_ = peak > waveEnv_ ? waveEnv_ + (peak - waveEnv_) * 0.6f : waveEnv_ * static_cast<float>(std::exp(-hop / 1.5));
    waveEnv_ = std::max(waveEnv_, 0.03f);
    const float g = 0.9f / waveEnv_;
    for (float& v : wave_) v = std::clamp(v * g, -1.f, 1.f);
}

void Analyzer::update(const float* samples, size_t count, uint64_t position, uint32_t rate, double dt, bool playing) {
    if (count_ == 0) configure(64, 0);
    dt = std::clamp(dt, 0.0, 0.25);
    out_.beat *= static_cast<float>(std::exp(-dt / kBeatDecay));
    if (playing && samples && count >= kWindow && rate > 0) {
        if (position != position_) {   // new audio since the last frame
            analyse(samples + (count - kWindow), position, rate);
            detectBeats(samples, count, position, rate);
        }
    } else {
        // At rest: the goals go to zero, the next play starts a fresh analysis, the waveform flattens.
        std::fill(target_.begin(), target_.end(), 0.f);
        energyTarget_ = {};
        levelTarget_ = 0;
        position_ = grid_ = ~0ull;
        const float k = static_cast<float>(std::exp(-dt / 0.12));
        for (float& v : wave_) v *= k;
    }
    // Paused: the bars settle a little faster than music lets them fall (about a second to rest).
    const float a = coeff(dt, attack_), r = coeff(dt, playing ? release_ : release_ * 0.6f);
    const auto fdt = static_cast<float>(dt);
    for (size_t i = 0; i < target_.size(); ++i) {
        float& s = out_.bands[i];
        s = smoothStep(s, target_[i], a, r);
        float& pk = out_.peaks[i];
        if (s >= pk) {
            pk = s;
            hold_[i] = kPeakHold;
            fall_[i] = 0;
        } else if (hold_[i] > 0) {
            hold_[i] -= fdt;
        } else {
            fall_[i] += kPeakGravity * fdt;
            pk = std::max(s, pk - fall_[i] * fdt);
        }
    }
    const float ea = coeff(dt, kEnergyAttack), er = coeff(dt, playing ? kEnergyRelease : kEnergyRelease * 0.5f);
    out_.bass = smoothStep(out_.bass, energyTarget_[0], ea, er);
    out_.mid = smoothStep(out_.mid, energyTarget_[1], ea, er);
    out_.treble = smoothStep(out_.treble, energyTarget_[2], ea, er);
    out_.level = smoothStep(out_.level, levelTarget_, ea, er);
}

bool Analyzer::resting() const {
    constexpr float e = 0.004f;
    for (size_t i = 0; i < out_.bands.size(); ++i)
        if (out_.bands[i] > e || out_.peaks[i] > e || target_[i] > e) return false;
    if (out_.bass > e || out_.mid > e || out_.treble > e || out_.level > e || out_.beat > e) return false;
    for (float v : wave_)
        if (std::fabs(v) > e) return false;
    return true;
}

} // namespace st::audio::viz
