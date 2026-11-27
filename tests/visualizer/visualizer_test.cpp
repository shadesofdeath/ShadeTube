// Developer test for the full-screen visualizer's analysis (audio/VisualAnalysis), offline and deterministic:
//   bands      edges contiguous and increasing in 35 Hz .. 16 kHz, the low ones interpolated (narrower than a bin), a sine
//              at each band's centre lands in that band or its neighbour, a -6 dBFS sine at 1 kHz reads -6 dB
//   sample     sampleAt() interpolates and clamps
//   sweep      a 60 Hz -> 12 kHz sweep: the loudest band only moves up, from the bottom to the top
//   beats      synthetic music (drums, bass, chords, an arpeggio) at 90 / 120 / 140 BPM, fed as the engine pushes it
//              (10 ms blocks) and read at 60, 144 and 240 frames per second: every kick found once, within 0..90 ms,
//              nothing else; the same beats at every frame rate; also at 44.1 kHz and with 20 ms blocks (exclusive mode)
//   steady     a held chord, a sine: no beats after the first second; white noise: at most one stray beat in 7 s;
//              silence: no beats, no bars
//   rest       paused: everything settles (resting()) within 1.5 s, the peaks hold and then fall
//   wave       a 110 Hz tone stands still in the oscilloscope (trigger), auto-gained to ~0.9; paused it flattens
//   timing     one analysis (FFT, 96 bands, energies, beats, 320 waveform points): prints it, fails only on a gross
//              regression
//   visualizer_test                      the checks above
//   visualizer_test wav <file.wav> [s]   writes the synthetic song (default 90 s, 120 BPM, 48 kHz stereo 16 bit)
#include "audio/VisualAnalysis.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using namespace st::audio::viz;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

#define EXPECT(cond, ...)                                                                 \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            if (++g_failures < 30) {                                                      \
                std::printf("  FAIL  %s  (line %d): ", #cond, __LINE__);                  \
                std::printf(__VA_ARGS__);                                                 \
                std::printf("\n");                                                        \
            }                                                                             \
        }                                                                                 \
    } while (0)

constexpr double kPi = std::numbers::pi;

// ---- a small deterministic synthesizer: drums + bass + chords + an arpeggio --------------------------------------------

struct Rng {
    uint32_t s = 0x9E3779B9u;
    float next() {   // -1..1
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.f - 1.f;
    }
};

struct Song {
    int rate = 48000;
    std::vector<float> l, r;          // stereo
    std::vector<double> kicks;        // seconds
    std::vector<float> mono() const {
        std::vector<float> m(l.size());
        for (size_t i = 0; i < l.size(); ++i) m[i] = 0.5f * (l[i] + r[i]);
        return m;
    }
};

