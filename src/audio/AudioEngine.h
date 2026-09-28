#pragma once
// Low-latency, low-memory audio playback engine.
//
//   network (range requests) -> in-memory progressive buffer -> IMFByteStream -> Media Foundation
//   Source Reader (AAC/MP4, Opus/WebM when the OS has the decoder) -> float PCM -> volume ramp /
//   normalisation gain -> WASAPI shared-mode event-driven render (AUTOCONVERTPCM handles resampling)
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
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace st::audio {

struct StreamSource {
    std::string url;              // direct media URL (googlevideo) - or empty when localPath is set
    int64_t contentLength = 0;    // bytes; 0 = unknown (engine will probe)
    std::string mimeType;         // "audio/mp4" | "audio/webm" (hint for the MF byte stream)
    std::wstring localPath;       // play an already downloaded file instead of the URL
    int64_t durationMsHint = 0;   // shown before the container header is parsed
    float gainDb = 0;             // loudness normalisation gain to apply (0 = none)
    uint64_t tag = 0;             // opaque caller id, echoed back in events
    bool live = false;            // endless internet-radio stream (url = station / playlist / HLS URL)
    bool allowLocalNetwork = false;   // live: may reach this machine / its network (a test server); else public only
};

enum class State { Idle, Loading, Playing, Paused, Ended, Error };

enum class ErrorKind {
    Network,            // download failed after retries (URL may have expired -> caller re-resolves); live: the
                        //   station could not be reached / kept dropping after the reconnect attempts
    UnsupportedFormat,  // no decoder (e.g. WebM/Opus on a system without the extension) -> caller retries with mp4;
                        //   live: a codec / container the engine can't play (Ogg Vorbis, FLAC, AC-3, SAMPLE-AES...)
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

    // Snapshot values (cheap, lock-free) for the UI's 60 fps player bar.
    State state() const;
    uint64_t currentTag() const;
    int64_t positionMs() const;
    int64_t durationMs() const;
    float bufferedFraction() const;   // 0..1 of the current track downloaded

    // 32 log-spaced magnitude bands (0..1) of the most recent output, for visualisers.
    // Returns false if not playing.
    bool spectrum(float* bands, int count) const;

    // True if Media Foundation on this machine can decode Opus-in-WebM.
    static bool supportsWebm();
    // Whether a live stream in this codec can play here, by radio-browser codec name or MIME type ("MP3", "AAC+",
    // "OGG", "audio/aacp", "application/vnd.apple.mpegurl"...). Unknown / empty = true (the engine sniffs the
    // stream and fails with UnsupportedFormat if it can't). Cheap; for hiding stations up front. "OGG" is false:
    // Ogg Vorbis has no decoder here, although stations labelled OGG that actually send Opus do play.
    static bool supportsLiveCodec(std::string_view codec);

    // The live stream being played (LiveInfo::active false otherwise). Any thread; takes a short lock.
    LiveInfo liveInfo() const;

    // Bytes currently held in media buffers (current + preloaded).
    size_t bufferedBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace st::audio
