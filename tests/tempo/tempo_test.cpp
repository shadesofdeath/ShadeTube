// Developer test for the tempo estimator (audio/Tempo), offline, on synthetic audio at known tempos:
//   clicks      a click per beat (accented downbeats), 60-200 BPM and odd tempos: exact (within 2 %)
//   loops       drum loops (kick / snare / hi-hats: rock, four-on-the-floor, ballad with eighth hats), 70-180 BPM
//   swing       shuffled eighths (triplet swing) do not move the beat
//   traps       half-time (snare on 3), drum'n'bass at 174, sixteenth hats louder than the beat: the tempo or its
//               octave (reported as such)
//   noise       white noise / silence / a sustained tone: no tempo (or a low confidence)
//   robust      the same loops under a noise floor, at 44.1 kHz instead of 11.025, with a slight drift
//   file        a generated WAV through analyzeFile (decoder, 20 % seek, mono downmix at 11.025 kHz)
//   timing      70 s of audio: envelope + estimate
// Prints a table of every signal (truth, estimate, confidence, exact / octave) and the accuracy over all of them.
//   tempo_test                 every section
//   tempo_test file <path...>  estimates of real files (the decoded part, the time it took)
#include "audio/Tempo.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace st::audio;
namespace fs = std::filesystem;

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

constexpr uint32_t kSr = tempo::kRate;

struct Tally {
    int total = 0, exact = 0, octave = 0;
} g_tally;

bool within(double est, double truth, double tol = 0.02) { return truth > 0 && std::abs(est - truth) <= truth * tol; }
bool octaveOf(double est, double truth) {
    for (double f : {0.5, 2.0, 1.0 / 3, 3.0, 2.0 / 3, 1.5})
        if (within(est, truth * f, 0.03)) return true;
    return false;
}

// Records and prints one signal's result; returns whether it was exact.
bool report(const char* name, double truth, const tempo::Estimate& e) {
    const bool exact = within(e.bpm, truth);
    const bool oct = !exact && octaveOf(e.bpm, truth);
    ++g_tally.total;
    if (exact) ++g_tally.exact;
    else if (oct) ++g_tally.octave;
    std::printf("  %-34s truth %6.1f  est %6.1f  conf %.2f  %s\n", name, truth, e.bpm, e.confidence,
                exact ? "exact" : oct ? "OCTAVE" : "WRONG");
    return exact;
}

std::mt19937 g_rng(12345);
float noise() {
    static std::uniform_real_distribution<float> d(-1.f, 1.f);
    return d(g_rng);
}

// ---- Instruments (added into a buffer at sample `at`) ----------------------------------------------------------------

void addClick(std::vector<float>& b, size_t at, float amp, uint32_t sr) {
    const size_t len = sr / 50;   // 20 ms
    for (size_t i = 0; i < len && at + i < b.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        b[at + i] += amp * static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * t) * std::exp(-t * 200));
    }
}

void addKick(std::vector<float>& b, size_t at, float amp, uint32_t sr) {
    const size_t len = sr * 3 / 10;
    double phase = 0;
    for (size_t i = 0; i < len && at + i < b.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        const double f = 50 + 70 * std::exp(-t * 30);
        phase += 2 * std::numbers::pi * f / sr;
        b[at + i] += amp * static_cast<float>(std::sin(phase) * std::exp(-t * 12));
    }
}

void addSnare(std::vector<float>& b, size_t at, float amp, uint32_t sr) {
    const size_t len = sr / 5;
    for (size_t i = 0; i < len && at + i < b.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        const double body = std::sin(2 * std::numbers::pi * 190 * t) * std::exp(-t * 30);
        b[at + i] += amp * static_cast<float>(0.5 * body + 0.6 * noise() * std::exp(-t * 22));
    }
}

void addHat(std::vector<float>& b, size_t at, float amp, uint32_t sr) {
    const size_t len = sr / 25;
    float last = 0;
    for (size_t i = 0; i < len && at + i < b.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        const float n = noise();
        b[at + i] += amp * static_cast<float>((n - last) * 0.5 * std::exp(-t * 90));   // first difference: bright
        last = n;
    }
}

// A held chord under the drums (pads make the envelope less clean).
void addPad(std::vector<float>& b, float amp, uint32_t sr) {
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        b[i] += amp * static_cast<float>(std::sin(2 * std::numbers::pi * 220 * t) + 0.7 * std::sin(2 * std::numbers::pi * 277.2 * t) +
                                         0.5 * std::sin(2 * std::numbers::pi * 329.6 * t)) / 2.2f;
    }
}

// ---- Patterns ---------------------------------------------------------------------------------------------------------

