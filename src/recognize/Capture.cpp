#include "recognize/Capture.h"

#include "audio/Resampler.h"
#include "core/Log.h"
#include "recognize/Signature.h"

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

using Microsoft::WRL::ComPtr;

namespace st::recognize {

namespace {

constexpr char kTag[] = "recognize";
constexpr REFERENCE_TIME kBuffer = 10'000'000;   // 1 s of device buffer: the loop drains it every 20 ms

const char* sourceName(Source s) { return s == Source::System ? "system sound" : "microphone"; }

CaptureError errorOf(HRESULT hr) {
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return CaptureError::AccessDenied;
    if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return CaptureError::DeviceBusy;
    if (hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == AUDCLNT_E_DEVICE_INVALIDATED)
        return CaptureError::NoDevice;
    return CaptureError::Failed;
}

bool deviceLost(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// Completion of ActivateAudioInterfaceAsync (agile: Windows calls it on one of its own threads).
class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
                                          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() override {
        if (done_) CloseHandle(done_);
    }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr = E_FAIL;
        ComPtr<IUnknown> unk;
        const HRESULT got = op->GetActivateResult(&hr, &unk);
        result_ = FAILED(got) ? got : hr;
        if (SUCCEEDED(result_) && unk) unk.As(&client_);
        SetEvent(done_);
        return S_OK;
    }
    // Waits for the activation; the client or nullptr (result() says why).
    ComPtr<IAudioClient> wait(DWORD ms) {
        if (WaitForSingleObject(done_, ms) != WAIT_OBJECT_0) {
            result_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return nullptr;
        }
        return client_;
    }
    HRESULT result() const { return result_; }

private:
    HANDLE done_ = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> client_;
};

// Default-device changes and lost devices: a flag and the loop's wake-up (on a system thread).
class Notifier final : public IMMNotificationClient {
public:
    Notifier(HANDLE wake, EDataFlow flow) : wake_(wake), flow_(flow) {}
    bool take() { return changed_.exchange(false); }
    void setDevice(std::wstring id) {
        std::lock_guard lock(m_);
        id_ = std::move(id);
    }

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
        if (flow == flow_ && role == eConsole) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (state != DEVICE_STATE_ACTIVE && ours(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override {
        if (ours(id)) signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~Notifier() = default;
    bool ours(LPCWSTR id) {
        std::lock_guard lock(m_);
        return id && !id_.empty() && id_ == id;
    }
    void signal() {
        changed_ = true;
        SetEvent(wake_);
    }
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> changed_{false};
    HANDLE wake_;
    EDataFlow flow_;
    std::mutex m_;
    std::wstring id_;
};

struct Format {
    uint32_t rate = 0, channels = 0, bits = 0;
    bool isFloat = false;
};

Format readFormat(const WAVEFORMATEX* f) {
    Format out;
    out.rate = f->nSamplesPerSec;
    out.channels = f->nChannels;
    out.bits = f->wBitsPerSample;
    out.isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        out.isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return out;
}

WAVEFORMATEX makeFormat(uint32_t rate, uint32_t channels, bool isFloat) {
    WAVEFORMATEX f{};
    f.wFormatTag = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels = static_cast<WORD>(channels);
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = isFloat ? 32 : 16;
    f.nBlockAlign = static_cast<WORD>(channels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    return f;
}

} // namespace

struct Capture::Device {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<Notifier> notifier;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE packetEvent = nullptr;   // signalled by WASAPI (event-driven clients)
    HANDLE wake = nullptr;          // the notifier's
    Format format;
    bool processLoopback = false;
    std::wstring endpointId;

    ~Device() {
        close();
        if (enumerator && notifier) enumerator->UnregisterEndpointNotificationCallback(notifier.Get());
        notifier.Reset();
        enumerator.Reset();
        if (packetEvent) CloseHandle(packetEvent);
        if (wake) CloseHandle(wake);
    }
    void close() {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
    }
};

struct Capture::Convert {
    audio::Resampler resampler;
    uint32_t rate = 0;
    std::vector<float> mono, resampled;
    float blockPeak = 0;
    size_t blockSamples = 0;
};

namespace {

// Everything except this process tree (Windows 10 2004+). The format is ours to pick: 16 kHz mono float when the
// engine converts to it, else 48 kHz stereo float.
HRESULT openProcessLoopback(Capture::Device& d) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    if (!handler) return E_OUTOFMEMORY;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), &op);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioClient> client = handler->wait(3000);
    if (!client) return FAILED(handler->result()) ? handler->result() : E_FAIL;
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const WAVEFORMATEX tries[] = {makeFormat(kSampleRate, 1, true), makeFormat(48000, 2, true), makeFormat(44100, 2, false)};
    for (const auto& f : tries) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBuffer, 0, &f, nullptr);
        if (SUCCEEDED(hr)) {
            d.format = readFormat(&f);
            break;
        }
        // A client can be initialised once: a refused format needs a new one.
        handler = Microsoft::WRL::Make<ActivationHandler>();
        op.Reset();
        if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), &op)))
            return hr;
        client = handler->wait(3000);
        if (!client) return hr;
    }
    if (FAILED(hr)) return hr;
    d.client = client;
    d.processLoopback = true;
    return S_OK;
}

