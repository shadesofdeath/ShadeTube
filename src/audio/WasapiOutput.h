#pragma once
// WASAPI shared-mode, event-driven render client on the default endpoint (internal to st_audio).
//
// The client is initialised with the DECODER's float format and
// AUTOCONVERTPCM | SRC_DEFAULT_QUALITY, so Windows resamples / remixes to the mix format.
// Buffer: ~40 ms. Default-device changes and endpoint removal are reported through an
// IMMNotificationClient that sets a flag and signals the engine's wake event; the engine then
// reopens on its own thread (never from the notification callback).
//
// Threading: every method must be called on the engine thread (which has COM initialised as MTA),
// except that the notification callback runs on a system thread and only touches atomics.
#include <cstdint>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace st::audio {

class DeviceNotifier;

class WasapiOutput {
public:
    explicit WasapiOutput(void* wakeEvent);
    ~WasapiOutput();
    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    HRESULT open(uint32_t sampleRate, uint32_t channels);
    void close();
    bool isOpen() const { return client_ != nullptr; }
    bool running() const { return running_; }

    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t channels() const { return channels_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    void* event() const { return event_; }  // signalled by WASAPI when it wants more data

    HRESULT start();
    HRESULT stop();  // Stop + Reset (discards queued frames)
    HRESULT padding(uint32_t& frames);
    HRESULT getBuffer(uint32_t frames, float*& data);
    HRESULT releaseBuffer(uint32_t frames);

    // True once after the default render device changed or our device went away.
    bool takeDeviceChanged();

    static bool isDeviceLost(HRESULT hr);

private:
    void* wakeEvent_;
    void* event_ = nullptr;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<DeviceNotifier> notifier_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    uint32_t sampleRate_ = 0, channels_ = 0, bufferFrames_ = 0;
    bool running_ = false;
};

} // namespace st::audio
