#pragma once
// Compressed audio -> interleaved 32-bit float PCM, stereo, at the rate a RatePolicy picks (internal to st_audio):
//   - built-in (audio/XiphDecoder): Ogg Opus, Ogg Vorbis, WebM / Matroska with an Opus or Vorbis track — on every
//     Windows, whatever codecs are installed;
//   - everything else (AAC/MP4, MP3, FLAC, WAV, WMA, ALAC, Matroska with other codecs, Ogg with other codecs when a
//     handler is installed): the Media Foundation Source Reader, whose resampler converts the rate (Windows 8+).
//
// Not thread-safe: owned and used by exactly one Track decode thread (which must have COM
// initialised and MFStartup called by the engine).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace st::audio {

class ProgressiveBuffer;
class XiphDecoder;

// The sample rate a track decodes to. Shared output: `fixed`, the device's mix rate (every track the same, so they hand
// over gaplessly and crossfade). Exclusive output (fixed 0): the stream's own rate when the device takes it (`accepted`
// empty = any rate), else the nearest the device takes (an integer multiple of the stream's rate first).
struct RatePolicy {
    uint32_t fixed = 0;
    std::vector<uint32_t> accepted;
    uint32_t pick(uint32_t native) const;
};

class Decoder {
public:
    enum class Status { Ok, EndOfStream, Error };

    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Opens the stream in the buffer. On failure returns the HRESULT and sets `unsupported` when the failure means
    // "no byte-stream handler / decoder for this format".
    HRESULT open(const std::shared_ptr<ProgressiveBuffer>& buffer, const std::string& mimeType, bool& unsupported,
                 const RatePolicy& rates = {});
    void close();
    bool isOpen() const;

    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t channels() const { return channels_; }
    int64_t durationHns() const { return durationHns_; }  // 0 if unknown
    bool builtIn() const { return xiph_ != nullptr; }      // the Opus / Vorbis path
    // A built-in stream's ReplayGain track gain from its own tags (Ogg comments), NaN when it has none.
    float tagGainDb() const;

    HRESULT seek(int64_t hns);

    // Decodes the next sample, APPENDING interleaved floats to `out`. `timestampHns` is the
    // sample's presentation time (-1 if absent). `formatChanged` is set when the output type
    // changed (sampleRate()/channels() updated before the appended data).
    Status read(std::vector<float>& out, int64_t& timestampHns, bool& formatChanged, HRESULT& hr);

private:
    HRESULT configureOutput(const RatePolicy& rates);

    std::unique_ptr<XiphDecoder> xiph_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFByteStream> stream_;
    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
    int64_t durationHns_ = 0;
};

} // namespace st::audio
