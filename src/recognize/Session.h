#pragma once
// One listening run of "Şarkıyı tanı": capture (recognize/Capture) -> a growing signature (recognize/Signature) ->
// the recognition request (recognize/Shazam) at a few checkpoints, until a song is found, the time is up, nothing
// is heard, or stop().
//
// Schedule: a request once 4 s of sound are in, again at 7 s and at 11 s (each signature covers everything heard so
// far, at most 12 s: the service answers nothing for 15 s or more). An answer with a song ends the run, unless it came
// from a short signature with a time / frequency skew above confidentSkew: a few seconds of a song can also match a
// copy or a track sampling it, so the next, longer signature confirms it (its answer wins; with none the first one
// stands). No song at the last checkpoint is NotFound. Silence: when the loudest 32 ms block of the first 3 s stays
// under -54 dBFS (nothing plays, a muted microphone) the run ends as Silent without a request. Audio arriving while a
// request is out keeps being added; a request runs on its own thread, so stop() is never held up by the network.
//
// The audio exists only in this object's memory and goes with it; only signatures are sent.
//
// Threading: construct / stop() / level() on any one thread (the UI's); events come on the session's own thread
// ("st-recognize"): the app posts them to the UI thread. stop() and the destructor return within ~50 ms.
#include "recognize/Capture.h"
#include "recognize/Shazam.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace st::recognize {

enum class Phase {
    Listening,     // capturing (and maybe a request out)
    Identifying,   // a request is out
    Matched,       // done: `song`
    NotFound,      // done: heard enough, no song
    Silent,        // done: nothing to hear
    Failed,        // done: `failure`
};

enum class Failure { None, NoDevice, DeviceBusy, AccessDenied, Capture, Network, RateLimited, Service };

struct Event {
    Phase phase = Phase::Listening;
    std::optional<Song> song;
    Failure failure = Failure::None;
    int status = 0;             // HTTP status of a Service / RateLimited failure
    double heardSec = 0;        // audio captured so far
    int requests = 0;           // requests sent so far
    bool ownSoundIncluded = false;   // the system sound includes ShadeTube's own (no process loopback)
    bool confirming = false;    // an answer came and the session listens on to confirm it
    bool final() const { return phase != Phase::Listening && phase != Phase::Identifying; }
};

struct Options {
    Source source = Source::System;
    std::vector<double> checkpoints{4, 7, 11};   // seconds of audio
    double maxSeconds = 12;                       // the longest signature
    double silenceWindowSec = 3;
    float silenceLevel = 0.002f;                  // -54 dBFS
    double confidentSkew = 5e-4;                  // an earlier checkpoint's answer is taken at once up to this skew
    // Tests: the service (default: identify()) and, instead of a device, 16 kHz mono audio handed out in real time.
    std::function<Answer(const Signature&, const YoutubeExplode::CancellationToken&)> service;
    std::shared_ptr<const std::vector<float>> testAudio;
    double testSpeed = 1;                         // test audio: how much faster than real time
};

class Session {
public:
    Session(Options options, std::function<void(const Event&)> onEvent);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Ends the run: the capture stops at once, a request out is abandoned, no more events.
    void stop();
    float level() const;   // 0..1, the capture's last block peak
    double heardSec() const { return heard_.load(); }

private:
    struct Shared;
    void run(std::stop_token st);
    std::shared_ptr<Shared> shared_;
    Options options_;
    std::atomic<double> heard_{0};
    std::atomic<float> testLevel_{0};
    std::unique_ptr<Capture> capture_;
    std::jthread thread_;
};

const char* phaseName(Phase p);

} // namespace st::recognize
