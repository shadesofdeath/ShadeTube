#pragma once

#include "../Utils/Json.hpp"

#include <YoutubeExplode/Common/Thumbnail.hpp>
#include <YoutubeExplode/Common/Time.hpp>

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::detail {

struct ThumbnailData {
    std::optional<std::string> url;
    std::optional<int> width;
    std::optional<int> height;
};

/// Reads a {"thumbnails": [...]} style array (or a bare array) of {url,width,height}.
std::vector<ThumbnailData> parseThumbnails(const Json* array);

/// Converts thumbnail data into public objects; entries missing url/size are skipped.
/// Protocol-relative URLs ("//i.ytimg.com/...") are made absolute.
std::vector<Common::Thumbnail> toThumbnails(const std::vector<ThumbnailData>& data);

/// Parses "m:ss" / "h:mm:ss" text into a duration.
std::optional<TimeSpan> parseClockText(const std::optional<std::string>& text);

/// Returns the `content` attribute of the first <meta> tag whose `attribute` equals `value`.
std::optional<std::string> findMetaContent(const std::string& html, const std::string& attribute,
                                           const std::string& value);

} // namespace YoutubeExplode::detail
