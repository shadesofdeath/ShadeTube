#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace YoutubeExplode::detail::Url {

/// Percent-encodes everything except RFC 3986 unreserved characters (like Uri.EscapeDataString).
std::string encode(std::string_view value);
/// Decodes percent-escapes and '+' as space (like WebUtility.UrlDecode).
std::string decode(std::string_view value);

/// Parses "a=1&b=2" or a full URL's query into ordered key/value pairs.
std::vector<std::pair<std::string, std::string>> getQueryParameters(std::string_view urlOrQuery);
std::optional<std::string> tryGetQueryParameter(std::string_view urlOrQuery, std::string_view key);
bool containsQueryParameter(std::string_view url, std::string_view key);
std::string removeQueryParameter(const std::string& url, std::string_view key);
std::string setQueryParameter(const std::string& url, std::string_view key, std::string_view value);

/// Host part of an absolute URL ("www.youtube.com").
std::string getHost(std::string_view url);
/// Path part of an absolute URL ("/youtubei/v1/player"), without the query.
std::string getPath(std::string_view url);
/// "https://www.youtube.com"
std::string getOrigin(std::string_view url);

} // namespace YoutubeExplode::detail::Url