// One bar = 16 steps of sixteenth notes. Each step: kick / snare / hat level (0 = none).
struct Step {
    float kick = 0, snare = 0, hat = 0;
};
using Bar = std::vector<Step>;   // 16 steps

Bar rockBar() {
    Bar b(16);
    for (int s = 0; s < 16; s += 2) b[s].hat = 0.35f;          // eighth hats
    b[0].kick = b[8].kick = b[10].kick = 1.f;                  // 1, 3, the "and" of 3
    b[4].snare = b[12].snare = 0.9f;                           // 2, 4
    return b;
}
Bar fourBar() {
    Bar b(16);
    for (int s = 0; s < 16; s += 4) b[s].kick = 1.f;
    for (int s = 2; s < 16; s += 4) b[s].hat = 0.5f;           // offbeat hats
    b[4].snare = b[12].snare = 0.7f;
    return b;
}
Bar balladBar() {   // slow: kick 1, snare 3 of a 4/4 bar... as quarter = beat: kick on 1 and 3, snare 2 and 4, soft 8ths
    Bar b(16);
    for (int s = 0; s < 16; s += 2) b[s].hat = 0.25f;
    b[0].kick = b[8].kick = 1.f;
    b[4].snare = b[12].snare = 0.8f;
    return b;
}
Bar halfTimeBar() {   // trap: kick on 1 (and the "a" of 2), snare on 3 only, eighth hats
    Bar b(16);
    for (int s = 0; s < 16; s += 2) b[s].hat = 0.4f;
    b[0].kick = 1.f;
    b[7].kick = 0.8f;
    b[8].snare = 1.f;
    return b;
}
Bar dnbBar() {   // drum'n'bass two-step: kick 1 and the "and" of 3, snare 2 and 4, eighth hats
    Bar b(16);
    for (int s = 0; s < 16; s += 2) b[s].hat = 0.3f;
    b[0].kick = 1.f;
    b[10].kick = 0.9f;
    b[4].snare = b[12].snare = 1.f;
    return b;
}
Bar loudSixteenthsBar() {   // sixteenth hats nearly as loud as the beat
    Bar b(16);
    for (int s = 0; s < 16; ++s) b[s].hat = 0.7f;
    b[0].kick = b[8].kick = 1.f;
    b[4].snare = b[12].snare = 0.9f;
    return b;
}

// `bpm` quarter notes; `swing` 0..1/3: the off-beat eighth moves later (1/6 = triplet swing); `drift` = relative tempo
// change over the whole signal (rubato-ish ramp).
std::vector<float> render(const Bar& bar, double bpm, double seconds, uint32_t sr = kSr, double swing = 0, double drift = 0,
                          float pad = 0.f) {
    std::vector<float> out(static_cast<size_t>(seconds * sr), 0.f);
    double t = 0;   // seconds at the bar start
    while (t < seconds) {
        const double tb = bpm * (1 + drift * t / seconds);
        const double beat = 60 / tb, sixteenth = beat / 4;
        for (int s = 0; s < 16; ++s) {
            double at = t + s * sixteenth;
            if (swing > 0 && s % 4 == 2) at += swing * beat;   // the "and" of each beat moves later
            if (s % 2 == 1) at += swing * beat * 0.5;
            const size_t i = static_cast<size_t>(at * sr);
            if (i >= out.size()) break;
            if (bar[s].kick > 0) addKick(out, i, 0.5f * bar[s].kick, sr);
            if (bar[s].snare > 0) addSnare(out, i, 0.4f * bar[s].snare, sr);
            if (bar[s].hat > 0) addHat(out, i, 0.3f * bar[s].hat, sr);
        }
        t += beat * 4;
    }
    if (pad > 0) addPad(out, pad, sr);
    return out;
}

std::vector<float> clicks(double bpm, double seconds, uint32_t sr = kSr) {
    std::vector<float> out(static_cast<size_t>(seconds * sr), 0.f);
    const double beat = 60 / bpm;
    int n = 0;
    for (double t = 0; t < seconds; t += beat, ++n) addClick(out, static_cast<size_t>(t * sr), n % 4 == 0 ? 0.9f : 0.5f, sr);
    return out;
}

void addNoise(std::vector<float>& b, float amp) {
    for (auto& v : b) v += amp * noise();
}

tempo::Estimate est(const std::vector<float>& x, uint32_t sr = kSr) { return tempo::estimate(x.data(), x.size(), sr); }

// ---- Sections -----------------------------------------------------------------------------------------------------------

void testClicks() {
    std::printf("clicks\n");
    int exact = 0, total = 0;
    std::vector<double> tempos;
    for (int b = 60; b <= 200; b += 10) tempos.push_back(b);
    for (double b : {87.0, 123.0, 174.5, 96.5, 143.0}) tempos.push_back(b);
    for (double bpm : tempos) {
        char name[64];
        std::snprintf(name, sizeof name, "click %.1f", bpm);
        exact += report(name, bpm, est(clicks(bpm, 40))) ? 1 : 0;
        ++total;
    }
    std::printf("  -> %d / %d exact\n", exact, total);
    CHECK(exact == total);
}

