#pragma once
// The built-in decoder path next to Media Foundation's (internal to st_audio; Decoder picks it): Ogg Opus (opusfile),
// Ogg Vorbis (vorbisfile) and WebM / Matroska with an Opus or Vorbis track (audio/Matroska + libopus / libvorbis), read
// from the same ProgressiveBuffer, so downloads, memory-mapped temporary files, local mappings, range requests and
// interrupted reads behave as they do for Media Foundation.
//
// Output: interleaved float stereo (mono doubled, multichannel downmixed) at the stream's rate, or resampled
// (audio/Resampler) to the rate the RatePolicy picks. Timestamps are exact: opusfile / vorbisfile seek sample-accurately;
// WebM seeks to the cue before (target - SeekPreRoll) and hands out the frames from there with their positions (the
// Track drops those before the target). Opus pre-skip / CodecDelay and the last packet's end trimming (Ogg granule
// position, WebM DiscardPadding) are applied, so a song has its exact length and gapless handoffs line up. The output
// gain of an OpusHead is applied; R128_TRACK_GAIN / REPLAYGAIN_TRACK_GAIN tags are reported (replayGainDb).
//
// Not thread-safe: one Track decode thread.
#include "audio/Decoder.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <windows.h>

namespace st::audio {

class ProgressiveBuffer;

class XiphDecoder {
public:
    enum class Kind { None, Ogg, Matroska };
    // What the stream starts with (reads 4 bytes, blocking). hr: S_OK, E_ABORT (interrupted) or MF_E_NET_READ (failed).
    static Kind probe(ProgressiveBuffer& buffer, HRESULT& hr);

    XiphDecoder();
    ~XiphDecoder();
    XiphDecoder(const XiphDecoder&) = delete;
    XiphDecoder& operator=(const XiphDecoder&) = delete;

    // S_OK; S_FALSE: not ours after all (an Ogg FLAC / Speex stream, a Matroska file with an AAC track...: Media
    // Foundation may play it); E_ABORT interrupted, MF_E_NET_READ the buffer failed, else a decode error.
    HRESULT open(const std::shared_ptr<ProgressiveBuffer>& buffer, Kind kind, const RatePolicy& rates);
    void close();
    bool isOpen() const;

    uint32_t sampleRate() const { return outRate_; }
    uint32_t channels() const { return 2; }
    int64_t durationHns() const { return durationHns_; }
    const char* codecName() const;   // "Opus", "Vorbis"
    // Track gain in dB towards -18 LUFS from the stream's tags (Opus: R128_TRACK_GAIN + 5 dB), if it has one.
    std::optional<float> replayGainDb() const { return replayGain_; }

    HRESULT seek(int64_t hns);
    Decoder::Status read(std::vector<float>& out, int64_t& timestampHns, HRESULT& hr);

private:
    struct Impl;
    HRESULT readFailure() const;
    bool readSource(std::vector<float>& stereo, int64_t& srcPos, bool& eos, HRESULT& hr);

    std::unique_ptr<Impl> impl_;
    uint32_t srcRate_ = 0, outRate_ = 0;
    int64_t durationHns_ = 0;
    std::optional<float> replayGain_;
};

} // namespace st::audio
