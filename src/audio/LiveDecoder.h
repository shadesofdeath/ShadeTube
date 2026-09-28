#pragma once
// Media Foundation decoder MFT fed one compressed frame at a time (internal to st_audio): MPEG audio layer III (MP3
// decoder MFT) and layer I / II (MPEG audio decoder MFT), ADTS AAC incl. HE-AAC v1 / v2 (AAC decoder MFT) and Opus
// (Opus decoder MFT) -> interleaved 32-bit float PCM. Live streams have no container Media Foundation could open as a
// file, so the framing is ours (live::FrameParser) and only the decoder is Media Foundation's.
//
// Not thread-safe: owned by one Track decode thread (COM initialised, MFStartup done by the engine).
#include "audio/LiveParsers.h"

#include <cstdint>
#include <vector>

#include <mftransform.h>
#include <wrl/client.h>

namespace st::audio {

class LiveDecoder {
public:
    LiveDecoder() = default;
    ~LiveDecoder() { close(); }
    LiveDecoder(const LiveDecoder&) = delete;
    LiveDecoder& operator=(const LiveDecoder&) = delete;

    // (Re)creates the decoder for the configuration of `f`. On failure `unsupported` says that no decoder on this
    // system accepts the format (as opposed to a transient error).
    HRESULT open(const live::Frame& f, bool& unsupported);
    bool isOpen() const { return mft_ != nullptr; }
    uint64_t configKey() const { return key_; }   // live::frameConfigKey of the open configuration
    void close();

    // Decodes one frame, APPENDING interleaved floats to `out` in the current output format. The format can change
    // while the first frames decode (HE-AAC doubles the rate, parametric stereo turns mono into stereo).
    HRESULT decode(const live::Frame& f, std::vector<float>& out);
    void flush();   // forget the decoder state (the next frame does not continue the previous one)

    uint32_t sampleRate() const { return rate_; }
    uint32_t channels() const { return channels_; }

private:
    HRESULT chooseOutputType();
    HRESULT drain(std::vector<float>& out);

    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFSample> outSample_;   // reused output sample (when the MFT does not allocate)
    uint64_t key_ = 0;
    bool floatOut_ = false;
    uint32_t bits_ = 0, rate_ = 0, channels_ = 0;
    bool mftAllocates_ = false;
    int64_t time_ = 0;   // 100 ns, running input timestamp
};

} // namespace st::audio
