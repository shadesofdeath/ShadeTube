#pragma once
// Video mode's official-video alignment (app/OfficialVideo): a YouTube audio stream decoded whole to mono PCM at a low
// sample rate, for video/AudioAlign. The stream is fetched with the audio module's ranged download (ProgressiveBuffer,
// no read-ahead window: a ~50 kbps Opus soundtrack of a 4 minute song is ~1.4 MB) and decoded by the engine's own
// audio::Decoder, so the timeline is the engine's: samples are placed by the first sample's time stamp and counted
// from there, as the engine does. The caller reads Opus (the smallest stream, MatchService::soundtrackStream): the
// built-in decoder trims its pre-skip, so the timeline is the presentation one the picture runs on as well (measured
// before that, Media Foundation's Opus came out 6.5 ms late and AAC-LC 36 ms after it, HE-AAC 96 ms).
//
// Worker threads only (blocks on the network; initialises COM / Media Foundation for the call).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace st::video {

struct SoundtrackSource {
    std::string url;
    std::wstring localPath;        // tests / dev: a local file instead of the URL
    int64_t contentLength = 0;     // bytes (0 = probed)
    std::string mimeType;          // "audio/webm" | "audio/mp4"
};

constexpr int kSoundtrackRate = 12000;

struct Soundtrack {
    int sampleRate = kSoundtrackRate;
    std::vector<float> mono;       // sample i is at i / sampleRate seconds of the stream's timeline
    double seconds() const { return sampleRate > 0 ? static_cast<double>(mono.size()) / sampleRate : 0; }
};

// Always kSoundtrackRate (decoded at the stream's own rate, then a plain decimation). Throws std::runtime_error when the
// download or the decoder fails or `cancelled` (optional, polled between samples) says so.
Soundtrack decodeSoundtrack(const SoundtrackSource& source, const std::function<bool()>& cancelled = {});

} // namespace st::video
