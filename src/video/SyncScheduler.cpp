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
    p.resyncUs = std::max<int64_t>(0, clockUs + static_cast<int64_t>(kSeekLeadUs * std::clamp(rate, 0.0, 3.0)));
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
    const int64_t lateUs = static_cast<int64_t>(kLateUs * std::max(1.0, rate));
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
            return show(i);
        }
        Pick p;
        p.consume = first;
        // Nothing from the new place for a long time (slow network, a decoder that can't keep up): aim again.
        if (!decoderEnded && cooled && clockUs - targetUs_ > lateUs) return resyncTo(clockUs, rate, nowMs, n);
        return p;
    }

    // The decoder fell behind: even its newest frame is long past.
    if (n > 0 && !decoderEnded && cooled && q.back().ptsUs + lateUs < clockUs) return resyncTo(clockUs, rate, nowMs, n);
    int due = -1;
    for (int i = 0; i < n && q[i].ptsUs <= clockUs + kToleranceUs; ++i) due = i;
    if (due >= 0) {
        stats_.dropped += static_cast<uint64_t>(due);
        return show(due);
    }
    // Nothing due: the picture on screen stays (repeats). A decoder far ahead, or one that delivers nothing while the
    // clock runs away from the last picture, is moved to the clock.
    if (n > 0 && cooled && q.front().ptsUs - clockUs > kAheadUs) return resyncTo(clockUs, rate, nowMs, n);
    if (n == 0 && !decoderEnded && cooled && shownPts_ != kNone && clockUs - (shownPts_ + shownDur_) > lateUs)
        return resyncTo(clockUs, rate, nowMs, 0);
    return {};
}

} // namespace st::video
