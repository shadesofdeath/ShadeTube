#pragma once
// 10-band graphic equalizer (internal to st_audio, also used by the Ayarlar › SES page for its response curve).
//
// Bands: peaking biquads (RBJ cookbook, Q ~ one octave) at 31 Hz .. 16 kHz, run in double precision on interleaved
// float PCM. A preamp is applied first, together with an automatic headroom cut equal to the largest boost of the
// combined response, so a boosted curve never pushes a full-scale master into the limiter. Flat bands cost nothing.
//
// Threading: an Equalizer instance belongs to one thread (the engine thread). The free functions are pure.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::audio {

inline constexpr int kEqBands = 10;
inline constexpr std::array<float, kEqBands> kEqFrequencies = {31.f, 62.f, 125.f, 250.f, 500.f,
                                                             1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
inline constexpr float kEqMaxGainDb = 12.f;   // per band, +/-

struct EqSettings {
    bool enabled = false;
    float preampDb = 0;                        // user preamp (-12 .. +12), before the automatic headroom
    std::array<float, kEqBands> gainsDb{};     // per band, -12 .. +12

    bool flat() const;                         // every band and the preamp at 0
    bool operator==(const EqSettings&) const = default;
};

struct EqPreset {
    const char* id;                            // stable id stored in Settings::eqPreset ("flat", "bass", ...); the
                                               // Ayarlar page owns the (translated) names
    std::array<float, kEqBands> gainsDb;
};
const std::vector<EqPreset>& eqPresets();
const EqPreset* findEqPreset(std::string_view id);

// Magnitude response of the band filters at `freqHz` in dB (preamp and headroom not included).
double eqResponseDb(const std::array<float, kEqBands>& gainsDb, double freqHz, double sampleRate = 48000);
// The automatic headroom: the largest boost of the combined response over the audible range (>= 0 dB).
double eqHeadroomDb(const std::array<float, kEqBands>& gainsDb, double sampleRate = 48000);

class Equalizer {
public:
    // Recomputes the coefficients; filter state is kept when only the gains change (no click on a slider drag),
    // reset when the format changes.
    void configure(const EqSettings& settings, uint32_t sampleRate, uint32_t channels);
    bool active() const { return active_; }
    void process(float* interleaved, size_t frames);   // in place; no-op when inactive
    void reset();                                      // clears the filter memory

private:
    struct Coeffs {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct State {
        double z1 = 0, z2 = 0;   // transposed direct form II
    };
    std::array<Coeffs, kEqBands> coeffs_{};
    std::array<bool, kEqBands> bandOn_{};
    std::vector<State> state_;   // [band * channels + channel]
    uint32_t rate_ = 0, channels_ = 0;
    float gain_ = 1.f;           // preamp - headroom, linear
    bool active_ = false;
};

} // namespace st::audio
