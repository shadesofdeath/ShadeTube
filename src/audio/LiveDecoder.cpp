#include "audio/LiveDecoder.h"

#include "core/Log.h"

#include <windows.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace st::audio {

namespace {

// MFAudioFormat_Opus (WAVE_FORMAT_OPUS = 0x704F); defined locally for older SDKs.
constexpr GUID kAudioFormatOpus = {0x0000704F, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr DWORD kMinOutputBytes = 96 * 1024;   // HE-AAC 5.1: 2048 frames * 6 ch * 4 bytes = 48 KB

HRESULT mpegInputType(const live::Frame& f, ComPtr<IMFMediaType>& type) {
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    if (f.layer == 3) {
        MPEGLAYER3WAVEFORMAT w{};
        w.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
        w.wfx.nChannels = static_cast<WORD>(f.channels);
        w.wfx.nSamplesPerSec = f.sampleRate;
        w.wfx.nAvgBytesPerSec = f.bitrate / 8;
        w.wfx.nBlockAlign = 1;
        w.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
        w.wID = MPEGLAYER3_ID_MPEG;
        w.fdwFlags = MPEGLAYER3_FLAG_PADDING_OFF;
        w.nBlockSize = static_cast<WORD>(std::min<size_t>(f.data.size(), 0xFFFF));
        w.nFramesPerBlock = 1;
        return MFInitMediaTypeFromWaveFormatEx(type.Get(), &w.wfx, sizeof w);
    }
    MPEG1WAVEFORMAT w{};
    w.wfx.wFormatTag = WAVE_FORMAT_MPEG;
    w.wfx.nChannels = static_cast<WORD>(f.channels);
    w.wfx.nSamplesPerSec = f.sampleRate;
    w.wfx.nAvgBytesPerSec = f.bitrate / 8;
    w.wfx.nBlockAlign = 1;
    w.wfx.cbSize = sizeof(MPEG1WAVEFORMAT) - sizeof(WAVEFORMATEX);
    w.fwHeadLayer = f.layer == 2 ? ACM_MPEG_LAYER2 : ACM_MPEG_LAYER1;
    w.dwHeadBitrate = f.bitrate;
    w.fwHeadMode = f.channels == 1 ? ACM_MPEG_SINGLECHANNEL : ACM_MPEG_STEREO;
    w.fwHeadModeExt = 1;
    w.wHeadEmphasis = 1;
    w.fwHeadFlags = ACM_MPEG_ID_MPEG1;
    return MFInitMediaTypeFromWaveFormatEx(type.Get(), &w.wfx, sizeof w);
}

// ADTS AAC: payload type 1; MF_MT_USER_DATA = the HEAACWAVEINFO fields after the WAVEFORMATEX (12 bytes), optionally
// followed by the AudioSpecificConfig (the decoder reads every ADTS header anyway).
HRESULT aacInputType(const live::Frame& f, bool withConfig, ComPtr<IMFMediaType>& type) {
    const auto adts = live::parseAdtsHeader(f.data.data(), f.data.size());
    if (!adts) return E_INVALIDARG;
    HRESULT hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, f.sampleRate);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, f.channels);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
    if (FAILED(hr)) return hr;
    uint8_t user[14] = {};
    user[0] = 1;      // wPayloadType = ADTS
    user[2] = 0xFE;   // wAudioProfileLevelIndication = unknown
    UINT32 size = 12;
    if (withConfig) {   // AudioSpecificConfig: object type (5 bits), frequency index (4), channel configuration (4)
        user[12] = static_cast<uint8_t>((adts->objectType << 3) | (adts->freqIndex >> 1));
        user[13] = static_cast<uint8_t>(((adts->freqIndex & 1) << 7) | (adts->channelConfig << 3));
        size = 14;
    }
    return type->SetBlob(MF_MT_USER_DATA, user, size);
}

