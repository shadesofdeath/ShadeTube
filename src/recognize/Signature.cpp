#include "recognize/Signature.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace st::recognize {

namespace {

constexpr int kFft = 2048;
constexpr int kBins = kFft / 2 + 1;   // 1025
constexpr int kHistory = 256;         // spectra kept (a power of two: indexes wrap with & 255)
constexpr float kMinPower = 1.f / 64;

constexpr uint32_t kMagic1 = 0xcafe2580;
constexpr uint32_t kMagic2 = 0x94119c00;
constexpr uint32_t kFixed = (15u << 19) + 0x40000;
constexpr uint32_t kContents = 0x40000000;
constexpr uint32_t kBandTag = 0x60030040;
constexpr size_t kHeader = 48;

// Spread spectra, relative to the next one to be written, that a peak must stand above (one bin lower).
constexpr int kOtherHops[] = {-53, -45, 165, 172, 179, 186, 193, 200, 214, 221, 228, 235, 242, 249};
// Bins of the spread spectrum 49 hops back, around the candidate, that it must stand above.
constexpr int kNeighbors[] = {-10, -7, -4, -3, 1, 2, 5, 8};

uint32_t sampleRateId(uint32_t rate) {
    switch (rate) {
    case 8000: return 1;
    case 11025: return 2;
    case 16000: return 3;
    case 32000: return 4;
    case 44100: return 5;
    case 48000: return 6;
    default: return 3;
    }
}

uint32_t rateOfId(uint32_t id) {
    static constexpr uint32_t rates[] = {0, 8000, 11025, 16000, 32000, 44100, 48000};
    return id < 7 ? rates[id] : 0;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void set32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
uint16_t get16(std::span<const uint8_t> b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }

} // namespace

// ---- helpers ---------------------------------------------------------------------------------------------------------

uint32_t crc32(std::span<const uint8_t> bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string base64(std::span<const uint8_t> bytes) {
    static constexpr char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += abc[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = uint32_t(bytes[i]) << 16;
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

int bandOf(double hz) {
    if (hz >= 250 && hz < 520) return 0;
    if (hz >= 520 && hz < 1450) return 1;
    if (hz >= 1450 && hz < 3500) return 2;
    if (hz >= 3500 && hz <= 5500) return 3;
    return -1;
}

// ---- Signature -------------------------------------------------------------------------------------------------------

size_t Signature::peakCount() const {
    size_t n = 0;
    for (const auto& b : bands) n += b.size();
    return n;
}

std::vector<uint8_t> Signature::encode() const {
    std::vector<uint8_t> out(kHeader, 0);
    put32(out, kContents);
    put32(out, 0);   // the size, below
    for (int band = 0; band < kBands; ++band) {
        const auto& peaks = bands[static_cast<size_t>(band)];
        if (peaks.empty()) continue;
        std::vector<uint8_t> buf;
        uint32_t offset = 0;
        for (const auto& p : peaks) {
            const uint32_t pass = std::max(p.pass, offset);   // callers keep them in order; never go back
            if (pass - offset >= 255) {
                buf.push_back(0xff);
                put32(buf, pass);
                offset = pass;
            }
            buf.push_back(static_cast<uint8_t>(pass - offset));
            put16(buf, p.magnitude);
            put16(buf, p.bin);
            offset = pass;
        }
        put32(out, kBandTag + static_cast<uint32_t>(band));
        put32(out, static_cast<uint32_t>(buf.size()));
        out.insert(out.end(), buf.begin(), buf.end());
        out.resize(out.size() + ((4 - buf.size() % 4) % 4), 0);
    }
    const uint32_t size = static_cast<uint32_t>(out.size() - kHeader);
    set32(out, kHeader + 4, size);
    set32(out, 0, kMagic1);
    set32(out, 8, size);
    set32(out, 12, kMagic2);
    set32(out, 28, sampleRateId(sampleRate) << 27);
    set32(out, 40, samples + static_cast<uint32_t>(sampleRate * 0.24));
    set32(out, 44, kFixed);
    set32(out, 4, crc32(std::span<const uint8_t>(out).subspan(8)));
    return out;
}

std::string Signature::dataUri() const {
    const auto bytes = encode();
    return "data:audio/vnd.shazam.sig;base64," + base64(bytes);
}

std::optional<Signature> Signature::decode(std::span<const uint8_t> b) {
    if (b.size() < kHeader + 8) return std::nullopt;
    if (get32(b, 0) != kMagic1 || get32(b, 12) != kMagic2) return std::nullopt;
    if (get32(b, 4) != crc32(b.subspan(8))) return std::nullopt;
    const uint32_t size = get32(b, 8);
    if (size != b.size() - kHeader || get32(b, kHeader) != kContents || get32(b, kHeader + 4) != size) return std::nullopt;
    Signature s;
    s.sampleRate = rateOfId(get32(b, 28) >> 27);
    if (!s.sampleRate) return std::nullopt;
    const uint32_t withLead = get32(b, 40);
    const uint32_t lead = static_cast<uint32_t>(s.sampleRate * 0.24);
    s.samples = withLead >= lead ? withLead - lead : 0;
    size_t at = kHeader + 8;
    while (at + 8 <= b.size()) {
        const uint32_t tag = get32(b, at), len = get32(b, at + 4);
        at += 8;
        if (tag < kBandTag || tag >= kBandTag + kBands || at + len > b.size()) return std::nullopt;
        auto& peaks = s.bands[tag - kBandTag];
        const size_t end = at + len;
        uint32_t offset = 0;
        while (at < end) {
            uint8_t delta = b[at++];
            if (delta == 0xff) {
                if (at + 5 > end) return std::nullopt;
                offset = get32(b, at);
                at += 4;
                delta = b[at++];
            }
            if (at + 4 > end) return std::nullopt;
            Peak p;
            p.pass = offset + delta;
            p.magnitude = get16(b, at);
            p.bin = get16(b, at + 2);
            at += 4;
            offset = p.pass;
            peaks.push_back(p);
        }
        at = end + ((4 - len % 4) % 4);
    }
    return s;
}

// ---- SignatureGenerator ------------------------------------------------------------------------------------------------

SignatureGenerator::SignatureGenerator() {
    window_.resize(kFft);
    for (int i = 0; i < kFft; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * std::numbers::pi * (i + 1) / (kFft + 1)));
    cosT_.resize(kFft / 2);
    sinT_.resize(kFft / 2);
    for (int i = 0; i < kFft / 2; ++i) {
        cosT_[i] = static_cast<float>(std::cos(2 * std::numbers::pi * i / kFft));
        sinT_[i] = static_cast<float>(-std::sin(2 * std::numbers::pi * i / kFft));
    }
    bitrev_.resize(kFft);
    for (int i = 0; i < kFft; ++i) {
        int r = 0;
        for (int b = 0, v = i; b < 11; ++b, v >>= 1) r = (r << 1) | (v & 1);
        bitrev_[i] = static_cast<uint16_t>(r);
    }
    re_.resize(kFft);
    im_.resize(kFft);
    reset();
}

void SignatureGenerator::reset() {
    ring_.assign(kFft, 0.f);
    ringPos_ = 0;
    pending_.clear();
    spectra_.assign(kHistory, {});
    spread_.assign(kHistory, {});
    written_ = 0;
    for (auto& p : peaks_) p.clear();
    fed_ = 0;
}

void SignatureGenerator::feed(const float* samples, size_t count) {
    fed_ += static_cast<uint32_t>(count);
    size_t i = 0;
    if (!pending_.empty()) {
        const size_t take = std::min(count, static_cast<size_t>(kHop) - pending_.size());
        pending_.insert(pending_.end(), samples, samples + take);
        i = take;
        if (pending_.size() < static_cast<size_t>(kHop)) return;
        process(pending_.data());
        pending_.clear();
    }
    for (; i + kHop <= count; i += kHop) process(samples + i);
    if (i < count) pending_.assign(samples + i, samples + count);
}

void SignatureGenerator::process(const float* hop) {
    // 16-bit sample levels: the thresholds below are made for them.
    for (int i = 0; i < kHop; ++i) ring_[ringPos_ + i] = std::clamp(hop[i] * 32768.f, -32768.f, 32767.f);
    ringPos_ = (ringPos_ + kHop) & (kFft - 1);
    fft();
    spread();
    ++written_;
    if (written_ >= 46) findPeaks();
}

void SignatureGenerator::fft() {
    // Oldest sample first, windowed, in bit-reversed order for the in-place radix-2 FFT.
    for (int i = 0; i < kFft; ++i) {
        re_[bitrev_[i]] = ring_[(i + ringPos_) & (kFft - 1)] * window_[i];
        im_[bitrev_[i]] = 0;
    }
    for (int len = 2; len <= kFft; len <<= 1) {
        const int half = len / 2, step = kFft / len;
        for (int start = 0; start < kFft; start += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = cosT_[k * step], wi = sinT_[k * step];
                const int a = start + k, b = a + half;
                const float tr = re_[b] * wr - im_[b] * wi;
                const float ti = re_[b] * wi + im_[b] * wr;
                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
            }
        }
    }
    auto& out = spectra_[written_ & (kHistory - 1)];
    for (int i = 0; i < kBins; ++i)
        out[i] = std::max((re_[i] * re_[i] + im_[i] * im_[i]) / static_cast<float>(1 << 17), 1e-10f);
}

void SignatureGenerator::spread() {
    auto s = spectra_[written_ & (kHistory - 1)];
    // Frequency: each bin takes the largest of itself and the next two (ascending, so those are still unspread).
    for (int i = 0; i < kBins - 2; ++i) s[i] = std::max({s[i], s[i + 1], s[i + 2]});
    // Time: carried into the spread spectra 1, 3 and 6 hops older, each the maximum of itself and the newer chain.
    for (int i = 0; i < kBins; ++i) {
        float v = s[i];
        for (int back : {1, 3, 6}) {
            auto& older = spread_[(written_ - back) & (kHistory - 1)];
            v = std::max(v, older[i]);
            older[i] = v;
        }
    }
    spread_[written_ & (kHistory - 1)] = s;
}

void SignatureGenerator::findPeaks() {
    const size_t next = written_;   // the slot the next hop will fill
    const auto& cand = spectra_[(next - 46) & (kHistory - 1)];
    const auto& before = spread_[(next - 49) & (kHistory - 1)];
    const uint32_t pass = static_cast<uint32_t>(written_ - 46);
    for (int bin = 10; bin <= 1014; ++bin) {
        const float v = cand[bin];
        if (v < kMinPower || v < before[bin - 1]) continue;
        float around = 0;
        for (int off : kNeighbors) around = std::max(around, before[bin + off]);
        if (v <= around) continue;
        float others = around;
        for (int off : kOtherHops) others = std::max(others, spread_[(next + off) & (kHistory - 1)][bin - 1]);
        if (v <= others) continue;
        const auto mag = [](float p) { return std::log(std::max(kMinPower, p)) * 1477.3f + 6144.f; };
        const float m = mag(v), lo = mag(cand[bin - 1]), hi = mag(cand[bin + 1]);
        const float curve = m * 2 - lo - hi;
        const float shift = curve > 0 ? (hi - lo) * 32 / curve : 0.f;
        const float bin64 = bin * 64.f + shift;
        if (bin64 <= 0 || bin64 >= 65535) continue;
        Peak p;
        p.pass = pass;
        p.magnitude = static_cast<uint16_t>(std::clamp(m, 0.f, 65535.f));
        p.bin = static_cast<uint16_t>(bin64);
        const int band = bandOf(p.hz());
        if (band < 0) continue;
        peaks_[static_cast<size_t>(band)].push_back(p);
    }
}

Signature SignatureGenerator::signature() const {
    Signature s;
    s.samples = fed_;
    s.bands = peaks_;
    return s;
}

Signature makeSignature(const float* samples, size_t count) {
    SignatureGenerator g;
    g.feed(samples, count);
    return g.signature();
}

} // namespace st::recognize
