#pragma once
// In-memory progressive media buffer with random access (internal to st_audio).
//
// One contiguous allocation of `length` bytes is filled by a private download thread using ~1 MB
// range requests (YouTube's googlevideo servers throttle long single connections; short ranged
// requests are served at full speed). Downloaded byte ranges are tracked, so the buffer can be
// read at ANY offset: a read far beyond the download frontier (e.g. the user seeks to 80% right
// after the start) makes the downloader abandon its current request and restart at that offset;
// the skipped gap is filled afterwards.
//
// Networking is raw WinHTTP in asynchronous mode, wrapped so the download thread can wait on
// "operation completed" OR "attention" (cancel / restart request). Every WinHTTP handle is only
// ever touched by the download thread itself, which makes cancellation immediate and race-free.
//
// Threading:
//   - download thread: writes into the not-yet-published part of the buffer, then publishes the
//     new range under `mutex_` and notifies readers.
//   - readers (Media Foundation work-queue threads via MfByteStream): read() blocks until the
//     requested range is available, the buffer is cancelled/failed, or interruptWaiters() is called.
//   - any thread: cancel(), fraction(), failed(), hasWaiters().
//   - owner: release() joins the download thread and frees the memory (call after cancel()).
//
// Local files (`localPath`) use the same machinery: the thread reads the file into memory.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace st::audio {

class ProgressiveBuffer {
public:
    enum class ReadStatus { Ok, Interrupted, Cancelled, Failed };

    // `memCounter` (optional) tracks the bytes held by all buffers of one engine.
    ProgressiveBuffer(std::string url, std::wstring localPath, int64_t contentLength,
                      std::atomic<int64_t>* memCounter);
    ~ProgressiveBuffer();  // cancel() + release()
    ProgressiveBuffer(const ProgressiveBuffer&) = delete;
    ProgressiveBuffer& operator=(const ProgressiveBuffer&) = delete;

    void start();    // spawns the download thread
    void cancel();   // permanent, non-blocking, any thread: aborts the network and fails all reads
    void release();  // joins the download thread and frees the memory (blocks only briefly after cancel)
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    // Blocks until the total length is known. Returns -1 on failure / cancellation.
    int64_t waitForLength();
    int64_t length() const { return length_.load(std::memory_order_acquire); }

    // Blocks until [offset, offset + size) is downloaded (size is clamped to the length).
    ReadStatus read(int64_t offset, void* dst, size_t size);

    // Wakes all currently blocked readers with ReadStatus::Interrupted (later reads work normally).
    void interruptWaiters();
    bool hasWaiters() const { return waiters_.load(std::memory_order_acquire) > 0; }

    float fraction() const;  // downloaded bytes / length (0..1)
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    // Permanent HTTP failure (403/404/410): the URL most likely expired.
    bool expired() const { return expired_.load(std::memory_order_acquire); }
    std::string errorMessage() const;

private:
    enum class Fetch { Ok, Restart, Cancelled, Retry, Fatal };
    struct HttpState;  // WinHTTP session/connection + parsed URL (download thread only)

    void run();
    void runLocal();
    void runHttp();
    // Downloads [start, end) (end = -1: unknown length, probe). `got` = bytes stored.
    // side = the parallel tail request: ignores restart requests and does not touch reqPos_.
    Fetch fetch(HttpState& http, int64_t start, int64_t end, bool side, int64_t& got, std::string& error);
    int waitOp(void* doneEvent, bool side);  // 0 = completed, 1 = cancelled, 2 = restart requested
    bool allocate(int64_t length);
    void fail(std::string message, bool expired);
    bool backoff(int attempt);  // false if cancelled while waiting

    // Range bookkeeping (mutex_ held).
    void addRangeLocked(int64_t begin, int64_t end);
    bool availableLocked(int64_t begin, int64_t end) const;
    // Scheduler view (the reserved tail counts as present):
    int64_t firstMissingLocked(int64_t from) const;     // -1 if nothing missing at/after `from`
    int64_t nextRangeStartLocked(int64_t pos) const;    // first downloaded/reserved start > pos (or length)

    const std::string url_;
    const std::wstring localPath_;
    std::atomic<int64_t>* memCounter_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;           // readers: data / length / failure / interrupt
    std::unique_ptr<uint8_t[]> data_;      // guarded by mutex_ for reads; written outside it by the thread
    int64_t allocated_ = 0;
    std::vector<std::pair<int64_t, int64_t>> ranges_;  // sorted, merged [begin, end)
    int64_t priority_ = 0;                 // where readers want data next
    int64_t reqPos_ = -1;                  // write position of the active request (-1 = none)
    bool restart_ = false;                 // a reader wants the active request abandoned
    int64_t reservedBegin_ = -1;           // [begin, end) being fetched by the parallel tail request;
    int64_t reservedEnd_ = -1;             //   the scheduler treats it as present, readers don't
    uint64_t interruptGen_ = 0;
    std::string error_;

    std::atomic<int64_t> length_{-1};
    std::atomic<int64_t> downloaded_{0};
    std::atomic<int> waiters_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> expired_{false};
    std::atomic<bool> finished_{false};

    void* attention_ = nullptr;    // HANDLE, auto-reset: cancel / restart / side-done for the main loop
    void* cancelEvent_ = nullptr;  // HANDLE, manual-reset: set by cancel() (wakes the side request)
    std::thread thread_;
};

} // namespace st::audio
