#pragma once
// The full-screen visualizer's analysis (app/Visualizer) of the engine's waveform tap (AudioEngine::waveform).
// Standalone: no UI, no device, no allocation per call once configured (visualizer_test). One update per painted frame
// with the latest 2048 mono samples:
//
//  - Bands: `count` log-spaced bands (35 Hz .. 16 kHz). A band takes its strongest FFT bin; where it is narrower than a
//    bin (the low end: 23 Hz per bin at 48 kHz) the magnitude is interpolated at its center, so neighbouring bass bands
//    rise and fall smoothly instead of in steps of identical bars. A +2.5 dB / octave tilt around 1 kHz (-8..+9 dB; music falls off
//    towards the treble), a ceiling that follows the loudest band (fast up, slow down: a quiet podcast and a loud
//    master both fill the range), a 50 dB range and a soft gamma. Attack / release smoothing on the display clock and
//    peaks that hold, then fall with gravity.
//  - Energies: bass (40-140 Hz), mid (250 Hz-2 kHz) and treble (2.5-10 kHz), each with its own adaptive range.
//  - Beats (BeatDetector): onsets in the low spectrum (40-250 Hz) on the audio's own clock (the samples' positions, not
//    frames, so 60 and 240 Hz displays see the same beats): the mean rise of those bins against 35 ms earlier, over an
//    adaptive threshold (mean + 2.5 mean deviations of the last ~1.5 s), at most one per 260 ms, never in silence. A
//    pulse 1 at the onset that decays in ~0.2 s.
//  - Waveform: the window triggered on a rising zero crossing (periodic sounds stand still), low-passed, decimated to
//    the oscilloscope's points and auto-gained (fast attack, slow release).
// Paused: update(..., playing = false) lets everything settle to rest; resting() says when no more frames are needed.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace st::audio::viz {

inline constexpr size_t kWindow = 2048;            // samples per analysis
inline constexpr size_t kHistory = 4096;           // samples a frame hands over (the tap's history)
inline constexpr size_t kBins = kWindow / 2 + 1;   // power spectrum bins 0..N/2
inline constexpr float kLowHz = 35.f, kHighHz = 16000.f;

// Power spectrum of a Hann-windowed window, normalised so that a full-scale sine centred on a bin gives 1 (0 dB).
// `re` / `im`: kWindow floats of scratch each.
void powerSpectrum(const float* samples, float* power, float* re, float* im);
float toDb(float power);   // 10 log10, floored at -120 dB

struct Band {
    float lo = 0, hi = 0, center = 0;   // Hz
    int k0 = 0, k1 = -1;                // bins whose centres fall inside [lo, hi)
    float bin = 0;                      // fractional bin of `center`
    bool narrow = false;                // under a bin wide: interpolated at `center` (the low end)
    bool interpolate() const { return narrow || k1 < k0; }
};
// `count` bands spaced evenly in log frequency from fLo to fHi (clamped below Nyquist) for a kWindow FFT at `rate`.
std::vector<Band> makeBands(int count, float fLo, float fHi, uint32_t rate);
// A band's power from the bin powers (powerSpectrum).
float bandPower(const Band& b, const float* power);
// Summed power of the bins from fLo to fHi (energies).
float rangePower(const float* power, float fLo, float fHi, uint32_t rate);

// Linear interpolation over evenly spaced values at x in 0..1 (a view with fewer or more bars than bands).
float sampleAt(const std::vector<float>& v, float x);

class BeatDetector {
public:
    static constexpr int kMaxBins = 24;
    // One onset-function sample at audio time `t` (seconds; only differences matter): the levels in dB of the low bins
    // (`binsDb`, `count` <= kMaxBins: the spectrum from 40 to 250 Hz) and the bass level that gates silence. The onset
    // function is the mean over those bins of how much each rose against kLag earlier (a kick lifts them all at once;
    // the random ups and downs of noise average out). Returns the onset's strength (0.35..1) when this sample starts a
    // beat, else 0.
    float push(double t, const float* binsDb, int count, float gateDb);
    void reset();
    int beats() const { return beats_; }
    double lastBeat() const { return lastBeat_; }
    float flux() const { return flux_; }
    float threshold() const { return mean_ + kK * dev_ + kFloorDb; }

