#include "audio/Decoder.h"

#include "audio/Fmp4Index.h"
#include "audio/MfByteStream.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/XiphDecoder.h"
#include "core/Log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace st::audio {

namespace {
constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

bool isWebm(const std::string& mime) { return mime.find("webm") != std::string::npos; }
} // namespace

uint32_t RatePolicy::pick(uint32_t native) const {
    if (fixed) return fixed;
    if (native == 0 || accepted.empty() || std::find(accepted.begin(), accepted.end(), native) != accepted.end()) return native;
    uint32_t multiple = 0, above = 0, best = 0;
    for (const uint32_t r : accepted) {
        if (r > native && r % native == 0 && (!multiple || r < multiple)) multiple = r;
        if (r > native && (!above || r < above)) above = r;
        if (r > best) best = r;
    }
    return multiple ? multiple : above ? above : best;
}

Decoder::Decoder() = default;
Decoder::~Decoder() { close(); }

bool Decoder::isOpen() const { return reader_ != nullptr || xiph_ != nullptr; }

float Decoder::tagGainDb() const {
    if (xiph_)
        if (const auto g = xiph_->replayGainDb()) return *g;
    return std::numeric_limits<float>::quiet_NaN();
}

HRESULT Decoder::open(const std::shared_ptr<ProgressiveBuffer>& buffer, const std::string& mimeType,
                      bool& unsupported, const RatePolicy& rates) {
    close();
    unsupported = false;

    // Built-in first: Ogg and WebM / Matroska with Opus or Vorbis decode here, whatever Media Foundation has.
    HRESULT hr = S_OK;
    if (const XiphDecoder::Kind kind = XiphDecoder::probe(*buffer, hr); kind != XiphDecoder::Kind::None) {
        auto xiph = std::make_unique<XiphDecoder>();
        hr = xiph->open(buffer, kind, rates);
        if (hr == S_OK) {
            sampleRate_ = xiph->sampleRate();
            channels_ = xiph->channels();
            durationHns_ = xiph->durationHns();
            xiph_ = std::move(xiph);
            return S_OK;
        }
        if (hr == E_ABORT || hr == MF_E_NET_READ) return hr;
        if (FAILED(hr)) ST_LOG_WARN("audio", "built-in decoder failed (hr=0x{:08X}); trying Media Foundation", static_cast<uint32_t>(hr));
        // S_FALSE: another codec in an Ogg / Matroska container: Media Foundation may have a handler for it.
    } else if (FAILED(hr)) {
        return hr;
    }

    // "audio/mp4; codecs=..." -> "audio/mp4"
    std::string mime = mimeType.substr(0, mimeType.find(';'));
    while (!mime.empty() && mime.back() == ' ') mime.pop_back();

    // A fragmented MP4 whose track has an edit list (some of YouTube's Art Tracks' AAC streams, their passthrough
    // downloads and cached copies): Media Foundation's MPEG-4 source presents no sample of it at all, so the song would
    // "end" at 0:00. The source reads a view with the edit list freed (audio/Fmp4Index) and the edit (the encoder's
    // priming) comes off here: read() drops what lies before it and shifts the time stamps, seek() aims past it.
    editHns_ = 0;
    std::shared_ptr<ByteStreamView> view;
    if (mime.empty() || mime.find("mp4") != std::string::npos || mime.find("m4a") != std::string::npos) {
        const int64_t length = buffer->length();
        std::vector<uint8_t> head(static_cast<size_t>(std::clamp<int64_t>(length, 0, 64 * 1024)));
        const auto status = head.empty() ? ProgressiveBuffer::ReadStatus::Ok : buffer->read(0, head.data(), head.size());
        if (status == ProgressiveBuffer::ReadStatus::Interrupted) return E_ABORT;   // a seek: the caller opens again
        if (status != ProgressiveBuffer::ReadStatus::Ok) return MF_E_NET_READ;
        if (!head.empty()) {
            if (auto fix = freeFragmentedEditList(head)) {
                view = std::make_shared<ByteStreamView>();
                view->head = std::move(fix->init);
                view->bodyOffset = fix->bodyOffset;
                editHns_ = fix->editUs * 10;
                ST_LOG_INFO("audio", "fragmented MP4 with an edit list ({:.1f} ms): read with the edit list freed", fix->editUs / 1000.0);
            }
        }
    }

    hr = createMfByteStream(buffer, mime, &stream_, view);
    if (FAILED(hr)) return hr;

    ComPtr<IMFSourceResolver> resolver;
    hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) return hr;
    // The file-name hint lets the resolver pick the handler by extension when the mime type is
    // missing; CONTENT_DOES_NOT_HAVE_TO_MATCH lets it sniff the content if the hint is wrong.
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> object;
    hr = resolver->CreateObjectFromByteStream(
        stream_.Get(), isWebm(mime) ? L"stream.webm" : L"stream.m4a",
        MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
        nullptr, &type, &object);
    if (FAILED(hr)) {
        unsupported = hr != E_ABORT && hr != MF_E_NET_READ;
        close();
        return hr;
    }
    hr = object.As(&source_);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source_.Get(), nullptr, &reader_);
    if (SUCCEEDED(hr)) hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr)) hr = reader_->SetStreamSelection(kAudio, TRUE);
    if (FAILED(hr)) {
        unsupported = hr == MF_E_INVALIDSTREAMNUMBER;  // no audio stream at all
        close();
        return hr;
    }
    hr = configureOutput(rates);
    if (FAILED(hr)) {
        unsupported = hr != E_ABORT && hr != MF_E_NET_READ;  // typically MF_E_TOPO_CODEC_NOT_FOUND
        close();
        return hr;
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                    MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8)
        durationHns_ = static_cast<int64_t>(var.uhVal.QuadPart);
    PropVariantClear(&var);
    return S_OK;
}