// Sections (bars of four beats): intro (pad + hats), groove, breakdown (pad + bass), groove. `full` = groove only.
Song makeSong(int rate, double bpm, double seconds, bool full) {
    Song s;
    s.rate = rate;
    const size_t n = static_cast<size_t>(seconds * rate);
    s.l.assign(n, 0.f);
    s.r.assign(n, 0.f);
    const double beat = 60.0 / bpm, bar = beat * 4;
    const int bars = static_cast<int>(std::ceil(seconds / bar));
    Rng rng;
    auto section = [&](int b) {   // 0 intro, 1 groove, 2 breakdown
        if (full) return 1;
        const int k = b % 32;
        return k < 4 ? 0 : k < 20 ? 1 : k < 24 ? 2 : 1;
    };
    // Am - F - C - G: bass roots and the pad's voicings (Hz).
    const double roots[4] = {55.0, 43.65, 65.41, 49.0};
    const double chords[4][3] = {{220.0, 261.63, 329.63}, {174.61, 220.0, 261.63}, {261.63, 329.63, 392.0}, {196.0, 246.94, 293.66}};
    auto add = [&](double t0, double len, float panL, float panR, auto&& voice) {
        const size_t a = static_cast<size_t>(std::max(0.0, t0) * rate);
        const size_t b = std::min(n, static_cast<size_t>((t0 + len) * rate));
        for (size_t i = a; i < b; ++i) {
            const double t = static_cast<double>(i) / rate - t0;
            const float v = voice(t);
            s.l[i] += v * panL;
            s.r[i] += v * panR;
        }
    };
    std::vector<double> kickTimes;
    for (int b = 0; b < bars; ++b) {
        const double t0 = b * bar;
        const int sec = section(b), ch = b % 4;
        const bool drums = sec == 1;
        // Kick on every beat, snare on 2 and 4, closed hats on the eighths (also in the intro), an open hat before 1.
        for (int q = 0; q < 4; ++q) {
            const double tb = t0 + q * beat;
            if (drums) {
                kickTimes.push_back(tb);
                double phase = 0;
                double last = 0;
                add(tb, 0.42, 0.9f, 0.9f, [&](double t) {
                    const double f = 48.0 + 110.0 * std::exp(-t / 0.03);
                    phase += 2 * kPi * f * (t - last);
                    last = t;
                    const float click = rng.next() * static_cast<float>(std::exp(-t / 0.002)) * 0.25f;
                    return static_cast<float>(std::sin(phase) * std::exp(-t / 0.16) * 0.85) + click;
                });
                if (q % 2 == 1) {
                    float prev = 0;
                    add(tb, 0.3, 0.75f, 0.85f, [&](double t) {
                        const float w = rng.next();
                        const float hp = w - prev;   // brighter noise
                        prev = w;
                        return static_cast<float>(std::sin(2 * kPi * 185 * t) * std::exp(-t / 0.06) * 0.22 +
                                                  hp * std::exp(-t / 0.09) * 0.2);
                    });
                }
            }
            if (sec != 2) {
                for (int e = 0; e < 2; ++e) {
                    float prev = 0, prevHp = 0;
                    const bool open = q == 3 && e == 1;
                    add(tb + e * beat * 0.5, open ? 0.35 : 0.12, 0.55f, 0.75f, [&, open](double t) {
                        // Noise high-passed, then its very top taken off (a cymbal's band, not a hiss up to Nyquist).
                        const float w = rng.next();
                        const float hp = w - prev;
                        prev = w;
                        const float band = 0.5f * (hp + prevHp);
                        prevHp = hp;
                        return band * static_cast<float>(std::exp(-t / (open ? 0.12 : 0.025)) * (open ? 0.06 : 0.07));
                    });
                }
            }
        }
        // Bass: the root held over two beats, attacked with the kick on 1 and 3 (a one-pole low pass over a saw).
        if (sec != 0) {
            for (int h = 0; h < 2; ++h) {
                const double f = roots[ch];
                float lp = 0;
                const double len = beat * 2;
                add(t0 + h * beat * 2, len, 0.8f, 0.8f, [&, f, len](double t) {
                    const double ph = f * t;
                    const float saw = static_cast<float>(2.0 * (ph - std::floor(ph + 0.5)));
                    lp += (saw - lp) * 0.04f;
                    const double env = std::min(1.0, t / 0.004) * (0.7 + 0.3 * std::exp(-t / 0.15)) *
                                       std::min(1.0, (len - t) / 0.03);
                    return static_cast<float>(lp * env * 0.55);
                });
            }
        }
        // Pad: three detuned triangles per chord note, a slow attack.
        for (int v = 0; v < 3; ++v) {
            const double f = chords[ch][v];
            add(t0, bar, v == 0 ? 0.7f : 0.45f, v == 2 ? 0.7f : 0.45f, [f, bar](double t) {
                auto tri = [](double ph) { return 4.0 * std::fabs(ph - std::floor(ph + 0.5)) - 1.0; };
                const double env = std::min(1.0, t / 0.3) * std::min(1.0, (bar - t) / 0.08);
                return static_cast<float>((tri(f * t) + tri(f * 1.003 * t) * 0.6) * env * 0.045);
            });
        }
        // Arpeggio two octaves up on the eighths (the groove only).
        if (drums) {
            for (int e = 0; e < 8; ++e) {
                const double f = chords[ch][e % 3] * 4.0;
                add(t0 + e * beat * 0.5, 0.4, e % 2 ? 0.35f : 0.6f, e % 2 ? 0.6f : 0.35f, [f](double t) {
                    return static_cast<float>((std::sin(2 * kPi * f * t) + 0.3 * std::sin(4 * kPi * f * t)) * std::exp(-t / 0.12) * 0.06);
                });
            }
        }
    }
    // Side-chain the tonal parts a little under every kick is implied by the mix; soft-clip and normalise.
    float peak = 0;
    for (size_t i = 0; i < n; ++i) {
        s.l[i] = std::tanh(s.l[i] * 1.2f) / 1.2f;
        s.r[i] = std::tanh(s.r[i] * 1.2f) / 1.2f;
        peak = std::max({peak, std::fabs(s.l[i]), std::fabs(s.r[i])});
    }
    const float g = peak > 0 ? 0.89f / peak : 1.f;
    for (size_t i = 0; i < n; ++i) {
        s.l[i] *= g;
        s.r[i] *= g;
    }
    for (double k : kickTimes)
        if (k < seconds) s.kicks.push_back(k);
    return s;
}

