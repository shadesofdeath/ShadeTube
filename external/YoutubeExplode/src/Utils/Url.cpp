#include "Url.hpp"

#include "StringUtils.hpp"

namespace YoutubeExplode::detail::Url {

std::string encode(std::string_view value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (isAsciiAlnum(static_cast<char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < value.size()) {
            int hi = hexValue(value[i + 1]), lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

static std::string_view queryOf(std::string_view urlOrQuery) {
    auto q = urlOrQuery.find('?');
    auto query = q == std::string_view::npos ? urlOrQuery : urlOrQuery.substr(q + 1);
    auto hash = query.find('#');
    return hash == std::string_view::npos ? query : query.substr(0, hash);
}

std::vector<std::pair<std::string, std::string>> getQueryParameters(std::string_view urlOrQuery) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& parameter : split(queryOf(urlOrQuery), '&')) {
        auto eq = parameter.find('=');
        auto key = decode(eq == std::string::npos ? std::string_view(parameter) : std::string_view(parameter).substr(0, eq));
        auto value = eq == std::string::npos ? std::string() : decode(std::string_view(parameter).substr(eq + 1));
        if (isBlank(key))
            continue;
        result.emplace_back(std::move(key), std::move(value));
    }
    return result;
}

std::optional<std::string> tryGetQueryParameter(std::string_view urlOrQuery, std::string_view key) {
    for (auto& [k, v] : getQueryParameters(urlOrQuery))
        if (k == key)
            return v;
    return std::nullopt;
}

bool containsQueryParameter(std::string_view url, std::string_view key) {
    return tryGetQueryParameter(url, key).has_value();
}

std::string removeQueryParameter(const std::string& url, std::string_view key) {
    auto q = url.find('?');
    if (q == std::string::npos || !containsQueryParameter(url, key))
        return url;
    auto hashPos = url.find('#', q);
    std::string fragment = hashPos == std::string::npos ? "" : url.substr(hashPos);
    std::string base = url.substr(0, q);
    std::string query;
    for (auto& [k, v] : getQueryParameters(url)) {
        if (k == key)
            continue;
        query += query.empty() ? '?' : '&';
        query += encode(k) + "=" + encode(v);
    }
    return base + query + fragment;
}

std::string setQueryParameter(const std::string& url, std::string_view key, std::string_view value) {
    auto without = removeQueryParameter(url, key);
    auto hashPos = without.find('#');
    std::string fragment = hashPos == std::string::npos ? "" : without.substr(hashPos);
    if (hashPos != std::string::npos)
        without.resize(hashPos);
    bool hasQuery = without.find('?') != std::string::npos;
    return without + (hasQuery ? "&" : "?") + encode(key) + "=" + encode(value) + fragment;
}

static std::string_view authorityOf(std::string_view url) {
    auto scheme = url.find("://");
    auto start = scheme == std::string_view::npos ? 0 : scheme + 3;
    auto end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

std::string getHost(std::string_view url) {
    auto authority = authorityOf(url);
    auto at = authority.rfind('@');
    if (at != std::string_view::npos)
        authority = authority.substr(at + 1);
    auto colon = authority.find(':');
    return toLower(authority.substr(0, colon));
}

std::string getPath(std::string_view url) {
    auto scheme = url.find("://");
    auto start = scheme == std::string_view::npos ? 0 : scheme + 3;
    auto slash = url.find('/', start);
    if (slash == std::string_view::npos)
        return "/";
    auto end = url.find_first_of("?#", slash);
    return std::string(url.substr(slash, end == std::string_view::npos ? std::string_view::npos : end - slash));
}

std::string getOrigin(std::string_view url) {
    auto scheme = url.find("://");
    std::string s = scheme == std::string_view::npos ? "https" : std::string(url.substr(0, scheme));
    return s + "://" + std::string(authorityOf(url));
}

} // namespace YoutubeExplode::detail::Url
