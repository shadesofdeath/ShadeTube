#pragma once
// Sample-rate conversion for the built-in decoders (internal to st_audio; tests use it too): a windowed-sinc
// interpolator (Kaiser window, ~80 dB stop band, pass band to 90 % of the lower Nyquist frequency, the filter widened
// when decimating) with a finely sampled, linearly interpolated kernel table, so any rate pair works.
//
// Zero delay: output frame j is the input at time j / outRate, so positions convert exactly (input frame
// i <-> output frame i * outRate / inRate) and a whole stream of N input frames gives ceil(N * outRate / inRate) output
// frames once flushed. Before the first input frame the stream counts as silent (after a reset too).
//
// Interleaved float in and out; not thread-safe (one decode thread).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace st::audio {

class Resampler {
public:
    // Clears everything when anything changes. inRate == outRate: process() copies.
    void configure(uint32_t inRate, uint32_t outRate, uint32_t channels);
    bool passthrough() const { return in_ == out_; }
    uint32_t inRate() const { return in_; }
    uint32_t outRate() const { return out_; }

    // Drops the buffered input: the next frame is input frame `startFrame` of the stream (a seek). Output frames stay
    // on the stream's grid (output frame j is input time j * inRate / outRate), so a seek doesn't shift them by a
    // fraction of a frame; the first one is outputStart().
    void reset(int64_t startFrame = 0);
    int64_t outputStart() const { return outStart_; }
    // Appends the output for `frames` more input frames.
    void process(const float* in, size_t frames, std::vector<float>& out);
    // End of the input: appends the rest (the kernel's look-ahead past the last frame is silence).
    void flush(std::vector<float>& out);

    // Output frames for a stream of `inFrames` input frames (rounded up), and the reverse (rounded down).
    int64_t toOutput(int64_t inFrames) const;
    int64_t toInput(int64_t outFrames) const;

private:
    void produce(std::vector<float>& out, bool final);

    uint32_t in_ = 0, out_ = 0, ch_ = 0;
    int half_ = 0;                 // taps on each side of the interpolation point
    static constexpr int kTable = 256;   // kernel samples per input frame
    std::vector<float> kernel_;    // h(u / kTable - half_), u = 0 .. 2 * half_ * kTable + 1
    std::vector<float> buf_;       // input frames, interleaved; buf_[0] is frame bufStart_
    int64_t bufStart_ = 0;
    int64_t received_ = 0;         // input frames since the reset
    uint64_t next_ = 0;            // next output frame since the reset
    uint64_t phase_ = 0;           // input position of output frame 0 after the reset, in 1 / out_ input frames
    int64_t outStart_ = 0;         // stream output frame of next_ == 0
    std::vector<float> coef_;
};

} // namespace st::audio
