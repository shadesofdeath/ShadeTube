#pragma once
// Offline download of one track's audio. Reuses the playback stack (ProgressiveBuffer for throttle-safe
// ranged download, Decoder for MF decode) and adds a Media Foundation MP3 encoder (Sink Writer with an
// MFAudioFormat_MP3 output — no FFmpeg, keeps the single-exe build). Two modes:
//   - mp3Kbps > 0 : decode -> PCM -> MP3 at that bitrate, then prepend an ID3v2.3 tag (title/artist/album
//                   /year + optional JPEG cover). Optionally CUTS time ranges (SponsorBlock segments) out
//                   of the PCM before encoding: sample-accurate, with a short raised-cosine fade at every
//                   splice so the joins don't click; the output timeline is continuous (no gaps).
//   - mp3Kbps == 0: passthrough — write the original AAC/Opus stream bytes verbatim (fastest, source quality).
//                   `cutMs` is IGNORED here: removing audio from a compressed stream needs a re-encode
//                   (or container surgery on AAC/Opus frame boundaries, which isn't sample-accurate).
// Blocking; call from a worker thread. Thread-safe across independent calls.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace st::audio {

struct DownloadRequest {
    std::string url;             // googlevideo audio stream URL (from youtube::MatchService)
    std::wstring localPath;      // alternative source: read this local file instead of `url` (tests)
    std::string mimeType;        // "audio/mp4" | "audio/webm" (| "audio/wav" for a local test source)
    int64_t contentLength = 0;   // 0 = probe
    std::wstring outPath;        // full destination path INCLUDING extension (.mp3 / .m4a / .webm)
    int mp3Kbps = 320;           // 0 = passthrough (no transcode)

    // Source-timeline ranges [startMs, endMs) to remove (MP3 mode only). Any order; overlapping ranges are
    // merged and ranges are clamped to the decoded length. Empty = keep everything.
    std::vector<std::pair<int64_t, int64_t>> cutMs;

    // ID3 metadata (MP3 mode only).
    std::string title, artist, album, year;
    std::vector<uint8_t> coverJpeg;   // optional front-cover APIC
    // Lyrics (MP3 mode only): the plain text goes to an USLT frame, synced lines to a SYLT frame. Empty = none. Lyrics
    // are timed to the song, which is what the file holds once the video's non-music parts are cut: no mapping.
    std::string lyricsText;
    std::vector<std::pair<int64_t, std::string>> syncedLyrics;
};

// What actually happened to the audio (filled on success; MP3 mode).
struct DownloadStats {
    int segmentsCut = 0;        // cut ranges that removed at least one frame
    int64_t cutMs = 0;          // audio removed, in ms
    int64_t sourceMs = 0;       // decoded source length, in ms
    int64_t outputMs = 0;       // encoded (PCM) length, in ms = sourceMs - cutMs
};

enum class DownloadStatus { Ok, Cancelled, NetworkError, DecodeError, EncodeError, IoError };

const char* toString(DownloadStatus s);

// Downloads and (optionally) transcodes. `progress` is called with 0..1 on the calling thread (it follows the
// SOURCE timeline, cut parts included); `cancel` is polled and aborts promptly. The destination directory
// must already exist. `stats` (optional) receives what was cut.
DownloadStatus downloadTrack(const DownloadRequest& req, const std::function<void(float)>& progress,
                             const std::atomic<bool>& cancel, DownloadStats* stats = nullptr);

// Sorts, clamps (start >= 0) and merges overlapping/touching ranges; drops empty ones. Exposed for tests.
std::vector<std::pair<int64_t, int64_t>> normalizeCuts(std::vector<std::pair<int64_t, int64_t>> cuts);

// The ID3v2.3 lyrics frames written for `text` (USLT) and `synced` (SYLT, ms): UTF-16 with a BOM, language "eng",
// empty descriptors. Exposed for tests.
std::vector<uint8_t> id3LyricsFrames(const std::string& text, const std::vector<std::pair<int64_t, std::string>>& synced);

} // namespace st::audio
