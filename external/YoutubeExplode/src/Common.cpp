#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Common/Language.hpp>
#include <YoutubeExplode/Common/Thumbnail.hpp>
#include <YoutubeExplode/Exceptions.hpp>

#include "Utils/StringUtils.hpp"

#include <algorithm>
#include <stdexcept>

namespace YoutubeExplode {

void CancellationToken::throwIfCancellationRequested() const {
    if (isCancellationRequested())
        throw Exceptions::OperationCanceledException();
}

namespace Common {

std::vector<Thumbnail> Thumbnail::getDefaultSet(const std::string& videoId) {
    return {
        Thumbnail("https://img.youtube.com/vi/" + videoId + "/default.jpg", Resolution(120, 90)),
        Thumbnail("https://img.youtube.com/vi/" + videoId + "/mqdefault.jpg", Resolution(320, 180)),
        Thumbnail("https://img.youtube.com/vi/" + videoId + "/hqdefault.jpg", Resolution(480, 360)),
    };
}

std::optional<Thumbnail> tryGetWithHighestResolution(const std::vector<Thumbnail>& thumbnails) {
    auto it = std::max_element(thumbnails.begin(), thumbnails.end(), [](const Thumbnail& a, const Thumbnail& b) {
        return a.resolution().area() < b.resolution().area();
    });
    if (it == thumbnails.end())
        return std::nullopt;
    return *it;
}

Thumbnail getWithHighestResolution(const std::vector<Thumbnail>& thumbnails) {
    auto result = tryGetWithHighestResolution(thumbnails);
    if (!result)
        throw std::invalid_argument("Input thumbnail collection is empty.");
    return *result;
}

bool operator==(const Language& a, const Language& b) { return detail::iequals(a.code_, b.code_); }

} // namespace Common

} // namespace YoutubeExplode
