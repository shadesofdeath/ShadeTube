#pragma once

#include <string>

namespace YoutubeExplode::Common {

/// Reference to a channel that owns a specific YouTube video or playlist.
class Author {
public:
    Author() = default;
    Author(std::string channelId, std::string channelTitle)
        : channelId_(std::move(channelId)), channelTitle_(std::move(channelTitle)) {}

    const std::string& channelId() const noexcept { return channelId_; }
    std::string channelUrl() const { return "https://www.youtube.com/channel/" + channelId_; }
    const std::string& channelTitle() const noexcept { return channelTitle_; }
    const std::string& title() const noexcept { return channelTitle_; }

    std::string toString() const { return channelTitle_; }

private:
    std::string channelId_;
    std::string channelTitle_;
};

} // namespace YoutubeExplode::Common
