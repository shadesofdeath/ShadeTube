#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Videos/Streams/MediaStream.hpp>
#include <YoutubeExplode/Videos/Streams/StreamManifest.hpp>
#include <YoutubeExplode/Videos/VideoId.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <future>
#include <ostream>
#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Operations related to media streams of YouTube videos.
class StreamClient {
public:
    explicit StreamClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    /// Gets the manifest that lists available streams for the specified video.
    /// All returned URLs are fully resolved (signature deciphered, n-parameter transformed
    /// when possible) and directly playable.
    StreamManifest getManifest(const VideoId& videoId, const CancellationToken& cancellationToken = {}) const;

    std::future<StreamManifest> getManifestAsync(const VideoId& videoId, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, videoId, cancellationToken] {
            return self.getManifest(videoId, cancellationToken);
        });
    }

    /// Gets the HTTP Live Stream (HLS) manifest URL for the specified video (live streams only).
    std::string getHttpLiveStreamUrl(const VideoId& videoId, const CancellationToken& cancellationToken = {}) const;

    /// Opens a readable, seekable stream over the specified media stream.
    MediaStream get(const IStreamInfo& streamInfo, const CancellationToken& cancellationToken = {}) const;

    /// Copies the specified media stream into an output stream.
    void copyTo(const IStreamInfo& streamInfo, std::ostream& destination, const Progress& progress = {},
                const CancellationToken& cancellationToken = {}) const;

    /// Downloads the specified media stream to a file.
    void download(const IStreamInfo& streamInfo, const std::string& filePath, const Progress& progress = {},
                  const CancellationToken& cancellationToken = {}) const;

    std::future<void> downloadAsync(std::shared_ptr<const IStreamInfo> streamInfo, std::string filePath,
                                    Progress progress = {}, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, streamInfo, filePath, progress, cancellationToken] {
            self.download(*streamInfo, filePath, progress, cancellationToken);
        });
    }

private:
    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Videos::Streams
