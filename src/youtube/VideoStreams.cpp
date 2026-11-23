#include "youtube/VideoStreams.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace st::youtube {

VideoCodec codecOf(const std::string& codec) {
    if (codec.rfind("avc1", 0) == 0 || codec.rfind("avc3", 0) == 0 || codec == "h264") return VideoCodec::H264;
    if (codec.rfind("vp9", 0) == 0 || codec.rfind("vp09", 0) == 0) return VideoCodec::Vp9;
    if (codec.rfind("av01", 0) == 0 || codec == "av1") return VideoCodec::Av1;
    return VideoCodec::Other;
}

namespace {

// The rows the quality cap is about: the smaller side, so a portrait (vertical) video at 720x1280 counts as 720p.
int rowsOf(const VideoCandidate& s) {
    if (s.width > 0 && s.height > 0) return std::min(s.width, s.height);
    return s.height;
}

bool decodable(const VideoCandidate& s, const VideoDecoders& d) {
    switch (codecOf(s.codec)) {
    case VideoCodec::H264: return s.container != "webm";
    case VideoCodec::Vp9: return d.vp9 && s.container == "webm";
    case VideoCodec::Av1: return d.av1 && s.container == "mp4";
    default: return false;
    }
}

// Best first: more rows, then up to 30 fps, then not upscaled, then the higher bit rate.
bool better(const VideoCandidate& a, const VideoCandidate& b) {
    auto key = [](const VideoCandidate& s) {
        return std::make_tuple(rowsOf(s), s.fps > 0 && s.fps <= 30, !s.upscaled, s.bitrate);
    };
    return key(a) > key(b);
}

std::optional<VideoCandidate> best(const std::vector<VideoCandidate>& streams, int maxRows, const VideoDecoders& d,
                                   bool h264Only) {
    std::optional<VideoCandidate> pick;
    for (const auto& s : streams) {
        if (s.muxed || !decodable(s, d) || rowsOf(s) <= 0 || rowsOf(s) > maxRows) continue;
        if (h264Only != (codecOf(s.codec) == VideoCodec::H264)) continue;
        if (!pick || better(s, *pick)) pick = s;
    }
    return pick;
}

} // namespace

std::optional<VideoCandidate> pickVideoStream(const std::vector<VideoCandidate>& streams, int maxRows,
                                              const VideoDecoders& decoders) {
    maxRows = std::max(144, maxRows);
    const auto h264 = best(streams, maxRows, decoders, true);
    // H.264 that reaches 480 rows (or the cap) wins; otherwise VP9 / AV1 when they are bigger and decodable here.
    if (!h264 || rowsOf(*h264) < std::min(480, maxRows)) {
        const auto other = best(streams, maxRows, decoders, false);
        if (other && (!h264 || rowsOf(*other) > rowsOf(*h264))) return other;
    }
    if (h264) return h264;
    // No video-only stream: the muxed MP4 (its audio track is never decoded).
    std::optional<VideoCandidate> muxed;
    for (const auto& s : streams)
        if (s.muxed && codecOf(s.codec) == VideoCodec::H264 && s.container == "mp4" && rowsOf(s) > 0 &&
            (!muxed || better(s, *muxed)))
            muxed = s;
    return muxed;
}

int averageKbps(const VideoCandidate& s, double durationSec) {
    if (s.contentLength <= 0 || durationSec < 1) return 0;
    return static_cast<int>(std::lround(static_cast<double>(s.contentLength) * 8.0 / durationSec / 1000.0));
}

bool looksStatic(const VideoCandidate& s, double durationSec) {
    const int kbps = averageKbps(s, durationSec);
    if (kbps <= 0 || s.muxed) return false;   // unknown: show it; a muxed stream's size includes its audio
    const double pixels = static_cast<double>(std::max(1, s.width)) * std::max(1, s.height);
    const double threshold = std::max(45.0, 110.0 * pixels / (1280.0 * 720.0));
    return kbps < threshold;
}

} // namespace st::youtube
