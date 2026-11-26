#pragma once
// Low-latency, low-memory audio playback engine.
//
//   network (range requests) -> progressive buffer -> built-in decoder (Opus / Vorbis in WebM, Matroska or Ogg:
//   audio/XiphDecoder) or IMFByteStream -> Media Foundation Source Reader (AAC/MP4, MP3, FLAC...) -> float PCM ->
//   [crossfade mix] -> equalizer -> volume ramp / normalisation gain -> look-ahead limiter -> WASAPI shared-mode
//   event-driven render (AUTOCONVERTPCM handles resampling) on the Windows default device or the one picked in the
//   settings
//
// Threading: the engine owns one decode/render thread. All public methods are thread-safe and
// non-blocking (commands are queued to the engine thread). Events are raised ON THE ENGINE THREAD;
// the app marshals them to the UI thread with st::Dispatcher::post.
//
// Memory: only the current track (and optionally the preloaded next track) is buffered in memory
// (a 4-minute 128 kbps AAC track is ~4 MB). Buffers are released as soon as a track is closed.
//
// Live streams (StreamSource::live, internet radio): Icecast / Shoutcast MP3 and AAC (ADTS, HE-AAC), Ogg Opus and
// HLS (MPEG-TS, packed audio, fragmented MP4) behind station playlists (.pls / .m3u / .asx / .m3u8). They have no
// duration and ignore seek(); a bounded rolling buffer (at most 60 s / 4 MB of compressed audio) starts playing
// after ~2 s, reconnects with backoff when the connection drops and reports ICY / ID3 titles (onTitle). Pausing
// keeps only the newest few seconds (a long pause disconnects), so resuming plays live again. See LiveStream.h.
//
// Crossfade: a preloaded track whose StreamSource::crossfadeMs > 0 starts that long before the current one ends and the
// two are mixed with equal-power curves (the caller decides per transition: e.g. none within an album). It needs the
// next track decoded in time and the same output format; otherwise the handoff stays gapless. Every track decodes to the
// device's mix rate, so formats normally match. Not while either plays at another speed than 1.
//
// Preview voice (song preview, app/Preview): openPreview() loads a second track that plays over the current one through
// the same equalizer (its own filter state), volume, limiter and device, with its own loudness gain. Once it has
// ~0.5 s decoded it fades in (350 ms) while the current track fades out (250 ms) and is then held: a seekable track is
// no longer read, so its position stays where it fell silent (playback speed and a crossfade in progress keep their
// state; the outgoing track of a mix is dropped, like a pause does); a live stream, or any track with holdMain false,
// keeps running muted, so it resumes live. stopPreview(), the preview's end or an error fades the preview out (250 ms)
// and the current track back in (400 ms). The current track's state, position, tag and events are untouched
// throughout: open / pause / seek / stop of it work as usual (a track opened meanwhile stays held as long as the
// preview plays). One preview at a time: a new one replaces the last (an audible one fades out under it).
#include "audio/Equalizer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::audio {

struct StreamSource {
    std::string url;              // direct media URL (googlevideo) - or empty when localPath is set
    int64_t contentLength = 0;    // bytes; 0 = unknown (engine will probe)
    std::string mimeType;         // "audio/mp4" | "audio/webm" (hint for the MF byte stream)
    std::wstring localPath;       // play an already downloaded file instead of the URL
    int64_t durationMsHint = 0;   // shown before the container header is parsed
    float gainDb = 0;             // loudness normalisation gain to apply (0 = none)
    bool replayGain = false;      // local file: gainDb is added to the file's ReplayGain track gain; no tag = no gain
    float maxBoostDb = 8;         // replayGain: the most the sum may raise the level
    int crossfadeMs = 0;          // as a preloaded next track: crossfade into it over this long (0 = gapless)
    float speed = 1;              // playback speed, pitch kept (audio/TimeStretch, 0.5 .. 3); live streams play at 1
    uint64_t tag = 0;             // opaque caller id, echoed back in events
    bool live = false;            // endless internet-radio stream (url = station / playlist / HLS URL)
    bool allowLocalNetwork = false;   // live: may reach this machine / its network (a test server); else public only
};

enum class State { Idle, Loading, Playing, Paused, Ended, Error };

enum class ErrorKind {
    Network,            // download failed after retries (URL may have expired -> caller re-resolves); live: the
                        //   station could not be reached / kept dropping after the reconnect attempts
    UnsupportedFormat,  // no decoder for the codec / container (another codec than Opus / Vorbis in WebM, AAC on a
                        //   Windows N without the Media Feature Pack) -> caller retries with mp4;
                        //   live: a codec / container the engine can't play (FLAC, AC-3, SAMPLE-AES...)
    Device,             // audio device error (engine keeps retrying on device change)
    Other,
};

struct EngineEvents {
    std::function<void(State state, uint64_t tag)> onState;
    // Current track finished. If a preloaded track existed, playback has ALREADY continued with it
    // gaplessly and `nextTag` is its tag; otherwise nextTag == 0.
    std::function<void(uint64_t finishedTag, uint64_t nextTag)> onEnded;
    std::function<void(ErrorKind kind, const std::string& message, uint64_t tag)> onError;
    // Live streams: the station's title (ICY StreamTitle / ID3 / Ogg comments, UTF-8, "" = cleared) changed. Fired
    // when the audio it belongs to becomes audible, not when it is received.
    std::function<void(const std::string& title, uint64_t tag)> onTitle;
    // Preview voice (openPreview, tag = its StreamSource::tag): Loading, Playing (audible: the fade-in starts), Ended
    // (it played to its end), Error (could not load / play), Idle (stopPreview).
    std::function<void(State state, uint64_t tag)> onPreview;
};

