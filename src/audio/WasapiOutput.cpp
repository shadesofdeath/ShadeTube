#include "audio/WasapiOutput.h"

#include "core/Log.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <windows.h>

#include <atomic>
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
           hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

void WasapiOutput::setPreferredDevice(std::wstring id) { preferred_ = std::move(id); }

uint32_t WasapiOutput::mixRate() {
    if (!enumerator_) return 0;
    ComPtr<IMMDevice> device;
    if (!preferred_.empty()) {
        DWORD state = 0;
        if (FAILED(enumerator_->GetDevice(preferred_.c_str(), &device)) || FAILED(device->GetState(&state)) ||
            state != DEVICE_STATE_ACTIVE)
            device.Reset();
    }
    if (!device && FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return 0;
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
        return 0;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return 0;
    const uint32_t rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    return rate;
}

HRESULT WasapiOutput::open(uint32_t sampleRate, uint32_t channels) {
    close();
    if (!enumerator_) return E_NOINTERFACE;
    ComPtr<IMMDevice> device;
    HRESULT hr = E_FAIL;
    if (!preferred_.empty()) {
        DWORD state = 0;
        if (FAILED(enumerator_->GetDevice(preferred_.c_str(), &device)) || FAILED(device->GetState(&state)) ||
            state != DEVICE_STATE_ACTIVE) {
            device.Reset();
            ST_LOG_INFO("audio", "the chosen output device is not available: using the default device");
        }
    }
    const bool followDefault = !device;
    if (!device) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) return hr;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        if (notifier_) notifier_->setDevice(id, preferred_, followDefault);
        CoTaskMemFree(id);
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = static_cast<WORD>(channels);
    fmt.Format.nSamplesPerSec = sampleRate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    fmt.Format.nAvgBytesPerSec = sampleRate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = channels == 1   ? SPEAKER_FRONT_CENTER
                        : channels == 2 ? KSAUDIO_SPEAKER_STEREO
                        : channels == 4 ? KSAUDIO_SPEAKER_QUAD
                        : channels == 6 ? KSAUDIO_SPEAKER_5POINT1
                        : channels == 8 ? KSAUDIO_SPEAKER_7POINT1_SURROUND
                                        : 0;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

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
    ST_LOG_INFO("audio", "WASAPI open: {} Hz, {} ch, buffer {} frames", sampleRate, channels, frames);
    return S_OK;
}

void WasapiOutput::close() {
    if (client_ && running_) client_->Stop();
    running_ = false;
    render_.Reset();
    client_.Reset();
    sampleRate_ = channels_ = bufferFrames_ = 0;
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
    return hr;
}

HRESULT WasapiOutput::padding(uint32_t& frames) {
    UINT32 p = 0;
    const HRESULT hr = client_ ? client_->GetCurrentPadding(&p) : E_UNEXPECTED;
    frames = p;
    return hr;
}

HRESULT WasapiOutput::getBuffer(uint32_t frames, float*& data) {
    BYTE* p = nullptr;
    const HRESULT hr = render_ ? render_->GetBuffer(frames, &p) : E_UNEXPECTED;
    data = reinterpret_cast<float*>(p);
    return hr;
}

HRESULT WasapiOutput::releaseBuffer(uint32_t frames) { return render_ ? render_->ReleaseBuffer(frames, 0) : E_UNEXPECTED; }

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
