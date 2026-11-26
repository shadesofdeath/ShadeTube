#pragma once
// WASAPI event-driven render client (internal to st_audio) on the default endpoint, or on a preferred one
// (setPreferredDevice) while it is present: an unplugged preferred device falls back to the default and is taken again
// when it comes back.
//
// Shared mode (the default): the client is initialised with the DECODER's float format and
// AUTOCONVERTPCM | SRC_DEFAULT_QUALITY, so Windows resamples / remixes to the mix format. Buffer: ~40 ms.
//
// Exclusive mode (setExclusive): the stream owns the device, bypassing the Windows mixer. open() negotiates the best
// format the device takes at the requested rate and channel count (32-bit integer, 24-in-32, packed 24-bit, 32-bit
// float, 16-bit, in that order) with IsFormatSupported, initialises event-driven with equal buffer and period (20 ms,
// or the aligned size the driver asks for) and converts the engine's float blocks to it (16-bit with TPDF dither).
// Every WASAPI event wants one whole buffer: writable() says how much may be written now. When the device refuses
// (another app holds it, exclusive use is not allowed in its properties, no matching format) open() falls back to
// shared mode and info() says why; the next open() tries exclusive again.
//
// Default-device changes (while following the default), endpoint removal and the return of the preferred device are
// reported through an IMMNotificationClient that sets a flag and signals the engine's wake event; the engine then
// reopens on its own thread (never from the notification callback).
//
// Threading: every method must be called on the engine thread (which has COM initialised as MTA),
// except that the notification callback runs on a system thread and only touches atomics.
#include <cstdint>
#include <string>
#include <vector>

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

    enum class Fallback { None, Busy, NotAllowed, Format, Other };
    struct Info {
        bool exclusive = false;        // the open stream holds the device exclusively
        Fallback fallback = Fallback::None;   // exclusive wanted but refused at the last open
        uint32_t bits = 0;             // container bits of the device format (exclusive); 32 = float in shared mode
        uint32_t validBits = 0;
        bool floatFormat = false;
    };

    HRESULT open(uint32_t sampleRate, uint32_t channels);
    void close();
    // "" = the Windows default device. Takes effect at the next open().
    void setPreferredDevice(std::wstring id);
    void setExclusive(bool on) { wantExclusive_ = on; }   // takes effect at the next open()
    bool exclusiveWanted() const { return wantExclusive_; }
    // The shared-mode mix rate of the device open() would use now (the preferred one when present); 0 if unknown.
    uint32_t mixRate();
    // The rates that device takes in exclusive mode (stereo, any of the formats above); empty when it takes none
    // (exclusive use not allowed, held by another app) or it can't be asked.
    std::vector<uint32_t> exclusiveRates();
    const std::wstring& preferredDevice() const { return preferred_; }
    bool isOpen() const { return client_ != nullptr; }
    bool running() const { return running_; }
    const Info& info() const { return info_; }

    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t channels() const { return channels_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    // IAudioClient::GetStreamLatency in frames: how much later than it leaves our buffer a frame is heard.
    uint32_t latencyFrames() const { return latencyFrames_; }
    void* event() const { return event_; }  // signalled by WASAPI when it wants more data
    void signalled() { due_ = true; }        // the engine saw event() fire (exclusive: one buffer is due)

    HRESULT start();
    HRESULT stop();  // Stop + Reset (discards queued frames)
    // Frames written but not heard yet (exclusive: from the device clock).
    HRESULT padding(uint32_t& frames);
    // How many frames may be written now, given padding(): the free space in shared mode; in exclusive mode the whole
    // buffer once per event (and once before start()), else 0.
    uint32_t writable(uint32_t padding) const;
    // A float buffer for `frames` frames (the device's own in shared mode; a conversion buffer in exclusive mode).
    HRESULT getBuffer(uint32_t frames, float*& data);
    HRESULT releaseBuffer(uint32_t frames);

    // True once after the default render device changed (while following it), our device went away or the preferred
    // device came back.
    bool takeDeviceChanged();

    static bool isDeviceLost(HRESULT hr);

    // Active render endpoints with their friendly names; `defaultId` receives the default console device's id.
    // Any thread: initialises COM (MTA) for the call when needed.
    struct Endpoint {
        std::wstring id, name;
    };
    static std::vector<Endpoint> endpoints(std::wstring& defaultId);

private:
    Microsoft::WRL::ComPtr<IMMDevice> pickDevice(bool& followDefault, bool log);
    HRESULT openShared(IMMDevice* device, uint32_t sampleRate, uint32_t channels);
    HRESULT openExclusive(IMMDevice* device, uint32_t sampleRate, uint32_t channels);
    void convert(uint32_t frames);

    void* wakeEvent_;
    void* event_ = nullptr;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<DeviceNotifier> notifier_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    Microsoft::WRL::ComPtr<IAudioClock> clock_;   // exclusive: what has been played
    uint32_t sampleRate_ = 0, channels_ = 0, bufferFrames_ = 0, latencyFrames_ = 0;
    bool running_ = false;
    std::wstring preferred_;
    bool wantExclusive_ = false;
    Info info_;
    // ---- exclusive
    bool due_ = false;
    uint64_t clockFreq_ = 0;
    uint64_t released_ = 0;           // frames handed to the device since start
    std::vector<float> conv_;         // float block converted at releaseBuffer
    BYTE* devBuffer_ = nullptr;
    bool direct_ = false;             // the device format is our float layout: no conversion
    uint32_t dither_ = 0x12345678;
};

} // namespace st::audio
