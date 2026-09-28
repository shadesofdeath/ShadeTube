#pragma once

#include <YoutubeExplode/Channels/Channel.hpp>
#include <YoutubeExplode/Common/Batch.hpp>
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Playlists/Playlist.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <cstddef>
#include <future>
#include <limits>
#include <vector>

namespace YoutubeExplode::Channels {

/// Operations related to YouTube channels.
class ChannelClient {
public:
    explicit ChannelClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    /// Gets the metadata associated with the specified channel.
    Channel get(const ChannelId& channelId, const CancellationToken& cancellationToken = {}) const;
    /// Gets the metadata associated with the channel of the specified user (legacy /user/ URLs).
    Channel getByUser(const UserName& userName, const CancellationToken& cancellationToken = {}) const;
    /// Gets the metadata associated with the channel identified by the specified slug (legacy /c/ URLs).
    Channel getBySlug(const ChannelSlug& channelSlug, const CancellationToken& cancellationToken = {}) const;
    /// Gets the metadata associated with the channel identified by the specified handle (@handle).
    Channel getByHandle(const ChannelHandle& channelHandle, const CancellationToken& cancellationToken = {}) const;

    std::future<Channel> getAsync(const ChannelId& channelId, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, channelId, cancellationToken] {
            return self.get(channelId, cancellationToken);
        });
    }

    /// Enumerates batches of videos uploaded by the specified channel.
    void getUploadBatches(const ChannelId& channelId, const Common::BatchHandler<Playlists::PlaylistVideo>& handler,
                          const CancellationToken& cancellationToken = {}) const;

    /// Gets videos uploaded by the specified channel (newest first, up to `maxCount`).
    std::vector<Playlists::PlaylistVideo> getUploads(const ChannelId& channelId,
                                                     std::size_t maxCount = std::numeric_limits<std::size_t>::max(),
                                                     const CancellationToken& cancellationToken = {}) const;

private:
    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Channels
