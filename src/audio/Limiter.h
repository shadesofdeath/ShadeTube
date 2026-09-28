#pragma once
// Look-ahead peak limiter for the engine's output (internal to st_audio; tests use it too).
//
// The last stage before WASAPI: whatever the equalizer, a normalisation boost or a hot master push over the ceiling
// (-0.3 dBFS) is turned down smoothly instead of clipped. Design: the gain each frame needs is min-filtered over the
// look-ahead window and then averaged over the same window, and the audio is delayed by that window, so the gain is
// already down when a peak comes out (no overshoot) and never jumps (no clicks); it recovers over ~150 ms. Audio that
// stays under the ceiling passes unchanged, 5 ms late.
//
// Threading: one instance per thread (the engine thread). O(1) per frame.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace st::audio {

class Limiter {
public:
    static constexpr float kCeiling = 0.966f;   // -0.3 dBFS
    static constexpr float kLookaheadMs = 5.f;
    static constexpr float kReleaseMs = 150.f;

    // Sets the format; clears the delay line when it changes.
    void configure(uint32_t sampleRate, uint32_t channels);
    // Clears the delay line and the gain state (the output restarts from silence).
    void reset();
    // In place: out[i] = in[i - lookahead] * gain. Frames of silence flush the delay line.
    void process(float* interleaved, size_t frames);

    uint32_t latencyFrames() const { return lookahead_; }
    float gainReduction() const { return 1.f - release_; }   // current reduction (0 = none), for tests / meters

private:
    uint32_t rate_ = 0, channels_ = 0, lookahead_ = 0;
    std::vector<float> delay_;              // lookahead_ frames, interleaved, circular
    size_t pos_ = 0;
    std::deque<std::pair<uint64_t, float>> minq_;   // (frame index, required gain), increasing gains
    std::vector<float> boxRing_;            // the last lookahead_ min-filtered gains
    double boxSum_ = 0;
    size_t boxPos_ = 0;
    uint64_t frame_ = 0;
    float release_ = 1.f;                   // min-filtered gain after the release slew
    float releaseStep_ = 0;
};

} // namespace st::audio
