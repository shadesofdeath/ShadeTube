// Developer test for the audio DSP pieces of Ayarlar > SES (offline, no audio device needed):
//   1. Equalizer (audio/Equalizer): the response at each band centre equals its gain, the automatic headroom equals the
//      largest boost of the combined curve, a +6 dB band measured on a sine (with the headroom cut) comes out level, flat
//      settings are a no-op, extreme settings stay finite on noise, every preset stays within +/-12 dB.
//   2. ReplayGain tags (audio/ReplayGain): the gain text parser, ID3v2.2 / 2.3 / 2.4 TXXX frames (Latin-1, UTF-16 with
//      a BOM, UTF-8, a big cover before it, tag-wide unsynchronisation), FLAC Vorbis comments behind a PICTURE block, an
//      MP4 freeform atom, and a real temporary file; files without a gain give none.
//   3. Limiter (audio/Limiter): quiet audio passes unchanged, only delayed by the look-ahead; a +6 dB sine and a lone
//      spike never exceed the ceiling; the gain comes back after the loud part.
//   4. Output devices: AudioEngine::outputDevices() enumerates without failing (names are printed).
//   audiodsp_test
#include "audio/AudioEngine.h"
#include "audio/Equalizer.h"
#include "audio/Limiter.h"
#include "audio/ReplayGain.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace st::audio;

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

bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// RMS in dB of a stereo buffer after skipping the first `skip` frames (filter settling).
double rmsDb(const std::vector<float>& buf, size_t skip) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = skip * 2; i < buf.size(); ++i, ++n) sum += double(buf[i]) * buf[i];
    return 10 * std::log10(sum / std::max<size_t>(1, n));
}

std::vector<float> sine(double freq, uint32_t rate, size_t frames, float amp = 0.25f) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) v[i * 2] = v[i * 2 + 1] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * freq * i / rate));
    return v;
}

void testEqualizer() {
    std::printf("equalizer\n");
    std::array<float, kEqBands> g{};
    g[5] = 6;   // 1 kHz
    CHECK(approx(eqResponseDb(g, 1000, 48000), 6, 0.05));
    CHECK(approx(eqResponseDb(g, 100, 48000), 0, 0.3));      // far from the band
    CHECK(approx(eqHeadroomDb(g, 48000), 6, 0.1));
    std::array<float, kEqBands> cut{};
    cut[2] = -9;
    CHECK(approx(eqResponseDb(cut, 125, 44100), -9, 0.05));
    CHECK(eqHeadroomDb(cut, 44100) < 0.1);                  // cuts need no headroom
    std::array<float, kEqBands> two{};
    two[3] = two[4] = 6;                                    // neighbours add up between them
    CHECK(eqHeadroomDb(two, 48000) > 6.5);
    for (int b = 0; b < kEqBands; ++b) {                    // each band centre, alone
        std::array<float, kEqBands> one{};
        one[b] = 4;
        if (!approx(eqResponseDb(one, kEqFrequencies[b], 44100), 4, 0.1)) {
            std::printf("  FAIL  band %d centre %.2f dB\n", b, eqResponseDb(one, kEqFrequencies[b], 44100));
            ++g_failures;
        }
    }

    // A +6 dB band on a 1 kHz sine, with the automatic headroom (-6 dB): the level comes out unchanged.
    Equalizer eq;
    EqSettings s;
    s.enabled = true;
    s.gainsDb = g;
    eq.configure(s, 48000, 2);
    CHECK(eq.active());
    auto buf = sine(1000, 48000, 48000);
    const double before = rmsDb(buf, 4800);
    eq.process(buf.data(), 48000);
    CHECK(approx(rmsDb(buf, 4800) - before, 0, 0.2));
    // ... and a sine far away drops by the headroom.
    eq.reset();
    buf = sine(60, 48000, 48000);
    const double lowBefore = rmsDb(buf, 9600);
    eq.process(buf.data(), 48000);
    CHECK(approx(rmsDb(buf, 9600) - lowBefore, -6, 0.4));

    // Flat / disabled: inactive, untouched samples.
    Equalizer flat;
    EqSettings off;
    off.enabled = true;
    flat.configure(off, 44100, 2);
    CHECK(!flat.active());
    auto same = sine(440, 44100, 1000);
    const auto copy = same;
    flat.process(same.data(), 1000);
    CHECK(same == copy);
    EqSettings disabled = s;
    disabled.enabled = false;
    flat.configure(disabled, 44100, 2);
    CHECK(!flat.active());

    // Extremes on noise stay finite; a sample-rate change resets cleanly.
    EqSettings wild;
    wild.enabled = true;
    wild.preampDb = 12;
    for (int b = 0; b < kEqBands; ++b) wild.gainsDb[b] = b % 2 ? 12.f : -12.f;
    Equalizer w;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1, 1);
    bool finite = true;
    for (uint32_t rate : {44100u, 48000u, 32000u}) {
        w.configure(wild, rate, 2);
        std::vector<float> n(rate * 2);
        for (auto& x : n) x = noise(rng);
        w.process(n.data(), rate);
        for (float x : n) finite = finite && std::isfinite(x) && std::fabs(x) < 100;
    }
    CHECK(finite);

    // Presets: unique ids, within range, "flat" is flat.
    bool ranged = true;
    for (const auto& p : eqPresets())
        for (float v : p.gainsDb) ranged = ranged && v >= -kEqMaxGainDb && v <= kEqMaxGainDb;
    CHECK(ranged);
    CHECK(findEqPreset("flat") && findEqPreset("rock") && !findEqPreset("nope"));
    EqSettings flatPreset;
    flatPreset.gainsDb = findEqPreset("flat")->gainsDb;
    CHECK(flatPreset.flat());
}

