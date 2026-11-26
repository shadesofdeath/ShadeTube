#pragma once
// Packet decoders over the vendored Xiph libraries (internal to st_audio): Opus (libopus multistream decoder: mapping
// families 0, 1, 2 and 255) and Vorbis (libvorbis synthesis), fed one compressed packet at a time by a demuxer
// (WebM / Matroska in XiphDecoder, Ogg pages from a live stream in LiveDecoder). Ogg files are decoded through opusfile /
// vorbisfile instead, which do their own paging and seeking.
//
// Not thread-safe: one decode thread per instance.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct OpusMSDecoder;

namespace st::audio::xiph {

struct OpusHead {
    uint32_t channels = 0;
    uint32_t preSkip = 0;          // 48 kHz samples to drop at the start
    uint32_t inputRate = 0;        // the encoder's input rate (informational: Opus always decodes at 48 kHz here)
    int outputGainQ8 = 0;          // dB in Q7.8, applied by the decoder
    uint32_t family = 0;
    uint32_t streams = 1, coupled = 0;
    std::vector<uint8_t> mapping;  // one entry per channel
};
std::optional<OpusHead> parseOpusHead(const uint8_t* p, size_t n);
// Samples per channel at 48 kHz in an Opus packet (0 = not a valid packet).
int opusPacketSamples(const uint8_t* p, size_t n);

class OpusPacketDecoder {
public:
    OpusPacketDecoder() = default;
    ~OpusPacketDecoder();
    OpusPacketDecoder(const OpusPacketDecoder&) = delete;
    OpusPacketDecoder& operator=(const OpusPacketDecoder&) = delete;

    bool open(const uint8_t* head, size_t n);   // false: not an OpusHead this decoder can use (e.g. family 3)
    void close();
    bool isOpen() const { return dec_ != nullptr; }
    const OpusHead& head() const { return head_; }
    uint32_t channels() const { return head_.channels; }
    void reset();   // decoder state forgotten (a seek / a gap); the next packets converge within ~80 ms
    // Appends the packet's 48 kHz frames (interleaved, head().channels channels); the frame count, or < 0 if corrupt.
    int decode(const uint8_t* p, size_t n, std::vector<float>& out);

private:
    OpusMSDecoder* dec_ = nullptr;
    OpusHead head_;
    std::vector<float> scratch_;
};

class VorbisPacketDecoder {
public:
    VorbisPacketDecoder();
    ~VorbisPacketDecoder();
    VorbisPacketDecoder(const VorbisPacketDecoder&) = delete;
    VorbisPacketDecoder& operator=(const VorbisPacketDecoder&) = delete;

    // The identification, comment and setup headers (as separate packets, or Xiph-laced as in Matroska's CodecPrivate).
    // synthesis false: only read them (blockSize() works, decode() doesn't; a demuxer's packet durations).
    bool open(const std::vector<std::vector<uint8_t>>& headers, bool synthesis = true);
    bool openLaced(const uint8_t* p, size_t n);
    void close();
    bool isOpen() const;
    uint32_t sampleRate() const;
    uint32_t channels() const;
    void reset();   // after a seek: the next packet only primes the decoder (no output)
    // Appends the packet's frames (interleaved); the frame count, or < 0 if corrupt.
    int decode(const uint8_t* p, size_t n, std::vector<float>& out);
    // Block size of an audio packet (frames between packets = (previous + this) / 4); 0 if it isn't one.
    int blockSize(const uint8_t* p, size_t n) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

// Splits Matroska's Xiph-laced CodecPrivate (count - 1, the sizes of all but the last, the packets).
std::optional<std::vector<std::vector<uint8_t>>> splitXiphLacing(const uint8_t* p, size_t n);

// Interleaved frames of `ch` channels in Vorbis channel order (Opus mapping family 1 uses it too) appended as stereo:
// mono doubled; 3..8 channels downmixed with -3 dB centre and surrounds, LFE left out, normalised so full-scale input
// cannot overshoot. Stereo is copied.
void appendStereo(const float* in, size_t frames, uint32_t ch, std::vector<float>& out);

// Vorbis comments ("KEY=value", KEY case-insensitive) of an OpusTags / Vorbis comment packet body after its magic.
std::vector<std::pair<std::string, std::string>> parseComments(const uint8_t* p, size_t n);

} // namespace st::audio::xiph