// An endpoint's own client: the default render endpoint in loopback, or the default microphone.
HRESULT openEndpoint(Capture::Device& d, Source source) {
    if (!d.enumerator) return E_FAIL;
    ComPtr<IMMDevice> device;
    HRESULT hr = d.enumerator->GetDefaultAudioEndpoint(source == Source::System ? eRender : eCapture, eConsole, &device);
    if (FAILED(hr) || !device) return FAILED(hr) ? hr : E_NOTFOUND;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        d.endpointId = id;
        CoTaskMemFree(id);
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return FAILED(hr) ? hr : E_FAIL;
    // Loopback: polled (no packets at all arrive while nothing plays). The microphone: event-driven.
    const DWORD flags = source == Source::System ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBuffer, 0, mix, nullptr);
    d.format = readFormat(mix);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return hr;
    d.client = client;
    d.processLoopback = false;
    return S_OK;
}

// Dev / tests: SHADETUBE_RECOGNIZE_LOOPBACK=endpoint uses the output's own loopback (ShadeTube's sound included), as on a
// Windows without process loopback.
bool processLoopbackAllowed() {
    wchar_t v[16] = {};
    return !(GetEnvironmentVariableW(L"SHADETUBE_RECOGNIZE_LOOPBACK", v, 16) && std::wstring(v) == L"endpoint");
}

HRESULT openDevice(Capture::Device& d, Source source, bool allowProcess) {
    d.close();
    HRESULT hr = E_FAIL;
    bool usedEvent = false;
    if (source == Source::System && allowProcess) {
        hr = openProcessLoopback(d);
        if (FAILED(hr)) ST_LOG_INFO(kTag, "process loopback unavailable (hr=0x{:08X}): the output's loopback instead", static_cast<uint32_t>(hr));
        usedEvent = SUCCEEDED(hr);
    }
    if (!d.client) {
        hr = openEndpoint(d, source);
        if (FAILED(hr)) return hr;
        usedEvent = source == Source::Microphone;
    }
    if (usedEvent) {
        if (!d.packetEvent) d.packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = d.client->SetEventHandle(d.packetEvent);
        if (FAILED(hr)) return hr;
    }
    hr = d.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(d.capture.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (d.notifier) d.notifier->setDevice(d.processLoopback ? std::wstring() : d.endpointId);
    hr = d.client->Start();
    if (FAILED(hr)) return hr;
    ST_LOG_INFO(kTag, "capturing the {} ({}): {} Hz x {} {}-bit {}", sourceName(source),
                d.processLoopback ? "every app but ShadeTube" : "the default endpoint", d.format.rate, d.format.channels,
                d.format.bits, d.format.isFloat ? "float" : "PCM");
    return S_OK;
}

} // namespace

const char* captureErrorName(CaptureError e) {
    switch (e) {
    case CaptureError::None: return "none";
    case CaptureError::NoDevice: return "no device";
    case CaptureError::DeviceBusy: return "device busy";
    case CaptureError::AccessDenied: return "access denied";
    case CaptureError::Failed: return "failed";
    }
    return "?";
}

bool Capture::outputHeldExclusively() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool held = false;
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) &&
            SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            held = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBuffer, 0, mix, nullptr) == AUDCLNT_E_DEVICE_IN_USE;
            CoTaskMemFree(mix);
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return held;
}

Capture::Capture() : convert_(std::make_unique<Convert>()) { stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

Capture::~Capture() {
    stop();
    if (stopEvent_) CloseHandle(stopEvent_);
}

bool Capture::start(Source source) {
    stop();
    source_ = source;
    error_ = CaptureError::None;
    level_ = 0;
    reopened_ = 0;
    {
        std::lock_guard lock(mutex_);
        out_.clear();
    }
    *convert_ = Convert{};
    ResetEvent(stopEvent_);
    // The device is opened on the capture thread (its COM apartment owns every object); start() waits for the verdict.
    auto opened = std::make_shared<std::promise<HRESULT>>();
    auto verdict = opened->get_future();
    thread_ = std::jthread([this, source, opened](std::stop_token st) {
        SetThreadDescription(GetCurrentThread(), L"st-capture");
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            auto d = std::make_shared<Device>();
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d->enumerator));
            d->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (d->enumerator) {
                d->notifier.Attach(new Notifier(d->wake, source == Source::System ? eRender : eCapture));
                if (FAILED(d->enumerator->RegisterEndpointNotificationCallback(d->notifier.Get()))) d->notifier.Reset();
            }
            const HRESULT hr = openDevice(*d, source, processLoopbackAllowed());
            ownIncluded_ = source == Source::System && SUCCEEDED(hr) && !d->processLoopback;
            opened->set_value(hr);
            if (SUCCEEDED(hr)) run(st, d);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    });
    const HRESULT hr = verdict.get();
    if (FAILED(hr)) {
        ST_LOG_WARN(kTag, "the {} can't be captured (hr=0x{:08X})", sourceName(source), static_cast<uint32_t>(hr));
        error_ = errorOf(hr);
        thread_.join();
        return false;
    }
    return true;
}

