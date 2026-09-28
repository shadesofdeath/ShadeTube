#include "Common.hpp"

#include "../Utils/StringUtils.hpp"

#include <regex>

namespace YoutubeExplode::detail {

std::vector<ThumbnailData> parseThumbnails(const Json* node) {
    std::vector<ThumbnailData> result;
    if (!node)
        return result;
    const Json* array = node->is_array() ? node : dig(node, {"thumbnails"});
    if (!array || !array->is_array())
        return result;
    for (const auto& item : *array)
        result.push_back({jsonString(dig(item, {"url"})), jsonInt(dig(item, {"width"})), jsonInt(dig(item, {"height"}))});
    return result;
}

std::vector<Common::Thumbnail> toThumbnails(const std::vector<ThumbnailData>& data) {
    std::vector<Common::Thumbnail> result;
    for (const auto& t : data) {
        if (!t.url || t.url->empty() || !t.width || !t.height)
            continue;
        auto url = *t.url;
        if (startsWith(url, "//"))
            url = "https:" + url;
        result.emplace_back(url, Common::Resolution(*t.width, *t.height));
    }
    return result;
}

std::optional<TimeSpan> parseClockText(const std::optional<std::string>& text) {
    if (!text)
        return std::nullopt;
    auto seconds = tryParseClockDuration(*text);
    if (!seconds)
        return std::nullopt;
    return std::chrono::duration_cast<TimeSpan>(std::chrono::seconds(*seconds));
}

std::optional<std::string> findMetaContent(const std::string& html, const std::string& attribute,
                                           const std::string& value) {
    static const std::regex attributePattern(R"re(([\w:-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'))re");
    std::size_t from = 0;
    while (true) {
        auto pos = html.find("<meta", from);
        if (pos == std::string::npos)
            return std::nullopt;
        auto end = html.find('>', pos);
        if (end == std::string::npos)
            return std::nullopt;
        from = end;
        const auto tag = html.substr(pos, end - pos);
        if (tag.find(value) == std::string::npos)
            continue;
        std::optional<std::string> matchedValue, content;
        for (auto it = std::sregex_iterator(tag.begin(), tag.end(), attributePattern); it != std::sregex_iterator(); ++it) {
            const auto name = toLower((*it)[1].str());
            const auto v = (*it)[2].matched ? (*it)[2].str() : (*it)[3].str();
            if (name == attribute)
                matchedValue = v;
            else if (name == "content")
                content = htmlDecode(v);
        }
        if (matchedValue && *matchedValue == value && content)
            return content;
    }
}

} // namespace YoutubeExplode::detail