HRESULT opusInputType(const live::Frame& f, bool withConfig, ComPtr<IMFMediaType>& type) {
    HRESULT hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, kAudioFormatOpus);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, f.channels);
    if (SUCCEEDED(hr) && withConfig && !f.codecPrivate.empty())
        hr = type->SetBlob(MF_MT_USER_DATA, f.codecPrivate.data(), static_cast<UINT32>(f.codecPrivate.size()));
    return hr;
}

GUID inputSubtype(const live::Frame& f) {
    switch (f.codec) {
    case live::Codec::Mpeg: return f.layer == 3 ? MFAudioFormat_MP3 : MFAudioFormat_MPEG;
    case live::Codec::Aac: return MFAudioFormat_AAC;
    case live::Codec::Opus: return kAudioFormatOpus;
    case live::Codec::None: break;
    }
    return GUID_NULL;
}

} // namespace

void LiveDecoder::close() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    outSample_.Reset();
    mft_.Reset();
    if (activate_) activate_->ShutdownObject();
    activate_.Reset();
    key_ = 0;
    rate_ = channels_ = bits_ = 0;
}

HRESULT LiveDecoder::open(const live::Frame& f, bool& unsupported) {
    close();
    unsupported = false;
    const GUID subtype = inputSubtype(f);
    if (subtype == GUID_NULL || !f.sampleRate || !f.channels) {
        unsupported = true;
        return MF_E_INVALIDMEDIATYPE;
    }
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, nullptr, &activates, &count);
    if (FAILED(hr)) return hr;
    std::vector<ComPtr<IMFActivate>> candidates;
    for (UINT32 i = 0; i < count; ++i) {
        candidates.emplace_back(activates[i]);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    if (candidates.empty()) {
        unsupported = true;
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }
    hr = MF_E_INVALIDMEDIATYPE;
    for (auto& activate : candidates) {
        ComPtr<IMFTransform> mft;
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&mft)))) continue;
        // With and without the codec configuration blob: decoders differ in what they insist on.
        for (const bool withConfig : {true, false}) {
            ComPtr<IMFMediaType> type;
            switch (f.codec) {
            case live::Codec::Mpeg: hr = withConfig ? mpegInputType(f, type) : E_FAIL; break;
            case live::Codec::Aac: hr = aacInputType(f, withConfig, type); break;
            case live::Codec::Opus: hr = opusInputType(f, withConfig, type); break;
            case live::Codec::None: hr = E_FAIL; break;
            }
            if (SUCCEEDED(hr)) hr = mft->SetInputType(0, type.Get(), 0);
            if (SUCCEEDED(hr)) break;
        }
        if (SUCCEEDED(hr)) {
            mft_ = mft;
            activate_ = activate;
            break;
        }
        activate->ShutdownObject();
    }
    if (!mft_) {
        unsupported = true;
        return FAILED(hr) ? hr : MF_E_INVALIDMEDIATYPE;
    }
    hr = chooseOutputType();
    if (FAILED(hr)) {
        unsupported = true;
        close();
        return hr;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    key_ = live::frameConfigKey(f);
    return S_OK;
}

