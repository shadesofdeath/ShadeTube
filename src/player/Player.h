#pragma once
// Playback orchestration ("the Spotube logic"): queue, shuffle, repeat, YouTube resolution, prefetch of
// the next track, gapless handoff and error recovery. Lives on the UI thread; all heavy work (matching,
// manifest fetches) runs on the pool and is cancelled/ignored when superseded.
#include "audio/AudioEngine.h"
#include "core/Async.h"
#include "core/Settings.h"
#include "catalog/Models.h"
#include "youtube/MatchService.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace st::player {

using catalog::Track;

enum class Status { Idle, Resolving, Buffering, Playing, Paused, Error };

struct PlayContext {
    std::string uri;          // "album:<mbid>", "artist:<mbid>", "playlist:local:<id>", "liked", "search", ...
    std::wstring name;        // shown in the queue header ("Gece Sürüşü")
};

class Player {
public:
    explicit Player(youtube::MatchService& matcher);
    ~Player();

    // --- commands (UI thread) -------------------------------------------------------------------
    // startIndex = the row the user picked: it plays even when shouldSkip() says no (kara liste). -1 = no row picked
    // (a header / card play button): a random track the player may pick by itself when shuffle is on, else the first
    // one; if there is none, nothing starts and onError reports it. A caller that always wants the first such track,
    // even with shuffle on, passes firstPlayable(tracks).
    void playContext(std::vector<Track> tracks, int startIndex, PlayContext context);
    int firstPlayable(const std::vector<Track>& tracks) const;   // playable and not skipped, or -1
    // Keeps the current track playing and replaces the rest of the queue with `tracks` (a radio of the song).
    void replaceUpcoming(std::vector<Track> tracks, PlayContext context);
    void playNext(const Track& track);            // insert right after the current item
    void enqueue(const std::vector<Track>& tracks);
    // Inserts tracks (unplayable ones dropped) into the play order at `orderIndex`, clamped between the slot after the
    // current one and the end (drag and drop onto the queue). An empty queue starts playing them; a queue that had run
    // out continues with the first of them. Returns #inserted.
    int insertAt(int orderIndex, const std::vector<Track>& tracks);
    void togglePause();
    void play();
    void pause();
    void next();
    void previous();
    void seek(int64_t ms);
    void jumpTo(int orderIndex);                  // index in upcoming()/play order
    void removeAt(int orderIndex);
    void move(int fromOrderIndex, int toOrderIndex);
    void clearQueue();                            // keeps the current track
    void setVolume(float v);
    // Pushes Ayarlar > SES to the engine (equalizer, output device). Crossfade and loudness normalisation are read
    // from Settings for every track as it is loaded / preloaded.
    void applyAudioSettings();
    void setShuffle(bool on);
    void setRepeat(RepeatMode mode);
    void cycleRepeat();
    // User picked another YouTube video in "Yanlış eşleşme?": pin it and continue at the same position.
    void useMatch(const youtube::Match& match);

    // --- state -----------------------------------------------------------------------------------
    Status status() const { return status_; }
    bool isPlaying() const { return status_ == Status::Playing || status_ == Status::Buffering; }
    const Track* current() const;
    // Play order: order()[i] indexes into items(); currentOrderIndex() is the playing slot.
    const std::vector<Track>& items() const { return items_; }
    const std::vector<int>& order() const { return order_; }
    int currentOrderIndex() const { return pos_; }
    const PlayContext& context() const { return context_; }
    std::optional<youtube::Match> currentMatch() const { return match_; }
    const youtube::StreamInfo& currentStream() const { return stream_; }
    bool shuffle() const { return shuffle_; }
    RepeatMode repeat() const { return repeat_; }
    float volume() const { return engine_->volume(); }

    int64_t positionMs() const;
    int64_t durationMs() const;
    float bufferedFraction() const;
    bool spectrum(float* bands, int count) const { return engine_->spectrum(bands, count); }
    std::string lastError() const { return lastError_; }
    size_t bufferedBytes() const { return engine_->bufferedBytes(); }

    // Fired on the UI thread whenever state/track/queue changes (not for position ticks).
    std::function<void()> onChanged;
    std::function<void(const Track&)> onTrackChanged;
    std::function<void(const std::wstring& message)> onError;   // for toasts

    // Offline playback: asked (on the UI thread) before each resolve — return the local file path for a
    // downloaded track to play it from disk instead of YouTube, or "" to resolve normally.
    std::function<std::wstring(const std::string& trackId)> localFileFor;
    // Media type hint for a local file, by extension (mp3, m4a/mp4/aac, flac, wav, wma, ogg/opus, webm, mka).
    // "" = unknown (Media Foundation sniffs the content).
    static std::string localMimeType(const std::wstring& path);

    // Kara liste: asked (UI thread) whenever the player picks an item by itself — auto-advance, next / previous,
    // the start of a context played without a start index (header play / shuffle), the repeat wrap and the
    // prefetch. true = pass over it. An item the user picks directly (playContext's start index, jumpTo) still plays.
    std::function<bool(const Track&)> shouldSkip;
    // Endless playback: fired (UI thread, posted) when a track starts with at most one playable item left after it
    // and repeat off, and again when the queue runs out. The handler may append later with extend().
    std::function<void()> onQueueLow;
    // Playable (not skipped) items after the current slot, counting at most `limit`.
    int remainingPlayable(int limit = 1 << 30) const;
    // Bumped by playContext(): a new queue (features keep per-queue state against it).
    uint64_t queueGeneration() const { return queueGen_; }
    // Appends tracks (unplayable ones dropped) at the end of the play order, after a shuffled remainder too. When
    // playback had stopped at the end of the queue it continues with the first appended track. Returns #appended.
    int extend(const std::vector<Track>& tracks);
    // The skip rule changed (kara liste edit): drops a prepared next track that is now skipped and notifies.
    void skipRulesChanged();

