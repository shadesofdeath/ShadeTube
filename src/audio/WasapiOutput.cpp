#include "audio/WasapiOutput.h"

#include "core/Log.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace st::audio {

// Receives endpoint notifications on a system thread; only sets a flag and wakes the engine.
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(HANDLE wake) : wake_(wake) {}

    // The device in use, the preferred one ("" = none) and whether the default is being followed (no preference, or
    // the preferred device is missing).
    void setDevice(std::wstring id, std::wstring preferred, bool followDefault) {
        std::lock_guard lock(mutex_);
        deviceId_ = std::move(id);
        preferred_ = std::move(preferred);
        followDefault_ = followDefault;
    }
    bool take() { return changed_.exchange(false); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && followingDefault()) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (state != DEVICE_STATE_ACTIVE && isOurs(id)) signal();
        if (state == DEVICE_STATE_ACTIVE && preferredReturned(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override {
        if (isOurs(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) override {
        if (preferredReturned(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~DeviceNotifier() = default;
    bool isOurs(LPCWSTR id) {
        std::lock_guard lock(mutex_);
        return id && !deviceId_.empty() && deviceId_ == id;
    }
    bool followingDefault() {
        std::lock_guard lock(mutex_);
        return followDefault_;
    }
    // The preferred device (re)appeared while the default one plays instead.
    bool preferredReturned(LPCWSTR id) {
        std::lock_guard lock(mutex_);
        return id && !preferred_.empty() && followDefault_ && preferred_ == id;
    }
    void signal() {
        changed_.store(true);
        SetEvent(wake_);
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<bool> changed_{false};
    HANDLE wake_;
    std::mutex mutex_;
    std::wstring deviceId_, preferred_;
    bool followDefault_ = true;
};

WasapiOutput::WasapiOutput(void* wakeEvent) : wakeEvent_(wakeEvent) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (SUCCEEDED(hr)) {
        notifier_.Attach(new DeviceNotifier(static_cast<HANDLE>(wakeEvent_)));
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(notifier_.Get()))) notifier_.Reset();
    } else {
        ST_LOG_ERROR("audio", "MMDeviceEnumerator unavailable (hr=0x{:08X})", static_cast<uint32_t>(hr));
    }
}

WasapiOutput::~WasapiOutput() {
    close();
    if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_.Get());
    notifier_.Reset();
    enumerator_.Reset();
    if (event_) CloseHandle(event_);
}

bool WasapiOutput::isDeviceLost(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED || hr == E_NOTFOUND ||
           hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == AUDCLNT_E_RESOURCES_INVALIDATED;
}

void WasapiOutput::setPreferredDevice(std::wstring id) { preferred_ = std::move(id); }

namespace {

// Device formats tried in exclusive mode, best first: (container bits, valid bits, float).
struct SampleFormat {
    uint16_t bits, valid;
    bool isFloat;
};
constexpr SampleFormat kExclusiveFormats[] = {{32, 32, false}, {32, 24, false}, {24, 24, false}, {32, 32, true}, {16, 16, false}};
// Rates asked for in exclusiveRates().
constexpr uint32_t kProbeRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 32000, 22050, 16000};
constexpr REFERENCE_TIME kExclusivePeriod = 20 * 10'000;   // 20 ms: the engine thread has time to spare per event

WAVEFORMATEXTENSIBLE makeFormat(uint32_t rate, uint32_t channels, uint16_t bits, uint16_t valid, bool isFloat) {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = static_cast<WORD>(channels);
    fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = bits;
    fmt.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    fmt.Format.nAvgBytesPerSec = rate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = valid;
    fmt.dwChannelMask = channels == 1   ? SPEAKER_FRONT_CENTER
                        : channels == 2 ? KSAUDIO_SPEAKER_STEREO
                        : channels == 4 ? KSAUDIO_SPEAKER_QUAD
                        : channels == 6 ? KSAUDIO_SPEAKER_5POINT1
                        : channels == 8 ? KSAUDIO_SPEAKER_7POINT1_SURROUND
                                        : 0;
    fmt.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return fmt;
}

// The device would refuse every exclusive format: someone else holds it, or exclusive use is switched off for it.
bool refusesExclusive(HRESULT hr) { return hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED; }

} // namespace

ComPtr<IMMDevice> WasapiOutput::pickDevice(bool& followDefault, bool log) {
    ComPtr<IMMDevice> device;
    if (!enumerator_) return device;
    if (!preferred_.empty()) {
        DWORD state = 0;
        if (FAILED(enumerator_->GetDevice(preferred_.c_str(), &device)) || FAILED(device->GetState(&state)) ||
            state != DEVICE_STATE_ACTIVE) {
            device.Reset();
            if (log) ST_LOG_INFO("audio", "the chosen output device is not available: using the default device");
        }
    }
    followDefault = !device;
    if (!device) enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    return device;
}

uint32_t WasapiOutput::mixRate() {
    bool followDefault = true;
    ComPtr<IMMDevice> device = pickDevice(followDefault, false);
    if (!device) return 0;
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
        return 0;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return 0;
    const uint32_t rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    return rate;
}

std::vector<uint32_t> WasapiOutput::exclusiveRates() {
    std::vector<uint32_t> rates;
    bool followDefault = true;
    ComPtr<IMMDevice> device = pickDevice(followDefault, false);
    ComPtr<IAudioClient> client;
    if (!device ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
        return rates;
    for (const uint32_t rate : kProbeRates) {
        for (const auto& f : kExclusiveFormats) {
            WAVEFORMATEXTENSIBLE fmt = makeFormat(rate, 2, f.bits, f.valid, f.isFloat);
            const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr);
            if (refusesExclusive(hr)) {
                ST_LOG_INFO("audio", "exclusive mode unavailable on this device (hr=0x{:08X})", static_cast<uint32_t>(hr));
                return {};
            }
            if (hr == S_OK) {
                rates.push_back(rate);
                break;
            }
        }
    }
    std::string list;
    for (const uint32_t r : rates) list += std::format("{}{}", list.empty() ? "" : " ", r);
    ST_LOG_INFO("audio", "exclusive mode rates: {}", list.empty() ? "none" : list);
    return rates;
}

HRESULT WasapiOutput::open(uint32_t sampleRate, uint32_t channels) {
    close();
    if (!enumerator_) return E_NOINTERFACE;
    bool followDefault = true;
    ComPtr<IMMDevice> device = pickDevice(followDefault, true);
    if (!device) return E_NOTFOUND;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        if (notifier_) notifier_->setDevice(id, preferred_, followDefault);
        CoTaskMemFree(id);
    }
    info_ = {};
    if (wantExclusive_) {
        const HRESULT hr = openExclusive(device.Get(), sampleRate, channels);
        if (SUCCEEDED(hr)) return hr;
        if (isDeviceLost(hr)) return hr;
        info_.fallback = hr == AUDCLNT_E_DEVICE_IN_USE                ? Fallback::Busy
                         : hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ? Fallback::NotAllowed
                         : hr == AUDCLNT_E_UNSUPPORTED_FORMAT         ? Fallback::Format
                                                                      : Fallback::Other;
        ST_LOG_WARN("audio", "exclusive mode refused at {} Hz / {} ch (hr=0x{:08X}): shared mode", sampleRate, channels,
                    static_cast<uint32_t>(hr));
    }
    const Fallback fallback = info_.fallback;
    const HRESULT hr = openShared(device.Get(), sampleRate, channels);
    info_.fallback = fallback;
    return hr;
}

HRESULT WasapiOutput::openShared(IMMDevice* device, uint32_t sampleRate, uint32_t channels) {
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    WAVEFORMATEXTENSIBLE fmt = makeFormat(sampleRate, channels, 32, 32, true);
    constexpr REFERENCE_TIME kBufferDuration = 40 * 10'000;  // 40 ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                             kBufferDuration, 0, &fmt.Format, nullptr);
    if (SUCCEEDED(hr)) hr = client_->SetEventHandle(static_cast<HANDLE>(event_));
    UINT32 frames = 0;
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&frames);
    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) {
        close();
        return hr;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;
    bufferFrames_ = frames;
    info_ = {};
    info_.bits = info_.validBits = 32;
    info_.floatFormat = true;
    // The audio engine's own delay after it took a frame from our buffer. Some drivers report 0: one device period
    // (the engine's processing quantum, ~10 ms) is the floor then.
    REFERENCE_TIME latency = 0, period = 0;
    if (FAILED(client_->GetStreamLatency(&latency)) || latency <= 0) {
        REFERENCE_TIME minimum = 0;
        if (SUCCEEDED(client_->GetDevicePeriod(&period, &minimum))) latency = period;
    }
    latencyFrames_ = latency > 0 ? static_cast<uint32_t>(latency * sampleRate / 10'000'000) : 0;
    ST_LOG_INFO("audio", "WASAPI open: {} Hz, {} ch, buffer {} frames, stream latency {} frames", sampleRate, channels, frames,
                latencyFrames_);
    return S_OK;
}

