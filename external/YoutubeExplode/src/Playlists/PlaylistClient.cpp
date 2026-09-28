#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Playlists/PlaylistClient.hpp>

#include "../Utils/Json.hpp"
#include "PlaylistController.hpp"

#include <unordered_set>

namespace YoutubeExplode {

namespace detail {

namespace {
Json webClient(const YoutubeClientOptions& options, const std::optional<std::string>& visitorData = std::nullopt) {
    Json client = {{"clientName", "WEB"},
                   {"clientVersion", "2.20210408.08.00"},
                   {"hl", options.language},
                   {"gl", options.region},
                   {"utcOffsetMinutes", 0}};
    if (visitorData)
        client["visitorData"] = *visitorData;
    return client;
}
} // namespace

PlaylistData PlaylistController::getPlaylistBrowseResponse(const std::string& playlistId, const CancellationToken& ct) {
    Json body = {{"browseId", "VL" + playlistId}, {"context", {{"client", webClient(context_.options())}}}};
    auto data = parsePlaylistBrowseResponse(context_.postJson("https://www.youtube.com/youtubei/v1/browse", body.dump(), ct));
    if (!data.isAvailable)
        throw Exceptions::PlaylistUnavailableException("Playlist '" + playlistId + "' is not available.");
    return data;
}

PlaylistNextResponse PlaylistController::getPlaylistNextResponse(const std::string& playlistId,
                                                                 const std::optional<std::string>& videoId, int index,
                                                                 const std::optional<std::string>& visitorData,
                                                                 const CancellationToken& ct) {
    constexpr int retriesCount = 5;
    for (int retriesRemaining = retriesCount;; --retriesRemaining) {
        Json body = {{"playlistId", playlistId},
                     {"playlistIndex", index},
                     {"context", {{"client", webClient(context_.options(), visitorData)}}}};
        if (videoId)
            body["videoId"] = *videoId;
        auto response =
            PlaylistNextResponse::parse(context_.postJson("https://www.youtube.com/youtubei/v1/next", body.dump(), ct));

        if (!response.playlist.isAvailable) {
            // Some system playlists only materialize after someone opens them once.
            if (index <= 0 && !visitorData && retriesRemaining >= retriesCount) {
                Http::HttpRequest open;
                open.url = "https://www.youtube.com/playlist?list=" + playlistId;
                (void)context_.send(open, ct);
                continue;
            }
            // Not the first page and previous pages worked: most likely a transient error.
            if (index > 0 && visitorData && retriesRemaining > 0)
                continue;
            // The target video may be unavailable while the playlist itself is fine.
            if (retriesRemaining <= 0 && !response.videos.empty())
                return response;
            throw Exceptions::PlaylistUnavailableException("Playlist '" + playlistId + "' is not available.");
        }
        return response;
    }
}

PlaylistData PlaylistController::getPlaylistResponse(const std::string& playlistId, const CancellationToken& ct) {
    try {
        return getPlaylistBrowseResponse(playlistId, ct);
    } catch (const Exceptions::PlaylistUnavailableException&) {
        return getPlaylistNextResponse(playlistId, std::nullopt, 0, std::nullopt, ct).playlist;
    }
}

} // namespace detail

namespace Playlists {

using namespace YoutubeExplode::detail;

Playlist PlaylistClient::get(const PlaylistId& playlistId, const CancellationToken& cancellationToken) const {
    PlaylistController controller(*context_);
    auto data = controller.getPlaylistResponse(playlistId.value(), cancellationToken);
    if (!data.title)
        throw Exceptions::YoutubeExplodeException("Failed to extract the playlist title.");

    // System playlists have no author
    std::optional<Common::Author> author;
    if (data.channelId && data.author)
        author = Common::Author(*data.channelId, *data.author);

    return Playlist(playlistId, *data.title, author, data.description.value_or(""), data.count,
                    toThumbnails(data.thumbnails));
}

void PlaylistClient::getVideoBatches(const PlaylistId& playlistId, const Common::BatchHandler<PlaylistVideo>& handler,
                                     const CancellationToken& cancellationToken) const {
    PlaylistController controller(*context_);
    std::unordered_set<std::string> encounteredIds;
    std::optional<std::string> lastVideoId;
    int lastVideoIndex = 0;
    std::optional<std::string> visitorData;

    while (true) {
        cancellationToken.throwIfCancellationRequested();
        std::vector<PlaylistVideo> videos;
        try {
            auto response = controller.getPlaylistNextResponse(playlistId.value(), lastVideoId, lastVideoIndex,
                                                               visitorData, cancellationToken);
            for (const auto& v : response.videos) {
                if (!v.id)
                    throw Exceptions::YoutubeExplodeException("Failed to extract the video ID.");
                if (!v.index)
                    throw Exceptions::YoutubeExplodeException("Failed to extract the video index.");
                lastVideoId = v.id;
                lastVideoIndex = *v.index;

                // Don't yield the same video twice
                if (!encounteredIds.insert(*v.id).second)
                    continue;

                auto videoId = Videos::VideoId::tryParse(*v.id);
                if (!videoId || !v.author || !v.channelId)
                    continue;  // Deleted/private entries have no author

                auto thumbnails = toThumbnails(v.thumbnails);
                auto defaults = Common::Thumbnail::getDefaultSet(*v.id);
                thumbnails.insert(thumbnails.end(), defaults.begin(), defaults.end());

                videos.emplace_back(playlistId, *videoId, v.title.value_or(""), Common::Author(*v.channelId, *v.author),
                                    v.duration, std::move(thumbnails));
            }

            // Stop if there are no new videos
            if (videos.empty())
                break;
            if (!visitorData)
                visitorData = response.visitorData;
        } catch (const Exceptions::PlaylistUnavailableException&) {
            // Playlist became unavailable after some videos were extracted: treat as the end.
            if (lastVideoIndex > 0)
                break;
            throw;
        }

        if (!handler(Common::Batch<PlaylistVideo>(std::move(videos))))
            break;
    }
}

std::vector<PlaylistVideo> PlaylistClient::getVideos(const PlaylistId& playlistId, std::size_t maxCount,
                                                     const CancellationToken& cancellationToken) const {
    std::vector<PlaylistVideo> result;
    if (maxCount == 0)
        return result;
    getVideoBatches(
        playlistId,
        [&](const Common::Batch<PlaylistVideo>& batch) {
            for (const auto& v : batch.items()) {
                result.push_back(v);
                if (result.size() >= maxCount)
                    return false;
            }
            return true;
        },
        cancellationToken);
    return result;
}

} // namespace Playlists

} // namespace YoutubeExplode
