#pragma once

#include <YoutubeExplode/Common/Resolution.hpp>

#include <optional>
#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Video quality, e.g. "1080p60".
class VideoQuality {
public:
    VideoQuality() = default;
    VideoQuality(std::string label, int maxHeight, int framerate)
        : label_(std::move(label)), maxHeight_(maxHeight), framerate_(framerate) {}
    VideoQuality(int maxHeight, int framerate);

    const std::string& label() const noexcept { return label_; }
    int maxHeight() const noexcept { return maxHeight_; }
    int framerate() const noexcept { return framerate_; }
    bool isHighDefinition() const noexcept { return maxHeight_ >= 1080; }

    const std::string& toString() const noexcept { return label_; }

    /// Parses labels such as "1080p", "1080p60", "1080s60", "2160p60 HDR".
    static std::optional<VideoQuality> tryFromLabel(const std::string& label, int framerateFallback);
    /// Resolves quality from a well-known itag. Returns nothing for unknown itags.
    static std::optional<VideoQuality> tryFromItag(int itag, int framerate);
    /// Typical resolution for this quality (used when the stream does not report one).
    Common::Resolution getDefaultVideoResolution() const;

    int compare(const VideoQuality& other) const;
    friend bool operator==(const VideoQuality& a, const VideoQuality& b) { return a.compare(b) == 0; }
    friend bool operator!=(const VideoQuality& a, const VideoQuality& b) { return a.compare(b) != 0; }
    friend bool operator<(const VideoQuality& a, const VideoQuality& b) { return a.compare(b) < 0; }
    friend bool operator>(const VideoQuality& a, const VideoQuality& b) { return a.compare(b) > 0; }

private:
    std::string label_;
    int maxHeight_ = 0;
    int framerate_ = 0;
};

} // namespace YoutubeExplode::Videos::Streams
