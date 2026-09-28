#pragma once

#include <YoutubeExplode/detail/StringId.hpp>

namespace YoutubeExplode::Channels {

struct ChannelIdTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "channel ID or URL";
};
struct ChannelHandleTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "channel handle or custom URL";
};
struct ChannelSlugTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "channel slug or legacy custom URL";
};
struct UserNameTraits {
    static std::optional<std::string> tryNormalize(const std::string& input);
    static constexpr const char* description = "user name or profile URL";
};

/// UC3xnGqlcL3y-GXz5N3wiTJQ or https://www.youtube.com/channel/UC3xnGqlcL3y-GXz5N3wiTJQ
using ChannelId = detail::StringId<ChannelIdTraits>;
/// Tyrrrz (or @Tyrrrz) or https://www.youtube.com/@Tyrrrz
using ChannelHandle = detail::StringId<ChannelHandleTraits>;
/// Tyrrrz or https://www.youtube.com/c/Tyrrrz
using ChannelSlug = detail::StringId<ChannelSlugTraits>;
/// TheTyrrr or https://www.youtube.com/user/TheTyrrr
using UserName = detail::StringId<UserNameTraits>;

} // namespace YoutubeExplode::Channels
