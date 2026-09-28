// WinHTTP transport (default on Windows). All Windows-specific code lives in this file.
#if defined(YTE_HTTP_WINHTTP)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include "../Utils/StringUtils.hpp"

#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

// Constants that older SDK headers may not define.
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif

namespace YoutubeExplode::Http::detail {

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string toUtf8(const wchar_t* s, std::size_t length) {
    if (length == 0)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(length), &r[0], n, nullptr, nullptr);
    return r;
}

struct HandleCloser {
    void operator()(HINTERNET h) const noexcept {
        if (h)
            WinHttpCloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

[[noreturn]] void throwLastError(const std::string& what, const std::string& url) {
    const DWORD error = GetLastError();
    throw Exceptions::HttpRequestException("HTTP request to '" + url + "' failed: " + what +
                                           " (WinHTTP error " + std::to_string(error) + ").");
}

class WinHttpClient final : public IHttpClient {
public:
    explicit WinHttpClient(HttpClientOptions options) : options_(std::move(options)) {
        const wchar_t* agent = L"YoutubeExplode-cpp";
        if (!options_.proxy.empty()) {
            auto proxy = toWide(options_.proxy);
            session_.reset(WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0));
        } else {
            // Automatic proxy (Windows 8.1+) honors system/WPAD settings; fall back for older systems.
            session_.reset(WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session_)
                session_.reset(WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0));
        }
        if (!session_)
            throwLastError("WinHttpOpen", "");

        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(session_.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof decompression);
        DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(session_.get(), WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof http2);

        const int timeout = static_cast<int>(options_.timeout.count());
        WinHttpSetTimeouts(session_.get(), timeout, timeout, timeout, timeout);
    }

    HttpResponse send(const HttpRequest& request, const CancellationToken& cancellationToken) override {
        return perform(request, cancellationToken);
    }

private:
    HttpResponse perform(const HttpRequest& request, const CancellationToken& cancellationToken) {
        cancellationToken.throwIfCancellationRequested();

        const std::wstring wideUrl = toWide(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
            throwLastError("invalid URL", request.url);

        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring pathAndQuery(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.lpszExtraInfo)
            pathAndQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (pathAndQuery.empty())
            pathAndQuery = L"/";
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

        Handle connection(WinHttpConnect(session_.get(), host.c_str(), parts.nPort, 0));
        if (!connection)
            throwLastError("WinHttpConnect", request.url);

        const std::wstring method = toWide(request.method);
        Handle req(WinHttpOpenRequest(connection.get(), method.c_str(), pathAndQuery.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req)
            throwLastError("WinHttpOpenRequest", request.url);

        // Cookies are managed by the library itself.
        DWORD disable = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable);

        std::wstring headerBlock;
        for (const auto& [name, value] : request.headers)
            headerBlock += toWide(name) + L": " + toWide(value) + L"\r\n";

        const BOOL sent = WinHttpSendRequest(
            req.get(), headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            headerBlock.empty() ? 0 : static_cast<DWORD>(-1L),
            request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
            static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
        if (!sent)
            throwLastError("WinHttpSendRequest", request.url);
        if (!WinHttpReceiveResponse(req.get(), nullptr))
            throwLastError("WinHttpReceiveResponse", request.url);

        HttpResponse response;

        DWORD status = 0;
        DWORD statusSize = sizeof status;
        WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        response.statusCode = static_cast<int>(status);

        DWORD headersSize = 0;
        WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &headersSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headersSize > 0) {
            std::vector<wchar_t> buffer(headersSize / sizeof(wchar_t) + 1);
            if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    buffer.data(), &headersSize, WINHTTP_NO_HEADER_INDEX)) {
                const auto raw = toUtf8(buffer.data(), headersSize / sizeof(wchar_t));
                for (const auto& line : YoutubeExplode::detail::split(raw, '\n')) {
                    auto colon = line.find(':');
                    if (colon == std::string::npos || YoutubeExplode::detail::startsWith(line, "HTTP/"))
                        continue;
                    response.headers.emplace_back(YoutubeExplode::detail::trim(line.substr(0, colon)),
                                                  YoutubeExplode::detail::trim(line.substr(colon + 1)));
                }
            }
        }

        std::vector<char> buffer(64 * 1024);
        while (true) {
            if (cancellationToken.isCancellationRequested())
                throw Exceptions::OperationCanceledException();
            DWORD read = 0;
            if (!WinHttpReadData(req.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                throwLastError("WinHttpReadData", request.url);
            if (read == 0)
                break;
            response.body.append(buffer.data(), read);
        }
        return response;
    }

    HttpClientOptions options_;
    Handle session_;
};

} // namespace

std::shared_ptr<IHttpClient> createPlatformHttpClient(const HttpClientOptions& options) {
    return std::make_shared<WinHttpClient>(options);
}

} // namespace YoutubeExplode::Http::detail

#endif // YTE_HTTP_WINHTTP
