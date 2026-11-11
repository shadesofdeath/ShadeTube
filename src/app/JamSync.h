#pragma once
// Listen together ("Birlikte dinle", app/Together): how a guest follows the host.
//
// Clock: guest and host each read their own steady clock (ms). A guest pings the host (t1 = guest send, t2 = host
// receive, t3 = host send, t4 = guest receive); NTP's offset ((t2 - t1) + (t3 - t4)) / 2 is the host clock minus the
// guest clock as seen through that round trip, wrong by at most half its asymmetry. ClockEstimator keeps the last 9
// samples and answers the median offset of the faster half (the slow round trips are the asymmetric ones).
//
// Reconciliation: decide() compares the host's state (its current item, where it is now — position + the time since
// it was sampled, on the estimated host clock — and whether it advances) with what this player does, and says what to
// change: load the host's queue at the host's position, jump to an upcoming item that became current, replace the
// upcoming items, seek when the drift passes a threshold (1 s; at most every 3 s, never while buffering, aiming a
// little ahead to cover the seek itself), play or pause. Items this player failed to play twice are held (paused)
// until the host moves on; podcasts and radio stations (ItemKind::Unsupported) are not followed.
//
// Standalone and pure (tests/jam compiles it): app/JamProtocol only.
#include "app/JamProtocol.h"

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace st::app::jam {

class ClockEstimator {
public:
    // One ping round trip. Samples with a negative round trip (clock jumps) are dropped.
    void add(int64_t t1, int64_t t2, int64_t t3, int64_t t4);
    bool ready() const { return !samples_.empty(); }
    int64_t offsetMs() const;   // host clock = guest clock + offset
    int64_t rttMs() const;      // median round trip of the samples used
    int samples() const { return static_cast<int>(samples_.size()); }
    void reset() { samples_.clear(); }

    static constexpr size_t kMaxSamples = 9;

private:
    struct Sample {
        int64_t offset, rtt;
    };
    std::deque<Sample> samples_;
};

// What the guest's own player is doing.
struct LocalView {
    std::string currentId;                 // "" = nothing current
    std::vector<std::string> upcomingIds;  // after the current item, in play order
    bool playing = false;                  // playing (or buffering towards playing)
    bool buffering = false;                // resolving / loading / buffering: positions aren't meaningful yet
    bool error = false;                    // the current item failed
    int64_t positionMs = 0;
    int64_t durationMs = 0;
};

// What the guest remembers between decisions (owned by the caller, updated by decide()).
struct SyncMemory {
    std::set<std::string> failed;          // items this player could not play (held, not loaded again)
    std::string attemptId;                 // the item the last Load / Jump was for
    int attempts = 0;                      // loads of attemptId that didn't end up playing
    bool errorSeen = false;                // attemptId's current failure was counted
    int64_t lastSeekAt = -1'000'000;       // guest clock of the last seek
    bool seekPending = false;              // the last seek's result is still to be measured (learnSeekLead)
    int64_t seekLeadMs = 250;              // how far ahead a seek aims (the time a seek takes to be heard)
};

struct SyncTuning {
    int64_t driftThresholdMs = 1000;       // re-seek beyond this
    int64_t seekCooldownMs = 3000;
    int64_t endGuardMs = 2000;             // no seeks into the last seconds of a track
    int maxLoadAttempts = 2;
    int jumpLookahead = 3;                 // a host item this close in the local upcoming list is jumped to
};

struct Decision {
    enum class Load { None, Queue, Jump };
    Load load = Load::None;
    int jumpIndex = -1;          // Jump: index in LocalView::upcomingIds
    bool replaceUpcoming = false;
    int64_t seekMs = -1;         // also the start position of Load::Queue
    bool play = false, pause = false;
    bool autoplay = false;       // Load::Queue starts playing
    enum class Hold { None, Idle, Unsupported, Failed, Waiting };
    Hold hold = Hold::None;      // why the guest isn't following (shown in the panel)
    int64_t driftMs = 0;         // local - expected (same item, both advancing)
    bool hasDrift = false;
};

// Where the host is now on its own clock (`hostNowMs`): positionMs + the time since atHostMs while it advances,
// clamped to the item's length.
int64_t expectedPosition(const SessionState& s, int64_t hostNowMs);
bool hostAdvancing(const SessionState& s);   // playing and the host's player reports Playing

Decision decide(const SessionState& host, int64_t hostNowMs, int64_t guestNowMs, const LocalView& local, SyncMemory& mem,
                const SyncTuning& tuning = {});

// After a seek was heard: `residualMs` = local - expected measured once the player plays again. Moves the lead
// towards what the seek really took (bounded 0 .. 1500 ms).
void learnSeekLead(SyncMemory& mem, int64_t residualMs);

} // namespace st::app::jam
