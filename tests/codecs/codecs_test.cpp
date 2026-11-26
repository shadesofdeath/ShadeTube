// Developer test for the built-in Opus / Vorbis decoding (offline; fixtures in tests/codecs/fixtures, made by ffmpeg
// from chirps, so every decoded sample can be compared with the formula it came from):
//   1. RatePolicy: fixed (shared mode), the stream's own rate, the nearest accepted one (integer multiples first).
//   2. Resampler: lengths (ceil(N * out / in)), a 1 kHz sine at 48 -> 44.1 and 44.1 -> 48 kHz against the exact sine,
//      a tone above the new Nyquist rejected, an impulse landing at its converted position, chunked = one-shot output.
//   3. Matroska demuxer: track info, CodecDelay / SeekPreRoll / duration, cues at the front and at the end (read at the
//      first seek), frame timestamps and DiscardPadding, seeking, the Vorbis CodecPrivate, Xiph / EBML / fixed lacing on
//      hand-built blocks, an unknown-size cluster, truncated and garbage input.
//   4. Decoder (built-in path): Ogg Opus, Ogg Vorbis, WebM Opus (cues front / end), Matroska Vorbis, 5.1 and mono Opus:
//      exact lengths (pre-skip and end trimming), the time alignment of the decoded chirp (best lag 0), seeks landing on
//      the requested sample, resampling to another rate (length and alignment), the stream tags' ReplayGain.
//   5. XiphTags: comments, multiple artists, track / disc / date, duration, the front cover chosen over the back one.
//   6. Live Ogg: OggDemuxer on the Vorbis and Opus files fed in small chunks (frames, durations, titles, a chained
//      second stream), LiveDecoder (libopus / libvorbis) decoding them with the pre-skip dropped.
//   7. AudioEngine (needs an audio device, else skipped): a local .opus and .ogg play, seek, end, and hand over
//      gaplessly.
//   --exclusive instead (takes the default device for ~15 s): exclusive mode at the file's own rate, a second engine
//      refused (Busy, shared instead), the device released while paused, a 44.1 -> 48 kHz handoff reopening it, a
//      crossfade keeping the rate, the preview voice, switching back to shared mode while playing.
//   codecs_test [--exclusive] [fixtures dir]
#include "audio/AudioEngine.h"
#include "audio/Decoder.h"
#include "audio/LiveDecoder.h"
#include "audio/LiveParsers.h"
#include "audio/Matroska.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/Resampler.h"
#include "audio/XiphCodecs.h"
#include "audio/XiphTags.h"
#include "core/Log.h"

#include <windows.h>
#include <mfapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <thread>
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

fs::path g_dir;

std::vector<uint8_t> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The fixtures' signal: left 0.4 sin(2 pi (220 + 300 t) t), right 0.4 sin(2 pi (330 + 200 t) t).
double chirp(int ch, double t) {
    return ch == 0 ? 0.4 * std::sin(2 * std::numbers::pi * (220 + 300 * t) * t) : 0.4 * std::sin(2 * std::numbers::pi * (330 + 200 * t) * t);
}

// SNR (dB) of interleaved stereo `pcm` whose first frame is track frame `first` at `rate`, against the chirp shifted by
// `lag` frames, over frames [from, to) of the buffer.
double chirpSnr(const std::vector<float>& pcm, uint32_t rate, int64_t first, int lag, size_t from, size_t to) {
    double sig = 0, err = 0;
    to = std::min(to, pcm.size() / 2);
    for (size_t i = from; i < to; ++i) {
        const double t = double(first + int64_t(i) - lag) / rate;
        for (int c = 0; c < 2; ++c) {
            const double ref = chirp(c, t);
            const double d = pcm[i * 2 + c] - ref;
            sig += ref * ref;
            err += d * d;
        }
    }
    return 10 * std::log10(sig / std::max(err, 1e-20));
}

// The lag (frames, -range..range) at which the decoded audio matches the chirp best.
int bestLag(const std::vector<float>& pcm, uint32_t rate, int64_t first, size_t from, size_t to, int range, double* snr = nullptr) {
    int best = 0;
    double bestSnr = -1e9;
    for (int lag = -range; lag <= range; ++lag) {
        const double s = chirpSnr(pcm, rate, first, lag, from, to);
        if (s > bestSnr) {
            bestSnr = s;
            best = lag;
        }
    }
    if (snr) *snr = bestSnr;
    return best;
}

// The lag (frames) at which `pcm` (starting at track frame `first`) matches `ref` (starting at frame 0) best, over the
// first `frames` frames of `pcm`.
int bestLagRef(const std::vector<float>& pcm, int64_t first, const std::vector<float>& ref, size_t frames, int range, double* snr) {
    int best = 0;
    double bestSnr = -1e9;
    frames = std::min(frames, pcm.size() / 2);
    for (int lag = -range; lag <= range; ++lag) {
        double sig = 0, err = 0;
        for (size_t i = 0; i < frames; ++i) {
            const int64_t at = first + int64_t(i) - lag;
            if (at < 0 || at >= int64_t(ref.size() / 2)) continue;
            for (int c = 0; c < 2; ++c) {
                const double r = ref[size_t(at) * 2 + c], d = pcm[i * 2 + c] - r;
                sig += r * r;
                err += d * d;
            }
        }
        const double s = 10 * std::log10(sig / std::max(err, 1e-20));
        if (s > bestSnr) {
            bestSnr = s;
            best = lag;
        }
    }
    if (snr) *snr = bestSnr;
    return best;
}