HRESULT LiveDecoder::chooseOutputType() {
    // Float PCM when offered, else the widest integer PCM (converted to float in drain()).
    ComPtr<IMFMediaType> best;
    int bestScore = 0;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = mft_->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) break;   // MF_E_NO_MORE_TYPES
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        const UINT32 bits = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
        int score = 0;
        if (subtype == MFAudioFormat_Float && bits == 32) score = 4;
        else if (subtype == MFAudioFormat_PCM && (bits == 32 || bits == 24)) score = 3;
        else if (subtype == MFAudioFormat_PCM && bits == 16) score = 2;
        if (score > bestScore) {
            best = type;
            bestScore = score;
        }
    }
    if (!best) return MF_E_INVALIDMEDIATYPE;
    HRESULT hr = mft_->SetOutputType(0, best.Get(), 0);
    if (FAILED(hr)) return hr;
    GUID subtype{};
    best->GetGUID(MF_MT_SUBTYPE, &subtype);
    floatOut_ = subtype == MFAudioFormat_Float;
    bits_ = MFGetAttributeUINT32(best.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    rate_ = MFGetAttributeUINT32(best.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    channels_ = MFGetAttributeUINT32(best.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    if (!rate_ || !channels_) return MF_E_INVALIDMEDIATYPE;

    MFT_OUTPUT_STREAM_INFO info{};
    hr = mft_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;
    mftAllocates_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    outSample_.Reset();
    if (!mftAllocates_) {
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, kMinOutputBytes), &buffer);
        if (SUCCEEDED(hr)) hr = MFCreateSample(&outSample_);
        if (SUCCEEDED(hr)) hr = outSample_->AddBuffer(buffer.Get());
    }
    return hr;
}

void LiveDecoder::flush() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

HRESULT LiveDecoder::decode(const live::Frame& f, std::vector<float>& out) {
    if (!mft_) return MF_E_NOT_INITIALIZED;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(f.data.size()), &buffer);
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(dst, f.data.data(), f.data.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(f.data.size()));
    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;
    const int64_t duration = f.sampleRate ? static_cast<int64_t>(f.samples) * 10'000'000 / f.sampleRate : 0;
    sample->SetSampleTime(time_);
    sample->SetSampleDuration(duration);
    time_ += duration;
    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {   // output pending: collect it, then retry
        hr = drain(out);
        if (SUCCEEDED(hr)) hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    return drain(out);
}

HRESULT LiveDecoder::drain(std::vector<float>& out) {
    for (int guard = 0; guard < 64; ++guard) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        if (!mftAllocates_) {
            ComPtr<IMFMediaBuffer> b;
            if (SUCCEEDED(outSample_->GetBufferByIndex(0, &b))) b->SetCurrentLength(0);
            ob.pSample = outSample_.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        ComPtr<IMFSample> produced;
        if (mftAllocates_) produced.Attach(ob.pSample);
        else produced = outSample_;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {   // e.g. SBR / PS detected: new rate or channel count
            hr = chooseOutputType();
            if (FAILED(hr)) return hr;
            ST_LOG_INFO("audio", "live decoder output changed: {} Hz, {} ch", rate_, channels_);
            continue;
        }
        if (FAILED(hr)) return hr;
        if (!produced) continue;
        ComPtr<IMFMediaBuffer> buffer;
        hr = produced->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return hr;
        BYTE* data = nullptr;
        DWORD length = 0;
        hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr)) return hr;
        const size_t old = out.size();
        if (floatOut_) {
            const size_t n = length / sizeof(float);
            out.resize(old + n);
            std::memcpy(out.data() + old, data, n * sizeof(float));
        } else if (bits_ == 16) {
            const size_t n = length / 2;
            out.resize(old + n);
            const auto* s = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < n; ++i) out[old + i] = static_cast<float>(s[i]) / 32768.f;
        } else if (bits_ == 24) {
            const size_t n = length / 3;
            out.resize(old + n);
            for (size_t i = 0; i < n; ++i) {
                const int32_t v = static_cast<int32_t>((uint32_t(data[i * 3]) << 8) | (uint32_t(data[i * 3 + 1]) << 16) |
                                                       (uint32_t(data[i * 3 + 2]) << 24)) >> 8;
                out[old + i] = static_cast<float>(v) / 8388608.f;
            }
        } else if (bits_ == 32) {
            const size_t n = length / 4;
            out.resize(old + n);
            const auto* s = reinterpret_cast<const int32_t*>(data);
            for (size_t i = 0; i < n; ++i) out[old + i] = static_cast<float>(static_cast<double>(s[i]) / 2147483648.0);
        }
        buffer->Unlock();
    }
    return S_OK;
}

} // namespace st::audio
