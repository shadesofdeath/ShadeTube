#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <string>

namespace YoutubeExplode::Music {

/// One request to YouTube Music's InnerTube API.
struct MusicRequest {
    /// Path below https://music.youtube.com/youtubei/v1/: "search", "browse", "next", "music/get_search_suggestions".
    std::string endpoint;
    /// The request object as JSON, without "context" (it is added).
    std::string body = "{}";
    /// Extra URL query, e.g. "ctoken=<t>&continuation=<t>&type=next" for an old-style continuation.
    std::string query;
    /// Interface language ("hl") and region ("gl") of this request; empty = the client's options.
    std::string language;
    std::string region;
};

/// Raw access to YouTube Music (music.youtube.com) as the anonymous web client ("WEB_REMIX"), through the
/// YoutubeClient's session: the consent cookie, the cookies YouTube sets, the 5xx retries and the 429 mapping are shared
/// with every other request of the client. YouTube Music keeps a visitor of its own: the first answer names one
/// (responseContext.visitorData), which is then sent with every request of the session (context + X-Goog-Visitor-Id)
/// and dropped by resetSession(). The www.youtube.com visitor data is not sent: with it YouTube Music answers the
/// Home feed's continuations with nothing. YouTube Music answers with deeply nested renderer trees whose shape changes
/// often, so the body is handed back as JSON text for the caller to interpret. Cheap to copy; thread-safe.
class MusicClient {
public:
    explicit MusicClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    /// Sends the request and returns the response body. Throws HttpRequestException on a non-2xx status,
    /// RequestLimitExceededException on HTTP 429, YoutubeExplodeException when `body` is not a JSON object.
    std::string send(const MusicRequest& request, const CancellationToken& cancellationToken = {}) const;

    /// The WEB_REMIX client version sent: "1.<yyyymmdd>.01.00" of a few days ago, the form the web app sends (a
    /// fixed version would age; a clock before 2025 falls back to a known one).
    static std::string clientVersion();

private:
    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Music
