#pragma once
// Thin convenience layer over YoutubeExplode's IHttpClient (WinHTTP, gzip, HTTP/2, thread-safe).
// One shared transport for the whole app so WinHTTP can reuse TLS sessions/connections.
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::http {

using YoutubeExplode::CancellationToken;
using YoutubeExplode::Http::Headers;
using YoutubeExplode::Http::HttpRequest;
using YoutubeExplode::Http::HttpResponse;

std::shared_ptr<YoutubeExplode::Http::IHttpClient> client();

HttpResponse send(HttpRequest request, const CancellationToken& ct = {});
HttpResponse get(std::string url, Headers headers = {}, const CancellationToken& ct = {});
HttpResponse postForm(std::string url, const std::vector<std::pair<std::string, std::string>>& form,
                      Headers headers = {}, const CancellationToken& ct = {});

std::string urlEncode(std::string_view s);
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& form);

// Thrown for non-2xx responses by helpers that require success.
struct HttpError : std::runtime_error {
    int status;
    std::string body;
    HttpError(int s, std::string b, const std::string& what) : std::runtime_error(what), status(s), body(std::move(b)) {}
};

} // namespace st::http
