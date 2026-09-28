#pragma once
// Network side of a live (endless) internet-radio stream (internal to st_audio). One thread per stream:
//
//   station URL -> playlists (.pls / .m3u / .asx, nested, redirects followed)
//     -> ICY / HTTP body ("Icy-MetaData: 1"; interleaved metadata stripped, StreamTitle kept)
//        or HLS (master -> media playlist, refreshed; MPEG-TS / packed audio / fMP4 segments, AES-128 decrypted)
//     -> live::FrameParser / OggDemuxer -> a bounded queue of compressed frames -> Track decode thread.
//
// The queue is the rolling buffer: at most kMaxQueueMs of audio and kMaxQueueBytes (when it is full the thread stops
// reading, i.e. TCP back-pressure; nothing is dropped). While paused only the newest kPausedKeepMs (and never more
// than kMaxQueueBytes) are kept, and after kSuspendAfterMs the connection is closed; resuming then reconnects. A
// dropped connection reconnects with exponential backoff (0.5 s .. 8 s); the stream fails with ErrorKind::Network after
// kMaxReconnects failed attempts in a row (kMaxFirstAttempts before anything ever played), and with UnsupportedFormat
// for what the engine can't decode (Ogg Vorbis / FLAC, AC-3 or LATM in HLS, SAMPLE-AES, an HTML page, no MPEG / AAC
// frames at all). Attempts that fail while paused never count (nobody hears the gap): the stream keeps retrying until
// the resume or, after kSuspendAfterMs, closes as above; the resume then starts with a fresh count.
//
// Unless `allowLocalNetwork` (tests with a server on 127.0.0.1), every request goes to the public internet only
// (HttpStream::setPublicOnly): a station's URL, playlist entries, redirects and HLS segments come from a public directory.
//
// Threading: the stream thread produces; the decode thread consumes with pop(); setPaused() / info() / cancel()
// from the engine thread or any other.
#include "audio/AudioEngine.h"
#include "audio/LiveParsers.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace st::audio {

class HttpStream;

class LiveStream {
public:
    static constexpr int64_t kMaxQueueMs = 60'000;
    static constexpr size_t kMaxQueueBytes = 4 * 1024 * 1024;
    static constexpr int64_t kPausedKeepMs = 4'000;
    static constexpr int64_t kSuspendAfterMs = 60'000;
    static constexpr int kMaxReconnects = 6;
    static constexpr int kMaxFirstAttempts = 3;

    // `memCounter` (optional) accounts the queued bytes with the engine's other buffers.
    LiveStream(std::string url, std::string mimeHint, std::atomic<int64_t>* memCounter, bool allowLocalNetwork = false);
    ~LiveStream();   // cancel() + join()
    LiveStream(const LiveStream&) = delete;
    LiveStream& operator=(const LiveStream&) = delete;

    void start();
    void cancel();   // any thread, non-blocking
    void join();     // after cancel(): waits for the thread (quick) and frees the queue
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    enum class Pop { Frame, Cancelled, Failed };
    // Decode thread: blocks until a frame is queued. Failed only once the queue is drained.
    Pop pop(live::Frame& out);
    int64_t bufferedMs() const;

    void setPaused(bool paused);
    // After setPaused(false): true when audio was dropped (or the connection closed) while paused, so what the
    // decoder still holds is stale. Resets the flag.
    bool takeStale();

    ErrorKind errorKind() const;
    std::string errorMessage() const;
    // The decoder's output format (the Track reports it; HE-AAC shows as a doubled rate).
    void setDecodedFormat(uint32_t sampleRate, uint32_t channels);
    LiveInfo info() const;

private:
    enum class End { Dropped, Ended, Suspended, Fatal, Cancelled };   // Ended: a finite body / HLS ENDLIST played out
    struct Outcome {
        End end = End::Dropped;
        ErrorKind kind = ErrorKind::Network;
        std::string message;
    };

    void run();
    Outcome session(HttpStream& http, int attempt);
    Outcome pumpBody(HttpStream& http, std::string head);
    Outcome hlsSession(HttpStream& http, std::string url, std::string body);
    bool fetch(HttpStream& http, const std::string& url, int64_t offset, int64_t length, size_t maxBytes, std::string& body,
               std::string& error);
    bool push(live::Frame&& f);        // false: cancelled
    bool suspendDue();                 // paused for too long: close the connection
    void suspend();                    // closed after suspendDue(): drop the queue, wait for the resume
    void attachTitle(live::Frame& f);
    void markDiscontinuity();
    void fail(ErrorKind kind, std::string message);
    bool sleep(uint32_t ms);           // false: cancelled

    const std::string url_;
    const std::string mimeHint_;
    std::atomic<int64_t>* memCounter_;
    const bool allowLocalNetwork_;
    void* cancelEvent_ = nullptr;      // HANDLE, manual-reset

    mutable std::mutex mutex_;
    std::condition_variable dataCv_;   // consumer: frames / failure / cancel
    std::condition_variable spaceCv_;  // producer: queue space / resume / cancel
    // ---- guarded by mutex_
    std::deque<live::Frame> queue_;
    int64_t queuedUs_ = 0;
    size_t queuedBytes_ = 0;
    bool paused_ = false;
    uint64_t pausedAt_ = 0;
    bool suspended_ = false;
    bool stale_ = false;
    bool discontinuity_ = false;       // the next pushed frame follows lost audio
    bool failed_ = false;
    ErrorKind errorKind_ = ErrorKind::Network;
    std::string error_;
    LiveInfo info_;
    live::Codec codec_ = live::Codec::None;
    uint32_t codedRate_ = 0;
    uint32_t headerKbps_ = 0;          // icy-br
    // ---- stream thread only
    std::optional<std::string> pendingTitle_;
    std::string lastTitle_;
    int64_t deliveredUs_ = 0;          // audio pushed by the current session
    int64_t measuredBytes_ = 0, measuredUs_ = 0;

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

} // namespace st::audio
