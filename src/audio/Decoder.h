#pragma once
// Media Foundation Source Reader wrapper: compressed audio (AAC/MP4, Opus/WebM) -> interleaved
// 32-bit float PCM, stereo, at a requested sample rate (the Source Reader's resampler, Windows 8+) or else the stream's
// native one (internal to st_audio).
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

class Decoder {
public:
    enum class Status { Ok, EndOfStream, Error };

    Decoder() = default;
    ~Decoder() { close(); }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Creates media source + source reader over the buffer. On failure returns the HRESULT and sets
    // `unsupported` when the failure means "no byte-stream handler / decoder for this format".
    // `targetRate` (0 = native): the sample rate to decode to, so tracks of different rates share one output format.
    HRESULT open(const std::shared_ptr<ProgressiveBuffer>& buffer, const std::string& mimeType, bool& unsupported,
                 uint32_t targetRate = 0);
    void close();
    bool isOpen() const { return reader_ != nullptr; }

    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t channels() const { return channels_; }
    int64_t durationHns() const { return durationHns_; }  // 0 if unknown

    HRESULT seek(int64_t hns);

    // Decodes the next sample, APPENDING interleaved floats to `out`. `timestampHns` is the
    // sample's presentation time (-1 if absent). `formatChanged` is set when the output type
    // changed (sampleRate()/channels() updated before the appended data).
    Status read(std::vector<float>& out, int64_t& timestampHns, bool& formatChanged, HRESULT& hr);

private:
    HRESULT configureOutput();

    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFByteStream> stream_;
    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
    uint32_t targetRate_ = 0;
    int64_t durationHns_ = 0;
};

} // namespace st::audio