    static constexpr double kLag = 0.035;      // the rise is measured against the spectrum this much earlier
    static constexpr double kMinGap = 0.26;    // at most one beat per 260 ms (230 BPM)
    static constexpr double kStatsTau = 1.5;   // adaptive threshold's memory
    static constexpr float kK = 2.5f, kFloorDb = 2.0f;
    static constexpr float kBinFloorDb = -75.f;   // quieter bins count as this (no flux from the noise floor)
    static constexpr float kSilenceDb = -58.f;    // no beats in (near) silence

private:
    struct Sample {
        double t = 0;
        std::array<float, kMaxBins> db{};
    };
    std::array<Sample, 48> ring_{};
    size_t head_ = 0, size_ = 0;
    int bins_ = 0;
    float mean_ = 3.f, dev_ = 1.5f, flux_ = 0, prevRaw_ = 0;
    double lastT_ = -1e9, lastBeat_ = -1e9;
    bool armed_ = true;
    int beats_ = 0;
};

struct Levels {
    std::vector<float> bands;    // 0..1, smoothed
    std::vector<float> peaks;    // 0..1, held and falling
    float bass = 0, mid = 0, treble = 0;   // 0..1, smoothed
    float level = 0;             // overall loudness 0..1 (RMS of the window, -60..-6 dBFS)
    float beat = 0;              // 1 at an onset, decaying
    int beats = 0;               // onsets so far
};

class Analyzer {
public:
    // Resets everything. `waveformPoints`: the oscilloscope's resolution (0 = none).
    void configure(int bandCount, int waveformPoints);
    // Smoothing time constants of the bands (seconds); reduce motion asks for slower ones.
    void setResponse(float attack, float release) {
        attack_ = attack;
        release_ = release;
    }
    // A painted frame. `samples`: the latest `count` samples (oldest first, at least kWindow; kHistory lets the beat
    // detector see every step of audio between frames up to ~40 ms apart); `position`: how many were pushed in all up
    // to the newest (AudioEngine::WaveInfo); `dt`: display seconds since the previous frame. `playing` false (paused,
    // stopped, loading): no new analysis, everything decays to rest.
    void update(const float* samples, size_t count, uint64_t position, uint32_t rate, double dt, bool playing);
    const Levels& levels() const { return out_; }
    const std::vector<float>& waveform() const { return wave_; }   // -1..1
    bool resting() const;
    // Analyses done (new audio) since configure (SHADETUBE_VIZ_LOG) and the beat detector (tests).
    int analyses() const { return analyses_; }
    const BeatDetector& beats() const { return beat_; }

private:
    void analyse(const float* samples, uint64_t position, uint32_t rate);
    void detectBeats(const float* samples, size_t count, uint64_t position, uint32_t rate);
    void buildWaveform(const float* samples, uint32_t rate, double hop);

    std::vector<Band> bands_;
    uint32_t rate_ = 0;
    int count_ = 0;
    std::vector<float> power_ = std::vector<float>(kBins), beatPower_ = std::vector<float>(kBins);
    std::vector<float> re_ = std::vector<float>(kWindow), im_ = std::vector<float>(kWindow);
    std::vector<float> target_;          // the latest analysis' band levels (0..1), the smoothing's goal
    std::vector<float> hold_, fall_;     // peaks: seconds of hold left, falling speed
    std::array<float, 3> energyTarget_{};
    std::array<float, 3> energyCeil_{-20.f, -30.f, -40.f};
    float levelTarget_ = 0;
    float ceilDb_ = -20.f;               // the bands' adaptive ceiling
    float attack_ = 0.035f, release_ = 0.24f;
    uint64_t position_ = ~0ull;          // of the latest analysis
    uint64_t grid_ = ~0ull;              // the beat detector's latest step
    BeatDetector beat_;
    std::vector<float> wave_, lowpass_;
    float waveEnv_ = 0.05f;
    int analyses_ = 0;
    Levels out_;
};

} // namespace st::audio::viz
