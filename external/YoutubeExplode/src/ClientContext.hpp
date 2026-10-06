#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>
#include <YoutubeExplode/YoutubeClient.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::detail {

class PlayerSource;

/// Shared per-client state. Equivalent of YoutubeHttpHandler + the caches kept by the
/// controllers in the C# library.
class ClientContext {
public:
    explicit ClientContext(YoutubeClientOptions options);

    const YoutubeClientOptions& options() const noexcept { return options_; }

    /// Sends a request through the YouTube pipeline:
    ///  - adds the innertube API key and `hl` to youtube.com requests,
    ///  - adds Origin, User-Agent, cookies and SAPISIDHASH authorization,
    ///  - stores cookies from Set-Cookie,
    ///  - retries 5xx responses, maps 429 to RequestLimitExceededException.
    Http::HttpResponse send(Http::HttpRequest request, const CancellationToken& cancellationToken);

    /// GET and return the body; throws HttpRequestException on non-2xx.
    std::string getString(const std::string& url, const CancellationToken& cancellationToken,
                          const Http::Headers& headers = {});
    /// POST a JSON body and return the response body; throws HttpRequestException on non-2xx.
    std::string postJson(const std::string& url, const std::string& json, const CancellationToken& cancellationToken,
                         const Http::Headers& headers = {});

    /// Visitor data identifier (cached for the lifetime of the client).
    std::optional<std::string> visitorData(const CancellationToken& cancellationToken);

    /// Drops the cached visitor data and the cookies YouTube set (keeps the consent and the initial cookies).
    void resetSession();

    /// Current player JavaScript (cached; the player version is resolved from /iframe_api).
    std::shared_ptr<const PlayerSource> playerSource(const CancellationToken& cancellationToken);

    static void ensureSuccess(const Http::HttpResponse& response, const std::string& url);

private:
    struct StoredCookie {
        std::string name;
        std::string value;
        std::string domain;  // lower-case, without leading dot
        bool hostOnly = false;
        std::string path = "/";
    };

    void prepare(Http::HttpRequest& request);
    void handleResponse(const Http::HttpRequest& request, const Http::HttpResponse& response);
    void storeCookie(const std::string& setCookie, const std::string& requestHost);
    std::string cookieHeaderFor(const std::string& host, const std::string& path);
    std::optional<std::string> authorizationFor(const std::string& origin);

    YoutubeClientOptions options_;
    std::shared_ptr<Http::IHttpClient> http_;

    std::mutex cookiesMutex_;
    std::vector<StoredCookie> cookies_;
    std::vector<StoredCookie> initialCookies_;  // consent + options.initialCookies: what resetSession() returns to

    std::mutex cacheMutex_;
    std::optional<std::string> visitorData_;
    bool visitorDataResolved_ = false;
    std::shared_ptr<const PlayerSource> playerSource_;
};

} // namespace YoutubeExplode::detail
