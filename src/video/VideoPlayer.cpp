#include "video/VideoPlayer.h"

#include "core/Log.h"

#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>

#include <algorithm>
#include <format>
#include <mutex>

namespace st::video {

using Microsoft::WRL::ComPtr;

namespace {
std::string hex(HRESULT hr) { return std::format("0x{:08X}", static_cast<uint32_t>(hr)); }
} // namespace

VideoPlayer::VideoPlayer(Source source, int64_t startUs, bool software) {
    auto& dev = gfx::Device::get();
    ID3D11Device* d3d = nullptr;
    ComPtr<ID2D1Multithread> lock;
    if (!software) {
        dev.ensure();
        d3d = dev.d3d();
        ComPtr<ID3D10Multithread> mt;
        ComPtr<ID3D11VideoDevice> video;
        // Media Foundation's decoder uses the device from its own threads: the immediate context must be guarded.
        if (d3d && SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&mt))) && SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&video)))) {
            mt->SetMultithreadProtected(TRUE);
            dev.d2dFactory()->QueryInterface(IID_PPV_ARGS(&lock));
        } else {
            ST_LOG_WARN("video", "the graphics device has no video API: software decoding");
            d3d = nullptr;
        }
    }
    generation_ = dev.generation();
    decoder_ = std::make_unique<VideoDecoder>(std::move(source), d3d, lock.Get(), startUs);
    sched_.restart(startUs);
}

VideoPlayer::~VideoPlayer() { releaseAll(); }

void VideoPlayer::releaseAll() {
    decoder_.reset();   // joins the decoder thread: its reader, surface pool and slots go first
    bitmap_.Reset();
    outView_.Reset();
    display_.Reset();
    for (int i = 0; i < VideoDecoder::kSlots; ++i) {
        inViews_[i].Reset();
        inViewTex_[i] = nullptr;
    }
    vp_.Reset();
    venum_.Reset();
    vctx_.Reset();
    vdev_.Reset();
}

DecoderState VideoPlayer::state() const { return decoder_ ? decoder_->state() : DecoderState::Failed; }

size_t VideoPlayer::gpuBytes() const {
    return (decoder_ ? decoder_->bufferedBytes() : 0) + (bitmap_ ? static_cast<size_t>(format_.width) * format_.height * 4 : 0);
}

D2D1_RECT_F VideoPlayer::sourceRect() const {
    return D2D1::RectF(0, 0, static_cast<float>(format_.width), static_cast<float>(format_.height));
}

double VideoPlayer::nextFrameInMs(int64_t clockUs, double rate) const {
    if (!decoder_ || lost_) return 1e9;
    if (sched_.waitingForFirst() || queue_.empty()) return state() == DecoderState::Ended && !sched_.waitingForFirst() ? 1e9 : 30;
    if (rate <= 0) return 1e9;
    const double ms = static_cast<double>(queue_.front().ptsUs - clockUs) / 1000.0 / rate;
    return std::clamp(ms, 1.0, 250.0);
}

ID2D1Bitmap1* VideoPlayer::frame(ID2D1DeviceContext*, int64_t clockUs, double rate, double nowMs) {
    if (lost_ || !decoder_) return nullptr;
    if (gfx::Device::get().generation() != generation_) {   // device loss: everything made on the old one is stale
        releaseAll();
        lost_ = true;
        return nullptr;
    }
    const DecoderState st = decoder_->state();
    if (st == DecoderState::Opening || st == DecoderState::Failed) return bitmap_.Get();
    decoder_->peek(queue_);
    times_.resize(queue_.size());
    for (size_t i = 0; i < queue_.size(); ++i) times_[i] = {queue_[i].ptsUs, queue_[i].durUs};
    const auto p = sched_.pick(times_, clockUs, rate, nowMs, st == DecoderState::Ended);
    if (p.resync) {
        ST_LOG_DEBUG("video", "resync: clock {} ms, decoder at {} ms", clockUs / 1000,
                     queue_.empty() ? sched_.shownPts() / 1000 : queue_.front().ptsUs / 1000);
        decoder_->seek(p.resyncUs);
        queue_.clear();
        return bitmap_.Get();
    }
    if (p.show >= 0) {
        format_ = decoder_->format();
        const bool ok = convert(nullptr, queue_[static_cast<size_t>(p.show)]);
        if (lost_) return nullptr;   // no video processor: the owner reopens in software (needsSoftware())
        if (!ok && !convertFailed_) {
            convertFailed_ = true;
            ST_LOG_WARN("video", "a frame could not be converted");
        }
    }
    if (p.consume > 0) {
        decoder_->take(p.consume);
        queue_.erase(queue_.begin(), queue_.begin() + std::min<size_t>(queue_.size(), static_cast<size_t>(p.consume)));
    }
    return bitmap_.Get();
}

