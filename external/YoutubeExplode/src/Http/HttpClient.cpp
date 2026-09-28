#include <YoutubeExplode/Http/HttpClient.hpp>

#include "../Utils/StringUtils.hpp"

namespace YoutubeExplode::Http {

static std::optional<std::string> findHeader(const Headers& headers, const std::string& name) {
    for (const auto& [k, v] : headers)
        if (detail::iequals(k, name))
            return v;
    return std::nullopt;
}

std::optional<std::string> HttpRequest::header(const std::string& name) const { return findHeader(headers, name); }

std::optional<std::string> HttpResponse::header(const std::string& name) const { return findHeader(headers, name); }

std::vector<std::string> HttpResponse::headerValues(const std::string& name) const {
    std::vector<std::string> values;
    for (const auto& [k, v] : headers)
        if (detail::iequals(k, name))
            values.push_back(v);
    return values;
}

namespace detail {
std::shared_ptr<IHttpClient> createPlatformHttpClient(const HttpClientOptions& options);
}

std::shared_ptr<IHttpClient> createDefaultHttpClient(const HttpClientOptions& options) {
    return detail::createPlatformHttpClient(options);
}

} // namespace YoutubeExplode::Http
