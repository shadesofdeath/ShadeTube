#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Videos/ClosedCaptions/ClosedCaptionClient.hpp>
#include <YoutubeExplode/Videos/Streams/StreamClient.hpp>
#include <YoutubeExplode/Videos/Video.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <future>

namespace YoutubeExplode::Videos {

/// Operations related to YouTube videos.
class VideoClient {
public:
    explicit VideoClient(detail::ClientContextPtr context)
        : context_(context), streams_(context), closedCaptions_(context) {}

    /// Operations related to media streams.
    const Streams::StreamClient& streams() const noexcept { return streams_; }
    /// Operations related to closed captions.
    const ClosedCaptions::ClosedCaptionClient& closedCaptions() const noexcept { return closedCaptions_; }

    /// Gets the metadata associated with the specified video.
    Video get(const VideoId& videoId, const CancellationToken& cancellationToken = {}) const;

    std::future<Video> getAsync(const VideoId& videoId, CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, videoId, cancellationToken] {
            return self.get(videoId, cancellationToken);
        });
    }

private:
    detail::ClientContextPtr context_;
    Streams::StreamClient streams_;
    ClosedCaptions::ClosedCaptionClient closedCaptions_;
};

} // namespace YoutubeExplode::Videos
