#pragma once
// Video mode's decoder: one picture stream (no audio) on its own thread.
//
//   googlevideo (ranged requests, audio/ProgressiveBuffer with a read-ahead window: a few seconds ahead of the decoder
//   only) -> audio/MfByteStream -> Media Foundation Source Reader (video stream only) -> decoded frames
//
// Hardware path (a D3D11 device is given): the Source Reader gets a DXGI device manager on the app's own device, so the
// stock H.264 decoder (and the VP9 / AV1 extensions) decode with DXVA into the decoder's surface pool. Each frame is
// copied on the GPU into one of a few NV12 slot textures and the sample goes back to the decoder at once (holding
// decoder surfaces could starve its pool). The UI thread converts a slot to BGRA for Direct2D (VideoPlayer). The device
// must be multithread protected (VideoPlayer does it); the copy runs inside Direct2D's lock (ID2D1Multithread), so it
// never lands in the middle of a Direct2D batch.
// Software path (no device / no DXVA): the Source Reader's own converter delivers 32-bit RGB in system memory.
//
// Slots: a frame waits in the queue until the UI takes it (take()); the decoder pauses when every slot is full, so it
// never runs more than a handful of frames ahead and a paused player costs nothing. seek() drops the queue and skips
// frames before the target on the way from the key frame.
//
// Threading: public methods are UI thread (any thread for the getters). stop() / the destructor cancel the download
// (a blocked read fails at once) and join the thread, which releases every Media Foundation and D3D object it made.
#include "video/Fmp4Index.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID2D1Multithread;

namespace st::audio {
class ProgressiveBuffer;
}

namespace st::video {

struct Source {
    std::string url;               // the stream URL; empty with localPath
    std::wstring localPath;        // a local file (dev: SHADETUBE_VIDEO_FILE)
    int64_t contentLength = 0;     // bytes (0 = probed)
    std::string mimeType;          // "video/mp4" | "video/webm"
    int64_t readAheadBytes = 0;    // download window ahead of the decoder (0 = the whole stream)
};

enum class DecoderState { Opening, Running, Ended, Failed };

// Format of the decoded frames (known once the state left Opening).
struct VideoFormat {
    int width = 0, height = 0;         // visible picture (aperture), pixels
    int texWidth = 0, texHeight = 0;   // decoded surface (H.264 1080p decodes to 1088 rows)
    double fps = 0;
    bool hardware = false;             // NV12 textures (else BGRA in memory)
    bool bt709 = true;                 // YUV matrix (BT.601 otherwise)
    bool fullRange = false;            // 0-255 luma (else 16-235)
};

class VideoDecoder {
public:
    struct Frame {
        int64_t ptsUs = 0, durUs = 0;
        int slot = -1;
    };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;   // hardware: NV12
        std::vector<uint8_t> pixels;                         // software: BGRA, width * 4 per row
    };

    // `device` / `d2dLock`: the app's device (multithread protected) and its Direct2D factory's lock, or nullptr for
    // software decoding. Starts the thread at once; frames begin at `startUs`.
    VideoDecoder(Source source, ID3D11Device* device, ID2D1Multithread* d2dLock, int64_t startUs);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void seek(int64_t us);
    // Catching up without a seek: frames that end before `us` are decoded but not delivered (queued ones are dropped).
    void skipUntil(int64_t us);
    void stop();
    // How long the last seek took from the request to its first frame (ms; 0 before the first).
    double lastSeekLatencyMs() const { return seekLatencyMs_.load(std::memory_order_relaxed); }

    DecoderState state() const { return state_.load(std::memory_order_acquire); }
    std::string error() const;
    bool expired() const { return expired_.load(); }   // HTTP 403 / 404 / 410: the URL needs a refresh
    VideoFormat format() const;
    // Bumped when the slots were made anew (a format change): textures / views built on old slots are stale.
    uint32_t slotGeneration() const { return slotGen_.load(std::memory_order_acquire); }

    // UI side. The frames ready now (pts order) into `out`.
    void peek(std::vector<Frame>& out) const;
    // Takes the first `count` frames off the queue; their slots go back to the decoder.
    void take(int count);
    // Slot of a frame that is still in the queue (between peek and take).
    const Slot& slot(int index) const { return slots_[static_cast<size_t>(index)]; }
    float bufferedFraction() const;
    size_t bufferedBytes() const;   // the slots (GPU or memory)

    static constexpr int kSlots = 6;

private:
    void run();
    bool open();                    // the download (and the segment index), then openReader()
    // A Source Reader on the buffer (hardware: DXVA, falling back to software); with a segment index, on a view that
    // starts at the segment holding `atUs`.
    bool openReader(bool hardware, int64_t atUs);
    bool decodeOne();
    void flushQueueLocked();
    void fail(std::string message);

    Source source_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Multithread> d2dLock_;
    std::shared_ptr<audio::ProgressiveBuffer> buffer_;
    struct Mf;   // Media Foundation objects (decode thread only)
    std::unique_ptr<Mf> mf_;
    // Decode thread only: the segment index (fragmented MP4), where the current view starts, the decoder's position.
    Fmp4Index index_;
    int64_t viewStartUs_ = 0;
    int64_t ptsOffsetUs_ = 0;
    int64_t lastPtsUs_ = 0;
    bool checkOffset_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Frame> ready_;
    std::vector<int> free_;
    Slot slots_[kSlots];
    int64_t seekTo_ = -1;            // a seek waiting for the decode thread
    int64_t discardUntilUs_ = 0;     // frames ending before it are skipped (the way from the key frame)
    uint32_t seekGen_ = 0;
    uint64_t seekStartedAt_ = 0;     // GetTickCount64 of the seek whose first frame is awaited (log, seek latency)
    uint64_t seekRequestedAt_ = 0;   // when seek() was called (the start counts from the constructor: 0 = open time)
    int64_t seekTarget_ = 0;
    VideoFormat format_;
    std::string error_;
    bool quit_ = false;
    std::atomic<DecoderState> state_{DecoderState::Opening};
    std::atomic<bool> expired_{false};
    std::atomic<uint32_t> slotGen_{0};
    std::atomic<size_t> slotBytes_{0};
    std::atomic<double> seekLatencyMs_{0};
    std::thread thread_;
};

} // namespace st::video
