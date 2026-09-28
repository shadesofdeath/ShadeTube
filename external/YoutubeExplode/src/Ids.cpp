#include <YoutubeExplode/Channels/ChannelIds.hpp>
#include <YoutubeExplode/Playlists/PlaylistId.hpp>
#include <YoutubeExplode/Videos/VideoId.hpp>

#include "Utils/StringUtils.hpp"
#include "Utils/Url.hpp"

#include <algorithm>
#include <regex>

namespace YoutubeExplode {

namespace {

template <typename Predicate>
std::optional<std::string> extract(const std::string& input, const char* pattern, Predicate isValid) {
    const std::regex re(pattern, std::regex::icase);
    std::smatch m;
    if (!std::regex_search(input, m, re))
        return std::nullopt;
    auto value = detail::Url::decode(m[1].str());
    if (detail::isBlank(value) || !isValid(value))
        return std::nullopt;
    return value;
}

bool allOf(const std::string& s, bool (*pred)(char)) { return std::all_of(s.begin(), s.end(), pred); }
bool isIdChar(char c) { return detail::isAsciiAlnum(c) || c == '_' || c == '-'; }
bool isHandleChar(char c) { return detail::isAsciiAlnum(c) || c == '_' || c == '-' || c == '.'; }
bool isSlugChar(char c) { return detail::isAsciiAlnum(c); }

} // namespace

// ---- Videos ---------------------------------------------------------------------------------------

namespace Videos {

static bool isValidVideoId(const std::string& id) { return id.size() == 11 && allOf(id, isIdChar); }

std::optional<std::string> VideoIdTraits::tryNormalize(const std::string& rawInput) {
    const auto input = detail::trim(rawInput);
    if (input.empty())
        return std::nullopt;
    if (isValidVideoId(input))
        return input;

    for (const char* pattern : {
             R"(youtube\..+?/watch.*?v=(.*?)(?:&|/|#|$))",       // https://www.youtube.com/watch?v=yIVRs6YSbOM
             R"(youtu\.be/watch.*?v=(.*?)(?:\?|&|/|#|$))",       // https://youtu.be/watch?v=Fcds0_MrgNU
             R"(youtu\.be/(.*?)(?:\?|&|/|#|$))",                 // https://youtu.be/yIVRs6YSbOM
             R"(youtube\..+?/embed/(.*?)(?:\?|&|/|#|$))",        // https://www.youtube.com/embed/yIVRs6YSbOM
             R"(youtube\..+?/shorts/(.*?)(?:\?|&|/|#|$))",       // https://www.youtube.com/shorts/sKL1vjP0tIo
             R"(youtube\..+?/live/(.*?)(?:\?|&|/|#|$))",         // https://www.youtube.com/live/jfKfPfyJRdk
             R"(youtube-nocookie\..+?/embed/(.*?)(?:\?|&|/|#|$))",
         }) {
        if (auto id = extract(input, pattern, isValidVideoId))
            return id;
    }
    return std::nullopt;
}

} // namespace Videos

// ---- Playlists ------------------------------------------------------------------------------------

namespace Playlists {

static bool isValidPlaylistId(const std::string& id) { return id.size() >= 2 && allOf(id, isIdChar); }

std::optional<std::string> PlaylistIdTraits::tryNormalize(const std::string& rawInput) {
    const auto input = detail::trim(rawInput);
    if (input.empty())
        return std::nullopt;
    if (isValidPlaylistId(input))
        return input;
    for (const char* pattern : {
             R"(youtube\..+?/playlist.*?list=(.*?)(?:&|/|#|$))",
             R"(youtube\..+?/watch.*?list=(.*?)(?:&|/|#|$))",
             R"(youtu\.be/.*?/.*?list=(.*?)(?:&|/|#|$))",
             R"(youtu\.be/.*?list=(.*?)(?:&|/|#|$))",
             R"(youtube\..+?/embed/.*?/.*?list=(.*?)(?:&|/|#|$))",
         }) {
        if (auto id = extract(input, pattern, isValidPlaylistId))
            return id;
    }
    return std::nullopt;
}

} // namespace Playlists

// ---- Channels -------------------------------------------------------------------------------------

namespace Channels {

static bool isValidChannelId(const std::string& id) {
    return id.size() == 24 && detail::startsWith(id, "UC") && allOf(id, isIdChar);
}
static bool isValidHandle(const std::string& h) { return !h.empty() && allOf(h, isHandleChar); }
static bool isValidSlug(const std::string& s) { return !s.empty() && allOf(s, isSlugChar); }
static bool isValidUserName(const std::string& s) { return !s.empty() && s.size() <= 20 && allOf(s, isSlugChar); }

std::optional<std::string> ChannelIdTraits::tryNormalize(const std::string& rawInput) {
    const auto input = detail::trim(rawInput);
    if (isValidChannelId(input))
        return input;
    return extract(input, R"(youtube\..+?/channel/(.*?)(?:\?|&|/|#|$))", isValidChannelId);
}

std::optional<std::string> ChannelHandleTraits::tryNormalize(const std::string& rawInput) {
    auto input = detail::trim(rawInput);
    if (detail::startsWith(input, "@") && isValidHandle(input.substr(1)))
        return input.substr(1);
    if (isValidHandle(input))
        return input;
    return extract(input, R"(youtube\..+?/@(.*?)(?:\?|&|/|#|$))", isValidHandle);
}

std::optional<std::string> ChannelSlugTraits::tryNormalize(const std::string& rawInput) {
    const auto input = detail::trim(rawInput);
    if (isValidSlug(input))
        return input;
    return extract(input, R"(youtube\..+?/c/(.*?)(?:\?|&|/|#|$))", isValidSlug);
}

std::optional<std::string> UserNameTraits::tryNormalize(const std::string& rawInput) {
    const auto input = detail::trim(rawInput);
    if (isValidUserName(input))
        return input;
    return extract(input, R"(youtube\..+?/user/(.*?)(?:\?|&|/|#|$))", isValidUserName);
}

} // namespace Channels

} // namespace YoutubeExplode
