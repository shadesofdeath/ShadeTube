#include "video/SyncScheduler.h"

#include <algorithm>
#include <cmath>

namespace st::video {

void SyncScheduler::restart(int64_t targetUs) {
    awaitFirst_ = true;
    targetUs_ = std::max<int64_t>(0, targetUs);
}

SyncScheduler::Pick SyncScheduler::resyncTo(int64_t clockUs, double rate, double nowMs, int queued) {
    lastResyncMs_ = nowMs;
    ++stats_.resyncs;
    Pick p;
    p.resync = true;
    // Aim where the clock will be when the decoder gets there: a seek on the network takes a while.
    const double leadUs = std::clamp(seekLatencyMs_ * 1000.0 * 1.2, static_cast<double>(kSeekLeadUs), 3'000'000.0);
    p.resyncUs = std::max<int64_t>(0, clockUs + static_cast<int64_t>(leadUs * std::clamp(rate, 0.0, 3.0)));
    p.consume = queued;
    restart(p.resyncUs);
    return p;
}

SyncScheduler::Pick SyncScheduler::pick(const std::vector<FrameTime>& q, int64_t clockUs, double rate, double nowMs,
                                        bool decoderEnded) {
    const int n = static_cast<int>(q.size());
    // A jump of the clock: where it is against where it should be after the real time that passed (at either rate:
    // a resume or a speed change in between moves it by something between the two).
    if (haveClock_) {
        const double elapsedUs = std::max(0.0, nowMs - lastNowMs_) * 1000.0;
        const double lo = lastClockUs_ + elapsedUs * std::min(rate, lastRate_);
        const double hi = lastClockUs_ + elapsedUs * std::max(rate, lastRate_);
        const double c = static_cast<double>(clockUs);
        lastClockUs_ = clockUs;
        lastRate_ = rate;
        lastNowMs_ = nowMs;
        if (c < lo - kJumpUs || c > hi + kJumpUs) return resyncTo(clockUs, rate, nowMs, n);
    } else {
        haveClock_ = true;
        lastClockUs_ = clockUs;
        lastRate_ = rate;
        lastNowMs_ = nowMs;
    }
    const bool cooled = nowMs - lastResyncMs_ >= kResyncCooldownMs;
    auto show = [&](int i) {
        Pick p;
        p.show = i;
        p.consume = i + 1;
        shownPts_ = q[i].ptsUs;
        shownDur_ = q[i].durUs;
        ++stats_.shown;
        stats_.lastErrorUs = q[i].ptsUs - clockUs;
        return p;
    };

    // How far the decoder's newest output is behind the clock: it catches up by skipping (cheap: nothing is copied or
    // shown), and only a long way behind with frames on hand it is moved by a seek (which skips the download too).
    const double scale = std::max(1.0, rate);
    const int64_t newest = n > 0 ? q.back().ptsUs : shownPts_ != kNone && !awaitFirst_ ? shownPts_ : targetUs_;
    const int64_t behind = decoderEnded ? 0 : clockUs - newest;
    const bool farBehind = cooled && (n > 0 || (!awaitFirst_ && shownPts_ != kNone)) && behind > static_cast<int64_t>(kFarBehindUs * scale);
    const int64_t catchUp = behind > static_cast<int64_t>(kCatchUpUs * scale) ? clockUs + static_cast<int64_t>(kCatchUpLeadUs * scale) : -1;

    if (awaitFirst_) {
        // Frames from before the target are leftovers of the decoder's way from the key frame: never shown.
        int first = 0;
        while (first < n && q[first].ptsUs + std::max<int64_t>(q[first].durUs, 1) <= targetUs_) ++first;
        if (first < n) {
            // The newest one that is due, else the first one (early: the old scene would be wrong).
            int i = first;
            while (i + 1 < n && q[i + 1].ptsUs <= clockUs + kToleranceUs) ++i;
            stats_.dropped += static_cast<uint64_t>(i - first);
            awaitFirst_ = false;
            Pick p = show(i);
            p.catchUpUs = catchUp;
            return p;
        }
        // Still on its way (opening, the network, the key frame): never aimed again, which would restart the wait;
        // what it decodes before the clock by the time it gets there is skipped.
        Pick p;
        p.consume = first;
        p.catchUpUs = catchUp;
        return p;
    }

    if (farBehind) return resyncTo(clockUs, rate, nowMs, n);
    int due = -1;
    for (int i = 0; i < n && q[i].ptsUs <= clockUs + kToleranceUs; ++i) due = i;
    if (due >= 0) {
        stats_.dropped += static_cast<uint64_t>(due);
        Pick p = show(due);
        p.catchUpUs = catchUp;
        return p;
    }
    // Nothing due: the picture on screen stays (repeats). A decoder far ahead (the clock went back without a jump the
    // checks above saw) is moved to the clock.
    if (n > 0 && cooled && q.front().ptsUs - clockUs > kAheadUs) return resyncTo(clockUs, rate, nowMs, n);
    Pick p;
    p.catchUpUs = catchUp;
    return p;
}

} // namespace st::video
