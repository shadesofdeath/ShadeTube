#include "audio/WasapiOutput.h"

#include "core/Log.h"

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

    void setDevice(std::wstring id) {
        std::lock_guard lock(mutex_);
        deviceId_ = std::move(id);
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
        if (flow == eRender && role == eConsole) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (state != DEVICE_STATE_ACTIVE && isOurs(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override {
        if (isOurs(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~DeviceNotifier() = default;
    bool isOurs(LPCWSTR id) {
        std::lock_guard lock(mutex_);
        return id && !deviceId_.empty() && deviceId_ == id;
    }
    void signal() {
        changed_.store(true);
        SetEvent(wake_);
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<bool> changed_{false};
    HANDLE wake_;
    std::mutex mutex_;
    std::wstring deviceId_;
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

HRESULT WasapiOutput::open(uint32_t sampleRate, uint32_t channels) {
    close();
    if (!enumerator_) return E_NOINTERFACE;
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) return hr;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        if (notifier_) notifier_->setDevice(id);
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
    if (notifier_) notifier_->setDevice({});
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

} // namespace st::audio
