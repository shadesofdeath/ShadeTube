#include "audio/Decoder.h"

#include "audio/MfByteStream.h"
#include "audio/ProgressiveBuffer.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace st::audio {

namespace {
constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

bool isWebm(const std::string& mime) { return mime.find("webm") != std::string::npos; }
} // namespace

HRESULT Decoder::open(const std::shared_ptr<ProgressiveBuffer>& buffer, const std::string& mimeType,
                      bool& unsupported, uint32_t targetRate) {
    close();
    unsupported = false;
    targetRate_ = targetRate;
    // "audio/mp4; codecs=..." -> "audio/mp4"
    std::string mime = mimeType.substr(0, mimeType.find(';'));
    while (!mime.empty() && mime.back() == ' ') mime.pop_back();

    HRESULT hr = createMfByteStream(buffer, mime, &stream_);
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
    hr = configureOutput();
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

HRESULT Decoder::configureOutput() {
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
    const UINT32 rate = targetRate_ && targetRate_ != nativeRate ? targetRate_ : 0;
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
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
    if (stream_) stream_->Close();
    stream_.Reset();
}

HRESULT Decoder::seek(int64_t hns) {
    if (!reader_) return MF_E_NOT_INITIALIZED;
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = hns;
    return reader_->SetCurrentPosition(GUID_NULL, pos);
}

Decoder::Status Decoder::read(std::vector<float>& out, int64_t& timestampHns, bool& formatChanged, HRESULT& hr) {
    timestampHns = -1;
    formatChanged = false;
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
    }
    hr = S_OK;
    return (flags & MF_SOURCE_READERF_ENDOFSTREAM) ? Status::EndOfStream : Status::Ok;
}

} // namespace st::audio
