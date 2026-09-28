#pragma once

#include <YoutubeExplode/detail/StringId.hpp>

namespace YoutubeExplode::Playlists {

struct PlaylistIdTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "playlist ID or URL";
};

/// Represents a syntactically valid YouTube playlist ID.
///   PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H
///   https://www.youtube.com/playlist?list=PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H
///   https://www.youtube.com/watch?v=b8m9zhNAgKs&list=PL9tY0BWXOZFuFEG_GtOBZ8-8wbkH-NVAr
using PlaylistId = detail::StringId<PlaylistIdTraits>;

} // namespace YoutubeExplode::Playlists