// An active render endpoint (AudioEngine::outputDevices()).
struct OutputDevice {
    std::wstring id;              // WASAPI endpoint id (StreamSource-independent; persisted by the caller)
    std::wstring name;            // friendly name ("Hoparlör (Realtek(R) Audio)")
    bool isDefault = false;       // the Windows default (console) render device
};

// What the engine knows about the live stream being played (all empty / 0 while nothing live plays).
struct LiveInfo {
    bool active = false;
    std::string codec;            // "MP3", "AAC", "HE-AAC", "Opus", ... ("" until the first frame)
    uint32_t bitrateKbps = 0;     // from the frame headers / icy-br, else measured
    uint32_t sampleRate = 0;      // decoded output
    uint32_t channels = 0;
    std::string stationName;      // icy-name
    std::string streamUrl;        // what finally plays (after playlists, HLS variants and redirects)
    bool hls = false;
    bool connected = false;
    int64_t bufferedMs = 0;       // compressed audio waiting to be decoded
    int reconnects = 0;
};

class AudioEngine {
public:
    explicit AudioEngine(EngineEvents events);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Replaces the current track. Starts buffering immediately; starts playing as soon as enough data
    // is decoded if `autoplay`. `startMs` allows resuming at a position.
    void open(const StreamSource& source, int64_t startMs = 0, bool autoplay = true);
    // Prepares the next track for gapless transition (download + decoder init, no output).
    void preload(const StreamSource& source);
    void clearPreload();

    void play();
    void pause();
    void togglePause();
    void stop();                  // releases the current track and its buffers
    void seek(int64_t ms);

    void setVolume(float linear); // 0..1; engine applies a perceptual curve and a 30 ms ramp (no clicks)
    float volume() const;

    // Speed of the current track (0.5 .. 3, pitch kept); the next one starts at its own StreamSource::speed. Ignored
    // for live streams. Positions and durations stay in track time.
    void setSpeed(float speed);

    // Preview voice (see the header comment). `startMs`: where the preview starts in its track. holdMain false keeps
    // the current track running muted under it (listen together: the session must not drift).
    void openPreview(const StreamSource& source, int64_t startMs, bool holdMain = true);
    void stopPreview();
    int64_t previewPositionMs() const;   // track position of the preview's last decoded block (0 = none yet)

    // Equalizer for everything played (applied within ~10 ms, no restart).
    void setEqualizer(const EqSettings& eq);
    // Render device: a WASAPI endpoint id, or "" to follow the Windows default. A device that is missing (unplugged)
    // falls back to the default and is taken again when it comes back.
    void setOutputDevice(const std::wstring& id);
    // The active render devices. Blocking (COM enumeration, a few ms); any thread.
    static std::vector<OutputDevice> outputDevices();

    // Snapshot values (cheap, lock-free) for the UI's 60 fps player bar.
    State state() const;
    uint64_t currentTag() const;
    int64_t positionMs() const;
    int64_t durationMs() const;
    float bufferedFraction() const;   // 0..1 of the current track downloaded

    // Audio clock for a picture that follows the sound (video mode): the track position heard at `atMs` (a QPC time
    // in ms, the same clock as ui::frame::realNow), with the output latency (the audio engine's stream latency and the
    // limiter's look-ahead) already taken off, and how fast it advances: the speed while sound plays, 0 while it stands
    // still (paused, loading, stalled, idle). Cheap (a short lock); any thread.
    struct Clock {
        uint64_t tag = 0;       // the track it belongs to
        double posMs = 0;
        double atMs = 0;
        double rate = 0;
        // The position heard at QPC time `qpcMs` (e.g. when the next frame is shown); extrapolated at most 120 ms past
        // the engine's last update, so a stall that has not been reported yet does not run away.
        double at(double qpcMs) const;
    };
    Clock clock() const;
    static double qpcNowMs();   // the clock's time base

    // 32 log-spaced magnitude bands (0..1) of the most recent output, for visualisers.
    // Returns false if not playing.
    bool spectrum(float* bands, int count) const;

    // Opus-in-WebM decodes on every machine (the built-in decoder), so this is true; kept for the stream choice.
    static bool supportsWebm();
    // Whether a live stream in this codec can play here, by radio-browser codec name or MIME type ("MP3", "AAC+",
    // "OGG", "audio/aacp", "application/vnd.apple.mpegurl"...). Unknown / empty = true (the engine sniffs the
    // stream and fails with UnsupportedFormat if it can't). Cheap; for hiding stations up front. Ogg Opus and Ogg
    // Vorbis ("OGG") play with the built-in decoders; FLAC, WMA and the like don't.
    static bool supportsLiveCodec(std::string_view codec);

    // The live stream being played (LiveInfo::active false otherwise). Any thread; takes a short lock.
    LiveInfo liveInfo() const;

    // Bytes of downloaded stream currently held (current + preloaded), in memory or in temporary files.
    size_t bufferedBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace st::audio