// ---- tag builders -----------------------------------------------------------------------------------------------------
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
void put(Bytes& b, const std::string& s) { put(b, s.data(), s.size()); }
void be32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void be24(Bytes& b, uint32_t v) { for (int i = 2; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void syncsafe(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>((v >> (7 * i)) & 0x7F)); }

Bytes txxxBody(uint8_t enc, const std::string& desc, const std::string& value) {
    Bytes b{enc};
    auto text = [&](const std::string& s) {
        if (enc == 1) {   // UTF-16 LE with BOM
            b.push_back(0xFF);
            b.push_back(0xFE);
            for (char c : s) {
                b.push_back(static_cast<uint8_t>(c));
                b.push_back(0);
            }
        } else {
            put(b, s);
        }
    };
    text(desc);
    if (enc == 1) {
        b.push_back(0);
        b.push_back(0);
    } else {
        b.push_back(0);
    }
    text(value);
    return b;
}

Bytes id3(int major, const std::vector<std::pair<std::string, Bytes>>& frames, uint8_t flags = 0) {
    Bytes body;
    for (const auto& [id, data] : frames) {
        put(body, id);
        if (major == 2) be24(body, static_cast<uint32_t>(data.size()));
        else if (major == 4) syncsafe(body, static_cast<uint32_t>(data.size()));
        else be32(body, static_cast<uint32_t>(data.size()));
        if (major != 2) {
            body.push_back(0);
            body.push_back(0);
        }
        put(body, data.data(), data.size());
    }
    body.resize(body.size() + 64, 0);   // padding
    Bytes tag{'I', 'D', '3', static_cast<uint8_t>(major), 0, flags};
    syncsafe(tag, static_cast<uint32_t>(body.size()));
    put(tag, body.data(), body.size());
    tag.resize(tag.size() + 400, 0xAB);   // "audio"
    return tag;
}

Bytes flac(const std::vector<std::string>& comments, bool pictureFirst) {
    Bytes f{'f', 'L', 'a', 'C'};
    auto block = [&](uint8_t type, const Bytes& data, bool last) {
        f.push_back(static_cast<uint8_t>((last ? 0x80 : 0) | type));
        be24(f, static_cast<uint32_t>(data.size()));
        put(f, data.data(), data.size());
    };
    block(0, Bytes(34, 0x11), false);   // STREAMINFO
    if (pictureFirst) block(6, Bytes(5000, 0x22), false);
    Bytes vc;
    le32(vc, 9);
    put(vc, "reference");
    le32(vc, static_cast<uint32_t>(comments.size()));
    for (const auto& c : comments) {
        le32(vc, static_cast<uint32_t>(c.size()));
        put(vc, c);
    }
    block(4, vc, true);
    f.resize(f.size() + 200, 0xF0);
    return f;
}

Bytes box(const char* type, const Bytes& content) {
    Bytes b;
    be32(b, static_cast<uint32_t>(content.size() + 8));
    put(b, type, 4);
    put(b, content.data(), content.size());
    return b;
}

Bytes mp4(const std::string& name, const std::string& value) {
    Bytes mean{0, 0, 0, 0};
    put(mean, "com.apple.iTunes");
    Bytes nm{0, 0, 0, 0};
    put(nm, name);
    Bytes data;
    be32(data, 1);   // type: UTF-8
    be32(data, 0);   // locale
    put(data, value);
    Bytes freeform = box("mean", mean);
    const Bytes n = box("name", nm), d = box("data", data);
    put(freeform, n.data(), n.size());
    put(freeform, d.data(), d.size());
    Bytes nam;
    Bytes namData;
    be32(namData, 1);
    be32(namData, 0);
    put(namData, "Title");
    nam = box("\xA9nam", box("data", namData));
    Bytes ilstContent = nam;
    const Bytes ff = box("----", freeform);
    put(ilstContent, ff.data(), ff.size());
    Bytes metaContent{0, 0, 0, 0};   // full box
    const Bytes hdlr = box("hdlr", Bytes(25, 0)), ilst = box("ilst", ilstContent);
    put(metaContent, hdlr.data(), hdlr.size());
    put(metaContent, ilst.data(), ilst.size());
    const Bytes meta = box("meta", metaContent);
    const Bytes udta = box("udta", meta);
    const Bytes mvhd = box("mvhd", Bytes(100, 0));
    Bytes moovContent = mvhd;
    put(moovContent, udta.data(), udta.size());
    Bytes file = box("ftyp", Bytes{'M', '4', 'A', ' ', 0, 0, 0, 0});
    const Bytes mdat = box("mdat", Bytes(3000, 0x55));
    put(file, mdat.data(), mdat.size());   // audio first, moov at the end (seeked over)
    const Bytes moov = box("moov", moovContent);
    put(file, moov.data(), moov.size());
    return file;
}

void testReplayGain() {
    std::printf("replaygain\n");
    using namespace replaygain;
    CHECK(parseGain("-6.54 dB") && approx(*parseGain("-6.54 dB"), -6.54, 1e-4));
    CHECK(parseGain("+1.20 dB") && approx(*parseGain("+1.20 dB"), 1.2, 1e-4));
    CHECK(parseGain("-6,5dB") && approx(*parseGain("-6,5dB"), -6.5, 1e-4));
    CHECK(parseGain(" 3 ") && approx(*parseGain(" 3 "), 3, 1e-4));
    CHECK(!parseGain(""));
    CHECK(!parseGain("dB"));
    CHECK(!parseGain("-6.5 LUFS"));
    CHECK(!parseGain("99 dB"));

    const Bytes cover(200000, 0xCC);   // a cover before the gain frame (seeked over)
    Bytes apic{0};
    put(apic, "image/jpeg");
    apic.push_back(0);
    apic.push_back(3);
    apic.push_back(0);
    put(apic, cover.data(), cover.size());
    const Bytes v3 = id3(3, {{"TIT2", Bytes{0, 'X'}}, {"APIC", apic},
                             {"TXXX", txxxBody(0, "replaygain_album_gain", "-9.00 dB")},
                             {"TXXX", txxxBody(0, "REPLAYGAIN_TRACK_GAIN", "-7.25 dB")}});
    auto g = fromId3(v3.data(), v3.size());
    CHECK(g && approx(*g, -7.25, 1e-4));
    const Bytes v3u16 = id3(3, {{"TXXX", txxxBody(1, "REPLAYGAIN_TRACK_GAIN", "+2.10 dB")}});
    g = fromId3(v3u16.data(), v3u16.size());
    CHECK(g && approx(*g, 2.1, 1e-4));
    const Bytes v4 = id3(4, {{"TXXX", txxxBody(3, "replaygain_track_gain", "-3.00 dB")}});
    g = fromId3(v4.data(), v4.size());
    CHECK(g && approx(*g, -3, 1e-4));
    const Bytes v2 = id3(2, {{"TXX", txxxBody(0, "REPLAYGAIN_TRACK_GAIN", "-1.5 dB")}});
    g = fromId3(v2.data(), v2.size());
    CHECK(g && approx(*g, -1.5, 1e-4));
    const Bytes none = id3(3, {{"TXXX", txxxBody(0, "REPLAYGAIN_ALBUM_GAIN", "-4 dB")}});
    CHECK(!fromId3(none.data(), none.size()));
    // Tag-wide unsynchronisation (v2.3): an FF byte in the frame is followed by an inserted 00.
    {
        Bytes body = txxxBody(0, "REPLAYGAIN_TRACK_GAIN", "-5.50 dB");
        Bytes frame;
        put(frame, "TXXX");
        be32(frame, static_cast<uint32_t>(body.size()));
        frame.push_back(0);
        frame.push_back(0);
        put(frame, body.data(), body.size());
        Bytes pre;   // a frame holding FF bytes (unsynchronised on disk)
        put(pre, "PRIV");
        be32(pre, 4);
        pre.push_back(0);
        pre.push_back(0);
        for (int i = 0; i < 4; ++i) pre.push_back(0xFF);
        Bytes raw;
        for (uint8_t b : pre) {
            raw.push_back(b);
            if (b == 0xFF) raw.push_back(0x00);
        }
        put(raw, frame.data(), frame.size());
        Bytes tag{'I', 'D', '3', 3, 0, 0x80};
        syncsafe(tag, static_cast<uint32_t>(raw.size()));
        put(tag, raw.data(), raw.size());
        g = fromId3(tag.data(), tag.size());
        CHECK(g && approx(*g, -5.5, 1e-4));
    }

    const Bytes fl = flac({"TITLE=x", "REPLAYGAIN_TRACK_PEAK=0.98", "REPLAYGAIN_TRACK_GAIN=-8.13 dB"}, true);
    g = fromFlac(fl.data(), fl.size());
    CHECK(g && approx(*g, -8.13, 1e-4));
    const Bytes flNone = flac({"TITLE=x"}, false);
    CHECK(!fromFlac(flNone.data(), flNone.size()));

    const Bytes m4 = mp4("replaygain_track_gain", "-4.44 dB");
    g = fromMp4(m4.data(), m4.size());
    CHECK(g && approx(*g, -4.44, 1e-4));
    const Bytes m4None = mp4("iTunNORM", " 00000A");
    CHECK(!fromMp4(m4None.data(), m4None.size()));

    // Files on disk (the engine's entry point).
    const fs::path dir = fs::temp_directory_path() / (L"shadetube_audiodsp_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto write = [&](const wchar_t* name, const Bytes& b) {
        std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return dir / name;
    };
    g = readTrackGainDb(write(L"a.mp3", v3));
    CHECK(g && approx(*g, -7.25, 1e-4));
    g = readTrackGainDb(write(L"b.flac", fl));
    CHECK(g && approx(*g, -8.13, 1e-4));
    g = readTrackGainDb(write(L"c.m4a", m4));
    CHECK(g && approx(*g, -4.44, 1e-4));
    CHECK(!readTrackGainDb(write(L"d.wav", Bytes(100, 0))));
    CHECK(!readTrackGainDb(dir / L"missing.mp3"));
    fs::remove_all(dir, ec);
}

void testLimiter() {
    std::printf("limiter\n");
    constexpr uint32_t kRate = 48000;
    Limiter lim;
    lim.configure(kRate, 2);
    const uint32_t L = lim.latencyFrames();
    CHECK(L == 240);   // 5 ms
    // Under the ceiling: bit-exact, L frames late.
    auto quiet = sine(440, kRate, 4800, 0.5f);
    const auto original = quiet;
    lim.process(quiet.data(), 4800);
    bool exact = true;
    for (size_t i = L; i < 4800; ++i) exact = exact && quiet[i * 2] == original[(i - L) * 2];
    CHECK(exact);
    CHECK(lim.gainReduction() == 0.f);
    // A sine at 2x full scale: never above the ceiling (+ float slack), processed in odd-sized blocks.
    lim.reset();
    auto loud = sine(1000, kRate, kRate, 2.f);
    float peak = 0;
    for (size_t off = 0; off < loud.size() / 2;) {
        const size_t n = std::min<size_t>(441, loud.size() / 2 - off);
        lim.process(loud.data() + off * 2, n);
        off += n;
    }
    for (float x : loud) peak = std::max(peak, std::fabs(x));
    CHECK(peak <= Limiter::kCeiling + 1e-4f);
    CHECK(peak > Limiter::kCeiling - 0.02f);   // it limits, it doesn't just attenuate
    // A lone spike in quiet audio: capped, and the gain moves smoothly (no step larger than the look-ahead ramp).
    lim.reset();
    auto spiky = sine(200, kRate, 9600, 0.2f);
    spiky[4000 * 2] = spiky[4000 * 2 + 1] = 3.f;
    lim.process(spiky.data(), 9600);
    peak = 0;
    for (float x : spiky) peak = std::max(peak, std::fabs(x));
    CHECK(peak <= Limiter::kCeiling + 1e-4f);
    // After the loud part, silence then quiet audio: the gain has recovered.
    std::vector<float> after(size_t(kRate) * 2, 0.f);
    lim.process(after.data(), kRate);
    CHECK(lim.gainReduction() < 0.01f);
}

void testDevices() {
    std::printf("output devices\n");
    const auto devices = AudioEngine::outputDevices();
    int defaults = 0;
    for (const auto& d : devices) {
        std::printf("        %s %ls\n", d.isDefault ? "*" : " ", d.name.c_str());
        defaults += d.isDefault ? 1 : 0;
        if (d.id.empty() || d.name.empty()) ++g_failures;
    }
    CHECK(defaults <= 1);
    CHECK(devices.empty() || defaults == 1);
}

} // namespace

int main() {
    testEqualizer();
    testReplayGain();
    testLimiter();
    testDevices();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
