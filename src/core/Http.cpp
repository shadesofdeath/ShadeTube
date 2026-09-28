#include "core/Http.h"

#include "core/Utf.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace st::http {

namespace {

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36 ShadeTube/0.1";

bool startWinsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

// An IPv4 address (host byte order) outside the public internet.
bool localIpv4(uint32_t a) {
    const uint32_t b0 = a >> 24, b1 = (a >> 16) & 0xFF, b2 = (a >> 8) & 0xFF;
    return b0 == 0 || b0 == 10 || b0 == 127 ||          // "this network", RFC 1918, loopback
           (b0 == 100 && (b1 & 0xC0) == 64) ||           // shared address space (CGNAT)
           (b0 == 169 && b1 == 254) ||                   // link-local
           (b0 == 172 && (b1 & 0xF0) == 16) ||           // RFC 1918
           (b0 == 192 && b1 == 168) ||                   // RFC 1918
           (b0 == 192 && b1 == 0 && b2 == 0) ||          // IETF protocol assignments
           (b0 == 198 && (b1 & 0xFE) == 18) ||           // benchmarking
           b0 >= 224;                                    // multicast, reserved, broadcast
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool localAddress(const sockaddr* sa) {
    if (sa->sa_family == AF_INET) return localIpv4(ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr));
    if (sa->sa_family != AF_INET6) return true;
    const uint8_t* b = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr.s6_addr;
    const auto zero = [&](int from, int to) { return std::all_of(b + from, b + to, [](uint8_t v) { return v == 0; }); };
    if (zero(0, 12)) return localIpv4(be32(b + 12));                                         // ::, ::1, IPv4-compatible
    if (zero(0, 10) && b[10] == 0xFF && b[11] == 0xFF) return localIpv4(be32(b + 12));       // IPv4-mapped
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B && zero(4, 12)) return localIpv4(be32(b + 12));   // NAT64
    if (b[0] == 0x20 && b[1] == 0x02) return localIpv4(be32(b + 2));                         // 6to4
    return (b[0] & 0xFE) == 0xFC ||                    // unique local
           (b[0] == 0xFE && (b[1] & 0x80) == 0x80) ||  // link-local, site-local
           b[0] == 0xFF;                               // multicast
}

struct Handle {
    HINTERNET h;
    explicit Handle(HINTERNET handle) : h(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    operator HINTERNET() const { return h; }
};

HINTERNET limitedSession() {
    static const HINTERNET s = [] {
        const std::wstring agent = toWide(kUserAgent);
        HINTERNET h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)   // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
            h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        return h;
    }();
    return s;
}

std::wstring queryHeader(HINTERNET req, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

} // namespace

std::shared_ptr<YoutubeExplode::Http::IHttpClient> client() {
    static const auto instance = YoutubeExplode::Http::createDefaultHttpClient({std::chrono::seconds(20), {}, {}});
    return instance;
}

static void addDefaultHeaders(HttpRequest& r) {
    if (!r.hasHeader("User-Agent")) r.headers.emplace_back("User-Agent", kUserAgent);
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

// ---- untrusted URLs -------------------------------------------------------------------------------------------------

bool isLocalHost(std::string_view hostIn, bool resolve) {
    std::string host(hostIn);
    for (char& c : host)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return false;
    if (host == "localhost" || host.ends_with(".localhost")) return true;
    if (host.find('%') != std::string::npos) return true;   // an IPv6 address with a zone: only ever link-local
    if (!startWinsock()) return false;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = resolve ? 0 : AI_NUMERICHOST;   // numeric: literals only, never a DNS query
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(toWide(host).c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool local = false;
    for (const ADDRINFOW* p = res; p && !local; p = p->ai_next) local = p->ai_addr && localAddress(p->ai_addr);
    FreeAddrInfoW(res);
    return local;
}

bool isLocalUrl(std::string_view url, bool resolve) {
    // The host exactly as WinHTTP will see it.
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName || !parts.dwHostNameLength) return false;
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    return isLocalHost(toUtf8(std::wstring_view(parts.lpszHostName, parts.dwHostNameLength)), resolve);
}

HttpResponse getLimited(const std::string& url, const Limits& limits, const CancellationToken& ct) {
    using Clock = std::chrono::steady_clock;
    ct.throwIfCancellationRequested();
    const auto deadline = Clock::now() + limits.timeout;
    auto left = [&](const char* stage) -> DWORD {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0) throw std::runtime_error(std::format("{} timed out", stage));
        return static_cast<DWORD>(std::min<long long>(ms, 60000));
    };
    auto failed = [](const char* stage) {
        const DWORD e = GetLastError();
        return std::runtime_error(e == ERROR_WINHTTP_TIMEOUT ? std::format("{} timed out", stage)
                                                             : std::format("{} failed (WinHTTP {})", stage, e));
    };
    if (!limitedSession()) throw std::runtime_error("WinHTTP unavailable");

    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName || !parts.dwHostNameLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        throw std::runtime_error("invalid URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring{});
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle con(WinHttpConnect(limitedSession(), host.c_str(), parts.nPort, 0));
    if (!con) throw failed("connect");
    Handle req(WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) throw failed("open");
    DWORD t = left("connect");
    WinHttpSetTimeouts(req, static_cast<int>(t), static_cast<int>(t), static_cast<int>(t), static_cast<int>(t));
    DWORD features = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;   // sends Accept-Encoding; the cap counts decompressed bytes
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) throw failed("send");
    if (!WinHttpReceiveResponse(req, nullptr)) throw failed("receive");

    HttpResponse response;
    DWORD status = 0, statusSize = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = static_cast<int>(status);
    const std::string type = toUtf8(queryHeader(req, WINHTTP_QUERY_CONTENT_TYPE));
    response.headers.emplace_back("Content-Type", type);
    if (!response.isSuccessStatusCode()) return response;
    if (limits.accept && !limits.accept(type)) throw std::runtime_error(std::format("unexpected content type \"{}\"", type));
    if (const std::wstring len = queryHeader(req, WINHTTP_QUERY_CONTENT_LENGTH);
        !len.empty() && _wtoi64(len.c_str()) > static_cast<int64_t>(limits.maxBytes))
        throw std::runtime_error("response too large");

    char buf[16384];
    for (;;) {
        ct.throwIfCancellationRequested();
        t = left("read");
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) throw failed("read");
        if (available == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, std::min(available, static_cast<DWORD>(sizeof buf)), &got)) throw failed("read");
        if (got == 0) break;
        if (response.body.size() + got > limits.maxBytes) throw std::runtime_error("response too large");
        response.body.append(buf, got);
    }
    return response;
}

} // namespace st::http
