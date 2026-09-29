#pragma once
// Tempo change without a pitch change, for playback speed (internal to st_audio; tests use it too).
//
// WSOLA, the way SoundTouch's time-domain stretcher works: the output is built from input "sequences". Each one starts
// where the previous sequence's tail (the overlap) best continues — the offset with the highest cross-correlation in a
// short seek window — crossfades that tail into it, copies the rest and keeps its own tail for the next crossfade. The
// input advances by (sequence - overlap) x tempo per sequence, so the output runs at 1 / tempo of the input's length
// while every waveform period stays intact. Sequence and seek lengths follow the tempo (longer when slowed down).
//
// Input is pulled on demand through a callback (the engine reads its Track); every produced block reports the input
// frame it starts at, so positions stay in track time. One instance per thread (the engine thread).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace st::audio {

class TimeStretch {
public:
    static constexpr double kMinTempo = 0.5, kMaxTempo = 3.0;

    // `pull(dst, frames, firstFrame)` reads up to `frames` input frames (interleaved) and returns how many it read;
    // `firstFrame` receives the input position of the first one. 0 = nothing right now.
    using Pull = std::function<size_t(float* dst, size_t frames, int64_t& firstFrame)>;

    void configure(uint32_t sampleRate, uint32_t channels);   // clears on a format change
    void setTempo(double tempo);                              // clamped to [kMinTempo, kMaxTempo]
    double tempo() const { return tempo_; }
    void clear();                                             // drops buffered input and output (seek, new track)
    bool empty() const { return in_.empty() && pendingOff_ * channels_ >= pending_.size() && !haveTail_; }

    // Produces up to `frames` output frames into `out`; `firstPos` = input position of the first one (-1 if none were
    // produced). When `pull` has nothing and `inputEnded()` says the source is done, the buffered input is flushed.
    size_t process(float* out, size_t frames, int64_t& firstPos, const Pull& pull, const std::function<bool()>& inputEnded);

private:
    void updateLengths();
    bool haveSequenceInput() const;
    void runSequence();
    void flushRemainder();
    size_t seekBest() const;

    uint32_t rate_ = 0, channels_ = 0;
    double tempo_ = 1.0;
    size_t seq_ = 0, overlap_ = 0, seek_ = 0;   // frames
    std::vector<float> in_;                     // buffered input, interleaved
    int64_t inPos_ = -1;                        // input position of in_[0]
    double skipFract_ = 0;                      // fractional input advance carried to the next sequence
    uint64_t skipDebt_ = 0;                     // input frames a fast hop went past that weren't read yet
    std::vector<float> tail_;                   // the previous sequence's overlap tail
    bool haveTail_ = false;
    std::vector<float> pending_;                // produced output not handed out yet
    size_t pendingOff_ = 0;                     // frames of pending_ already handed out
    double pendingPos_ = -1;                    // input position of the next frame handed out
    std::vector<float> scratch_;
};

} // namespace st::audio
