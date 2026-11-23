#pragma once
// Video mode's A/V scheduler: which decoded frame goes on screen for the audio clock, which ones are dropped, and when
// the decoder has to be moved (seek / resync). Pure logic with the times passed in, so tests/video drives it with a
// fake clock.
//
// The audio is the master. `clockUs` is the media time that will be heard while the frame being painted is on screen
// (the engine's heard position, extrapolated to the frame's vblank). The frame shown is the newest one that has started
// by then (pts <= clock + 8 ms, half a 60 Hz refresh): older ones are dropped unshown, a frame that is not due yet
// waits (the current picture repeats). The clock is checked against where it should be after the real time that passed
// at its rate: a jump of more than 400 ms (a seek, a SponsorBlock skip, a listen-together correction, a new track at
// another position) moves the decoder there; so does a decoder that fell more than a second (times the rate) behind or
// sits more than 3 s ahead, at most once per 2 s. After a move, the first frame from the new place is shown at once,
// even before its time (the old picture would be the wrong scene), then the clock takes over again.
#include <cstdint>
#include <vector>

namespace st::video {

struct FrameTime {
    int64_t ptsUs = 0;
    int64_t durUs = 0;
};

struct SchedulerStats {
    uint64_t shown = 0;       // frames put on screen
    uint64_t dropped = 0;     // decoded frames skipped because a newer one was due
    uint64_t resyncs = 0;     // decoder moves asked for (jumps + late / ahead decoders)
    int64_t lastErrorUs = 0;  // shown frame's start minus the clock when it was picked (+ = early)
};

class SyncScheduler {
public:
    static constexpr int64_t kToleranceUs = 8'000;
    static constexpr int64_t kJumpUs = 400'000;
    static constexpr int64_t kLateUs = 1'000'000;
    static constexpr int64_t kAheadUs = 3'000'000;
    static constexpr double kResyncCooldownMs = 2000;
    static constexpr int64_t kSeekLeadUs = 250'000;   // aim a resync this far ahead (times the rate): seeking + decoding

    struct Pick {
        int show = -1;        // index in the queue of the frame to show now; frames before it are dropped. -1 = keep
                              // the picture on screen.
        int consume = 0;      // frames to take off the queue's front (the dropped ones and the shown one)
        bool resync = false;  // move the decoder to resyncUs (restart(resyncUs) is already applied); the queue is stale
        int64_t resyncUs = 0;
    };

    // `queue`: decoded, not yet shown frames in decode order (pts ascending). `rate`: the clock's speed (0 = standing
    // still). `nowMs`: real time. `decoderEnded`: the decoder reached the end of the stream (no more frames coming).
    Pick pick(const std::vector<FrameTime>& queue, int64_t clockUs, double rate, double nowMs, bool decoderEnded);

    // The decoder was (re)started at `targetUs`: the first frame at or after it is shown as soon as it arrives.
    void restart(int64_t targetUs);
    bool waitingForFirst() const { return awaitFirst_; }
    bool hasShown() const { return shownPts_ != kNone; }
    int64_t shownPts() const { return shownPts_; }
    const SchedulerStats& stats() const { return stats_; }

private:
    static constexpr int64_t kNone = INT64_MIN;
    Pick resyncTo(int64_t clockUs, double rate, double nowMs, int queued);

    int64_t shownPts_ = kNone;
    int64_t shownDur_ = 0;
    bool awaitFirst_ = true;
    int64_t targetUs_ = 0;
    bool haveClock_ = false;
    int64_t lastClockUs_ = 0;
    double lastRate_ = 0;
    double lastNowMs_ = 0;
    double lastResyncMs_ = -1e18;
    SchedulerStats stats_;
};

} // namespace st::video