HRESULT WasapiOutput::openExclusive(IMMDevice* device, uint32_t sampleRate, uint32_t channels) {
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    // Format negotiation: the requested rate and channels in the best sample format the device takes.
    WAVEFORMATEXTENSIBLE fmt{};
    const SampleFormat* chosen = nullptr;
    for (const auto& f : kExclusiveFormats) {
        WAVEFORMATEXTENSIBLE candidate = makeFormat(sampleRate, channels, f.bits, f.valid, f.isFloat);
        hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &candidate.Format, nullptr);
        if (refusesExclusive(hr) || isDeviceLost(hr)) return hr;
        if (hr == S_OK) {
            fmt = candidate;
            chosen = &f;
            break;
        }
    }
    if (!chosen) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME period = std::max(kExclusivePeriod, minPeriod);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period, &fmt.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED || (FAILED(hr) && !refusesExclusive(hr) && defPeriod > 0 && period != defPeriod)) {
        // The driver wants another size: its aligned buffer (or its default period), on a fresh client.
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned = 0;
            if (SUCCEEDED(client->GetBufferSize(&aligned)) && aligned > 0)
                period = static_cast<REFERENCE_TIME>(10'000'000.0 * aligned / sampleRate + 0.5);
        } else {
            period = defPeriod;
        }
        client.Reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period, &fmt.Format, nullptr);
    }
    if (FAILED(hr)) return hr;
    UINT32 frames = 0;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    UINT64 freq = 0;
    hr = client->SetEventHandle(static_cast<HANDLE>(event_));
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&frames);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&clock));
    if (SUCCEEDED(hr)) hr = clock->GetFrequency(&freq);
    if (SUCCEEDED(hr) && (frames == 0 || freq == 0)) hr = E_UNEXPECTED;
    if (FAILED(hr)) return hr;
    client_ = client;
    render_ = render;
    clock_ = clock;
    clockFreq_ = freq;
    sampleRate_ = sampleRate;
    channels_ = channels;
    bufferFrames_ = frames;
    released_ = 0;
    due_ = true;   // one buffer is filled before start()
    direct_ = chosen->isFloat && chosen->bits == 32;
    info_ = {};
    info_.exclusive = true;
    info_.bits = chosen->bits;
    info_.validBits = chosen->valid;
    info_.floatFormat = chosen->isFloat;
    REFERENCE_TIME latency = 0;
    if (FAILED(client_->GetStreamLatency(&latency)) || latency <= 0) latency = period;
    latencyFrames_ = static_cast<uint32_t>(latency * sampleRate / 10'000'000);
    ST_LOG_INFO("audio", "WASAPI exclusive: {} Hz, {} ch, {} bit{} ({} valid), buffer {} frames ({:.1f} ms), latency {} frames",
                sampleRate, channels, chosen->bits, chosen->isFloat ? " float" : "", chosen->valid, frames,
                frames * 1000.0 / sampleRate, latencyFrames_);
    return S_OK;
}

