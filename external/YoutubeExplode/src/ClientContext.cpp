#include "ClientContext.hpp"

#include "Bridge/PlayerSource.hpp"
#include "Utils/Crypto.hpp"
#include "Utils/Json.hpp"
#include "Utils/StringUtils.hpp"
#include "Utils/Url.hpp"

#include <YoutubeExplode/Exceptions.hpp>

#include <chrono>
#include <regex>

namespace YoutubeExplode::detail {

namespace {

constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

// Public innertube key used by the web client; it has not changed in years.
constexpr const char* kInnertubeApiKey = "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w";

// Consent cookie ("accept all") so that EU requests are not redirected to consent.youtube.com.
// Supposed to be valid for 13 months; see https://policies.google.com/technologies/cookies/embedded
constexpr const char* kConsentCookieValue = "CAISEwgDEgk4MTM4MzYzNTIaAmVuIAEaBgiApPzGBg";

bool isYoutubeHost(const std::string& host) {
    return host == "youtube.com" || endsWith(host, ".youtube.com") || host == "youtu.be";
}

bool domainMatches(const std::string& host, const std::string& domain, bool hostOnly) {
    if (hostOnly)
        return host == domain;
    return host == domain || endsWith(host, "." + domain);
}

} // namespace

ClientContext::ClientContext(YoutubeClientOptions options) : options_(std::move(options)) {
    http_ = options_.httpClient ? options_.httpClient : Http::createDefaultHttpClient();

    cookies_.push_back({"SOCS", kConsentCookieValue, "youtube.com", false, "/"});
    for (const auto& c : options_.initialCookies) {
        auto domain = toLower(c.domain);
        while (startsWith(domain, "."))
            domain.erase(0, 1);
        if (domain.empty())
            domain = "youtube.com";
        // Replace existing cookie with the same name/domain.
        cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                      [&](const StoredCookie& s) { return s.name == c.name && s.domain == domain; }),
                       cookies_.end());
        cookies_.push_back({c.name, c.value, domain, false, c.path.empty() ? "/" : c.path});
    }
    initialCookies_ = cookies_;
}

void ClientContext::resetSession() {
    {
        std::lock_guard<std::mutex> lock(cookiesMutex_);
        cookies_ = initialCookies_;
    }
    std::lock_guard<std::mutex> lock(cacheMutex_);
    visitorData_.reset();
    visitorDataResolved_ = false;
}

std::string ClientContext::cookieHeaderFor(const std::string& host, const std::string& path) {
    std::lock_guard<std::mutex> lock(cookiesMutex_);
    std::string header;
    for (const auto& c : cookies_) {
        if (!domainMatches(host, c.domain, c.hostOnly) || !startsWith(path, c.path))
            continue;
        if (!header.empty())
            header += "; ";
        header += c.name + "=" + c.value;
    }
    return header;
}

std::optional<std::string> ClientContext::authorizationFor(const std::string& origin) {
    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(cookiesMutex_);
        for (const auto& c : cookies_)
            if (c.name == "__Secure-3PAPISID") sessionId = c.value;
        if (sessionId.empty())
            for (const auto& c : cookies_)
                if (c.name == "SAPISID") sessionId = c.value;
    }
    if (sessionId.empty())
        return std::nullopt;
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    const auto token = std::to_string(timestamp) + " " + sessionId + " " + origin;
    return "SAPISIDHASH " + std::to_string(timestamp) + "_" + sha1Hex(token);
}

void ClientContext::storeCookie(const std::string& setCookie, const std::string& requestHost) {
    auto attributes = split(setCookie, ';');
    if (attributes.empty())
        return;
    auto pair = attributes[0];
    auto eq = pair.find('=');
    if (eq == std::string::npos)
        return;
    StoredCookie cookie;
    cookie.name = trim(pair.substr(0, eq));
    cookie.value = trim(pair.substr(eq + 1));
    cookie.domain = requestHost;
    cookie.hostOnly = true;
    bool expired = false;
    for (std::size_t i = 1; i < attributes.size(); ++i) {
        auto attr = trim(attributes[i]);
        auto aeq = attr.find('=');
        auto key = toLower(trim(attr.substr(0, aeq)));
        auto val = aeq == std::string::npos ? std::string() : trim(attr.substr(aeq + 1));
        if (key == "domain" && !val.empty()) {
            auto domain = toLower(val);
            while (startsWith(domain, "."))
                domain.erase(0, 1);
            // Ignore cookies for foreign domains (like CookieContainer does).
            if (!domainMatches(requestHost, domain, false))
                return;
            cookie.domain = domain;
            cookie.hostOnly = false;
        } else if (key == "path" && !val.empty()) {
            cookie.path = val;
        } else if (key == "max-age") {
            if (auto v = tryParseInt64(val); v && *v <= 0)
                expired = true;
        }
    }
    if (cookie.name.empty())
        return;

    std::lock_guard<std::mutex> lock(cookiesMutex_);
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                  [&](const StoredCookie& s) {
                                      return s.name == cookie.name && s.domain == cookie.domain && s.path == cookie.path;
                                  }),
                   cookies_.end());
    if (!expired)
        cookies_.push_back(std::move(cookie));
}

