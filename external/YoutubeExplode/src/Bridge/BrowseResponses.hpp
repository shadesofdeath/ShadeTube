#pragma once

#include "Common.hpp"

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::detail {

// ---- Search ---------------------------------------------------------------------------------------

struct SearchVideoData {
    std::optional<std::string> id, title, author, channelId;
    std::optional<TimeSpan> duration;
    std::vector<ThumbnailData> thumbnails;
};

struct SearchPlaylistData {
    std::optional<std::string> id, title, author, channelId;
    std::vector<ThumbnailData> thumbnails;
};

struct SearchChannelData {
    std::optional<std::string> id, title;
    std::vector<ThumbnailData> thumbnails;
};

struct SearchResponse {
    std::vector<SearchVideoData> videos;
    std::vector<SearchPlaylistData> playlists;
    std::vector<SearchChannelData> channels;
    std::optional<std::string> continuationToken;

    static SearchResponse parse(const std::string& raw);
};

// ---- Playlists ------------------------------------------------------------------------------------

struct PlaylistVideoData {
    std::optional<int> index;
    std::optional<std::string> id, title, author, channelId;
    std::optional<TimeSpan> duration;
    std::vector<ThumbnailData> thumbnails;
};

/// Common view over /browse (rich metadata, user playlists only) and /next (all playlists).
struct PlaylistData {
    bool isAvailable = false;
    std::optional<std::string> title, author, channelId, description;
    std::optional<int> count;
    std::vector<ThumbnailData> thumbnails;
};

PlaylistData parsePlaylistBrowseResponse(const std::string& raw);

struct PlaylistNextResponse {
    PlaylistData playlist;
    std::vector<PlaylistVideoData> videos;
    std::optional<std::string> visitorData;

    static PlaylistNextResponse parse(const std::string& raw);
};

} // namespace YoutubeExplode::detail