void testLoops() {
    std::printf("loops\n");
    int exact = 0, total = 0;
    for (int bpm = 70; bpm <= 180; bpm += 10) {
        char name[64];
        std::snprintf(name, sizeof name, "rock %d", bpm);
        exact += report(name, bpm, est(render(rockBar(), bpm, 40))) ? 1 : 0;
        ++total;
    }
    for (double bpm : {118.0, 124.0, 128.0, 132.0}) {
        char name[64];
        std::snprintf(name, sizeof name, "four-on-the-floor %.0f", bpm);
        exact += report(name, bpm, est(render(fourBar(), bpm, 40, kSr, 0, 0, 0.05f))) ? 1 : 0;
        ++total;
    }
    for (double bpm : {66.0, 72.0, 80.0}) {
        char name[64];
        std::snprintf(name, sizeof name, "ballad %.0f", bpm);
        exact += report(name, bpm, est(render(balladBar(), bpm, 45, kSr, 0, 0, 0.08f))) ? 1 : 0;
        ++total;
    }
    std::printf("  -> %d / %d exact\n", exact, total);
    CHECK(exact >= total - 1);   // at most one octave slip among the plain loops
}

void testSwing() {
    std::printf("swing\n");
    int exact = 0;
    for (double bpm : {96.0, 112.0, 126.0}) {
        char name[64];
        std::snprintf(name, sizeof name, "triplet swing %.0f", bpm);
        exact += report(name, bpm, est(render(rockBar(), bpm, 40, kSr, 1.0 / 6))) ? 1 : 0;
    }
    CHECK(exact == 3);
}

void testTraps() {
    std::printf("traps (the tempo or its octave)\n");
    struct Trap {
        const char* name;
        Bar bar;
        double bpm;
    };
    const Trap traps[] = {{"half-time 140 (snare on 3)", halfTimeBar(), 140},
                          {"half-time 150 (snare on 3)", halfTimeBar(), 150},
                          {"drum'n'bass 174", dnbBar(), 174},
                          {"loud sixteenths 92", loudSixteenthsBar(), 92},
                          {"loud sixteenths 120", loudSixteenthsBar(), 120}};
    for (const auto& t : traps) {
        const auto e = est(render(t.bar, t.bpm, 40));
        report(t.name, t.bpm, e);
        CHECK(within(e.bpm, t.bpm) || octaveOf(e.bpm, t.bpm));
    }
}

void testNoise() {
    std::printf("noise\n");
    std::vector<float> n(kSr * 30, 0.f);
    addNoise(n, 0.3f);
    const auto e1 = est(n);
    std::printf("  white noise: %.1f BPM conf %.2f\n", e1.bpm, e1.confidence);
    CHECK(e1.bpm == 0 || e1.confidence < 0.15);
    const std::vector<float> silence(kSr * 30, 0.f);
    CHECK(est(silence).bpm == 0);
    std::vector<float> tone(kSr * 30);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * static_cast<float>(std::sin(2 * std::numbers::pi * 440 * i / kSr));
    const auto e3 = est(tone);
    std::printf("  sustained tone: %.1f BPM conf %.2f\n", e3.bpm, e3.confidence);
    auto envMean = [](const std::vector<float>& x) {
        double fr = 0;
        const auto env = tempo::onsetEnvelope(x.data(), x.size(), kSr, fr);
        double m = 0;
        for (float v : env) m += v;
        return env.empty() ? 0.0 : m / env.size();
    };
    std::printf("  envelope means: tone %.5f noise %.5f rock %.5f click %.5f ballad %.5f\n", envMean(tone), envMean(n),
                envMean(render(rockBar(), 128, 20)), envMean(clicks(120, 20)), envMean(render(balladBar(), 70, 20, kSr, 0, 0, 0.08f)));
    CHECK(e3.bpm == 0 || e3.confidence < 0.15);
    CHECK(est(std::vector<float>(kSr * 3, 0.1f)).bpm == 0);   // too short
}