bool writeWav(const std::wstring& path, const Song& s) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t frames = static_cast<uint32_t>(s.l.size()), bytes = frames * 4;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<uint32_t>(s.rate));
    u32(static_cast<uint32_t>(s.rate) * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        pcm[i * 2] = static_cast<int16_t>(std::lround(std::clamp(s.l[i], -1.f, 1.f) * 32767.f));
        pcm[i * 2 + 1] = static_cast<int16_t>(std::lround(std::clamp(s.r[i], -1.f, 1.f) * 32767.f));
    }
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    return static_cast<bool>(f);
}

// ---- feeding the analyzer the way the app does -------------------------------------------------------------------------

struct Run {
    std::vector<double> onsets;       // display time of each beat
    std::vector<std::vector<float>> bandsAt;   // optional snapshots
    double analysisMs = 0;
    int analyses = 0;
};

// The engine pushes `block` samples at a time (10 ms shared, 20 ms exclusive) into the 2048-sample history; the view
// reads the newest window every display frame (`fps`) and updates the analyzer with the frame time.
Run feed(Analyzer& a, const std::vector<float>& mono, int rate, double fps, int block, double from = 0, double to = -1,
         bool playing = true) {
    Run run;
    const double end = to < 0 ? static_cast<double>(mono.size()) / rate : to;
    std::vector<float> window(kHistory);
    int lastBeats = a.levels().beats;
    for (double t = from; t < end; t += 1.0 / fps) {
        const uint64_t pushed = static_cast<uint64_t>(t * rate) / block * block;
        for (size_t i = 0; i < kHistory; ++i) {
            const int64_t k = static_cast<int64_t>(pushed) - static_cast<int64_t>(kHistory) + static_cast<int64_t>(i);
            window[i] = k >= 0 && k < static_cast<int64_t>(mono.size()) ? mono[static_cast<size_t>(k)] : 0.f;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const int before = a.analyses();
        a.update(window.data(), kHistory, pushed, static_cast<uint32_t>(rate), 1.0 / fps, playing);
        if (a.analyses() != before) {
            run.analysisMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++run.analyses;
        }
        if (a.levels().beats != lastBeats) {
            lastBeats = a.levels().beats;
            run.onsets.push_back(t);
        }
        static const bool trace = GetEnvironmentVariableW(L"VIZ_TRACE", nullptr, 0) > 0;
        if (trace && a.analyses() != before)
            std::printf("    t %.3f flux %5.2f thr %5.2f beats %d\n", t, a.beats().flux(), a.beats().threshold(), a.levels().beats);
    }
    return run;
}

std::vector<float> tone(int rate, double seconds, double hz, float amp) {
    std::vector<float> v(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
    return v;
}

// ---- checks ------------------------------------------------------------------------------------------------------------

void testBands() {
    std::printf("bands\n");
    for (uint32_t rate : {44100u, 48000u, 96000u}) {
        const auto bands = makeBands(96, kLowHz, kHighHz, rate);
        bool contiguous = true, increasing = true, inRange = true;
        int interpolated = 0;
        for (size_t i = 0; i < bands.size(); ++i) {
            const auto& b = bands[i];
            if (!(b.hi > b.lo) || !(b.center > b.lo && b.center < b.hi)) increasing = false;
            if (i > 0 && std::fabs(bands[i - 1].hi - b.lo) > 1e-2f) contiguous = false;
            if (b.lo < kLowHz - 0.01f || b.hi > static_cast<float>(rate) * 0.5f) inRange = false;
            if (b.interpolate()) ++interpolated;
        }
        EXPECT(bands.size() == 96, "rate %u: %zu bands", rate, bands.size());
        EXPECT(contiguous && increasing && inRange, "rate %u: contiguous %d increasing %d in range %d", rate, contiguous, increasing, inRange);
        // The low end is never below one bin (96 kHz: 47 Hz).
        const float lo = std::max(kLowHz, static_cast<float>(rate) / kWindow);
        EXPECT(std::fabs(bands.front().lo - lo) < 0.01f && std::fabs(bands.back().hi - kHighHz) < 1.f, "rate %u: %.2f..%.1f Hz", rate,
               bands.front().lo, bands.back().hi);
        EXPECT(bands.front().interpolate() && !bands.back().interpolate(), "rate %u: the first band is interpolated, the last is not", rate);
        // Interpolated bands only at the low end, never between two binned ones.
        bool prefix = true;
        for (size_t i = 1; i < bands.size(); ++i)
            if (bands[i].interpolate() && !bands[i - 1].interpolate()) prefix = false;
        EXPECT(prefix, "rate %u: interpolated bands form the low end", rate);
        std::printf("  rate %u: %d of 96 bands interpolated (below %.0f Hz)\n", rate, interpolated,
                    interpolated > 0 ? bands[static_cast<size_t>(interpolated - 1)].hi : 0.f);
    }
    CHECK(true);
    // A sine at a band's centre: that band (or a neighbour, the window's main lobe spans four bins) is the loudest.
    const uint32_t rate = 48000;
    const auto bands = makeBands(96, kLowHz, kHighHz, rate);
    std::vector<float> power(kBins), re(kWindow), im(kWindow);
    int misses = 0;
    for (size_t i = 0; i < bands.size(); i += 3) {
        const auto s = tone(rate, 0.1, bands[i].center, 0.5f);
        powerSpectrum(s.data(), power.data(), re.data(), im.data());
        size_t best = 0;
        float bp = -1;
        for (size_t j = 0; j < bands.size(); ++j)
            if (const float p = bandPower(bands[j], power.data()); p > bp) {
                bp = p;
                best = j;
            }
        // Low bands are narrower than the window's main lobe: allow the neighbours within that lobe (two bins).
        const float binHz = static_cast<float>(rate) / kWindow;
        const bool nearby = best == i || std::fabs(bands[best].center - bands[i].center) <= 2.f * binHz;
        if (!nearby) {
            ++misses;
            std::printf("  band %zu (%.0f Hz): loudest band %zu (%.0f Hz)\n", i, bands[i].center, best, bands[best].center);
        }
    }
    CHECK(misses == 0);
    // Level: a -6 dBFS sine at 1 kHz reads about -6 dB in its band (scalloping of the Hann window: < 1.5 dB).
    {
        const auto s = tone(rate, 0.1, 1000.0, 0.5f);
        powerSpectrum(s.data(), power.data(), re.data(), im.data());
        float best = -200;
        for (const auto& b : bands) best = std::max(best, toDb(bandPower(b, power.data())));
        std::printf("  1 kHz at -6 dBFS: %.2f dB\n", best);
        CHECK(best > -7.6f && best < -5.5f);
        float sum = toDb(rangePower(power.data(), 900, 1100, rate));
        std::printf("  its range power 900-1100 Hz: %.2f dB\n", sum);
        CHECK(sum > -6.5f && sum < -3.f);   // the main lobe's bins summed
    }
    CHECK(toDb(0) == -120.f && std::fabs(toDb(1) - 0.f) < 1e-6f && std::fabs(toDb(0.01f) + 20.f) < 1e-4f);
}

void testSample() {
    std::printf("sample\n");
    const std::vector<float> v{0.f, 1.f, 0.f};
    CHECK(sampleAt(v, 0) == 0.f && sampleAt(v, 0.5f) == 1.f && sampleAt(v, 1) == 0.f);
    CHECK(std::fabs(sampleAt(v, 0.25f) - 0.5f) < 1e-6f && sampleAt(v, -1) == 0.f && sampleAt(v, 2) == 0.f);
    CHECK(sampleAt({}, 0.5f) == 0.f && sampleAt({0.7f}, 0.3f) == 0.7f);
}

void testSweep() {
    std::printf("sweep\n");
    const int rate = 48000;
    const double secs = 6;
    std::vector<float> s(static_cast<size_t>(secs * rate));
    double phase = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double u = static_cast<double>(i) / s.size();
        const double f = 60.0 * std::pow(200.0, u);   // 60 Hz .. 12 kHz
        phase += 2 * kPi * f / rate;
        s[i] = 0.4f * static_cast<float>(std::sin(phase));
    }
    Analyzer a;
    a.configure(96, 0);
    std::vector<float> window(kHistory);
    int lastBest = -1, backwards = 0, first = -1;
    for (double t = 0.1; t < secs; t += 1.0 / 60) {
        const uint64_t pushed = static_cast<uint64_t>(t * rate) / 480 * 480;
        for (size_t i = 0; i < kHistory; ++i) window[i] = pushed + i >= kHistory ? s[pushed + i - kHistory] : 0.f;
        a.update(window.data(), kHistory, pushed, rate, 1.0 / 60, true);
        const auto& b = a.levels().bands;
        const int best = static_cast<int>(std::max_element(b.begin(), b.end()) - b.begin());
        if (first < 0) first = best;
        if (lastBest >= 0 && best < lastBest - 1) ++backwards;   // smoothing may lag by a band
        lastBest = best;
    }
    std::printf("  loudest band from %d to %d, %d steps back\n", first, lastBest, backwards);
    CHECK(first <= 12);
    CHECK(lastBest >= 84);
    CHECK(backwards == 0);
}

void testBeats() {
    std::printf("beats\n");
    struct Case {
        int rate;
        double bpm;
        int block;
    };
    for (const Case cs : {Case{48000, 120, 480}, Case{48000, 90, 480}, Case{48000, 140, 480}, Case{44100, 120, 441},
                          Case{48000, 120, 960}}) {
        const Song song = makeSong(cs.rate, cs.bpm, 16, true);
        const auto mono = song.mono();
        std::vector<int> counts;
        for (double fps : {60.0, 144.0, 240.0}) {
            Analyzer a;
            a.configure(96, 320);
            const Run run = feed(a, mono, cs.rate, fps, cs.block);
            // Every kick after the first beat (the threshold settles) found once within 0..90 ms, nothing else.
            int found = 0, missed = 0, extra = 0;
            double worst = 0;
            std::vector<bool> used(run.onsets.size(), false);
            for (double k : song.kicks) {
                if (k < 0.6) continue;
                bool hit = false;
                for (size_t i = 0; i < run.onsets.size(); ++i) {
                    const double d = run.onsets[i] - k;
                    if (!used[i] && d >= -0.005 && d <= 0.09) {
                        used[i] = hit = true;
                        worst = std::max(worst, d);
                        break;
                    }
                }
                hit ? ++found : ++missed;
            }
            for (size_t i = 0; i < run.onsets.size(); ++i)
                if (!used[i] && run.onsets[i] >= 0.6) {
                    ++extra;
                    double nearest = 1e9;
                    for (double k : song.kicks) nearest = std::fabs(run.onsets[i] - k) < std::fabs(nearest) ? run.onsets[i] - k : nearest;
                    if (extra <= 3) std::printf("    extra beat at %.3f s (%+.0f ms from a kick)\n", run.onsets[i], nearest * 1000);
                }
            counts.push_back(static_cast<int>(run.onsets.size()));
            std::printf("  %d Hz, %.0f BPM, %d-sample blocks, %3.0f fps: %d kicks found, %d missed, %d extra, latest %.0f ms\n", cs.rate,
                        cs.bpm, cs.block, fps, found, missed, extra, worst * 1000);
            EXPECT(missed == 0 && extra == 0, "%.0f BPM at %.0f fps: %d missed, %d extra", cs.bpm, fps, missed, extra);
        }
        EXPECT(std::abs(counts[0] - counts[1]) <= 1 && std::abs(counts[0] - counts[2]) <= 1, "beats per frame rate %d %d %d", counts[0],
               counts[1], counts[2]);
    }
    // The beat pulse: 1-ish at an onset, under 0.1 half a second later.
    {
        const Song song = makeSong(48000, 120, 4, true);
        const auto mono = song.mono();
        Analyzer a;
        a.configure(96, 0);
        float maxBeat = 0;
        std::vector<float> window(kHistory);
        float atHalf = 1;
        for (double t = 0; t < 4; t += 1.0 / 120) {
            const uint64_t pushed = static_cast<uint64_t>(t * 48000) / 480 * 480;
            for (size_t i = 0; i < kHistory; ++i) {
                const int64_t k = static_cast<int64_t>(pushed) - static_cast<int64_t>(kHistory) + static_cast<int64_t>(i);
                window[i] = k >= 0 ? mono[static_cast<size_t>(k)] : 0.f;
            }
            a.update(window.data(), kHistory, pushed, 48000, 1.0 / 120, true);
            maxBeat = std::max(maxBeat, a.levels().beat);
            if (std::fabs(t - (3.0 + 0.45)) < 0.004) atHalf = a.levels().beat;   // ~0.45 s after the kick at 3.0 s
        }
        std::printf("  beat pulse: max %.2f, %.3f about 0.45 s after a kick\n", maxBeat, atHalf);
        CHECK(maxBeat >= 0.5f && maxBeat <= 1.f);
        CHECK(atHalf < 0.12f);
    }
    // The arranged song (intro without a kick, breakdown): no beats in the intro.
    {
        const Song song = makeSong(48000, 120, 16, false);   // 8 s intro of pad + hats, then the groove
        Analyzer a;
        a.configure(96, 0);
        const Run run = feed(a, song.mono(), 48000, 60, 480);
        int intro = 0;
        for (double t : run.onsets)
            if (t < 7.9) ++intro;
        std::printf("  arranged song: %zu beats, %d in the 8 s intro (hats and pad only)\n", run.onsets.size(), intro);
        CHECK(intro == 0);
        CHECK(run.onsets.size() >= 14);
    }
}

void testSteady() {
    std::printf("steady\n");
    const int rate = 48000;
    // `allowed`: stray beats tolerated after the first second (noise has a random onset now and then).
    auto noBeatsAfter = [&](const std::vector<float>& s, const char* what, int allowed = 0) {
        Analyzer a;
        a.configure(96, 0);
        const Run run = feed(a, s, rate, 60, 480);
        int late = 0;
        for (double t : run.onsets)
            if (t > 1.0) ++late;
        std::printf("  %s: %zu beats, %d after 1 s\n", what, run.onsets.size(), late);
        EXPECT(late <= allowed, "%s: %d beats", what, late);
    };
    noBeatsAfter(tone(rate, 8, 55.0, 0.5f), "55 Hz sine");
    noBeatsAfter(tone(rate, 8, 440.0, 0.5f), "440 Hz sine");
    {
        std::vector<float> chord(static_cast<size_t>(8 * rate));
        for (size_t i = 0; i < chord.size(); ++i) {
            const double t = static_cast<double>(i) / rate;
            chord[i] = static_cast<float>(0.2 * std::sin(2 * kPi * 55 * t) + 0.15 * std::sin(2 * kPi * 220 * t) +
                                          0.12 * std::sin(2 * kPi * 261.63 * t) + 0.1 * std::sin(2 * kPi * 329.63 * t));
        }
        noBeatsAfter(chord, "held chord with bass");
    }
    {
        std::vector<float> noise(static_cast<size_t>(8 * rate));
        Rng rng;
        for (float& v : noise) v = 0.3f * rng.next();
        noBeatsAfter(noise, "white noise", 1);   // at most one stray pulse in 7 s
    }
    {
        const std::vector<float> silence(static_cast<size_t>(4 * rate), 0.f);
        Analyzer a;
        a.configure(96, 64);
        const Run run = feed(a, silence, rate, 60, 480);
        float maxBand = 0;
        for (float v : a.levels().bands) maxBand = std::max(maxBand, v);
        CHECK(run.onsets.empty());
        CHECK(maxBand < 0.001f);
        CHECK(a.resting());
    }
}

void testRest() {
    std::printf("rest\n");
    const Song song = makeSong(48000, 120, 6, true);
    const auto mono = song.mono();
    Analyzer a;
    a.configure(96, 320);
    feed(a, mono, 48000, 144, 480, 0, 4);
    float before = 0, peakBefore = 0;
    for (float v : a.levels().bands) before = std::max(before, v);
    for (float v : a.levels().peaks) peakBefore = std::max(peakBefore, v);
    CHECK(before > 0.5f && !a.resting());
    // Paused: frames keep coming (the view settles), the engine says "not playing".
    double settled = -1;
    std::vector<float> zeros(kHistory, 0.f);
    float peakAt100 = 0, peakAt600 = 0;
    for (int f = 1; f <= 144 * 3; ++f) {
        a.update(zeros.data(), kHistory, 0, 48000, 1.0 / 144, false);
        const double t = f / 144.0;
        if (f == 14) for (float v : a.levels().peaks) peakAt100 = std::max(peakAt100, v);
        if (f == 86) for (float v : a.levels().peaks) peakAt600 = std::max(peakAt600, v);
        if (settled < 0 && a.resting()) settled = t;
    }
    std::printf("  peaks: %.2f playing, %.2f after 0.1 s, %.2f after 0.6 s; resting after %.2f s\n", peakBefore, peakAt100, peakAt600,
                settled);
    CHECK(peakAt100 > peakBefore * 0.85f);   // held
    CHECK(peakAt600 < peakAt100);            // then falling
    CHECK(settled > 0 && settled <= 1.5);
    // Playing again: back to life within a few frames.
    feed(a, mono, 48000, 144, 480, 4, 4.3);
    float after = 0;
    for (float v : a.levels().bands) after = std::max(after, v);
    CHECK(after > 0.4f);
}

void testWave() {
    std::printf("wave\n");
    const int rate = 48000;
    const auto s = tone(rate, 2, 110.0, 0.3f);
    Analyzer a;
    a.configure(96, 320);
    std::vector<float> window(kHistory), prev;
    float maxDiff = 0, maxAmp = 0;
    for (double t = 0.5; t < 2; t += 1.0 / 60) {
        const uint64_t pushed = static_cast<uint64_t>(t * rate) / 480 * 480;
        for (size_t i = 0; i < kHistory; ++i) window[i] = s[pushed + i - kHistory];
        a.update(window.data(), kHistory, pushed, rate, 1.0 / 60, true);
        const auto& w = a.waveform();
        maxAmp = 0;
        for (float v : w) maxAmp = std::max(maxAmp, std::fabs(v));
        if (t > 1.0 && !prev.empty())
            for (size_t i = 0; i < w.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(w[i] - prev[i]));
        prev = w;
    }
    std::printf("  110 Hz: amplitude %.2f, largest change between frames %.3f\n", maxAmp, maxDiff);
    CHECK(maxAmp > 0.8f && maxAmp <= 1.f);
    CHECK(maxDiff < 0.08f);   // triggered: the tone stands still
    CHECK(a.waveform().size() == 320);
    // Paused: it flattens.
    std::vector<float> zeros(kHistory, 0.f);
    for (int f = 0; f < 60; ++f) a.update(zeros.data(), kHistory, 0, rate, 1.0 / 60, false);
    float rest = 0;
    for (float v : a.waveform()) rest = std::max(rest, std::fabs(v));
    std::printf("  paused 1 s: amplitude %.4f\n", rest);
    CHECK(rest < 0.01f);
}

void testTiming() {
    std::printf("timing\n");
    const Song song = makeSong(48000, 120, 12, true);
    Analyzer a;
    a.configure(96, 320);
    const Run run = feed(a, song.mono(), 48000, 240, 480);
    const double per = run.analyses ? run.analysisMs / run.analyses : 0;
    std::printf("  %d analyses, %.3f ms each (FFT, 96 bands, energies, beats, 320 waveform points)\n", run.analyses, per);
#ifdef NDEBUG
    CHECK(per < 0.5);
#else
    CHECK(per < 5.0);
#endif
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && std::wcscmp(argv[1], L"wav") == 0) {
        const double secs = argc >= 4 ? _wtof(argv[3]) : 90.0;
        const Song song = makeSong(48000, 120, secs > 0 ? secs : 90.0, false);
        const bool ok = writeWav(argv[2], song);
        std::printf("%s %.0f s of synthetic music (120 BPM, 48 kHz, drums + bass + chords + arpeggio)\n", ok ? "wrote" : "FAILED to write",
                    song.l.size() / 48000.0);
        return ok ? 0 : 1;
    }
    testBands();
    testSample();
    testSweep();
    testBeats();
    testSteady();
    testRest();
    testWave();
    testTiming();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
