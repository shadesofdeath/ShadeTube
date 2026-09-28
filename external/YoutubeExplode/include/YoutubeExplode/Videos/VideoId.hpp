#pragma once

#include <YoutubeExplode/detail/StringId.hpp>

namespace YoutubeExplode::Videos {

struct VideoIdTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "video ID or URL";
};

/// Represents a syntactically valid YouTube video ID.
///
/// Accepts a raw 11-character ID or any common URL form:
///   yIVRs6YSbOM
///   https://www.youtube.com/watch?v=yIVRs6YSbOM
///   https://youtu.be/yIVRs6YSbOM
///   https://www.youtube.com/embed/yIVRs6YSbOM
///   https://www.youtube.com/shorts/sKL1vjP0tIo
///   https://www.youtube.com/live/jfKfPfyJRdk
///   https://music.youtube.com/watch?v=yIVRs6YSbOM
using VideoId = detail::StringId<VideoIdTraits>;

} // namespace YoutubeExplode::Videos
