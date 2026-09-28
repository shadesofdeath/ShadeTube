#pragma once

#include <YoutubeExplode/Common/Resolution.hpp>

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::Common {

/// Thumbnail image.
class Thumbnail {
public:
    Thumbnail() = default;
    Thumbnail(std::string url, Resolution resolution) : url_(std::move(url)), resolution_(resolution) {}

    const std::string& url() const noexcept { return url_; }
    const Resolution& resolution() const noexcept { return resolution_; }

    std::string toString() const { return "Thumbnail (" + resolution_.toString() + ")"; }

    /// Default set of thumbnails that every video has (img.youtube.com).
    static std::vector<Thumbnail> getDefaultSet(const std::string& videoId);

private:
    std::string url_;
    Resolution resolution_;
};

/// Gets the thumbnail with the highest resolution, or nothing if the collection is empty.
std::optional<Thumbnail> tryGetWithHighestResolution(const std::vector<Thumbnail>& thumbnails);

/// Gets the thumbnail with the highest resolution. Throws std::invalid_argument if empty.
Thumbnail getWithHighestResolution(const std::vector<Thumbnail>& thumbnails);

} // namespace YoutubeExplode::Common