void WasapiOutput::close() {
    if (client_ && running_) client_->Stop();
    running_ = false;
    clock_.Reset();
    render_.Reset();
    client_.Reset();
    sampleRate_ = channels_ = bufferFrames_ = latencyFrames_ = 0;
    info_.exclusive = false;
    devBuffer_ = nullptr;
    due_ = false;
    if (notifier_) notifier_->setDevice({}, preferred_, true);
}

HRESULT WasapiOutput::start() {
    if (!client_) return E_UNEXPECTED;
    const HRESULT hr = client_->Start();
    running_ = SUCCEEDED(hr);
    return hr;
}

HRESULT WasapiOutput::stop() {
    if (!client_) return S_OK;
    HRESULT hr = client_->Stop();
    running_ = false;
    if (SUCCEEDED(hr)) hr = client_->Reset();
    released_ = 0;
    due_ = true;   // empty again: the next start prefills a buffer
    return hr;
}

HRESULT WasapiOutput::padding(uint32_t& frames) {
    frames = 0;
    if (!client_) return E_UNEXPECTED;
    if (info_.exclusive) {
        UINT64 pos = 0;
        const HRESULT hr = clock_->GetPosition(&pos, nullptr);
        if (FAILED(hr)) return hr;
        const uint64_t played = pos * sampleRate_ / clockFreq_;
        frames = static_cast<uint32_t>(std::min<uint64_t>(released_ > played ? released_ - played : 0, 2ull * bufferFrames_));
        return S_OK;
    }
    UINT32 p = 0;
    const HRESULT hr = client_->GetCurrentPadding(&p);
    frames = p;
    return hr;
}

