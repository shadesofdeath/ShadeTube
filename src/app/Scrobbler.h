#pragma once
// Scrobbling to Last.fm and ListenBrainz.
//
//   Last.fm      : the user brings their own API key + secret (last.fm/api/account/create). Desktop web-auth:
//                  auth.getToken -> the user approves https://www.last.fm/api/auth/?api_key=..&token=.. in the
//                  browser -> auth.getSession -> session key. Calls are signed (MD5, Windows CNG) and POSTed.
//   ListenBrainz : a user token (listenbrainz.org/settings), validated with /1/validate-token.
//
// Scrobble rule (both services): the track is longer than 30 s and the REAL listened time (accumulated only
// while playing, from position deltas; seeks are ignored) reaches min(50% of the duration, 4 minutes). Each
// play is scrobbled once, stamped with the unix time the track started playing.
//
// Credentials persist DPAPI-encrypted (CurrentUser) as JSON in %LOCALAPPDATA%\ShadeTube\scrobble.dat. They
// are never logged. All network runs on the worker pool; every callback runs on the UI thread and nothing
// throws out of this class. Failed scrobbles stay in an in-memory retry queue per service and are re-sent on
// the next scrobble / after the next successful submission.
//
// Threading: every public member is UI-thread only (the class owns a Lifetime that guards continuations).
// Standalone: depends on core + catalog only (no app headers).
#include "catalog/Models.h"
#include "core/Async.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::app {

struct ScrobbleTrack {
    std::string id;            // stable track id: onTrackStarted() with the same id may continue the same play
    std::string title;
    std::string artist;        // primary artist (Last.fm matches best on a single artist)
    std::string artistCredit;  // full credit "A, B" for ListenBrainz; empty = artist
    std::string album;
    int64_t durationMs = 0;    // 0 = unknown (then learned from onProgress's duration argument)
};

// catalog::Track -> ScrobbleTrack (first artist as `artist`, artistLine() as `artistCredit`).
ScrobbleTrack scrobbleTrackFrom(const catalog::Track& t);

namespace scrobble {

using Params = std::vector<std::pair<std::string, std::string>>;

// Lowercase hex MD5 of the bytes (Windows CNG). Throws std::runtime_error only if CNG itself fails.
std::string md5Hex(std::string_view bytes);

// Last.fm api_sig: md5(concat(name + value for every param sorted by name, excluding "format" and
// "callback") + secret), lowercase hex. Strings are hashed as their UTF-8 bytes.
std::string lastfmSignature(const Params& params, std::string_view secret);

// Pure listened-time accounting (no I/O, no clock of its own) - exposed for tests.
class ListenClock {
public:
    static constexpr int64_t kMinTrackMs = 30'000;          // tracks <= 30 s never scrobble
    static constexpr int64_t kMaxThresholdMs = 240'000;     // 4 minutes
    static constexpr int64_t kSeekToleranceMs = 1'500;      // position may run this far ahead of the wall clock

    // Starts a new play. wallMs: the monotonic clock "now"; the position baseline is 0.
    void reset(int64_t durationMs, int64_t wallMs);
    // Only fills in an unknown duration (the catalog duration wins once known).
    void learnDuration(int64_t durationMs);
    // Feeds one sample. A forward jump larger than the elapsed wall time (+tolerance) or any backward jump is
    // a seek and is not counted. Returns true exactly once: on the sample that crosses the threshold.
    bool update(int64_t positionMs, bool playing, int64_t wallMs);

    int64_t durationMs() const { return durationMs_; }
    int64_t listenedMs() const { return listenedMs_; }
    // min(duration / 2, 4 min); 4 min when the duration is unknown; -1 when the track is too short.
    int64_t thresholdMs() const;
    bool fired() const { return fired_; }

private:
    int64_t durationMs_ = 0;
    int64_t listenedMs_ = 0;
    int64_t lastPosMs_ = 0;
    int64_t lastWallMs_ = 0;
    bool lastPlaying_ = false;
    bool fired_ = false;
};

} // namespace scrobble

class Scrobbler {
public:
    // storeFile: where the encrypted credentials live; empty = %LOCALAPPDATA%\ShadeTube\scrobble.dat.
    explicit Scrobbler(std::filesystem::path storeFile = {});
    ~Scrobbler();
    Scrobbler(const Scrobbler&) = delete;
    Scrobbler& operator=(const Scrobbler&) = delete;

    // Reads scrobble.dat (tiny; DPAPI decrypt). Safe to call once at startup on the UI thread.
    void load();

