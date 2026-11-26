#pragma once
// Video mode's picture on the UI thread: a VideoDecoder, the A/V scheduler and the frame on screen as a Direct2D
// bitmap. Every view that shows the video (Now Playing, the video window) calls frame() from its paint() with the
// audio clock at that frame's vblank; the scheduler picks the decoded frame for it and the frame is converted once.
//
// Rendering choice: the video is drawn through the app's own renderer, not a swap chain or composition visual of its
// own. The windows already share one D3D11 device (gfx::Device), so the decoded NV12 frame is turned into a BGRA
// texture with the GPU's video processor (ID3D11VideoContext::VideoProcessorBlt: YUV -> RGB with the stream's matrix
// and range) on that device, and that texture is wrapped once as an ID2D1Bitmap1. Views then draw it like any image:
// letterboxed, clipped, under the controls and the fading overlays, in both windows, with no extra swap chain, no
// airspace problems and no copies through the CPU. The conversion happens only when the frame on screen changes.
// Software decoding (no DXVA) hands RGB32 rows that are copied into a CPU-side bitmap.
//
// Ordering: a slot's conversion is submitted on the immediate context before the decoder may reuse the slot (take()
// follows the Blt), and the display texture is only written at the start of a view's paint, after the other window's
// frame was flushed (EndDraw); the decode thread's copies take Direct2D's lock.
//
// Resources: the player holds the decoder thread, its slots, the decoder's surface pool, the display texture and the
// download. Destroying it frees all of them. The device's release hook (idle release, device loss) also drops them and
// marks the player lost(): the owner makes a new one at the clock's position once the video is on screen again.
#include "gfx/Device.h"
#include "video/SyncScheduler.h"
#include "video/VideoDecoder.h"

#include <d3d11.h>

#include <memory>
#include <string>
#include <vector>

namespace st::video {

class VideoPlayer {
public:
    // Opens the stream (decoder thread) at `startUs` on gfx::Device's device (hardware) or in software when there is
    // none or `software` is set.
    VideoPlayer(Source source, int64_t startUs, bool software = false);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // The picture for the audio clock at this paint (`clockUs` = the heard media time when the frame is shown,
    // `rate` its speed, `nowMs` real time). nullptr until the first frame is there. Call inside a paint (BeginDraw).
    ID2D1Bitmap1* frame(ID2D1DeviceContext* dc, int64_t clockUs, double rate, double nowMs);
    // The visible picture in the bitmap's pixels (the decoder's surfaces can be taller: 1088 rows for 1080p).
    D2D1_RECT_F sourceRect() const;
    int width() const { return format_.width; }
    int height() const { return format_.height; }
    double fps() const { return format_.fps > 0 ? format_.fps : 30; }
    // How long until the next frame is due at `rate` (ms; for the views' repaint), or a short poll while waiting.
    double nextFrameInMs(int64_t clockUs, double rate) const;

    // The decoder is on its way to a new place (opening, a seek): the frame on screen, if any, is from before it.
    bool waitingForFrame() const { return sched_.waitingForFirst(); }
    DecoderState state() const;
    bool expired() const { return decoder_ && decoder_->expired(); }
    std::string error() const { return decoder_ ? decoder_->error() : std::string("released"); }
    bool lost() const { return lost_; }                   // the device went: make a new player
    bool needsSoftware() const { return processorFailed_; }   // lost because the GPU can't convert: reopen in software
    bool hardware() const { return format_.hardware; }
    const SchedulerStats& stats() const { return sched_.stats(); }
    size_t gpuBytes() const;                              // slots + display texture (not the decoder's own pool)
    float bufferedFraction() const { return decoder_ ? decoder_->bufferedFraction() : 0.f; }   // of the stream downloaded

private:
    bool convert(ID2D1DeviceContext* dc, const VideoDecoder::Frame& f);
    bool ensureProcessor();
    void releaseAll();

    std::unique_ptr<VideoDecoder> decoder_;
    SyncScheduler sched_;
    std::vector<VideoDecoder::Frame> queue_;
    std::vector<FrameTime> times_;
    VideoFormat format_;
    uint64_t generation_ = 0;
    bool lost_ = false;

    // Hardware conversion (NV12 slot -> BGRA display texture).
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inViews_[VideoDecoder::kSlots];
    ID3D11Texture2D* inViewTex_[VideoDecoder::kSlots] = {};   // the texture each view was made on (identity only)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> display_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    bool processorFailed_ = false;
    bool convertFailed_ = false;   // logged once

    gfx::DeviceHook hook_{[this] {
        releaseAll();
        lost_ = true;
    }};
};

// Which optional codecs Media Foundation can decode here (VP9 with the WebM source, AV1). Cached after the first call.
struct OptionalDecoders {
    bool vp9 = false, av1 = false;
};
OptionalDecoders optionalDecoders();

} // namespace st::video