std::shared_ptr<ProgressiveBuffer> bufferFor(const fs::path& p) {
    auto b = std::make_shared<ProgressiveBuffer>(std::string(), p.wstring(), 0, nullptr);
    b->start();
    b->waitForLength();
    return b;
}

struct Decoded {
    bool ok = false;
    bool builtIn = false;
    uint32_t rate = 0, channels = 0;
    int64_t durationMs = 0;
    int64_t firstFrame = 0;   // track frame of pcm[0]
    std::vector<float> pcm;
    float tagGain = std::numeric_limits<float>::quiet_NaN();
};

// Opens `file` through the Decoder facade (as a Track does), optionally seeks, and decodes to the end, dropping what
// lies before the seek target the way the Track does.
Decoded decode(const fs::path& file, const RatePolicy& rates = {}, int64_t seekMs = -1) {
    Decoded d;
    auto buffer = bufferFor(file);
    Decoder dec;
    bool unsupported = false;
    if (FAILED(dec.open(buffer, "", unsupported, rates))) return d;
    d.builtIn = dec.builtIn();
    d.rate = dec.sampleRate();
    d.channels = dec.channels();
    d.durationMs = dec.durationHns() / 10'000;
    d.tagGain = dec.tagGainDb();
    int64_t trimUntil = 0;
    if (seekMs >= 0) {
        if (FAILED(dec.seek(seekMs * 10'000))) return d;
        trimUntil = seekMs * int64_t(d.rate) / 1000;
    }
    std::vector<float> scratch;
    int64_t next = -1;
    for (int guard = 0; guard < 100000; ++guard) {
        scratch.clear();
        int64_t ts = -1;
        bool changed = false;
        HRESULT hr = S_OK;
        const Decoder::Status s = dec.read(scratch, ts, changed, hr);
        if (s == Decoder::Status::Error) return d;
        const size_t frames = scratch.size() / 2;
        if (frames) {
            int64_t pos = next < 0 ? (ts * int64_t(d.rate) + 5'000'000) / 10'000'000 : next;
            next = pos + int64_t(frames);
            size_t skip = 0;
            if (pos < trimUntil) skip = size_t(std::min<int64_t>(int64_t(frames), trimUntil - pos));
            if (d.pcm.empty()) d.firstFrame = pos + int64_t(skip);
            d.pcm.insert(d.pcm.end(), scratch.begin() + skip * 2, scratch.end());
        }
        if (s == Decoder::Status::EndOfStream) break;
    }
    d.ok = true;
    return d;
}

// ---- 1. RatePolicy --------------------------------------------------------------------------------------------------

void testRatePolicy() {
    std::printf("RatePolicy\n");
    const RatePolicy shared{48000};
    CHECK(shared.pick(44100) == 48000);
    CHECK(RatePolicy().pick(44100) == 44100);
    const RatePolicy exclusive{0, {44100, 48000, 88200, 96000, 192000}};
    CHECK(exclusive.pick(44100) == 44100 && exclusive.pick(96000) == 96000);
    CHECK(exclusive.pick(22050) == 44100);    // an integer multiple
    CHECK(exclusive.pick(32000) == 96000);    // ... rather than the next higher 44.1 kHz
    CHECK(exclusive.pick(176400) == 192000);  // the next higher
    const RatePolicy low{0, {44100, 48000}};
    CHECK(low.pick(96000) == 48000);   // none higher: the highest
}

// ---- 2. Resampler ---------------------------------------------------------------------------------------------------

std::vector<float> sine(double hz, uint32_t rate, size_t frames, double phase0 = 0) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * hz * i / rate + phase0));
    return v;
}

