#pragma once

#include <YoutubeExplode/Videos/Streams/StreamInfo.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace YoutubeExplode::Videos::Streams {

/// Describes media streams available for a YouTube video.
class StreamManifest {
public:
    StreamManifest() = default;
    explicit StreamManifest(std::vector<std::shared_ptr<const IStreamInfo>> streams) : streams_(std::move(streams)) {}

    /// All available streams.
    const std::vector<std::shared_ptr<const IStreamInfo>>& streams() const noexcept { return streams_; }

    /// Streams that contain audio (audio-only and muxed).
    std::vector<std::shared_ptr<const IAudioStreamInfo>> getAudioStreams() const { return ofType<IAudioStreamInfo>(); }
    /// Streams that contain video (video-only and muxed).
    std::vector<std::shared_ptr<const IVideoStreamInfo>> getVideoStreams() const { return ofType<IVideoStreamInfo>(); }
    /// Streams that contain both audio and video.
    std::vector<std::shared_ptr<const MuxedStreamInfo>> getMuxedStreams() const { return ofType<MuxedStreamInfo>(); }
    /// Audio-only streams.
    std::vector<std::shared_ptr<const AudioOnlyStreamInfo>> getAudioOnlyStreams() const { return ofType<AudioOnlyStreamInfo>(); }
    /// Video-only streams.
    std::vector<std::shared_ptr<const VideoOnlyStreamInfo>> getVideoOnlyStreams() const { return ofType<VideoOnlyStreamInfo>(); }

    /// Highest-bitrate audio-only stream, optionally restricted to a container ("webm" = opus, "mp4" = m4a/aac).
    /// Prefers the default audio track on multi-language videos.
    std::shared_ptr<const AudioOnlyStreamInfo> tryGetBestAudioOnlyStream(const std::string& container = {}) const;

    bool empty() const noexcept { return streams_.empty(); }

private:
    template <typename T>
    std::vector<std::shared_ptr<const T>> ofType() const {
        std::vector<std::shared_ptr<const T>> result;
        for (const auto& s : streams_)
            if (auto cast = std::dynamic_pointer_cast<const T>(s))
                result.push_back(std::move(cast));
        return result;
    }

    std::vector<std::shared_ptr<const IStreamInfo>> streams_;
};

/// Gets the stream with the highest bitrate, or nullptr if the collection is empty.
template <typename T>
std::shared_ptr<const T> tryGetWithHighestBitrate(const std::vector<std::shared_ptr<const T>>& streams) {
    auto it = std::max_element(streams.begin(), streams.end(),
                               [](const auto& a, const auto& b) { return a->bitrate() < b->bitrate(); });
    return it != streams.end() ? *it : nullptr;
}

/// Gets the stream with the highest bitrate. Throws std::invalid_argument if the collection is empty.
template <typename T>
std::shared_ptr<const T> getWithHighestBitrate(const std::vector<std::shared_ptr<const T>>& streams) {
    auto result = tryGetWithHighestBitrate(streams);
    if (!result)
        throw std::invalid_argument("Input stream collection is empty.");
    return result;
}

/// Gets the video stream with the highest video quality, or nullptr if the collection is empty.
template <typename T>
std::shared_ptr<const T> tryGetWithHighestVideoQuality(const std::vector<std::shared_ptr<const T>>& streams) {
    auto it = std::max_element(streams.begin(), streams.end(),
                               [](const auto& a, const auto& b) { return a->videoQuality() < b->videoQuality(); });
    return it != streams.end() ? *it : nullptr;
}

/// Gets the video stream with the highest video quality. Throws std::invalid_argument if empty.
template <typename T>
std::shared_ptr<const T> getWithHighestVideoQuality(const std::vector<std::shared_ptr<const T>>& streams) {
    auto result = tryGetWithHighestVideoQuality(streams);
    if (!result)
        throw std::invalid_argument("Input stream collection is empty.");
    return result;
}

} // namespace YoutubeExplode::Videos::Streams
