#pragma once

#include "PlayerResponse.hpp"

#include <YoutubeExplode/Common/Time.hpp>

#include <optional>
#include <string>

namespace YoutubeExplode::detail {

/// youtube.com/watch?v=... HTML page.
class VideoWatchPage {
public:
    static std::optional<VideoWatchPage> tryParse(std::string html);

    bool isAvailable() const;
    std::optional<DateTimeOffset> uploadDate() const;
    std::optional<long long> likeCount() const;
    std::optional<long long> dislikeCount() const;
    std::optional<PlayerResponse> playerResponse() const;

private:
    explicit VideoWatchPage(std::string html) : html_(std::move(html)) {}
    std::string html_;
};

/// youtube.com/channel/..., /@handle, /c/..., /user/... HTML page.
class ChannelPage {
public:
    static std::optional<ChannelPage> tryParse(std::string html);

    std::optional<std::string> url() const;
    std::optional<std::string> id() const;
    std::optional<std::string> title() const;
    std::optional<std::string> logoUrl() const;

private:
    explicit ChannelPage(std::string html) : html_(std::move(html)) {}
    std::string html_;
};

} // namespace YoutubeExplode::detail