void testRobust() {
    std::printf("robust\n");
    {
        auto x = render(rockBar(), 128, 40, kSr, 0, 0, 0.05f);
        addNoise(x, 0.08f);
        const auto e = est(x);
        report("rock 128 + noise floor", 128, e);
        CHECK(within(e.bpm, 128));
    }
    {
        const auto x = render(rockBar(), 104, 40, 44100);
        const auto e = est(x, 44100);
        report("rock 104 at 44.1 kHz", 104, e);
        CHECK(within(e.bpm, 104));
    }
    {
        const auto x = render(fourBar(), 125, 40, kSr, 0, 0.01);
        const auto e = est(x);
        report("four 125, +1 % drift", 125.6, e);
        CHECK(within(e.bpm, 125.6, 0.02));
    }
    {   // precision: a tempo between integers
        const auto x = render(fourBar(), 127.3, 60);
        const auto e = est(x);
        std::printf("  four 127.3: %.1f\n", e.bpm);
        CHECK(std::abs(e.bpm - 127.3) <= 0.6);
    }
}

void writeWav(const fs::path& file, const std::vector<float>& mono, uint32_t sr) {
    std::ofstream f(file, std::ios::binary);
    const uint32_t channels = 2, bytes = static_cast<uint32_t>(mono.size() * channels * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(channels));
    u32(sr);
    u32(sr * channels * 2);
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    f.write("data", 4);
    u32(bytes);
    for (float v : mono) {
        const auto s = static_cast<int16_t>(std::clamp(v, -1.f, 1.f) * 30000);
        u16(static_cast<uint16_t>(s));
        u16(static_cast<uint16_t>(s));
    }
}

fs::path tempDir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    fs::path p = fs::path(buf) / (L"shadetube-tempo-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(p);
    return p;
}

void testFile() {
    std::printf("file\n");
    const fs::path dir = tempDir();
    const fs::path wav = dir / L"loop 128.wav";
    // 2 minutes at 44.1 kHz: an intro of hats only, then the beat (the 20 % seek lands in the beat).
    auto x = render(rockBar(), 128, 120, 44100, 0, 0, 0.05f);
    writeWav(wav, x, 44100);
    const auto r = tempo::analyzeFile(wav.wstring(), "audio/wav");
    std::printf("  ok %d, %.1f s analysed, decode %.0f ms, estimate %.0f ms: %.1f BPM conf %.2f %s\n", r.ok, r.seconds,
                r.decodeMs, r.analyzeMs, r.tempo.bpm, r.tempo.confidence, r.error.c_str());
    CHECK(r.ok);
    CHECK(r.seconds >= 69 && r.seconds <= 71);
    CHECK(within(r.tempo.bpm, 128));
    const auto missing = tempo::analyzeFile((dir / L"missing.mp3").wstring(), "audio/mpeg");
    CHECK(!missing.ok && !missing.error.empty());
    std::atomic<bool> cancel{true};
    CHECK(!tempo::analyzeFile(wav.wstring(), "audio/wav", 70, &cancel).ok);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void testTiming() {
    std::printf("timing\n");
    const auto x = render(rockBar(), 120, 70, kSr, 0, 0, 0.05f);
    const auto t0 = std::chrono::steady_clock::now();
    double fr = 0;
    const auto env = tempo::onsetEnvelope(x.data(), x.size(), kSr, fr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto e = tempo::fromEnvelope(env, fr);
    const auto t2 = std::chrono::steady_clock::now();
    const double envMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double estMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("  70 s: envelope %.1f ms (%zu frames at %.2f/s), estimate %.1f ms -> %.1f BPM\n", envMs, env.size(), fr,
                estMs, e.bpm);
    CHECK(envMs + estMs < 3000);   // a Debug build; Release is ~10x faster
}

int runFiles(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const fs::path p = std::u8string(reinterpret_cast<const char8_t*>(argv[i]));
        std::string mime;
        const auto ext = p.extension().string();
        if (ext == ".m4a" || ext == ".mp4") mime = "audio/mp4";
        else if (ext == ".mp3") mime = "audio/mpeg";
        else if (ext == ".webm") mime = "audio/webm";
        else if (ext == ".flac") mime = "audio/flac";
        else if (ext == ".wav") mime = "audio/wav";
        const auto r = tempo::analyzeFile(p.wstring(), mime);
        std::printf("%-60s %s %6.1f BPM conf %.2f  (%.0f s, decode %.0f ms, estimate %.0f ms) %s\n", p.filename().string().c_str(),
                    r.ok ? "ok " : "ERR", r.tempo.bpm, r.tempo.confidence, r.seconds, r.decodeMs, r.analyzeMs, r.error.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "file") return runFiles(argc, argv);
    testClicks();
    testLoops();
    testSwing();
    testTraps();
    testNoise();
    testRobust();
    testFile();
    testTiming();
    std::printf("\naccuracy over %d signals: %d exact (%.0f %%), %d more within an octave (acc2 %.0f %%)\n", g_tally.total,
                g_tally.exact, 100.0 * g_tally.exact / std::max(1, g_tally.total), g_tally.octave,
                100.0 * (g_tally.exact + g_tally.octave) / std::max(1, g_tally.total));
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall tempo checks passed\n");
    return 0;
}
