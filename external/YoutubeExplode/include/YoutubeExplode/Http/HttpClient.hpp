#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace YoutubeExplode::Http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;

    /// Returns the value of the first header with the given name (case-insensitive).
    std::optional<std::string> header(const std::string& name) const;
    bool hasHeader(const std::string& name) const { return header(name).has_value(); }
};

struct HttpResponse {
    int statusCode = 0;
    Headers headers;
    std::string body;

    std::optional<std::string> header(const std::string& name) const;
    /// All values of a header (useful for Set-Cookie).
    std::vector<std::string> headerValues(const std::string& name) const;
    bool isSuccessStatusCode() const noexcept { return statusCode >= 200 && statusCode < 300; }
};

/// Transport abstraction. Implement this to plug in your own HTTP stack
/// (e.g. to add a proxy, custom TLS settings, or to use a different library).
///
/// Implementations must:
///  - follow redirects,
///  - transparently decompress gzip/deflate responses,
///  - NOT throw on non-2xx statuses (return them in HttpResponse),
///  - throw YoutubeExplode::Exceptions::HttpRequestException on transport failures,
///  - be safe to call concurrently from multiple threads.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /// Sends a request and buffers the whole response body.
    virtual HttpResponse send(const HttpRequest& request, const CancellationToken& cancellationToken) = 0;
};

struct HttpClientOptions {
    /// Per-request timeout.
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
    /// Explicit proxy URL (e.g. "http://127.0.0.1:8080"). Empty = system/environment default.
    std::string proxy;
    /// Path to a PEM CA bundle (curl backend only). Empty = system default.
    std::string caBundlePath;
};

/// Creates the platform default client: WinHTTP on Windows, libcurl elsewhere.
std::shared_ptr<IHttpClient> createDefaultHttpClient(const HttpClientOptions& options = {});

/// Browser cookie. Pass cookies of a signed-in account to YoutubeClient to access
/// age-restricted or private content and to lift rate limits.
struct Cookie {
    std::string name;
    std::string value;
    std::string domain = ".youtube.com";
    std::string path = "/";
};

} // namespace YoutubeExplode::Http
