#pragma once
// Video mode (app/VideoMode): which picture stream of a YouTube video to play next to the audio, and whether that
// video is worth showing at all. Pure functions over plain data (no network): tests/video checks them on fixtures.
//
// Choice: video-only streams up to the quality cap (480 / 720 / 1080 rows; the smaller side of a portrait video counts).
// H.264 (avc1, MP4) is preferred: stock Windows decodes it on the GPU everywhere. VP9 (WebM) and AV1 (MP4) are taken
// only when the caller verified a Media Foundation decoder for them and H.264 doesn't reach 480 rows (or the cap, when
// that is lower). At the same size: a frame rate up to 30 before 60 (half the decoding), YouTube's original before a
// "Super Resolution" upscale, then the higher bit rate. Without any video-only stream a muxed MP4 (itag 18, 360p H.264)
// still serves the picture.
//
// Static picture: many songs resolve to an "Art Track" (a still of the cover over the audio) or a lyric / audio upload
// with one image. Such a stream compresses to almost nothing, so its average bit rate (content length over the video's
// duration) gives it away before a byte is downloaded: under 200 kbps for 1280x720 (scaled by the picture's area,
// floor 45 kbps). Measured 2026-11-23 at 720p: Art Tracks 26-32 kbps, "(Official Audio)" / "(Audio)" stills 71-140,
// a lyric video 334, music videos 438-1058 (averages; YouTube's peak figure is 1.3-3x that).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::youtube {

struct VideoCandidate {
    int itag = 0;
    std::string container;     // "mp4" | "webm"
    std::string codec;         // "avc1.4d401f", "vp9", "av01.0.05M.08", ...
    int width = 0, height = 0;
    int fps = 0;
    int64_t bitrate = 0;       // bits per second (YouTube's peak figure)
    int64_t contentLength = 0; // bytes; 0 = unknown
    bool muxed = false;        // audio + video (itag 18)
    bool upscaled = false;     // YouTube "Super Resolution"
};

enum class VideoCodec { H264, Vp9, Av1, Other };
VideoCodec codecOf(const std::string& codec);

struct VideoDecoders {
    bool vp9 = false;          // MF has a VP9 decoder and the WebM source (verified by the caller)
    bool av1 = false;          // MF has an AV1 decoder
};

// The stream to play, or nullopt when none fits (no video stream at all, only codecs without a decoder).
std::optional<VideoCandidate> pickVideoStream(const std::vector<VideoCandidate>& streams, int maxRows,
                                              const VideoDecoders& decoders);

// Average bit rate in kbps of a stream over `durationSec` (0 when either is unknown).
int averageKbps(const VideoCandidate& s, double durationSec);
// True when the stream is almost certainly one still picture (see the header comment). Unknown length or duration:
// false (the video is shown).
bool looksStatic(const VideoCandidate& s, double durationSec);

} // namespace st::youtube