void ClientContext::prepare(Http::HttpRequest& request) {
    const auto host = Url::getHost(request.url);
    const auto path = Url::getPath(request.url);
    const auto origin = Url::getOrigin(request.url);

    if (isYoutubeHost(host)) {
        if (startsWith(path, "/youtubei/") && !Url::containsQueryParameter(request.url, "key"))
            request.url = Url::setQueryParameter(request.url, "key", kInnertubeApiKey);
        if (!Url::containsQueryParameter(request.url, "hl"))
            request.url = Url::setQueryParameter(request.url, "hl", options_.language);
        if (!request.hasHeader("Origin"))
            request.headers.emplace_back("Origin", origin);
        if (!request.hasHeader("Authorization"))
            if (auto auth = authorizationFor(origin))
                request.headers.emplace_back("Authorization", *auth);
    }
    if (!request.hasHeader("User-Agent"))
        request.headers.emplace_back("User-Agent", kDefaultUserAgent);
    if (!request.hasHeader("Cookie")) {
        auto cookie = cookieHeaderFor(host, path);
        if (!cookie.empty())
            request.headers.emplace_back("Cookie", cookie);
    }
}

void ClientContext::handleResponse(const Http::HttpRequest& request, const Http::HttpResponse& response) {
    if (response.statusCode == 429) {
        throw Exceptions::RequestLimitExceededException(
            "Exceeded request rate limit. Please try again in a few hours. Alternatively, inject cookies "
            "corresponding to a pre-authenticated user when initializing an instance of YoutubeClient.");
    }
    const auto host = Url::getHost(request.url);
    for (const auto& value : response.headerValues("Set-Cookie"))
        storeCookie(value, host);
}

Http::HttpResponse ClientContext::send(Http::HttpRequest request, const CancellationToken& cancellationToken) {
    prepare(request);
    for (int retriesRemaining = 5;; --retriesRemaining) {
        cancellationToken.throwIfCancellationRequested();
        auto response = http_->send(request, cancellationToken);
        handleResponse(request, response);
        if (response.statusCode >= 500 && retriesRemaining > 0)
            continue;
        return response;
    }
}

void ClientContext::ensureSuccess(const Http::HttpResponse& response, const std::string& url) {
    if (!response.isSuccessStatusCode()) {
        throw Exceptions::HttpRequestException(
            "Response status code does not indicate success: " + std::to_string(response.statusCode) +
                ". Request URL: " + Url::removeQueryParameter(url, "key") + ".",
            response.statusCode);
    }
}

std::string ClientContext::getString(const std::string& url, const CancellationToken& cancellationToken,
                                     const Http::Headers& headers) {
    Http::HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers = headers;
    auto response = send(std::move(request), cancellationToken);
    ensureSuccess(response, url);
    return std::move(response.body);
}

std::string ClientContext::postJson(const std::string& url, const std::string& json,
                                    const CancellationToken& cancellationToken, const Http::Headers& headers) {
    Http::HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = headers;
    request.headers.emplace_back("Content-Type", "application/json");
    request.body = json;
    auto response = send(std::move(request), cancellationToken);
    ensureSuccess(response, url);
    return std::move(response.body);
}

std::optional<std::string> ClientContext::visitorData(const CancellationToken& cancellationToken) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (visitorDataResolved_)
            return visitorData_;
    }

    std::optional<std::string> value;
    try {
        auto raw = getString("https://www.youtube.com/sw.js_data", cancellationToken,
                             {{"Accept", "application/json"},
                              {"User-Agent", "com.google.android.youtube/20.10.38 (Linux; U; ANDROID 11) gzip"}});
        if (startsWith(raw, ")]}'"))
            raw.erase(0, 4);
        if (auto json = tryParseJson(raw)) {
            // This is an ordered (but unstructured) blob of data.
            value = jsonString(dig(*json, {0, 2, 0, 0, 13}));
        }
    } catch (const Exceptions::OperationCanceledException&) {
        throw;
    } catch (const Exceptions::RequestLimitExceededException&) {
        throw;
    } catch (const std::exception&) {
        // Fall through to the innertube endpoint below.
    }

    // Fallback: the innertube visitor_id endpoint (tiny response). Without visitor data, YouTube
    // answers player requests from some networks with "Sign in to confirm you're not a bot".
    if (!value || isBlank(*value)) {
        try {
            auto raw = postJson("https://www.youtube.com/youtubei/v1/visitor_id",
                                R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20210408.08.00","hl":"en","gl":"US"}}})",
                                cancellationToken);
            if (auto json = tryParseJson(raw))
                value = jsonString(dig(*json, {"responseContext", "visitorData"}));
        } catch (const Exceptions::OperationCanceledException&) {
            throw;
        } catch (const Exceptions::RequestLimitExceededException&) {
            throw;
        } catch (const std::exception&) {
            // Visitor data is helpful but not mandatory: continue without it.
        }
    }

    // Cache failures too: visitor data is optional and retrying on every request would only add latency.
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (value && isBlank(*value))
        value.reset();
    visitorData_ = value;
    visitorDataResolved_ = true;
    return value;
}

std::shared_ptr<const PlayerSource> ClientContext::playerSource(const CancellationToken& cancellationToken) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (playerSource_)
            return playerSource_;
    }

    // Locate the current player version through the iframe API loader, which is tiny and stable:
    //   ... "https:\/\/www.youtube.com\/s\/player\/0004de42\/www-widgetapi.vflset\/www-widgetapi.js" ...
    const auto iframe = getString("https://www.youtube.com/iframe_api", cancellationToken);
    static const std::regex versionPattern(R"(player\\?/([0-9a-fA-F]{8})\\?/)");
    std::smatch match;
    if (!std::regex_search(iframe, match, versionPattern))
        throw Exceptions::CipherExtractionException("Failed to extract the player version from /iframe_api.");
    const auto version = match[1].str();

    const auto playerUrl = "https://www.youtube.com/s/player/" + version + "/player_ias.vflset/en_US/base.js";
    auto source = std::make_shared<const PlayerSource>(playerUrl, getString(playerUrl, cancellationToken));

    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (!playerSource_)
        playerSource_ = std::move(source);
    return playerSource_;
}

} // namespace YoutubeExplode::detail
