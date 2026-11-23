#include "video/VideoDecoder.h"

#include "audio/MfByteStream.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Log.h"

#include <d2d1_1.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstring>
#include <format>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

namespace st::video {

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
std::string hex(HRESULT hr) { return std::format("0x{:08X}", static_cast<uint32_t>(hr)); }
} // namespace

struct VideoDecoder::Mf {
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFSourceReader> reader;
    ComPtr<ID3D11DeviceContext> context;
    LONG stride = 0;          // software: bytes per row of the RGB32 output (negative = bottom-up)
    bool hardware = false;
};

VideoDecoder::VideoDecoder(Source source, ID3D11Device* device, ID2D1Multithread* d2dLock, int64_t startUs)
    : source_(std::move(source)), device_(device), d2dLock_(d2dLock) {
    for (int i = kSlots - 1; i >= 0; --i) free_.push_back(i);
    seekTo_ = std::max<int64_t>(0, startUs);
    seekGen_ = 1;
    thread_ = std::thread([this] { run(); });
}

VideoDecoder::~VideoDecoder() { stop(); }

void VideoDecoder::stop() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        if (buffer_) buffer_->cancel();   // a read blocked on the network fails at once
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    ready_.clear();
    for (auto& s : slots_) {
        s.texture.Reset();
        s.pixels = {};
    }
    slotBytes_ = 0;
    device_.Reset();
    d2dLock_.Reset();
}

void VideoDecoder::seek(int64_t us) {
    {
        std::lock_guard lock(mutex_);
        seekTo_ = std::max<int64_t>(0, us);
        ++seekGen_;
        flushQueueLocked();
    }
    cv_.notify_all();
}

void VideoDecoder::flushQueueLocked() {
    for (const auto& f : ready_) free_.push_back(f.slot);
    ready_.clear();
}

std::string VideoDecoder::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

VideoFormat VideoDecoder::format() const {
    std::lock_guard lock(mutex_);
    return format_;
}

void VideoDecoder::peek(std::vector<Frame>& out) const {
    std::lock_guard lock(mutex_);
    out = ready_;
}

void VideoDecoder::take(int count) {
    {
        std::lock_guard lock(mutex_);
        count = std::min<int>(count, static_cast<int>(ready_.size()));
        for (int i = 0; i < count; ++i) free_.push_back(ready_[static_cast<size_t>(i)].slot);
        ready_.erase(ready_.begin(), ready_.begin() + count);
    }
    if (count > 0) cv_.notify_all();
}

float VideoDecoder::bufferedFraction() const {
    std::lock_guard lock(mutex_);
    return buffer_ ? buffer_->fraction() : 0.f;
}

size_t VideoDecoder::bufferedBytes() const { return slotBytes_.load(); }

void VideoDecoder::fail(std::string message) {
    ST_LOG_WARN("video", "decoder: {}", message);
    {
        std::lock_guard lock(mutex_);
        error_ = std::move(message);
    }
    if (buffer_ && buffer_->expired()) expired_ = true;
    state_.store(DecoderState::Failed, std::memory_order_release);
}

void VideoDecoder::run() {
    SetThreadDescription(GetCurrentThread(), L"st-video-decode");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(mf) && open()) {
        state_.store(DecoderState::Running, std::memory_order_release);
        while (decodeOne()) {
        }
    } else if (FAILED(mf)) {
        fail("MFStartup failed " + hex(mf));
    }
    if (mf_) {
        mf_->reader.Reset();   // the decoder's surface pool and the byte stream go with it
        if (mf_->manager) mf_->manager.Reset();
        mf_->context.Reset();
        mf_.reset();
    }
    if (buffer_) {
        buffer_->cancel();
        buffer_->release();
    }
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

bool VideoDecoder::open() {
    {
        std::lock_guard lock(mutex_);
        if (quit_) return false;
        buffer_ = std::make_shared<audio::ProgressiveBuffer>(source_.url, source_.localPath, source_.contentLength, nullptr);
        buffer_->setReadAhead(source_.readAheadBytes);
        buffer_->start();
    }
    if (buffer_->waitForLength() < 0) {
        fail(buffer_->errorMessage().empty() ? std::string("download failed") : buffer_->errorMessage());
        return false;
    }
    return openReader(device_ != nullptr);
}

