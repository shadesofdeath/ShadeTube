#pragma once

#include <YoutubeExplode/Common/Batch.hpp>
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Playlists/Playlist.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <cstddef>
#include <future>
#include <limits>
#include <vector>

namespace YoutubeExplode::Playlists {

/// Operations related to YouTube playlists.
class PlaylistClient {
public:
    explicit PlaylistClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    /// Gets the metadata associated with the specified playlist.
    Playlist get(const PlaylistId& playlistId, const CancellationToken& cancellationToken = {}) const;

    std::future<Playlist> getAsync(const PlaylistId& playlistId, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, playlistId, cancellationToken] {
            return self.get(playlistId, cancellationToken);
        });
    }

    /// Enumerates batches (pages) of videos included in the specified playlist.
    /// The handler returns false to stop the enumeration early.
    void getVideoBatches(const PlaylistId& playlistId, const Common::BatchHandler<PlaylistVideo>& handler,
                         const CancellationToken& cancellationToken = {}) const;

    /// Gets videos included in the specified playlist (up to `maxCount`).
    std::vector<PlaylistVideo> getVideos(const PlaylistId& playlistId,
                                         std::size_t maxCount = std::numeric_limits<std::size_t>::max(),
                                         const CancellationToken& cancellationToken = {}) const;

    std::future<std::vector<PlaylistVideo>> getVideosAsync(
        const PlaylistId& playlistId, std::size_t maxCount = std::numeric_limits<std::size_t>::max(),
        CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, playlistId, maxCount, cancellationToken] {
            return self.getVideos(playlistId, maxCount, cancellationToken);
        });
    }

private:
    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Playlists
