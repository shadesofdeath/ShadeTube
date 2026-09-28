#pragma once
// Thin convenience layer over YoutubeExplode's IHttpClient (WinHTTP, gzip, HTTP/2, thread-safe).
// One shared transport for the whole app so WinHTTP can reuse TLS sessions/connections.
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <chrono>
#include <functional>
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

// ---- URLs from untrusted sources (internet-radio directory entries: anyone can submit a station) --------------------

// The host is this machine or its local network: "localhost", or an address that is loopback, private (RFC 1918, ULA),
// link-local, shared (CGNAT), multicast, unspecified or reserved. IP literals are read the way the system resolver reads
// them (IPv6 with or without brackets). With `resolve` a name counts by every address it resolves to (blocks: worker
// threads only); a name that does not resolve here is not local (the request itself then fails, or a proxy reaches it).
// Used so that such URLs cannot make the app send requests into the user's own network.
bool isLocalHost(std::string_view host, bool resolve = true);
bool isLocalUrl(std::string_view url, bool resolve = true);   // an http(s) URL whose host isLocalHost()

// A GET that gives up early: at most `maxBytes` of (decompressed) body, `timeout` for the whole request, and only a
// response whose Content-Type `accept` takes (unset = any). Redirects are followed. Worker threads only. Throws
// std::runtime_error when a limit is hit or the transfer fails, OperationCanceledException when `ct` fires; a non-2xx
// status comes back in the response (Content-Type is its only header).
struct Limits {
    size_t maxBytes = 0;
    std::chrono::milliseconds timeout{0};
    std::function<bool(std::string_view contentType)> accept;
};
HttpResponse getLimited(const std::string& url, const Limits& limits, const CancellationToken& ct = {});

} // namespace st::http