    // --- Last.fm ---------------------------------------------------------------------------------------
    // Stores the user's API key + shared secret (trimmed). An empty secret with an unchanged key keeps the
    // stored secret (so a settings form can leave the secret field blank). Changing the key drops the session.
    void lastfmSetKeys(std::string apiKey, std::string secret);
    bool lastfmHasKeys() const;
    bool lastfmConnected() const;
    std::string lastfmUser() const;
    std::string lastfmApiKey() const;          // for pre-filling the settings field (the secret is never exposed)
    bool lastfmHasSecret() const;
    bool lastfmAuthPending() const;            // beginAuth succeeded; waiting for finishAuth
    // auth.getToken -> done(url, "") with the approval URL to open in the browser, or done("", error).
    void lastfmBeginAuth(std::function<void(std::string url, std::string error)> done);
    // auth.getSession with the pending token -> done(true, "") once the session key is stored.
    void lastfmFinishAuth(std::function<void(bool ok, std::string error)> done);
    void lastfmDisconnect();                   // forgets the session (keeps the API key/secret)

    // --- ListenBrainz ----------------------------------------------------------------------------------
    // Validates the token; on success stores it and calls done(true, userName), else done(false, error).
    void listenbrainzSetToken(std::string token, std::function<void(bool ok, std::string userOrError)> done);
    bool listenbrainzConnected() const;
    std::string listenbrainzUser() const;
    void listenbrainzDisconnect();

    // --- Playback --------------------------------------------------------------------------------------
    // A (new) track became current. Same id as the current play: the next progress sample decides - continuing
    // at about the same position (re-resolve, "Yanlış eşleşme?" switch, resume after an error) is the same play
    // (listened time and "already scrobbled" are kept); restarting from the top (repeat) is a new play.
    void onTrackStarted(const ScrobbleTrack& t);
    // Call periodically (every 1-2 s) with the player position. playing = audio is actually advancing
    // (not paused / buffering). durationMs (optional) fills in an unknown catalog duration.
    // "Now playing" is sent on the first playing sample of a play; the scrobble when the rule is met.
    void onProgress(int64_t positionMs, bool playing, int64_t durationMs = 0);
    // Playback stopped / queue cleared / logout: forget the current play (nothing is scrobbled).
    void onStopped();

    bool enabled = true;                       // master switch (Settings "scrobble"): false = send nothing

    // UI-thread notifications.
    std::function<void()> onChanged;           // a connection changed (e.g. a revoked session was dropped)
    std::function<void(const ScrobbleTrack&, int64_t timestamp)> onScrobble;   // a play qualified (while enabled)
    // Monotonic milliseconds used for seek detection. Default: steady_clock. Tests inject their own.
    std::function<int64_t()> clock;

    size_t pendingLastfm() const { return lfmQueue_.size(); }
    size_t pendingListenbrainz() const { return lbQueue_.size(); }

private:
    struct Pending {
        uint64_t seq = 0;
        ScrobbleTrack track;
        int64_t timestamp = 0;   // unix seconds
    };

    void save() const;
    void startPlay(const ScrobbleTrack& t, int64_t wallMs);
    int64_t nowMonotonic() const;
    void changed();
    void sendNowPlaying();
    void submitScrobble();
    void enqueue(std::deque<Pending>& q, Pending p);
    void flushLastfm();
    void flushListenbrainz();
    void dropLastfmSession(const char* why);
    void dropListenbrainzToken(const char* why);

    std::filesystem::path file_;
    Lifetime life_;

    // Credentials (never logged).
    std::string lfmKey_, lfmSecret_, lfmSession_, lfmUser_;
    std::string lfmPendingToken_;
    std::string lbToken_, lbUser_;

    // Current play.
    bool active_ = false;
    bool playStarted_ = false;       // first playing sample seen ("now playing" sent)
    ScrobbleTrack track_;
    int64_t startedAtUnix_ = 0;
    scrobble::ListenClock listen_;
    int64_t lastPositionMs_ = 0;     // last sampled position of the current play
    bool restartCheck_ = false;      // same id started again: the next sample tells a repeat from a re-resolve
    int64_t restartWallMs_ = 0;      // monotonic time of that onTrackStarted (start of a repeat)

    // Retry queues (oldest first) + in-flight guards.
    std::deque<Pending> lfmQueue_, lbQueue_;
    bool lfmBusy_ = false, lbBusy_ = false;
    uint64_t seq_ = 0;
    uint64_t lfmGen_ = 0, lbGen_ = 0;   // bumped on (dis)connect so stale continuations are ignored
};

} // namespace st::app
