#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Videos/ClosedCaptions/ClosedCaptions.hpp>
#include <YoutubeExplode/Videos/VideoId.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <future>
#include <ostream>
#include <string>

namespace YoutubeExplode::Videos::ClosedCaptions {

/// Operations related to closed captions of YouTube videos.
class ClosedCaptionClient {
public:
    explicit ClosedCaptionClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    /// Gets the manifest that lists available closed caption tracks for the specified video.
    ClosedCaptionManifest getManifest(const VideoId& videoId, const CancellationToken& cancellationToken = {}) const;

    std::future<ClosedCaptionManifest> getManifestAsync(const VideoId& videoId, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, videoId, cancellationToken] {
            return self.getManifest(videoId, cancellationToken);
        });
    }

    /// Gets the closed caption track identified by the specified metadata.
    ClosedCaptionTrack get(const ClosedCaptionTrackInfo& trackInfo, const CancellationToken& cancellationToken = {}) const;

    /// Writes the track in SubRip (.srt) format.
    void writeTo(const ClosedCaptionTrackInfo& trackInfo, std::ostream& writer, const Progress& progress = {},
                 const CancellationToken& cancellationToken = {}) const;

    /// Downloads the track in SubRip (.srt) format to a file.
    void download(const ClosedCaptionTrackInfo& trackInfo, const std::string& filePath, const Progress& progress = {},
                  const CancellationToken& cancellationToken = {}) const;

private:
    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Videos::ClosedCaptions