    // Internet radio: asked (UI thread) before each resolve, like localFileFor. A returned stream makes the item a
    // live, endless stream: no duration, no seeking, no prefetch / gapless handoff; the engine reconnects when the
    // stream drops. mimeType "" = let the engine sniff (audio/mpeg, audio/aac, audio/ogg, HLS playlists...).
    // positionMs() is the listening time since the item started; pause keeps only a few seconds and play resumes
    // live. next / previous walk the queue (the station list); a restored session resumes the station live.
    struct LiveStream {
        std::string url;
        std::string mimeType;
        bool allowLocalNetwork = false;   // tests with a local server; stations reach only the public internet
    };
    std::function<std::optional<LiveStream>(const Track&)> liveStreamFor;
    bool isLive() const { return live_; }            // the current item is a live stream
    // The station's current song (ICY "StreamTitle", e.g. "Artist - Title"), "" when it sends none. Changes fire
    // onLiveTitle on the UI thread (and onChanged).
    const std::wstring& liveTitle() const { return liveTitle_; }
    std::function<void(const std::wstring& title)> onLiveTitle;
    // Codec, bitrate, station name (icy-name), buffer and reconnect count of the live item (active = false otherwise).
    audio::LiveInfo liveInfo() const { return engine_->liveInfo(); }
    // A live item failed for good (UI thread, before the error toast and the move to the next item): kind is
    // UnsupportedFormat for a codec / container this engine can't play (see AudioEngine::supportsLiveCodec) and
    // Network when the station could not be reached or kept dropping.
    std::function<void(const Track& station, audio::ErrorKind kind)> onLiveFailed;

    // Resume support (last session). Set liveStreamFor first, so a restored station shows as live.
    void restoreSession();
    void saveSession() const;

private:
    struct Prepared {
        uint64_t tag = 0;
        int orderIndex = -1;
        std::string trackId;   // the item prepared (the gapless handoff re-checks its slot)
        youtube::Resolved resolved;   // empty for a local file
        audio::StreamSource source;   // what the engine preloaded (localPath set for a local file)
    };

    void loadCurrent(int64_t startMs, bool autoplay);
    void startResolve(int orderIndex, int64_t startMs, bool autoplay, bool bypassStreamCache);
    void maybePrefetch();
    void onEngineState(audio::State s, uint64_t tag);
    void onEngineEnded(uint64_t finished, uint64_t next);
    void onEngineError(audio::ErrorKind kind, const std::string& message, uint64_t tag);
    void onEngineTitle(const std::string& title, uint64_t tag);
    void clearLiveTitle();
    void advance(bool userInitiated);
    bool skipped(int orderIndex) const;
    int playableFrom(int orderIndex, int step) const;   // first non-skipped slot walking by `step`, or -1
    int countPlayable() const;                          // non-skipped slots in the whole play order
    int upcomingSlot() const;                           // the slot that plays after the current one, or -1
    void dropPrepared();                                // forget the prepared next track + any prefetch in flight
    void stopAtEnd();
    void checkQueueLow();
    void scheduleErrorAdvance();                        // after a terminal failure: advanceAfterError() in 1.5 s
    void advanceAfterError();
    void restoreSessionFile();
    void rebuildOrder(int keepItemIndex);
    void notify();
    audio::StreamSource sourceFor(const youtube::Resolved& r, uint64_t tag, int64_t durationHint) const;
    audio::StreamSource localSource(const std::wstring& path, uint64_t tag, int64_t durationHint) const;
    int crossfadeInto(int orderIndex) const;   // crossfade length (ms) from the current item into that slot, 0 = gapless

    youtube::MatchService& matcher_;
    std::unique_ptr<audio::AudioEngine> engine_;
    Lifetime life_;

    std::vector<Track> items_;
    std::vector<int> order_;
    int pos_ = -1;
    PlayContext context_;
    bool shuffle_ = false;
    RepeatMode repeat_ = RepeatMode::Off;

    Status status_ = Status::Idle;
    uint64_t tagCounter_ = 0;
    uint64_t currentTag_ = 0;           // tag of the load in flight / playing
    std::optional<youtube::Match> match_;
    youtube::StreamInfo stream_;
    std::optional<Prepared> prepared_;  // next track resolved (and handed to engine.preload)
    uint64_t prefetchTag_ = 0;          // prefetch resolve in flight
    std::string preloadFailed_;         // the track whose preload the engine rejected (not preloaded again)
    int retries_ = 0;
    int64_t pendingSeekMs_ = -1;
    bool allowWebm_ = false;
    bool currentLocal_ = false;         // the current item plays from a local file (no YouTube fallbacks)
    int failStreak_ = 0;                // consecutive tracks that failed to start (stops a queue that can't play)
    uint64_t errorTag_ = 0;             // the load scheduleErrorAdvance() is for
    bool endedAtEnd_ = false;           // stopped because the queue ran out (extend() continues)
    uint64_t queueGen_ = 0;
    std::string lastError_;
    std::shared_ptr<YoutubeExplode::CancellationTokenSource> resolveCts_;
    std::shared_ptr<YoutubeExplode::CancellationTokenSource> prefetchCts_;
    UINT_PTR timer_ = 0;
    bool live_ = false;                 // current item plays through liveStreamFor (internet radio)
    std::wstring liveTitle_;            // ICY StreamTitle of the current live item
};

} // namespace st::player