void testResampler() {
    std::printf("Resampler\n");
    for (const auto [in, out] : {std::pair{48000u, 44100u}, std::pair{44100u, 48000u}, std::pair{32000u, 48000u}, std::pair{96000u, 44100u}}) {
        Resampler r;
        r.configure(in, out, 2);
        const size_t n = in;   // 1 s
        const auto src = sine(1000, in, n);
        std::vector<float> dst;
        r.process(src.data(), n, dst);
        r.flush(dst);
        const size_t frames = dst.size() / 2;
        CHECK(int64_t(frames) == r.toOutput(int64_t(n)));
        // Against the exact sine at the output rate, away from the ends (the silent past / future).
        double sig = 0, err = 0;
        for (size_t i = 2000; i + 2000 < frames; ++i) {
            const double ref = 0.5 * std::sin(2 * std::numbers::pi * 1000 * double(i) / out);
            sig += ref * ref;
            err += (dst[2 * i] - ref) * (dst[2 * i] - ref);
        }
        const double snr = 10 * std::log10(sig / err);
        std::printf("        %u -> %u Hz: %zu frames, 1 kHz sine SNR %.1f dB\n", in, out, frames, snr);
        CHECK(snr > 70);
    }
    {   // 23 kHz at 48 kHz has no place at 44.1 kHz: filtered out.
        Resampler r;
        r.configure(48000, 44100, 2);
        const auto src = sine(23000, 48000, 48000);
        std::vector<float> dst;
        r.process(src.data(), 48000, dst);
        double e = 0;
        for (size_t i = 4000; i < dst.size() - 8000; ++i) e += double(dst[i]) * dst[i];
        const double db = 10 * std::log10(e / double(dst.size() - 12000) / 0.125);
        std::printf("        23 kHz tone after 48 -> 44.1 kHz: %.1f dB\n", db);
        CHECK(db < -60);
    }
    {   // An impulse at input frame 1000 peaks at output frame 1000 * out / in.
        Resampler r;
        r.configure(44100, 48000, 2);
        std::vector<float> src(4000 * 2, 0.f);
        src[2000] = src[2001] = 1.f;
        std::vector<float> dst;
        r.process(src.data(), 4000, dst);
        r.flush(dst);
        size_t peak = 0;
        for (size_t i = 0; i < dst.size() / 2; ++i)
            if (dst[2 * i] > dst[2 * peak]) peak = i;
        std::printf("        impulse at 1000 -> peak at %zu (expected %.1f)\n", peak, 1000.0 * 48000 / 44100);
        CHECK(peak == 1088 || peak == 1089);
    }
    {   // Chunked input gives exactly the one-shot output.
        const auto src = sine(440, 44100, 20000);
        Resampler a, b;
        a.configure(44100, 48000, 2);
        b.configure(44100, 48000, 2);
        std::vector<float> one, chunked;
        a.process(src.data(), 20000, one);
        a.flush(one);
        std::mt19937 rng(7);
        for (size_t at = 0; at < 20000;) {
            const size_t k = std::min<size_t>(20000 - at, 1 + rng() % 700);
            b.process(src.data() + at * 2, k, chunked);
            at += k;
        }
        b.flush(chunked);
        CHECK(one == chunked);
        CHECK(a.toInput(a.toOutput(12345)) == 12345 || a.toInput(a.toOutput(12345)) == 12344);
    }
    {
        Resampler r;
        r.configure(48000, 48000, 2);
        const auto src = sine(440, 48000, 100);
        std::vector<float> dst;
        r.process(src.data(), 100, dst);
        r.flush(dst);
        CHECK(r.passthrough() && dst == src);
    }
}

// ---- 3. Matroska ----------------------------------------------------------------------------------------------------

