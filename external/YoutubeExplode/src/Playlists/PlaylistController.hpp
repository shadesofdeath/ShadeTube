#pragma once

#include "../Bridge/BrowseResponses.hpp"
#include "../ClientContext.hpp"

#include <optional>
#include <string>

namespace YoutubeExplode::detail {

class PlaylistController {
public:
    explicit PlaylistController(ClientContext& context) : context_(context) {}

    /// Works only with user-made playlists, but provides rich metadata.
    PlaylistData getPlaylistBrowseResponse(const std::string& playlistId, const CancellationToken& ct);

    /// Works with all playlists (including mixes), but contains limited metadata.
    PlaylistNextResponse getPlaylistNextResponse(const std::string& playlistId, const std::optional<std::string>& videoId,
                                                 int index, const std::optional<std::string>& visitorData,
                                                 const CancellationToken& ct);

    PlaylistData getPlaylistResponse(const std::string& playlistId, const CancellationToken& ct);

private:
    ClientContext& context_;
};

} // namespace YoutubeExplode::detail
