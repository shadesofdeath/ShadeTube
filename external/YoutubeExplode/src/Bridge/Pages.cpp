#include "Pages.hpp"

#include "../Utils/StringUtils.hpp"

namespace YoutubeExplode::detail {

namespace {

// Finds `<number> <suffix>` where the number is preceded by `prefixChar`-style boundaries,
// e.g. `"123,456 likes"`. Returns digits only.
std::optional<long long> findCountBefore(const std::string& html, const std::string& suffix, const std::string& before) {
    std::size_t from = 0;
    while (true) {
        auto pos = html.find(suffix, from);
        if (pos == std::string::npos)
            return std::nullopt;
        from = pos + 1;
        std::size_t start = pos;
        while (start > 0 && (isAsciiDigit(html[start - 1]) || html[start - 1] == ',' || html[start - 1] == '.'))
            --start;
        if (start == pos)
            continue;
        if (!before.empty() && (start < before.size() || html.compare(start - before.size(), before.size(), before) != 0))
            continue;
        if (auto v = tryParseInt64(stripNonDigits(html.substr(start, pos - start))))
            return v;
    }
}

} // namespace

std::optional<VideoWatchPage> VideoWatchPage::tryParse(std::string html) {
    if (html.find("id=\"player\"") == std::string::npos && html.find("id=\"player-api\"") == std::string::npos &&
        html.find("ytInitialPlayerResponse") == std::string::npos)
        return std::nullopt;
    return VideoWatchPage(std::move(html));
}

bool VideoWatchPage::isAvailable() const { return findMetaContent(html_, "property", "og:url").has_value(); }

std::optional<DateTimeOffset> VideoWatchPage::uploadDate() const {
    for (const char* key : {"uploadDate", "datePublished"})
        if (auto v = findMetaContent(html_, "itemprop", key))
            if (auto d = tryParseIso8601(*v))
                return d;
    return std::nullopt;
}

std::optional<long long> VideoWatchPage::likeCount() const {
    if (auto v = findCountBefore(html_, " likes\"", "\""))
        return v;
    return findCountBefore(html_, " other people\"", "along with ");
}

std::optional<long long> VideoWatchPage::dislikeCount() const { return findCountBefore(html_, " dislikes\"", "\""); }

std::optional<PlayerResponse> VideoWatchPage::playerResponse() const {
    std::size_t from = 0;
    while (true) {
        auto pos = html_.find("ytInitialPlayerResponse", from);
        if (pos == std::string::npos)
            return std::nullopt;
        from = pos + 1;
        auto k = pos + std::char_traits<char>::length("ytInitialPlayerResponse");
        while (k < html_.size() && html_[k] == ' ') ++k;
        if (k >= html_.size() || html_[k] != '=')
            continue;
        ++k;
        while (k < html_.size() && html_[k] == ' ') ++k;
        if (k >= html_.size() || html_[k] != '{')
            continue;
        auto json = tryParseJson(extractJson(std::string_view(html_).substr(k)));
        if (json && json->is_object())
            return PlayerResponse(std::move(*json));
    }
}

std::optional<ChannelPage> ChannelPage::tryParse(std::string html) {
    if (!findMetaContent(html, "property", "og:url"))
        return std::nullopt;
    return ChannelPage(std::move(html));
}

std::optional<std::string> ChannelPage::url() const { return findMetaContent(html_, "property", "og:url"); }

std::optional<std::string> ChannelPage::id() const {
    auto u = url();
    if (!u)
        return std::nullopt;
    auto lower = toLower(*u);
    auto pos = lower.find("channel/");
    if (pos == std::string::npos)
        return std::nullopt;
    return substringUntil(u->substr(pos + 8), "?");
}

std::optional<std::string> ChannelPage::title() const { return findMetaContent(html_, "property", "og:title"); }
std::optional<std::string> ChannelPage::logoUrl() const { return findMetaContent(html_, "property", "og:image"); }

} // namespace YoutubeExplode::detail