void Capture::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    SetEvent(stopEvent_);
    thread_.join();
    level_ = 0;
}

void Capture::take(std::vector<float>& out) {
    std::lock_guard lock(mutex_);
    out.insert(out.end(), out_.begin(), out_.end());
    out_.clear();
}

void Capture::run(std::stop_token st, std::shared_ptr<Device> d) {
    std::vector<float> scratch;
    auto lastOk = std::chrono::steady_clock::now();
    bool lost = false;
    while (!st.stop_requested()) {
        HANDLE handles[3] = {stopEvent_, d->wake, d->packetEvent};
        const DWORD n = d->packetEvent && !lost ? 3 : 2;
        WaitForMultipleObjects(n, handles, FALSE, lost ? 250 : 20);
        if (st.stop_requested()) break;
        bool reopen = d->notifier && d->notifier->take();
        if (!lost && !reopen) {
            // Drain every packet that is ready.
            for (;;) {
                UINT32 packet = 0;
                HRESULT hr = d->capture->GetNextPacketSize(&packet);
                if (FAILED(hr)) {
                    if (deviceLost(hr)) reopen = true;
                    else ST_LOG_WARN(kTag, "GetNextPacketSize failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
                    break;
                }
                if (packet == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = d->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    if (deviceLost(hr)) reopen = true;
                    break;
                }
                const Format& f = d->format;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
                scratch.resize(static_cast<size_t>(frames) * f.channels);
                if (!silent) {
                    const size_t count = scratch.size();
                    if (f.isFloat && f.bits == 32) {
                        std::memcpy(scratch.data(), data, count * sizeof(float));
                    } else if (f.bits == 16) {
                        const auto* s = reinterpret_cast<const int16_t*>(data);
                        for (size_t i = 0; i < count; ++i) scratch[i] = s[i] / 32768.f;
                    } else if (f.bits == 32) {
                        const auto* s = reinterpret_cast<const int32_t*>(data);
                        for (size_t i = 0; i < count; ++i) scratch[i] = static_cast<float>(s[i] / 2147483648.0);
                    } else if (f.bits == 24) {
                        for (size_t i = 0; i < count; ++i) {
                            const BYTE* p = data + i * 3;
                            const int32_t v = (int32_t(p[0]) << 8 | int32_t(p[1]) << 16 | int32_t(p[2]) << 24) >> 8;
                            scratch[i] = v / 8388608.f;
                        }
                    } else {
                        std::fill(scratch.begin(), scratch.end(), 0.f);
                    }
                }
                deliver(scratch.data(), frames, f.channels, f.rate, silent);
                d->capture->ReleaseBuffer(frames);
                lastOk = std::chrono::steady_clock::now();
            }
        }
        if (reopen || lost) {
            // A new default device, or ours went away: the current default, retried for a while.
            const HRESULT hr = openDevice(*d, source_, processLoopbackAllowed());
            if (SUCCEEDED(hr)) {
                if (lost || reopen) ++reopened_;
                lost = false;
                lastOk = std::chrono::steady_clock::now();
                ownIncluded_ = source_ == Source::System && !d->processLoopback;
                convert_->rate = 0;   // the new format reconfigures the resampler
                ST_LOG_INFO(kTag, "capture reopened on the current default device");
            } else {
                lost = true;
                if (std::chrono::steady_clock::now() - lastOk > std::chrono::seconds(4)) {
                    ST_LOG_WARN(kTag, "no {} to capture any more (hr=0x{:08X})", sourceName(source_), static_cast<uint32_t>(hr));
                    error_ = errorOf(hr) == CaptureError::Failed ? CaptureError::NoDevice : errorOf(hr);
                    break;
                }
            }
        }
    }
    d->close();
}

void Capture::deliver(const float* in, uint32_t frames, uint32_t channels, uint32_t rate, bool silent) {
    auto& c = *convert_;
    if (c.rate != rate) {
        c.rate = rate;
        c.resampler.configure(rate, kSampleRate, 1);
    }
    c.mono.resize(frames);
    if (silent) {
        std::fill(c.mono.begin(), c.mono.end(), 0.f);
    } else {
        const float scale = 1.f / static_cast<float>(std::max<uint32_t>(1, channels));
        for (uint32_t i = 0; i < frames; ++i) {
            float s = 0;
            for (uint32_t ch = 0; ch < channels; ++ch) s += in[static_cast<size_t>(i) * channels + ch];
            c.mono[i] = s * scale;
        }
    }
    c.resampled.clear();
    c.resampler.process(c.mono.data(), c.mono.size(), c.resampled);
    // The level: the peak of each ~32 ms block.
    for (float s : c.resampled) {
        c.blockPeak = std::max(c.blockPeak, std::abs(s));
        if (++c.blockSamples >= 512) {
            level_.store(std::min(1.f, c.blockPeak), std::memory_order_relaxed);
            c.blockPeak = 0;
            c.blockSamples = 0;
        }
    }
    std::lock_guard lock(mutex_);
    out_.insert(out_.end(), c.resampled.begin(), c.resampled.end());
}

} // namespace st::recognize