bool VideoPlayer::ensureProcessor() {
    if (vp_ && outView_ && bitmap_) return true;
    if (processorFailed_) return false;
    auto* d3d = gfx::Device::get().d3d();
    if (!d3d) return false;
    HRESULT hr = d3d->QueryInterface(IID_PPV_ARGS(&vdev_));
    ComPtr<ID3D11DeviceContext> ctx;
    d3d->GetImmediateContext(&ctx);
    if (SUCCEEDED(hr)) hr = ctx.As(&vctx_);
    const auto rate = DXGI_RATIONAL{static_cast<UINT>(std::max(1.0, format_.fps) * 1000), 1000};
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = rate;
    cd.InputWidth = static_cast<UINT>(format_.texWidth);
    cd.InputHeight = static_cast<UINT>(format_.texHeight);
    cd.OutputFrameRate = rate;
    cd.OutputWidth = static_cast<UINT>(format_.width);
    cd.OutputHeight = static_cast<UINT>(format_.height);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessorEnumerator(&cd, &venum_);
    UINT in = 0, out = 0;
    if (SUCCEEDED(hr)) hr = venum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in);
    if (SUCCEEDED(hr)) hr = venum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out);
    if (SUCCEEDED(hr) && (!(in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)))
        hr = E_NOTIMPL;
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessor(venum_.Get(), 0, &vp_);
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cd.OutputWidth;
        td.Height = cd.OutputHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = d3d->CreateTexture2D(&td, nullptr, &display_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = vdev_->CreateVideoProcessorOutputView(display_.Get(), venum_.Get(), &od, &outView_);
    }
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGISurface> surface;
        hr = display_.As(&surface);
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (SUCCEEDED(hr)) hr = gfx::Device::get().resourceContext()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap_);
    }
    if (FAILED(hr)) {
        ST_LOG_WARN("video", "GPU video processor unavailable {}", hex(hr));
        releaseAll();   // also the decoder: the owner reopens in software (processorFailed_)
        processorFailed_ = true;
        lost_ = true;
        return false;
    }
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.YCbCr_Matrix = format_.bt709 ? 1 : 0;
    inSpace.Nominal_Range = format_.fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};   // full-range RGB
    vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outSpace);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    const RECT area{0, 0, format_.width, format_.height};
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &area);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &area);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &area);
    ST_LOG_INFO("video", "GPU conversion {}x{} -> BGRA {}x{}", format_.texWidth, format_.texHeight, format_.width, format_.height);
    return true;
}

bool VideoPlayer::convert(ID2D1DeviceContext*, const VideoDecoder::Frame& f) {
    const auto& slot = decoder_->slot(f.slot);
    if (!format_.hardware) {
        if (slot.pixels.empty()) return true;
        if (!bitmap_) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            const HRESULT hr = gfx::Device::get().resourceContext()->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(format_.texWidth), static_cast<UINT32>(format_.texHeight)), nullptr, 0, &props, &bitmap_);
            if (FAILED(hr)) return false;
        }
        return SUCCEEDED(bitmap_->CopyFromMemory(nullptr, slot.pixels.data(), static_cast<UINT32>(format_.texWidth) * 4));
    }
    if (!slot.texture || !ensureProcessor()) return false;
    const int i = f.slot;
    if (!inViews_[i] || inViewTex_[i] != slot.texture.Get()) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inViews_[i].Reset();
        const HRESULT hr = vdev_->CreateVideoProcessorInputView(slot.texture.Get(), venum_.Get(), &iv, &inViews_[i]);
        if (FAILED(hr)) {
            D3D11_TEXTURE2D_DESC td{};
            slot.texture->GetDesc(&td);
            ST_LOG_WARN("video", "video processor input view failed {} (bind flags 0x{:X}): software decoding", hex(hr), td.BindFlags);
            releaseAll();   // the owner reopens in software (needsSoftware())
            processorFailed_ = true;
            lost_ = true;
            return false;
        }
        inViewTex_[i] = slot.texture.Get();
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inViews_[i].Get();
    const HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), outView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) ST_LOG_WARN("video", "VideoProcessorBlt failed {}", hex(hr));
    return SUCCEEDED(hr);
}

namespace {
bool hasDecoder(const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, subtype};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                 &in, nullptr, &acts, &n);
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return SUCCEEDED(hr) && n > 0;
}

bool hasWebmSource() {
    for (const wchar_t* key : {L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\video/webm",
                               L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\.webm"}) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
            RegCloseKey(h);
            return true;
        }
    }
    return false;
}
} // namespace

OptionalDecoders optionalDecoders() {
    static OptionalDecoders cached;
    static std::once_flag once;
    std::call_once(once, [] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            cached.vp9 = hasWebmSource() && hasDecoder(MFVideoFormat_VP90);
            cached.av1 = hasDecoder(MFVideoFormat_AV1);
            MFShutdown();
        }
        if (SUCCEEDED(co)) CoUninitialize();
        ST_LOG_INFO("video", "optional decoders: VP9 {}, AV1 {}", cached.vp9 ? "yes" : "no", cached.av1 ? "yes" : "no");
    });
    return cached;
}

} // namespace st::video