// EBML element bytes: an id (as written), a size (8-byte form when `wide`, else the shortest), the payload.
std::vector<uint8_t> el(uint32_t id, const std::vector<uint8_t>& payload, bool unknownSize = false) {
    std::vector<uint8_t> out;
    for (int shift = 24; shift >= 0; shift -= 8)
        if ((id >> shift) || !out.empty() || shift == 0) out.push_back(static_cast<uint8_t>(id >> shift));
    if (unknownSize) {
        out.push_back(0x01);
        for (int i = 0; i < 7; ++i) out.push_back(0xFF);
    } else {
        const uint64_t n = payload.size();
        out.push_back(0x01);
        for (int i = 6; i >= 0; --i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
std::vector<uint8_t> uintEl(uint32_t id, uint64_t v) {
    std::vector<uint8_t> p;
    for (int i = 7; i >= 0; --i) p.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return el(id, p);
}
std::vector<uint8_t> strEl(uint32_t id, const std::string& s) { return el(id, std::vector<uint8_t>(s.begin(), s.end())); }
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void testMatroska() {
    std::printf("Matroska demuxer\n");
    {
        const auto bytes = readAll(g_dir / "chirp.webm");
        MemoryByteReader r(bytes.data(), bytes.size());
        mkv::Demuxer d;
        CHECK(d.open(r) == mkv::Result::Ok);
        const auto& t = d.track();
        std::printf("        %s: track %llu, %u Hz, %u ch, delay %lld ns, preroll %lld ns, %lld ms, %zu cues\n", t.codecId.c_str(),
                    static_cast<unsigned long long>(t.number), t.sampleRate, t.channels, static_cast<long long>(t.codecDelayNs),
                    static_cast<long long>(t.seekPreRollNs), static_cast<long long>(d.durationNs() / 1'000'000), d.cueCount());
        CHECK(t.codec == mkv::Codec::Opus && t.channels == 2 && t.sampleRate == 48000);
        CHECK(t.codecDelayNs == 6'500'000 && t.seekPreRollNs == 80'000'000);
        CHECK(std::llabs(d.durationNs() - 3'008'000'000) < 2'000'000);
        CHECK(d.cueCount() >= 3);   // cues at the front, read at open
        CHECK(xiph::parseOpusHead(t.codecPrivate.data(), t.codecPrivate.size()).has_value());
        mkv::Frame f;
        int frames = 0, samples = 0;
        int64_t lastTime = -1, discard = 0;
        bool increasing = true;
        while (d.next(f) == mkv::Result::Ok) {
            ++frames;
            samples += xiph::opusPacketSamples(f.data.data(), f.data.size());
            increasing = increasing && f.timeNs > lastTime;
            lastTime = f.timeNs;
            discard += f.discardPaddingNs;
        }
        std::printf("        %d frames, %d samples, last at %lld ms, discard padding %lld ns\n", frames, samples,
                    static_cast<long long>(lastTime / 1'000'000), static_cast<long long>(discard));
        CHECK(frames == 151 && samples == 151 * 960 && increasing);
        CHECK(discard > 0);   // the encoder's tail beyond 3 s
        CHECK(d.seek(1'500'000'000) == mkv::Result::Ok && d.next(f) == mkv::Result::Ok);
        CHECK(f.timeNs <= 1'500'000'000 && f.timeNs > 900'000'000);
        CHECK(d.seek(0) == mkv::Result::Ok && d.next(f) == mkv::Result::Ok && f.timeNs == 0);
    }
    {   // Cues at the end: read when the first seek needs them.
        const auto bytes = readAll(g_dir / "chirp-cues-end.webm");
        MemoryByteReader r(bytes.data(), bytes.size());
        mkv::Demuxer d;
        CHECK(d.open(r) == mkv::Result::Ok);
        const size_t before = d.cueCount();
        mkv::Frame f;
        CHECK(d.seek(2'000'000'000) == mkv::Result::Ok && d.next(f) == mkv::Result::Ok);
        std::printf("        cues at the end: %zu before the seek, %zu after; frame at %lld ms\n", before, d.cueCount(),
                    static_cast<long long>(f.timeNs / 1'000'000));
        CHECK(before == 0 && d.cueCount() >= 3 && f.timeNs <= 2'000'000'000 && f.timeNs > 1'400'000'000);
    }
    {   // Vorbis in Matroska: the Xiph-laced headers.
        const auto bytes = readAll(g_dir / "chirp-vorbis.mka");
        MemoryByteReader r(bytes.data(), bytes.size());
        mkv::Demuxer d;
        CHECK(d.open(r) == mkv::Result::Ok && d.track().codec == mkv::Codec::Vorbis);
        const auto headers = xiph::splitXiphLacing(d.track().codecPrivate.data(), d.track().codecPrivate.size());
        CHECK(headers && headers->size() == 3 && (*headers)[0][0] == 1 && (*headers)[1][0] == 3 && (*headers)[2][0] == 5);
        xiph::VorbisPacketDecoder v;
        CHECK(headers && v.open(*headers) && v.sampleRate() == 32000 && v.channels() == 2);
    }
    {   // Hand-built: Xiph, EBML and fixed-size lacing, a BlockGroup with DiscardPadding, another track's block, an
        // unknown-size cluster.
        const auto ebml = el(0x1A45DFA3, strEl(0x4282, "webm"));
        const auto tracks = el(0x1654AE6B, cat({el(0xAE, cat({uintEl(0xD7, 1), uintEl(0x83, 1), strEl(0x86, "V_VP9")})),
                                                el(0xAE, cat({uintEl(0xD7, 2), uintEl(0x83, 2), strEl(0x86, "A_OPUS"),
                                                              el(0xE1, uintEl(0x9F, 2))}))}));
        auto block = [](uint8_t track, int16_t rel, uint8_t flags, std::vector<uint8_t> body) {
            std::vector<uint8_t> b{static_cast<uint8_t>(0x80 | track), static_cast<uint8_t>(rel >> 8), static_cast<uint8_t>(rel), flags};
            b.insert(b.end(), body.begin(), body.end());
            return b;
        };
        // Xiph: 3 frames of 2, 300 and 1 bytes.
        std::vector<uint8_t> xiphBody{2, 2, 255, 45};
        for (int i = 0; i < 2; ++i) xiphBody.push_back('a');
        for (int i = 0; i < 300; ++i) xiphBody.push_back('b');
        xiphBody.push_back('c');
        // EBML: 3 frames of 4, 6 (+2) and 1 bytes.
        std::vector<uint8_t> ebmlBody{2, 0x84, 0xC1 /* +2 */};
        for (int i = 0; i < 4; ++i) ebmlBody.push_back('d');
        for (int i = 0; i < 6; ++i) ebmlBody.push_back('e');
        ebmlBody.push_back('f');
        // Fixed: 2 frames of 3 bytes.
        const std::vector<uint8_t> fixedBody{1, 'g', 'g', 'g', 'h', 'h', 'h'};
        const auto cluster1 = el(0x1F43B675, cat({uintEl(0xE7, 100), el(0xA3, block(1, 0, 0x80, {'v'})),
                                                  el(0xA3, block(2, 0, 0x82, xiphBody)), el(0xA3, block(2, 20, 0x86, ebmlBody)),
                                                  el(0xA3, block(2, 40, 0x84, fixedBody))}));
        const auto cluster2 = el(0x1F43B675, cat({uintEl(0xE7, 200), el(0xA0, cat({el(0xA1, block(2, 5, 0x00, {'z', 'z'})),
                                                                                  uintEl(0x75A2, 2'500'000)}))}),
                                 true);
        const auto bytes = cat({ebml, el(0x18538067, cat({el(0x1549A966, uintEl(0x2AD7B1, 1'000'000)), tracks, cluster1, cluster2}))});
        MemoryByteReader r(bytes.data(), bytes.size());
        mkv::Demuxer d;
        CHECK(d.open(r) == mkv::Result::Ok && d.track().number == 2 && d.track().codec == mkv::Codec::Opus);
        std::vector<std::pair<std::string, int64_t>> got;
        int64_t pad = 0;
        mkv::Frame f;
        while (d.next(f) == mkv::Result::Ok) {
            got.emplace_back(std::string(f.data.begin(), f.data.end()), f.timeNs);
            pad += f.discardPaddingNs;
        }
        constexpr int64_t kCont = mkv::Frame::kContinued;
        const std::vector<std::pair<std::string, int64_t>> want = {
            {"aa", 100'000'000}, {std::string(300, 'b'), kCont}, {"c", kCont}, {"dddd", 120'000'000}, {"eeeeee", kCont},
            {"f", kCont}, {"ggg", 140'000'000}, {"hhh", kCont}, {"zz", 205'000'000}};
        CHECK(got == want);
        CHECK(pad == 2'500'000);
        // Every truncation ends cleanly (no frame past the cut, no crash).
        bool clean = true;
        for (size_t cut = 0; cut < bytes.size(); cut += 7) {
            MemoryByteReader tr(bytes.data(), cut);
            mkv::Demuxer td;
            const auto res = td.open(tr);
            if (res != mkv::Result::Ok) {
                clean = clean && (res == mkv::Result::Invalid || res == mkv::Result::End);
                continue;
            }
            int n = 0;
            while (td.next(f) == mkv::Result::Ok && n < 100) ++n;
            clean = clean && n <= 9;
        }
        CHECK(clean);
    }
    {   // Garbage and a real file with random bytes flipped: never a crash, never an endless loop.
        std::mt19937 rng(1);
        std::vector<uint8_t> junk(4096);
        for (auto& b : junk) b = static_cast<uint8_t>(rng());
        MemoryByteReader r(junk.data(), junk.size());
        mkv::Demuxer d;
        CHECK(d.open(r) == mkv::Result::Invalid);
        auto bytes = readAll(g_dir / "chirp.webm");
        int opened = 0;
        for (int round = 0; round < 40; ++round) {
            auto damaged = bytes;
            for (int k = 0; k < 20; ++k) damaged[rng() % damaged.size()] = static_cast<uint8_t>(rng());
            MemoryByteReader dr(damaged.data(), damaged.size());
            mkv::Demuxer dd;
            if (dd.open(dr) != mkv::Result::Ok) continue;
            ++opened;
            mkv::Frame f;
            for (int n = 0; n < 1000 && dd.next(f) == mkv::Result::Ok; ++n) {}
            dd.seek(1'000'000'000);
        }
        std::printf("        %d of 40 damaged files still opened\n", opened);
        CHECK(true);
    }
}

// ---- 4. Decoder ------------------------------------------------------------------------------------------------------

// `name` decodes to `expectFrames` frames at `rate`, aligned with the chirp (checked over 0.1 .. 1 s, where even the
// lossiest fixture keeps the waveform), and every seek lands on its sample: the frames after it are the full decode's
// at that position (Matroska's block times are whole milliseconds: `tolerance` frames there).
void checkFile(const char* name, int64_t expectFrames, uint32_t rate, double minSnr, int tolerance, float gain = NAN,
               const RatePolicy& rates = {}) {
    std::printf("Decoder: %s%s\n", name, rates.fixed ? std::format(" at {} Hz", rates.fixed).c_str() : "");
    const Decoded d = decode(g_dir / name, rates);
    double snr = 0;
    const int lag = d.ok ? bestLag(d.pcm, d.rate, d.firstFrame, rate / 10, rate, 40, &snr) : 99;
    std::printf("        built-in %d, %u Hz, %u ch, %lld ms, %zu frames from %lld, best lag %d (SNR %.1f dB), tag gain %.2f\n", d.builtIn,
                d.rate, d.channels, static_cast<long long>(d.durationMs), d.pcm.size() / 2, static_cast<long long>(d.firstFrame), lag,
                snr, d.tagGain);
    CHECK(d.ok && d.builtIn && d.rate == rate && d.channels == 2);
    CHECK(int64_t(d.pcm.size() / 2) == expectFrames && d.firstFrame == 0);
    if (!rates.fixed) CHECK(std::llabs(d.durationMs - expectFrames * 1000 / rate) <= 10);
    CHECK(std::abs(lag) <= tolerance && snr > minSnr);
    if (!std::isnan(gain)) CHECK(std::fabs(d.tagGain - gain) < 0.01f);

    for (const int64_t ms : {int64_t(1234), int64_t(25), expectFrames * 1000 / rate - 500, int64_t(0)}) {
        const Decoded s = decode(g_dir / name, rates, ms);
        double ss = 0;
        const int sl = s.ok ? bestLagRef(s.pcm, s.firstFrame, d.pcm, rate / 5, std::max(tolerance, 8) * 2, &ss) : 99;
        const int64_t count = int64_t(s.pcm.size() / 2);
        std::printf("        seek %lld ms: first frame %lld, %lld frames, lag %d against the full decode (SNR %.1f dB)\n",
                    static_cast<long long>(ms), static_cast<long long>(s.firstFrame), static_cast<long long>(count), sl, ss);
        CHECK(s.ok && s.firstFrame == ms * int64_t(rate) / 1000);
        CHECK(std::llabs(count - (expectFrames - s.firstFrame)) <= tolerance && std::abs(sl) <= tolerance && ss > 25);
    }
}

void testDecoder() {
    // Lengths: 3 s at 48 kHz = 144000 (pre-skip / CodecDelay and the end trimming exact, in Ogg and in Matroska); 3 s at
    // 44.1 kHz; 2 s at 32 kHz.
    checkFile("chirp.opus", 144000, 48000, 20, 0, 2.f);   // R128_TRACK_GAIN -768 = -3 dB, + 5 dB to -18 LUFS
    checkFile("chirp.ogg", 132300, 44100, 20, 0, -6.5f);
    checkFile("chirp.webm", 144000, 48000, 20, 0);
    checkFile("chirp-cues-end.webm", 144000, 48000, 20, 0);
    checkFile("chirp-vorbis.mka", 64000, 32000, 15, 0);
    // Resampled: Opus 48 -> 44.1 kHz and Vorbis 44.1 -> 48 kHz (shared mode at those mix rates).
    checkFile("chirp.opus", 132300, 44100, 20, 0, NAN, {44100});
    checkFile("chirp.ogg", 144000, 48000, 20, 0, NAN, {48000});
    checkFile("chirp.webm", 132300, 44100, 20, 0, NAN, {44100});
    {
        const Decoded e = decode(g_dir / "chirp.ogg", RatePolicy{0, {48000, 88200, 96000}});
        CHECK(e.ok && e.rate == 88200 && e.pcm.size() / 2 == 264600);   // exclusive mode: an integer multiple
    }

    std::printf("Decoder: 5.1 WebM and mono Ogg Opus come out as stereo\n");
    {
        const Decoded s = decode(g_dir / "surround.webm");
        // ffmpeg's FL 440 Hz, FR 550 Hz, FC 660 Hz: left = FL + 0.707 C, right = FR + 0.707 C (normalised).
        auto tone = [&](int ch, double hz) {
            double re = 0, im = 0;
            for (size_t i = 4800; i < 4800 + 38400 && i < s.pcm.size() / 2; ++i) {
                re += s.pcm[i * 2 + ch] * std::cos(2 * std::numbers::pi * hz * i / 48000);
                im += s.pcm[i * 2 + ch] * std::sin(2 * std::numbers::pi * hz * i / 48000);
            }
            return std::sqrt(re * re + im * im) / 38400 * 2;
        };
        std::printf("        left 440/550/660: %.3f %.3f %.3f, right: %.3f %.3f %.3f\n", tone(0, 440), tone(0, 550), tone(0, 660),
                    tone(1, 440), tone(1, 550), tone(1, 660));
        CHECK(s.ok && s.channels == 2 && s.pcm.size() / 2 == 48000);
        const double side = 0.3 / (1 + 2 * 0.70710678), centre = side * 0.70710678;
        CHECK(std::fabs(tone(0, 440) - side) < 0.01 && std::fabs(tone(0, 660) - centre) < 0.01 && tone(0, 550) < 0.005);
        CHECK(std::fabs(tone(1, 550) - side) < 0.01 && std::fabs(tone(1, 660) - centre) < 0.01 && tone(1, 440) < 0.005);
        const Decoded m = decode(g_dir / "mono.opus");
        bool same = m.ok && !m.pcm.empty();
        for (size_t i = 0; same && i < m.pcm.size(); i += 2) same = m.pcm[i] == m.pcm[i + 1];
        CHECK(same && m.pcm.size() / 2 == 48000);
    }
}

// ---- 5. XiphTags ------------------------------------------------------------------------------------------------------

void testTags() {
    std::printf("XiphTags\n");
    const auto cover = readAll(g_dir / "cover.png");
    for (const char* name : {"chirp.opus", "chirp.ogg"}) {
        const auto info = xiph::readOggFile(g_dir / name, true);
        CHECK(info.has_value());
        if (!info) continue;
        std::printf("        %s: %s, %lld ms, %u Hz, %u ch, %zu tags, cover %zu bytes\n", name, info->codec.c_str(),
                    static_cast<long long>(info->durationMs), info->sampleRate, info->channels, info->tags.size(), info->picture.size());
        CHECK(info->tag("TITLE") == "Çağrı Tonu" && info->tag("ALBUM") == "Fixtures" && info->tag("ARTIST") == "Test Band; Guest");
        CHECK(info->tag("TRACKNUMBER").starts_with("3") && info->tag("DATE").starts_with("2024"));
        CHECK(info->durationMs == 3000);
        CHECK(std::string(cover.begin(), cover.end()) == info->picture);   // the front cover, not the back one
        CHECK(info->tag("METADATA_BLOCK_PICTURE").empty());
    }
    CHECK(!xiph::readOggFile(g_dir / "chirp.webm", false).has_value());
    CHECK(!xiph::readOggFile(g_dir / "missing.ogg", false).has_value());
    const auto comments = xiph::parseComments(reinterpret_cast<const uint8_t*>("\x02\x00\x00\x00vv\x02\x00\x00\x00\x05\x00\x00\x00title"
                                                                                "\x05\x00\x00\x00" "a=b=c"),
                                              29);
    CHECK(comments.size() == 1 && comments[0].first == "A" && comments[0].second == "b=c");
}

// ---- 6. Live Ogg ------------------------------------------------------------------------------------------------------

void testLive() {
    std::printf("Live Ogg (OggDemuxer + LiveDecoder)\n");
    for (const char* name : {"chirp.ogg", "chirp.opus"}) {
        auto bytes = readAll(g_dir / name);
        // Chained: the same stream twice (Icecast starts a new logical stream per song).
        const size_t one = bytes.size();
        bytes.insert(bytes.end(), bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(one));
        live::OggDemuxer demux;
        LiveDecoder decoder;
        std::vector<float> pcm;
        uint64_t samples = 0;
        int frames = 0, discontinuities = 0, bad = 0;
        std::optional<std::string> title;
        live::Frame f;
        for (size_t at = 0; at < bytes.size(); at += 1000) {
            demux.push(bytes.data() + at, std::min<size_t>(1000, bytes.size() - at));
            while (demux.next(f)) {
                ++frames;
                samples += f.samples;
                discontinuities += f.discontinuity;
                if (!decoder.isOpen() || decoder.configKey() != live::frameConfigKey(f)) {
                    bool unsupported = false;
                    if (FAILED(decoder.open(f, unsupported))) ++bad;
                } else if (f.discontinuity) {
                    decoder.flush();
                }
                if (FAILED(decoder.decode(f, pcm))) ++bad;
            }
            if (auto t = demux.takeTitle()) title = t;
        }
        const bool vorbis = std::string(name).ends_with(".ogg");
        std::printf("        %s x2: %d frames (%s), %llu samples, %d discontinuities, decoded %zu frames at %u Hz / %u ch, title \"%s\"\n",
                    name, frames, vorbis ? "Vorbis" : "Opus", static_cast<unsigned long long>(samples), discontinuities, pcm.size() / 2,
                    decoder.sampleRate(), decoder.channels(), title ? title->c_str() : "");
        CHECK(!demux.unsupported() && bad == 0 && discontinuities == 2);
        CHECK(f.codec == (vorbis ? live::Codec::Vorbis : live::Codec::Opus));
        CHECK(decoder.sampleRate() == (vorbis ? 44100u : 48000u) && decoder.channels() == 2);
        CHECK(title == std::string("Test Band; Guest - Çağrı Tonu"));
        // Durations add up to the decoded audio (Opus: the pre-skip is dropped twice, once per chained stream).
        const uint64_t decoded = pcm.size() / 2;
        if (vorbis) CHECK(decoded == samples && samples >= 2 * 132300 && samples < 2 * 132300 + 4096);
        else CHECK(decoded == samples - 2 * 312 && samples == 2 * 144312ull + 2 * 648);   // the last packet's padding is kept
        CHECK(std::string(live::codecName(f)) == (vorbis ? "Vorbis" : "Opus"));
    }
    CHECK(AudioEngine::supportsLiveCodec("OGG") && AudioEngine::supportsLiveCodec("Opus") && AudioEngine::supportsLiveCodec("audio/ogg"));
    CHECK(!AudioEngine::supportsLiveCodec("FLAC") && AudioEngine::supportsWebm());
}

// ---- 7. AudioEngine ---------------------------------------------------------------------------------------------------

void testEngine() {
    std::printf("AudioEngine: local Ogg Opus / Ogg Vorbis files\n");
    if (AudioEngine::outputDevices().empty()) {
        std::printf("        no audio device: skipped\n");
        return;
    }
    std::atomic<int> ended{0};
    std::atomic<uint64_t> endedNext{0};
    std::atomic<int> errors{0};
    EngineEvents ev;
    ev.onEnded = [&](uint64_t, uint64_t next) {
        endedNext = next;
        ++ended;
    };
    ev.onError = [&](ErrorKind, const std::string& m, uint64_t) {
        std::printf("        engine error: %s\n", m.c_str());
        ++errors;
    };
    AudioEngine engine(ev);
    engine.setVolume(0.05f);
    auto waitFor = [](auto pred, int ms) {
        for (int i = 0; i < ms / 10; ++i) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    };
    StreamSource a;
    a.localPath = (g_dir / "chirp.opus").wstring();
    a.tag = 1;
    engine.open(a);
    CHECK(waitFor([&] { return engine.state() == State::Playing; }, 5000));
    CHECK(std::llabs(engine.durationMs() - 3000) <= 5);
    engine.seek(2000);
    CHECK(waitFor([&] { return engine.state() == State::Playing && engine.positionMs() > 2100; }, 3000));
    StreamSource b;
    b.localPath = (g_dir / "chirp.ogg").wstring();
    b.tag = 2;
    engine.preload(b);
    CHECK(waitFor([&] { return ended.load() >= 1; }, 5000));
    CHECK(endedNext.load() == 2 && engine.currentTag() == 2);   // gapless handoff to the Vorbis file
    CHECK(waitFor([&] { return engine.state() == State::Playing && engine.positionMs() > 300; }, 3000));
    engine.seek(2700);
    CHECK(waitFor([&] { return ended.load() >= 2; }, 5000));
    CHECK(endedNext.load() == 0 && errors.load() == 0);
}

// ---- 8. Exclusive mode (--exclusive: takes the default device for a few seconds) ---------------------------------------

struct EngineProbe {
    std::atomic<int> ended{0}, errors{0}, previewPlaying{0};
    std::atomic<uint64_t> endedNext{0};
    EngineEvents events() {
        EngineEvents ev;
        ev.onEnded = [this](uint64_t, uint64_t next) {
            endedNext = next;
            ++ended;
        };
        ev.onError = [this](ErrorKind, const std::string& m, uint64_t) {
            std::printf("        engine error: %s\n", m.c_str());
            ++errors;
        };
        ev.onPreview = [this](State s, uint64_t) {
            if (s == State::Playing) ++previewPlaying;
        };
        return ev;
    }
};

template <class F>
bool waitMs(F pred, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void printOutput(const char* who, const OutputInfo& o) {
    std::printf("        %s: open %d, exclusive %d, fallback %d, %u Hz, %u ch, %u bit (%u valid)%s\n", who, o.open, o.exclusive,
                static_cast<int>(o.fallback), o.sampleRate, o.channels, o.bits, o.validBits, o.floatFormat ? " float" : "");
}

void testExclusive() {
    std::printf("Exclusive mode (default device)\n");
    StreamSource opus, vorbis;
    opus.localPath = (g_dir / "chirp.opus").wstring();
    vorbis.localPath = (g_dir / "chirp.ogg").wstring();
    EngineProbe pa, pb;
    AudioEngine a(pa.events());
    a.setVolume(0.05f);
    a.setExclusive(true);
    opus.tag = 1;
    a.open(opus);
    CHECK(waitMs([&] { return a.state() == State::Playing; }, 5000));
    printOutput("A, Opus", a.outputInfo());
    if (!a.outputInfo().exclusive) {
        std::printf("        the device refused exclusive mode (fallback %d): the rest is skipped\n", static_cast<int>(a.outputInfo().fallback));
        CHECK(a.outputInfo().fallback != OutputInfo::Fallback::None);
        return;
    }
    CHECK(a.outputInfo().sampleRate == 48000 && a.outputInfo().channels == 2);
    {   // A second engine can't take the device: it plays shared and says why.
        AudioEngine b(pb.events());
        b.setVolume(0.05f);
        b.setExclusive(true);
        vorbis.tag = 2;
        b.open(vorbis);
        waitMs([&] { return b.outputInfo().open || pb.errors.load() > 0; }, 4000);
        printOutput("B, while A holds it", b.outputInfo());
        CHECK(b.outputInfo().fallback == OutputInfo::Fallback::Busy && !b.outputInfo().exclusive);
        b.stop();
    }
    // Paused: the device is released.
    a.pause();
    CHECK(waitMs([&] { return !a.outputInfo().open; }, 3000));
    a.play();
    CHECK(waitMs([&] { return a.outputInfo().open && a.outputInfo().exclusive && a.state() == State::Playing; }, 3000));

    // Tracks at their own rate: 44.1 kHz Vorbis, then gaplessly preloaded 48 kHz Opus (the device reopens in between).
    vorbis.tag = 3;
    a.open(vorbis);
    CHECK(waitMs([&] { return a.state() == State::Playing && a.outputInfo().sampleRate == 44100; }, 5000));
    printOutput("A, Vorbis", a.outputInfo());
    opus.tag = 4;
    a.preload(opus);
    a.seek(2500);
    CHECK(waitMs([&] { return pa.ended.load() >= 1; }, 5000) && pa.endedNext.load() == 4);
    CHECK(waitMs([&] { return a.state() == State::Playing && a.outputInfo().sampleRate == 48000 && a.outputInfo().exclusive; }, 3000));
    printOutput("A, Opus after Vorbis", a.outputInfo());

    // A crossfade keeps the rate that plays: Vorbis at 44.1 kHz mixes into the Opus file resampled to it.
    vorbis.tag = 5;
    a.open(vorbis);
    CHECK(waitMs([&] { return a.state() == State::Playing && a.outputInfo().sampleRate == 44100; }, 5000));
    StreamSource fadeIn = opus;
    fadeIn.tag = 6;
    fadeIn.crossfadeMs = 1000;
    a.preload(fadeIn);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    a.seek(1500);
    CHECK(waitMs([&] { return pa.ended.load() >= 2; }, 5000) && pa.endedNext.load() == 6);
    CHECK(a.outputInfo().sampleRate == 44100 && a.outputInfo().exclusive);

    // The preview voice plays over it at the rate the device runs at.
    StreamSource pv = opus;
    pv.tag = 7;
    a.openPreview(pv, 500);
    CHECK(waitMs([&] { return pa.previewPlaying.load() == 1; }, 3000));
    CHECK(a.outputInfo().sampleRate == 44100 && a.outputInfo().exclusive);
    a.stopPreview();

    // Off again: shared mode at once, still playing.
    a.setExclusive(false);
    CHECK(waitMs([&] { return a.outputInfo().open && !a.outputInfo().exclusive && a.state() == State::Playing; }, 3000));
    printOutput("A, shared again", a.outputInfo());
    CHECK(pa.errors.load() == 0);
}

// ---- --live <url>: a station through the engine, its state twice a second for 15 s (a look at real streams) ----------

void watchLive(const std::string& url) {
    EngineProbe p;
    AudioEngine e(p.events());
    e.setVolume(0.f);
    StreamSource s;
    s.url = url;
    s.live = true;
    s.tag = 1;
    e.open(s);
    const char* names[] = {"Idle", "Loading", "Playing", "Paused", "Ended", "Error"};
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const LiveInfo li = e.liveInfo();
        std::printf("  %5.1f s  %-8s pos %6lld ms  %s %u kbps %u Hz %u ch  buffered %lld ms  reconnects %d\n", (i + 1) * 0.5,
                    names[static_cast<int>(e.state())], static_cast<long long>(e.positionMs()), li.codec.c_str(), li.bitrateKbps,
                    li.sampleRate, li.channels, static_cast<long long>(li.bufferedMs), li.reconnects);
    }
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    st::log::init();
    bool exclusive = false;
    std::string liveUrl;
    g_dir = fs::path(ST_CODECS_FIXTURES);
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--exclusive") exclusive = true;
        else if (std::string(argv[i]) == "--live" && i + 1 < argc) liveUrl = argv[++i];
        else g_dir = fs::path(argv[i]);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!liveUrl.empty()) {
        watchLive(liveUrl);
    } else if (exclusive) {
        if (AudioEngine::outputDevices().empty()) std::printf("no audio device: --exclusive skipped\n");
        else testExclusive();
    } else {
        testRatePolicy();
        testResampler();
        testMatroska();
        testDecoder();
        testTags();
        testLive();
        testEngine();
    }
    MFShutdown();
    std::printf("\n%d failure(s)\n", g_failures);
    st::log::shutdown();
    return g_failures ? 1 : 0;
}
