// Listen together: clock offset estimation and the guest's reconciliation (see JamSync.h).
#include "app/JamSync.h"

#include <algorithm>
#include <cstdlib>

namespace st::app::jam {

namespace {

int64_t median(std::vector<int64_t> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

} // namespace

// ---- Clock --------------------------------------------------------------------------------------------------------------

void ClockEstimator::add(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
    const int64_t rtt = (t4 - t1) - (t3 - t2);
    if (t4 < t1 || t3 < t2 || rtt < 0) return;
    const int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
    samples_.push_back({offset, rtt});
    while (samples_.size() > kMaxSamples) samples_.pop_front();
}

int64_t ClockEstimator::offsetMs() const {
    if (samples_.empty()) return 0;
    std::vector<Sample> s(samples_.begin(), samples_.end());
    std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
    s.resize((s.size() + 1) / 2);   // the faster half
    std::vector<int64_t> offsets;
    for (const auto& x : s) offsets.push_back(x.offset);
    return median(std::move(offsets));
}

int64_t ClockEstimator::rttMs() const {
    if (samples_.empty()) return 0;
    std::vector<Sample> s(samples_.begin(), samples_.end());
    std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
    s.resize((s.size() + 1) / 2);
    std::vector<int64_t> rtts;
    for (const auto& x : s) rtts.push_back(x.rtt);
    return median(std::move(rtts));
}

// ---- Reconciliation -----------------------------------------------------------------------------------------------------

bool hostAdvancing(const SessionState& s) { return s.playing && s.status == HostStatus::Playing; }

int64_t expectedPosition(const SessionState& s, int64_t hostNowMs) {
    int64_t pos = s.positionMs;
    if (hostAdvancing(s)) pos += std::max<int64_t>(0, hostNowMs - s.atHostMs);
    if (const QueueItem* cur = s.currentItem(); cur && cur->durationMs > 0) pos = std::min(pos, cur->durationMs);
    return std::max<int64_t>(0, pos);
}

void learnSeekLead(SyncMemory& mem, int64_t residualMs) {
    // Behind after the seek (residual < 0): aim further ahead next time; ahead: less. Half the error per seek.
    mem.seekLeadMs = std::clamp<int64_t>(mem.seekLeadMs - residualMs / 2, 0, 1500);
}

Decision decide(const SessionState& host, int64_t hostNowMs, int64_t guestNowMs, const LocalView& local, SyncMemory& mem,
                const SyncTuning& tuning) {
    Decision d;
    const QueueItem* cur = host.currentItem();
    auto holdPaused = [&](Decision::Hold why) {
        d.hold = why;
        if (local.playing) d.pause = true;
        return d;
    };
    if (!cur || (host.status == HostStatus::Idle && !host.playing)) return holdPaused(Decision::Hold::Idle);
    if (cur->kind == ItemKind::Unsupported) return holdPaused(Decision::Hold::Unsupported);
    if (mem.failed.count(cur->id)) return holdPaused(Decision::Hold::Failed);

    const bool advancing = hostAdvancing(host);
    const int64_t expected = expectedPosition(host, hostNowMs);
    const int64_t duration = cur->durationMs > 0 ? cur->durationMs : local.durationMs;
    auto clampSeek = [&](int64_t ms) {
        const int64_t top = duration > 0 ? std::max<int64_t>(0, duration - 250) : ms;
        return std::clamp<int64_t>(ms, 0, top);
    };
    std::vector<std::string> hostUpcoming;
    for (size_t i = static_cast<size_t>(host.current) + 1; i < host.queue.size(); ++i) hostUpcoming.push_back(host.queue[i].id);
    auto noteAttempt = [&] {
        if (mem.attemptId == cur->id) ++mem.attempts;
        else {
            mem.attemptId = cur->id;
            mem.attempts = 1;
        }
        mem.errorSeen = false;
    };

    if (local.currentId != cur->id) {
        // The host is still finding the YouTube video of a song: wait for it (the next state names it).
        if (cur->kind == ItemKind::Song && cur->videoId.empty() && host.status == HostStatus::Loading)
            return holdPaused(Decision::Hold::Waiting);
        if (mem.attemptId == cur->id && mem.attempts >= tuning.maxLoadAttempts) {
            mem.failed.insert(cur->id);
            return holdPaused(Decision::Hold::Failed);
        }
        int k = -1;
        for (int i = 0; i < static_cast<int>(local.upcomingIds.size()) && i < tuning.jumpLookahead; ++i)
            if (local.upcomingIds[static_cast<size_t>(i)] == cur->id) {
                k = i;
                break;
            }
        // About to hand over to it by itself (gapless / crossfade): let it.
        if (k == 0 && local.playing && !local.buffering && local.durationMs > 0 && local.durationMs - local.positionMs < 1500) {
            d.hold = Decision::Hold::Waiting;
            return d;
        }
        noteAttempt();
        if (k >= 0) {
            d.load = Decision::Load::Jump;
            d.jumpIndex = k;
            d.pause = !advancing && !host.playing;
        } else {
            d.load = Decision::Load::Queue;
            d.autoplay = advancing;
            d.seekMs = clampSeek(advancing ? expected + mem.seekLeadMs : host.positionMs);
            mem.lastSeekAt = guestNowMs;
            mem.seekPending = advancing && d.seekMs > 0;
        }
        return d;
    }

    // The same item.
    d.replaceUpcoming = local.upcomingIds != hostUpcoming;
    if (local.error) {
        if (mem.attemptId != cur->id) {
            mem.attemptId = cur->id;
            mem.attempts = 0;
            mem.errorSeen = false;
        }
        if (!mem.errorSeen) {
            ++mem.attempts;
            mem.errorSeen = true;
        }
        if (mem.attempts >= tuning.maxLoadAttempts) {
            mem.failed.insert(cur->id);
            d.hold = Decision::Hold::Failed;
        }
        return d;
    }
    if (local.buffering) return d;   // positions mean nothing yet; follow once it plays
    if (local.playing && mem.attemptId == cur->id) {   // it plays: earlier failures of this item are forgiven
        mem.attempts = 0;
        mem.errorSeen = false;
    }

    const int64_t target = advancing ? expected : host.positionMs;
    d.driftMs = local.positionMs - target;
    d.hasDrift = true;
    const bool cooled = guestNowMs - mem.lastSeekAt >= tuning.seekCooldownMs;
    const bool nearEnd = duration > 0 && expected >= duration - tuning.endGuardMs;
    if (advancing) {
        if (mem.seekPending && local.playing && guestNowMs - mem.lastSeekAt >= 800) {
            learnSeekLead(mem, d.driftMs);
            mem.seekPending = false;
        }
        if (!local.playing) {
            d.play = true;
            // Starting anyway: line up now (no audible jump) unless it is already close.
            if (std::llabs(d.driftMs) > 150 && !nearEnd) {
                d.seekMs = clampSeek(expected + mem.seekLeadMs);
                mem.lastSeekAt = guestNowMs;
                mem.seekPending = true;
            }
        } else if (std::llabs(d.driftMs) > tuning.driftThresholdMs && cooled && !nearEnd) {
            d.seekMs = clampSeek(expected + mem.seekLeadMs);
            mem.lastSeekAt = guestNowMs;
            mem.seekPending = true;
        }
        return d;
    }
    // The host doesn't advance: paused (follow the pause and its position) or loading (a short rebuffer: keep going,
    // the next state corrects the position).
    if (host.status == HostStatus::Loading && host.playing) return d;
    if (local.playing) d.pause = true;
    if (std::llabs(d.driftMs) > tuning.driftThresholdMs && cooled) {
        d.seekMs = clampSeek(host.positionMs);
        mem.lastSeekAt = guestNowMs;
        mem.seekPending = false;
    }
    return d;
}

} // namespace st::app::jam
