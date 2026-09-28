#pragma once

#include <YoutubeExplode/Channels/ChannelIds.hpp>
#include <YoutubeExplode/Common/Thumbnail.hpp>

#include <string>
#include <vector>

namespace YoutubeExplode::Channels {

/// Properties shared by channel metadata resolved from different sources.
class IChannel {
public:
    virtual ~IChannel() = default;
    virtual const ChannelId& id() const = 0;
    virtual std::string url() const = 0;
    virtual const std::string& title() const = 0;
    virtual const std::vector<Common::Thumbnail>& thumbnails() const = 0;
};

/// Metadata associated with a YouTube channel.
class Channel : public IChannel {
public:
    Channel(ChannelId id, std::string title, std::vector<Common::Thumbnail> thumbnails)
        : id_(std::move(id)), title_(std::move(title)), thumbnails_(std::move(thumbnails)) {}

    const ChannelId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/channel/" + id_.value(); }
    const std::string& title() const override { return title_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }

    std::string toString() const { return "Channel (" + title_ + ")"; }

private:
    ChannelId id_;
    std::string title_;
    std::vector<Common::Thumbnail> thumbnails_;
};

} // namespace YoutubeExplode::Channels
