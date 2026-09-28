// libcurl transport (default on non-Windows platforms, optional on Windows).
#if defined(YTE_HTTP_CURL)

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include "../Utils/StringUtils.hpp"

#include <curl/curl.h>

#include <memory>
#include <mutex>

namespace YoutubeExplode::Http::detail {

namespace {

struct TransferState {
    HttpResponse* response = nullptr;
    const CancellationToken* cancellationToken = nullptr;
    bool aborted = false;
};

std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* state = static_cast<TransferState*>(userdata);
    std::string line(data, size * count);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    // A new status line means a redirect (or 100-continue): start over.
    if (YoutubeExplode::detail::startsWith(line, "HTTP/")) {
        state->response->headers.clear();
        return size * count;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        state->response->headers.emplace_back(YoutubeExplode::detail::trim(line.substr(0, colon)),
                                              YoutubeExplode::detail::trim(line.substr(colon + 1)));
    }
    return size * count;
}

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* state = static_cast<TransferState*>(userdata);
    const auto length = size * count;
    if (state->cancellationToken->isCancellationRequested()) {
        state->aborted = true;
        return 0;
    }
    state->response->body.append(data, length);
    return length;
}

int onProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<TransferState*>(userdata);
    if (state->cancellationToken->isCancellationRequested()) {
        state->aborted = true;
        return 1;
    }
    return 0;
}

class CurlHttpClient final : public IHttpClient {
public:
    explicit CurlHttpClient(HttpClientOptions options) : options_(std::move(options)) {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    HttpResponse send(const HttpRequest& request, const CancellationToken& cancellationToken) override {
        return perform(request, cancellationToken);
    }

private:
    HttpResponse perform(const HttpRequest& request, const CancellationToken& cancellationToken) {
        cancellationToken.throwIfCancellationRequested();

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
        if (!curl)
            throw Exceptions::HttpRequestException("Failed to initialize libcurl.");

        HttpResponse response;
        TransferState state{&response, &cancellationToken};

        struct curl_slist* rawHeaders = nullptr;
        for (const auto& [name, value] : request.headers)
            rawHeaders = curl_slist_append(rawHeaders, (name + ": " + value).c_str());
        // Disable "Expect: 100-continue" for POST bodies.
        rawHeaders = curl_slist_append(rawHeaders, "Expect:");
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(rawHeaders, &curl_slist_free_all);

        CURL* h = curl.get();
        curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // all supported encodings, auto-decompress
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headerList.get());
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &onHeader);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &onBody);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, &onProgress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &state);

        // Timeout semantics: fail on connect timeout, or when the transfer stalls for `timeout`.
        const long timeoutMs = static_cast<long>(options_.timeout.count());
        const long timeoutSec = timeoutMs / 1000 > 0 ? timeoutMs / 1000 : 1;
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, timeoutSec);

        if (!options_.proxy.empty())
            curl_easy_setopt(h, CURLOPT_PROXY, options_.proxy.c_str());
        if (!options_.caBundlePath.empty())
            curl_easy_setopt(h, CURLOPT_CAINFO, options_.caBundlePath.c_str());

        const auto method = YoutubeExplode::detail::toLower(request.method);
        if (method == "post") {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        } else if (method == "head") {
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        } else if (method != "get") {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, request.method.c_str());
            if (!request.body.empty()) {
                curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.body.data());
                curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            }
        }

        const CURLcode code = curl_easy_perform(h);

        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        response.statusCode = static_cast<int>(status);

        if (cancellationToken.isCancellationRequested())
            throw Exceptions::OperationCanceledException();
        if (code != CURLE_OK) {
            throw Exceptions::HttpRequestException(
                std::string("HTTP request to '") + request.url + "' failed: " + curl_easy_strerror(code),
                response.statusCode);
        }
        return response;
    }

    HttpClientOptions options_;
};

} // namespace

std::shared_ptr<IHttpClient> createPlatformHttpClient(const HttpClientOptions& options) {
    return std::make_shared<CurlHttpClient>(options);
}

} // namespace YoutubeExplode::Http::detail

#endif // YTE_HTTP_CURL
