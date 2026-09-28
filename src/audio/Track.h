#pragma once
// One playable item (internal to st_audio):
//   ProgressiveBuffer (download thread) -> Decoder (decode thread) -> PCM ring -> engine thread, or for a live
//   stream (StreamSource::live): LiveStream (stream thread) -> LiveDecoder (decode thread) -> PCM ring.
//
// Threading:
//   - decode thread (owned): waits for the buffer length, opens the decoder, services seek
//     requests and decodes into a ~1 s ring of interleaved float frames. It blocks on a condition
//     variable while the ring is full, so a paused or preloaded track costs 0% CPU.
//   - engine thread: everything public. read() holds the mutex only for a memcpy.
//   Destruction: cancel() is non-blocking (wakes both threads); the destructor joins them, which
//   is quick after cancel(). The engine retires tracks with cancel() and destroys them once
//   finished() so it never waits on a thread.
//
// Live tracks have no duration and no seeking. Their output format may change mid-stream (a station switching
// streams, HE-AAC detected late): the decode thread then waits until the ring is drained and publishes the new
// format; read() returns nothing for a caller still expecting the old one, so the engine stalls and reopens the
// device. Titles are queued with the PCM position they start at and taken by the engine when that is audible.
#include "audio/AudioEngine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace st::audio {

class Decoder;
class LiveStream;
class ProgressiveBuffer;

class Track {
public:
    // `wakeEvent` (HANDLE) is signalled when the engine should look at this track again (format
    // known, data after requestWake(), end of stream, error).
    Track(const StreamSource& source, int64_t startMs, std::atomic<int64_t>* memCounter, void* wakeEvent);
    ~Track();
    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;

    const StreamSource& source() const { return source_; }
    uint64_t tag() const { return source_.tag; }
    const std::shared_ptr<ProgressiveBuffer>& buffer() const { return buffer_; }        // null for live tracks
    const std::shared_ptr<LiveStream>& liveStream() const { return live_; }             // null unless live
    bool live() const { return live_ != nullptr; }

    void cancel();
    bool finished() const;  // decode + download threads have exited

    bool formatReady() const { return formatReady_.load(std::memory_order_acquire); }
    uint32_t sampleRate() const { return sampleRate_.load(std::memory_order_acquire); }  // valid once formatReady()
    uint32_t channels() const { return channels_.load(std::memory_order_acquire); }
    int64_t durationMs() const;

    // Engine side of the ring. `firstFrame` = track position (frames) of the first copied frame. Nothing is copied
    // when the ring holds another format than the caller's `channels` / `rate` (a live format change).
    size_t read(float* dst, size_t frames, int64_t& firstFrame, uint32_t channels, uint32_t rate);
    size_t available() const;     // frames ready in the ring
    size_t capacity() const;      // ring capacity in frames
    bool ended() const;           // decoder reached the end AND the ring is drained
    bool decoderEnded() const;    // decoder reached the end (ring may still hold frames)
    void seek(int64_t ms);        // flushes the ring; the decode thread repositions
    void requestWake() { wakeRequested_.store(true, std::memory_order_release); }

    bool failed() const { return failed_.load(std::memory_order_acquire); }
    ErrorKind errorKind() const;
    // Loudness gain to apply, in dB: StreamSource::gainDb, or for a StreamSource::replayGain file its ReplayGain track
    // gain plus gainDb (0 when the file has none). The tags are read before the decoder opens: final once formatReady().
    float gainDb() const;
    std::string errorMessage() const;

    // ---- live tracks
    // Audio ready ahead of the read position: decoded (ring) + compressed (stream queue).
    int64_t bufferedAheadMs() const;
    // Pausing keeps only the newest seconds upstream. Resuming returns true when what the ring held was stale and
    // has been dropped (the engine then rebuffers from the live edge).
    bool setLivePaused(bool paused);
    // The newest title that starts at or before `audibleMs` (track position), once.
    std::optional<std::string> takeLiveTitle(int64_t audibleMs);

private:
    void run();
    void decodeLoop(Decoder& decoder);
    bool openDecoder(Decoder& decoder);
    void liveLoop();
    bool pushLive(const std::vector<float>& pcm, int64_t& nextPos);   // false: quit
    void setError(ErrorKind kind, std::string message);
    void wake();

    const StreamSource source_;
    std::shared_ptr<ProgressiveBuffer> buffer_;
    std::shared_ptr<LiveStream> live_;
    const int64_t liveStartMs_ = 0;   // live: position the clock starts at
    void* wakeEvent_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;  // decode thread: ring space / seek / quit
    // ---- guarded by mutex_
    std::vector<float> ring_;
    size_t capFrames_ = 0, readIdx_ = 0, count_ = 0;
    int64_t readPos_ = 0;      // track frame index of ring_[readIdx_]
    bool posValid_ = false;    // readPos_ is set by the first push after a seek
    bool seekPending_ = false;
    int64_t seekTarget_ = 0;   // ms
    bool decEof_ = false;
    bool quit_ = false;
    bool producerWaiting_ = false;
    bool liveFlush_ = false;               // live resume dropped the ring: drop pending PCM, continue at readPos_
    std::deque<std::pair<int64_t, std::string>> liveTitles_;   // (track frame, title)
    ErrorKind errorKind_ = ErrorKind::Other;
    std::string errorMessage_;
    // ----
    // Set before formatReady_ is published; a live track changes them (under mutex_) only while the ring is empty.
    std::atomic<uint32_t> sampleRate_{0}, channels_{0};
    std::atomic<bool> formatReady_{false};
    std::atomic<int64_t> durationMs_{0};
    std::atomic<float> replayGainDb_{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<bool> failed_{false};
    std::atomic<bool> inDecoder_{false};      // decode thread is inside a blocking MF call
    std::atomic<bool> wakeRequested_{false};
    std::atomic<bool> decodeDone_{false};
    std::thread thread_;
};

} // namespace st::audio