bool VideoDecoder::openReader(bool hardware) {
    for (int attempt = hardware ? 0 : 1; attempt < 2; ++attempt) {
        const bool hw = device_ && attempt == 0;
        mf_ = std::make_unique<Mf>();
        ComPtr<IMFByteStream> stream;
        HRESULT hr = audio::createMfByteStream(buffer_, source_.mimeType, &stream);
        if (FAILED(hr)) {
            fail("byte stream " + hex(hr));
            return false;
        }
        ComPtr<IMFAttributes> attr;
        MFCreateAttributes(&attr, 4);
        if (hw) {
            UINT token = 0;
            hr = MFCreateDXGIDeviceManager(&token, &mf_->manager);
            if (SUCCEEDED(hr)) hr = mf_->manager->ResetDevice(device_.Get(), token);
            if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mf_->manager.Get());
            if (FAILED(hr)) {
                ST_LOG_WARN("video", "DXGI device manager failed ({}): software decoding", hex(hr));
                continue;
            }
            device_->GetImmediateContext(&mf_->context);
        } else {
            attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // YUV -> RGB32 on the CPU
        }
        hr = MFCreateSourceReaderFromByteStream(stream.Get(), attr.Get(), &mf_->reader);
        if (FAILED(hr)) {
            fail("no media source for this stream " + hex(hr));
            return false;
        }
        mf_->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        mf_->reader->SetStreamSelection(kVideo, TRUE);
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, hw ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        hr = mf_->reader->SetCurrentMediaType(kVideo, nullptr, type.Get());
        if (FAILED(hr)) {
            if (hw) {
                ST_LOG_WARN("video", "no NV12 output with DXVA ({}): software decoding", hex(hr));
                continue;
            }
            fail("no decoder for this video " + hex(hr));
            return false;
        }
        ComPtr<IMFMediaType> current;
        if (FAILED(mf_->reader->GetCurrentMediaType(kVideo, &current))) {
            fail("no video format");
            return false;
        }
        VideoFormat f;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        f.texWidth = static_cast<int>(w);
        f.texHeight = static_cast<int>(h);
        f.width = f.texWidth;
        f.height = f.texHeight;
        MFVideoArea area{};
        if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) ||
            SUCCEEDED(current->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr))) {
            if (area.Area.cx > 0 && area.Area.cy > 0) {
                f.width = std::min<int>(area.Area.cx, f.texWidth);
                f.height = std::min<int>(area.Area.cy, f.texHeight);
            }
        }
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &num, &den)) && den > 0) f.fps = static_cast<double>(num) / den;
        const UINT32 matrix = MFGetAttributeUINT32(current.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
        f.bt709 = matrix == MFVideoTransferMatrix_Unknown ? f.height >= 720 : matrix != MFVideoTransferMatrix_BT601;
        f.fullRange = MFGetAttributeUINT32(current.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
        f.hardware = hw;
        mf_->hardware = hw;
        if (!hw) {
            UINT32 stride = 0;
            mf_->stride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride)
                                                                                    : static_cast<LONG>(w * 4);
        }
        if (f.texWidth <= 0 || f.texHeight <= 0) {
            fail("video without a size");
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            format_ = f;
        }
        ST_LOG_INFO("video", "decoding {}x{} ({}x{} surfaces) at {:.2f} fps, {}, {}{}", f.width, f.height, f.texWidth, f.texHeight,
                    f.fps, hw ? "DXVA (NV12)" : "software (RGB32)", f.bt709 ? "BT.709" : "BT.601", f.fullRange ? " full range" : "");
        return true;
    }
    fail("no decoder for this video");
    return false;
}

