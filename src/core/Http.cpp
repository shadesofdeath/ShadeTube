#include "core/Http.h"

#include <chrono>

namespace st::http {

std::shared_ptr<YoutubeExplode::Http::IHttpClient> client() {
    static const auto instance = YoutubeExplode::Http::createDefaultHttpClient({std::chrono::seconds(20), {}, {}});
    return instance;
}

static void addDefaultHeaders(HttpRequest& r) {
    if (!r.hasHeader("User-Agent"))
        r.headers.emplace_back("User-Agent",
                               "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                               "Chrome/128.0 Safari/537.36 ShadeTube/0.1");
    if (!r.hasHeader("Accept-Encoding")) r.headers.emplace_back("Accept-Encoding", "gzip, deflate");
}

HttpResponse send(HttpRequest request, const CancellationToken& ct) {
    addDefaultHeaders(request);
    return client()->send(request, ct);
}

HttpResponse get(std::string url, Headers headers, const CancellationToken& ct) {
    HttpRequest r;
    r.url = std::move(url);
    r.headers = std::move(headers);
    return send(std::move(r), ct);
}

HttpResponse postForm(std::string url, const std::vector<std::pair<std::string, std::string>>& form, Headers headers,
                      const CancellationToken& ct) {
    HttpRequest r;
    r.method = "POST";
    r.url = std::move(url);
    r.headers = std::move(headers);
    r.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    r.body = formEncode(form);
    return send(std::move(r), ct);
}

std::string urlEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& form) {
    std::string body;
    for (const auto& [k, v] : form) {
        if (!body.empty()) body.push_back('&');
        body += urlEncode(k);
        body.push_back('=');
        body += urlEncode(v);
    }
    return body;
}

} // namespace st::http
