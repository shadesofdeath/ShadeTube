#pragma once
// Video mode's official-video alignment (app/OfficialVideo): a YouTube audio stream decoded whole to mono PCM at a low
// sample rate, for video/AudioAlign. The stream is fetched with the audio module's ranged download (ProgressiveBuffer,
// no read-ahead window: a 48 kbps soundtrack of a 4 minute song is ~1.4 MB) and decoded by its Media Foundation Source
// Reader (audio::Decoder) opened as the engine opens it, so the timeline is the engine's own: samples are placed by the
// first sample's time stamp and counted from there, decoder delays included (measured 2026-11-26 on one video: AAC-LC
// comes out 36 ms after Opus, HE-AAC 96 ms; the caller therefore decodes both soundtracks in the codec the engine plays,
// MatchService::soundtrackStream). An MP4 with an edit list, of which Media Foundation presents nothing, is read again
// with the edit list freed (video/Fmp4Index).
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
    std::string mimeType;          // "audio/mp4" | "audio/webm"
};

constexpr int kSoundtrackRate = 12000;

struct Soundtrack {
    int sampleRate = kSoundtrackRate;
    std::vector<float> mono;       // sample i is at i / sampleRate seconds of the stream's timeline
    double seconds() const { return sampleRate > 0 ? static_cast<double>(mono.size()) / sampleRate : 0; }
};

// Always kSoundtrackRate (the Source Reader's resampler, else a plain decimation here). Throws std::runtime_error when
// the download or the decoder fails or `cancelled` (optional, polled between samples) says so.
Soundtrack decodeSoundtrack(const SoundtrackSource& source, const std::function<bool()>& cancelled = {});

} // namespace st::video