// One step of the decode loop: a pending seek, then one sample into a free slot. False = leave the loop.
bool VideoDecoder::decodeOne() {
    int64_t seekTarget = -1;
    uint32_t gen = 0;
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
            return quit_ || seekTo_ >= 0 || (!free_.empty() && state_.load() != DecoderState::Ended);
        });
        if (quit_) return false;
        if (seekTo_ >= 0) {
            seekTarget = seekTo_;
            seekTo_ = -1;
            flushQueueLocked();
            discardUntilUs_ = seekTarget;
        }
        gen = seekGen_;
    }
    if (seekTarget >= 0) {
        PROPVARIANT pos;
        InitPropVariantFromInt64(seekTarget * 10, &pos);
        const HRESULT hr = mf_->reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
        if (FAILED(hr)) ST_LOG_WARN("video", "seek to {} ms failed {}", seekTarget / 1000, hex(hr));
        state_.store(DecoderState::Running, std::memory_order_release);
    }

    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    const HRESULT hr = mf_->reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample);
    {
        std::lock_guard lock(mutex_);
        if (quit_) return false;
    }
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        fail(buffer_->failed() ? buffer_->errorMessage() : "decoding failed " + hex(FAILED(hr) ? hr : E_FAIL));
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        state_.store(DecoderState::Ended, std::memory_order_release);
        ST_LOG_DEBUG("video", "end of the video stream");
        return true;
    }
    if (!sample) return true;
    const int64_t ptsUs = ts / 10;
    LONGLONG d = 0;
    sample->GetSampleDuration(&d);
    const double fps = format().fps;
    const int64_t durUs = d > 0 ? d / 10 : fps > 0 ? static_cast<int64_t>(1e6 / fps) : 33'333;
    int slot = -1;
    {
        std::lock_guard lock(mutex_);
        // A seek arrived during the read, or the frame is from the way between the key frame and the target.
        if (gen != seekGen_ || seekTo_ >= 0 || ptsUs + durUs <= discardUntilUs_ || free_.empty()) return true;
        slot = free_.back();
        free_.pop_back();
    }
    Slot& s = slots_[slot];
    bool ok = false;
    ComPtr<IMFMediaBuffer> mb;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &mb))) {
        if (mf_->hardware) {
            ComPtr<IMFDXGIBuffer> dxgi;
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            if (SUCCEEDED(mb.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex))) && SUCCEEDED(dxgi->GetSubresourceIndex(&sub))) {
                D3D11_TEXTURE2D_DESC src{};
                tex->GetDesc(&src);
                D3D11_TEXTURE2D_DESC have{};
                if (s.texture) s.texture->GetDesc(&have);
                if (!s.texture || have.Width != src.Width || have.Height != src.Height || have.Format != src.Format) {
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = src.Width;
                    desc.Height = src.Height;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    desc.Format = src.Format;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    s.texture.Reset();
                    // The video processor takes its input from render-target capable textures (Intel refuses an input
                    // view on a shader-resource-only NV12 texture: E_INVALIDARG); narrower bindings as fallbacks.
                    HRESULT ch = E_FAIL;
                    for (UINT bind : {UINT(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE), UINT(D3D11_BIND_RENDER_TARGET),
                                      UINT(D3D11_BIND_DECODER), UINT(D3D11_BIND_SHADER_RESOURCE), 0u}) {
                        desc.BindFlags = bind;
                        ch = device_->CreateTexture2D(&desc, nullptr, &s.texture);
                        if (SUCCEEDED(ch)) break;
                    }
                    if (SUCCEEDED(ch)) {
                        if (have.Width) slotGen_.fetch_add(1, std::memory_order_acq_rel);
                        slotBytes_ += static_cast<size_t>(src.Width) * src.Height * 3 / 2;
                    } else {
                        fail(std::format("slot texture {}x{} failed {}", src.Width, src.Height, hex(ch)));
                        return false;
                    }
                }
                if (s.texture) {
                    if (d2dLock_) d2dLock_->Enter();
                    mf_->context->CopySubresourceRegion(s.texture.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
                    if (d2dLock_) d2dLock_->Leave();
                    ok = true;
                }
            } else {
                // The decoder fell back to system memory: start over in software (RGB32) mode at this frame.
                ST_LOG_WARN("video", "decoded frames are not GPU textures: switching to software decoding");
                {
                    std::lock_guard lock(mutex_);
                    free_.push_back(slot);
                }
                mf_.reset();
                if (!openReader(false)) return false;
                seek(ptsUs);
                return true;
            }
        } else {
            const VideoFormat f = format();
            const size_t row = static_cast<size_t>(f.texWidth) * 4;
            if (s.pixels.size() != row * f.texHeight) {
                if (!s.pixels.empty()) slotGen_.fetch_add(1, std::memory_order_acq_rel);
                slotBytes_ += row * f.texHeight - s.pixels.size();
                s.pixels.assign(row * f.texHeight, 0);
            }
            ComPtr<IMF2DBuffer> b2;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                for (int y = 0; y < f.texHeight; ++y) std::memcpy(s.pixels.data() + row * y, scan0 + static_cast<ptrdiff_t>(pitch) * y, row);
                b2->Unlock2D();
                ok = true;
            } else {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                    const LONG stride = mf_->stride;
                    const BYTE* first = stride < 0 ? data + static_cast<size_t>(-stride) * (f.texHeight - 1) : data;
                    if (len >= static_cast<DWORD>(std::abs(stride)) * f.texHeight) {
                        for (int y = 0; y < f.texHeight; ++y) std::memcpy(s.pixels.data() + row * y, first + static_cast<ptrdiff_t>(stride) * y, row);
                        ok = true;
                    }
                    mb->Unlock();
                }
            }
        }
    }
    std::lock_guard lock(mutex_);
    if (!ok || gen != seekGen_ || seekTo_ >= 0) {
        free_.push_back(slot);
        return true;
    }
    ready_.push_back({ptsUs, durUs, slot});
    return true;
}

} // namespace st::video
