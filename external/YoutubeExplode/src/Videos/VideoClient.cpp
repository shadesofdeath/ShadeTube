#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Videos/VideoClient.hpp>

#include "../ClientContext.hpp"
#include "VideoController.hpp"

namespace YoutubeExplode::Videos {

using namespace YoutubeExplode::detail;

Video VideoClient::get(const VideoId& videoId, const CancellationToken& cancellationToken) const {
    VideoController controller(*context_);
    auto watchPage = controller.getVideoWatchPage(videoId.value(), cancellationToken);

    std::optional<PlayerResponse> playerResponse = watchPage.playerResponse();
    // The watch page may lack details (consent walls, bot checks...): fall back to the API.
    if (!playerResponse || !playerResponse->title() || !playerResponse->channelId()) {
        try {
            playerResponse = controller.getPlayerResponse(videoId.value(), cancellationToken);
        } catch (const Exceptions::VideoUnplayableException&) {
            if (!playerResponse)
                throw;
        }
    }

    const auto title = playerResponse->title().value_or("");  // Videos without title are legal
    const auto author = playerResponse->author();
    if (!author)
        throw Exceptions::YoutubeExplodeException("Failed to extract the video author.");
    const auto channelId = playerResponse->channelId();
    if (!channelId)
        throw Exceptions::YoutubeExplodeException("Failed to extract the video channel ID.");
    auto uploadDate = playerResponse->uploadDate();
    if (!uploadDate)
        uploadDate = watchPage.uploadDate();
    if (!uploadDate)
        throw Exceptions::YoutubeExplodeException("Failed to extract the video upload date.");

    auto thumbnails = toThumbnails(playerResponse->thumbnails());
    auto defaults = Common::Thumbnail::getDefaultSet(videoId.value());
    thumbnails.insert(thumbnails.end(), defaults.begin(), defaults.end());

    return Video(videoId, title, Common::Author(*channelId, *author), *uploadDate,
                 playerResponse->description().value_or(""), playerResponse->duration(), std::move(thumbnails),
                 playerResponse->keywords(),
                 // Engagement statistics may be hidden
                 Engagement(playerResponse->viewCount().value_or(0), watchPage.likeCount().value_or(0),
                            watchPage.dislikeCount().value_or(0)),
                 playerResponse->isLive());
}

} // namespace YoutubeExplode::Videos
