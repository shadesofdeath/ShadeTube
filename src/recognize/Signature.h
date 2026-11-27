#pragma once
// Audio signature in the format Shazam's recognition service reads ("data:audio/vnd.shazam.sig;base64,..."): the
// spectral peaks of 16 kHz mono audio, grouped in four frequency bands. Pure (no I/O); worker threads.
//
// How it is made (the published description of the format: SongRec, ShazamIO; this is ShadeTube's own code):
//   - 128-sample hops (8 ms). Each hop, the last 2048 samples, Hann-windowed (a 2050-point Hann without its two zero
//     ends), go through a 2048-point FFT; the power |X|^2 / 2^17 of bins 0..1024 (at least 1e-10) is kept for the last
//     256 hops.
//   - A copy of each spectrum is "spread": every bin becomes the maximum of itself and the next two bins, then carried
//     back in time into the spread spectra 1, 3 and 6 hops older (each takes the maximum of itself and the newer one).
//   - A bin of the spectrum 46 hops back is a peak when it is at least 1/64, at least the spread spectrum 49 hops back
//     one bin lower, above that spread spectrum at bins -10, -7, -4, -3, +1, +2, +5, +8 around it, and above 14 other
//     spread spectra (hops -53, -45, -91, -84, ... -7 from the newest, one bin lower). Bins 10..1014 are looked at.
//   - A peak's magnitude is ln(max(1/64, p)) * 1477.3 + 6144; its frequency is refined by a parabola through the
//     magnitudes of the bins around it, in 1/64 bin steps (Hz = bin64 * 16000 / 2 / 1024 / 64). Peaks between 250 Hz
//     and 5.5 kHz are kept, in four bands: 250-520, 520-1450, 1450-3500, 3500-5500 Hz.
//
// Binary layout (little endian): a 48-byte header (magic 0xcafe2580, CRC-32 of everything after its first 8 bytes,
// the size after the header, magic 0x94119c00, the sample rate id << 27 (16 kHz = 3), samples + 0.24 s worth of
// samples, the fixed value 0x7c0000), then 0x40000000 and the size again, then per band with peaks: 0x60030040 + band,
// the byte length of its peaks, the peaks (zero-padded to 4 bytes). A peak is the hop delta since the previous peak
// of the band as one byte (a delta of 255 or more is first written as 0xff + the absolute hop as 4 bytes), the
// magnitude (2 bytes) and the frequency (2 bytes).
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace st::recognize {

inline constexpr uint32_t kSampleRate = 16000;   // what the signature is made from
inline constexpr int kHop = 128;                 // samples per FFT pass
inline constexpr int kBands = 4;

struct Peak {
    uint32_t pass = 0;        // FFT pass (hop) number from the start of the audio
    uint16_t magnitude = 0;   // ln(power) * 1477.3 + 6144
    uint16_t bin = 0;         // frequency in 1/64 FFT bins
    double hz() const { return bin * (static_cast<double>(kSampleRate) / 2 / 1024 / 64); }
    double seconds() const { return pass * static_cast<double>(kHop) / kSampleRate; }
    bool operator==(const Peak&) const = default;
};

struct Signature {
    uint32_t sampleRate = kSampleRate;
    uint32_t samples = 0;                        // audio it was made from
    std::array<std::vector<Peak>, kBands> bands;   // 250-520, 520-1450, 1450-3500, 3500-5500 Hz; by pass

    size_t peakCount() const;
    double seconds() const { return sampleRate ? static_cast<double>(samples) / sampleRate : 0; }
    std::vector<uint8_t> encode() const;
    std::string dataUri() const;   // "data:audio/vnd.shazam.sig;base64,<encode()>"
    // Reads encode()'s format back (tests). nullopt when the magic, the sizes, the CRC or a band is wrong.
    static std::optional<Signature> decode(std::span<const uint8_t> bytes);
};

// The band a frequency falls in, -1 outside 250 Hz - 5.5 kHz.
int bandOf(double hz);

// Streaming peak finder over 16 kHz mono audio. Samples are floats in -1..1 (scaled to 16-bit levels inside, as the
// format expects). Not thread-safe.
class SignatureGenerator {
public:
    SignatureGenerator();
    void reset();
    void feed(const float* samples, size_t count);
    void feed(const std::vector<float>& samples) { feed(samples.data(), samples.size()); }
    // Every peak found so far, for the audio fed since the last reset (the last ~0.4 s are still pending: a peak is
    // only decided once 46 more hops are in).
    Signature signature() const;
    uint32_t samplesFed() const { return fed_; }

private:
    void process(const float* hop);   // exactly kHop samples
    void fft();
    void spread();
    void findPeaks();

    std::vector<float> ring_;                 // last 2048 samples
    size_t ringPos_ = 0;
    std::vector<float> pending_;              // < kHop samples waiting for the next hop
    std::vector<std::array<float, 1025>> spectra_, spread_;   // 256 hops each
    size_t written_ = 0;                      // hops done
    std::vector<float> window_;
    std::vector<float> re_, im_;              // FFT work
    std::vector<float> cosT_, sinT_;
    std::vector<uint16_t> bitrev_;
    std::array<std::vector<Peak>, kBands> peaks_;
    uint32_t fed_ = 0;
};

// Convenience: the signature of a whole buffer.
Signature makeSignature(const float* samples, size_t count);

// Base64 (RFC 4648, with padding) and the CRC-32 the header carries (zlib's), exposed for tests.
std::string base64(std::span<const uint8_t> bytes);
uint32_t crc32(std::span<const uint8_t> bytes);

} // namespace st::recognize