uint32_t WasapiOutput::writable(uint32_t padding) const {
    if (info_.exclusive) return due_ ? bufferFrames_ : 0;
    return bufferFrames_ - std::min(padding, bufferFrames_);
}

HRESULT WasapiOutput::getBuffer(uint32_t frames, float*& data) {
    data = nullptr;
    if (!render_) return E_UNEXPECTED;
    BYTE* p = nullptr;
    const HRESULT hr = render_->GetBuffer(frames, &p);
    if (FAILED(hr)) return hr;
    if (info_.exclusive && !direct_) {
        devBuffer_ = p;
        conv_.resize(size_t(frames) * channels_);
        data = conv_.data();
    } else {
        data = reinterpret_cast<float*>(p);
    }
    return hr;
}

// The engine's float block (conv_) in the device's integer format (exclusive mode).
void WasapiOutput::convert(uint32_t frames) {
    const size_t n = size_t(frames) * channels_;
    const float* src = conv_.data();
    auto clamp = [](float v) { return v > 1.f ? 1.f : v < -1.f ? -1.f : v; };
    if (info_.bits == 32 && info_.validBits == 32) {
        auto* d = reinterpret_cast<int32_t*>(devBuffer_);
        for (size_t i = 0; i < n; ++i) d[i] = static_cast<int32_t>(std::lrint(std::min(2147483647.0, double(clamp(src[i])) * 2147483648.0)));
    } else if (info_.bits == 32) {   // 24 valid bits, left-justified
        auto* d = reinterpret_cast<int32_t*>(devBuffer_);
        for (size_t i = 0; i < n; ++i) d[i] = static_cast<int32_t>(std::min(8388607L, std::lrint(clamp(src[i]) * 8388608.f))) * 256;
    } else if (info_.bits == 24) {
        BYTE* d = devBuffer_;
        for (size_t i = 0; i < n; ++i) {
            const int32_t v = static_cast<int32_t>(std::min(8388607L, std::lrint(clamp(src[i]) * 8388608.f)));
            d[3 * i] = static_cast<BYTE>(v);
            d[3 * i + 1] = static_cast<BYTE>(v >> 8);
            d[3 * i + 2] = static_cast<BYTE>(v >> 16);
        }
    } else {   // 16 bits with triangular dither
        auto* d = reinterpret_cast<int16_t*>(devBuffer_);
        for (size_t i = 0; i < n; ++i) {
            dither_ = dither_ * 1664525u + 1013904223u;
            const float r1 = static_cast<float>(dither_ >> 8) * (1.f / 16777216.f);
            dither_ = dither_ * 1664525u + 1013904223u;
            const float r2 = static_cast<float>(dither_ >> 8) * (1.f / 16777216.f);
            const float v = clamp(src[i]) * 32767.f + (r1 - r2);
            d[i] = static_cast<int16_t>(std::clamp(std::lrint(v), -32768L, 32767L));
        }
    }
}

HRESULT WasapiOutput::releaseBuffer(uint32_t frames) {
    if (!render_) return E_UNEXPECTED;
    if (info_.exclusive) {
        if (!direct_ && devBuffer_) convert(frames);
        devBuffer_ = nullptr;
        released_ += frames;
        due_ = false;
    }
    return render_->ReleaseBuffer(frames, 0);
}

bool WasapiOutput::takeDeviceChanged() { return notifier_ && notifier_->take(); }

std::vector<WasapiOutput::Endpoint> WasapiOutput::endpoints(std::wstring& defaultId) {
    std::vector<Endpoint> out;
    defaultId.clear();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> devices;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
            ComPtr<IMMDevice> def;
            LPWSTR id = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && SUCCEEDED(def->GetId(&id)) && id) {
                defaultId = id;
                CoTaskMemFree(id);
            }
            UINT count = 0;
            devices->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device;
                ComPtr<IPropertyStore> props;
                if (FAILED(devices->Item(i, &device))) continue;
                Endpoint e;
                id = nullptr;
                if (FAILED(device->GetId(&id)) || !id) continue;
                e.id = id;
                CoTaskMemFree(id);
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal)
                        e.name = name.pwszVal;
                    PropVariantClear(&name);
                }
                if (e.name.empty()) e.name = e.id;
                out.push_back(std::move(e));
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

} // namespace st::audio