HRESULT Decoder::configureOutput(const RatePolicy& rates) {
    ComPtr<IMFMediaType> native;
    HRESULT hr = reader_->GetNativeMediaType(kAudio, 0, &native);
    if (FAILED(hr)) return hr;
    const UINT32 nativeChannels = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);

    const UINT32 nativeRate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);

    // Float PCM, stereo (mono is spread, multichannel downmixed), at the requested rate: every track then shares the
    // engine's output format, so gapless handoffs and crossfades work across 44.1 / 48 kHz sources without reopening
    // the device. Whatever the reader can't convert falls back step by step to the native layout and rate (WASAPI's
    // AUTOCONVERTPCM mixes and resamples those).
    auto trySet = [&](UINT32 channels, UINT32 rate) {
        ComPtr<IMFMediaType> type;
        HRESULT h = MFCreateMediaType(&type);
        if (SUCCEEDED(h)) h = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(h)) h = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(h) && channels) h = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (SUCCEEDED(h) && rate) h = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        if (SUCCEEDED(h)) h = reader_->SetCurrentMediaType(kAudio, nullptr, type.Get());
        return h;
    };
    const UINT32 target = rates.pick(nativeRate);
    const UINT32 rate = target && target != nativeRate ? target : 0;
    hr = E_FAIL;
    if (rate) hr = trySet(2, rate);
    if (FAILED(hr) && nativeChannels != 2) hr = trySet(2, 0);
    if (FAILED(hr)) hr = trySet(0, 0);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> current;
    hr = reader_->GetCurrentMediaType(kAudio, &current);
    if (FAILED(hr)) return hr;
    sampleRate_ = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    channels_ = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    if (sampleRate_ == 0 || channels_ == 0) return MF_E_INVALIDMEDIATYPE;
    return S_OK;
}

void Decoder::close() {
    xiph_.reset();
    editHns_ = 0;
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
    if (stream_) stream_->Close();
    stream_.Reset();
}

HRESULT Decoder::seek(int64_t hns) {
    if (xiph_) return xiph_->seek(hns);
    if (!reader_) return MF_E_NOT_INITIALIZED;
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = hns + editHns_;   // the source counts media time (the freed edit list's priming included)
    return reader_->SetCurrentPosition(GUID_NULL, pos);
}

Decoder::Status Decoder::read(std::vector<float>& out, int64_t& timestampHns, bool& formatChanged, HRESULT& hr) {
    timestampHns = -1;
    formatChanged = false;
    if (xiph_) return xiph_->read(out, timestampHns, hr);
    if (!reader_) {
        hr = MF_E_NOT_INITIALIZED;
        return Status::Error;
    }
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    hr = reader_->ReadSample(kAudio, 0, nullptr, &flags, &ts, &sample);
    if (FAILED(hr)) return Status::Error;
    if (flags & MF_SOURCE_READERF_ERROR) {
        hr = E_FAIL;
        return Status::Error;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(reader_->GetCurrentMediaType(kAudio, &current))) {
            sampleRate_ = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate_);
            channels_ = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, channels_);
        }
        formatChanged = true;
    }
    if (sample) {
        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return Status::Error;
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr)) return Status::Error;
        const size_t count = length / sizeof(float);
        const size_t old = out.size();
        out.resize(old + count);
        std::memcpy(out.data() + old, data, count * sizeof(float));
        buffer->Unlock();
        timestampHns = ts;
        if (editHns_ > 0) {
            // The freed edit list: presentation time = media time - the edit; what lies before 0 is the priming.
            int64_t p = ts - editHns_;
            if (p < 0 && sampleRate_ && channels_) {
                const size_t frames = count / channels_;
                const size_t drop = std::min<size_t>(frames, static_cast<size_t>((-p * static_cast<int64_t>(sampleRate_) + 5'000'000) / 10'000'000));
                out.erase(out.begin() + static_cast<ptrdiff_t>(old), out.begin() + static_cast<ptrdiff_t>(old + drop * channels_));
                p = drop < frames ? 0 : -1;   // nothing left of this sample: no time stamp
            }
            timestampHns = p;
        }
    }
    hr = S_OK;
    return (flags & MF_SOURCE_READERF_ENDOFSTREAM) ? Status::EndOfStream : Status::Ok;
}

} // namespace st::audio
