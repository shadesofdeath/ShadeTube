#include "audio/HttpStream.h"

#include "audio/LiveParsers.h"
#include "core/Http.h"
#include "core/Utf.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace st::audio {

namespace {

// Radio servers are picky about user agents in the other direction: Shoutcast serves its HTML status page to
// browsers ("Mozilla"), so live streams identify as a player.
constexpr wchar_t kUserAgent[] = L"ShadeTube (Windows; internet radio)";
constexpr char kUserAgentAscii[] = "ShadeTube (Windows; internet radio)";
constexpr DWORD kTimeoutMs = 12000;          // resolve / connect / send / receive
constexpr size_t kMaxRawHeader = 16 * 1024;
constexpr int kMaxRedirects = 5;              // followed here: Shoutcast v1 answers, public-only requests
constexpr ULONGLONG kHostCheckMs = 60000;     // a host's local-network check is reused for a minute

// Completion state of one asynchronous WinHTTP request (see ProgressiveBuffer.cpp, same pattern): `done` is signalled
// for every completed operation or error, `closed` by HANDLE_CLOSING, the last callback a request handle produces.
struct RequestContext {
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD bytes = 0;
    DWORD error = 0;
    RequestContext() = default;
    RequestContext(const RequestContext&) = delete;
    RequestContext& operator=(const RequestContext&) = delete;
    ~RequestContext() {
        CloseHandle(done);
        CloseHandle(closed);
    }
};

void CALLBACK onWinHttpStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    auto* ctx = reinterpret_cast<RequestContext*>(context);
    if (!ctx) return;   // session / connection handles carry no context
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        ctx->error = 0;
        SetEvent(ctx->done);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        ctx->bytes = length;
        ctx->error = 0;
        SetEvent(ctx->done);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* r = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        ctx->error = (r && r->dwError) ? r->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
        SetEvent(ctx->done);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(ctx->closed); break;
    default: break;
    }
}

std::string winHttpError(const char* what, DWORD code) {
    if (code == ERROR_WINHTTP_TIMEOUT) return std::format("{} timed out", what);
    if (code == ERROR_WINHTTP_CANNOT_CONNECT) return std::format("{}: cannot connect", what);
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return std::format("{}: host not found", what);
    return std::format("{} failed (WinHTTP error {})", what, code);
}

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct UrlParts {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring pathQuery;
};

bool crack(const std::string& url, UrlParts& out) {
    if (!live::isHttpUrl(url)) return false;
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.dwHostNameLength) return false;
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.pathQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) out.pathQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (out.pathQuery.empty()) out.pathQuery = L"/";
    return true;
}

std::wstring queryHeader(HINTERNET req, DWORD info, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, name, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, name, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

bool startWinsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

} // namespace

struct HttpStream::Impl {
    explicit Impl(HANDLE c) : cancel(c) {}
    ~Impl() {
        closeRequest();
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }

    HANDLE cancel;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    std::wstring connHost;
    INTERNET_PORT connPort = 0;
    HINTERNET request = nullptr;
    std::unique_ptr<RequestContext> ctx;

    int status = 0;
    std::string contentType, finalUrl;
    int64_t contentLength = -1;

    // Raw socket mode (Shoutcast v1 "ICY 200 OK").
    bool raw = false;
    SOCKET sock = INVALID_SOCKET;
    WSAEVENT sockEvent = WSA_INVALID_EVENT;
    std::string pending;   // body bytes received together with the headers
    size_t pendingPos = 0;
    std::map<std::string, std::string> rawHeaders;
    int redirects = 0;

    bool publicOnly = false;
    std::wstring checkedHost;
    ULONGLONG checkedAt = 0;
    bool checkedLocal = false;

    bool cancelled() const { return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; }

    // http::isLocalHost resolves names: on a helper thread, so that the cancel event never waits for a slow DNS
    // server. Cancelled: true (nothing is sent; the stream is going away).
    bool localHost(const std::wstring& host) {
        const ULONGLONG now = GetTickCount64();
        if (host == checkedHost && now - checkedAt <= kHostCheckMs) return checkedLocal;
        struct Check {
            HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<bool> local{false};
            Check() = default;
            Check(const Check&) = delete;
            Check& operator=(const Check&) = delete;
            ~Check() { CloseHandle(done); }
        };
        auto check = std::make_shared<Check>();
        std::thread([check, name = toUtf8(host)] {
            check->local = http::isLocalHost(name);
            SetEvent(check->done);
        }).detach();
        HANDLE handles[2] = {check->done, cancel};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) return true;
        checkedLocal = check->local;
        checkedHost = host;
        checkedAt = now;
        return checkedLocal;
    }

    // A redirect answer (public only: WinHTTP does not follow them then): opens its target instead.
    bool follow(const Request& r, std::string location, std::string& error) {
        closeRequest();
        if (redirects >= kMaxRedirects) {
            error = "too many redirects";
            return false;
        }
        Request next = r;
        next.url = live::resolveUrl(r.url, location);
        ++redirects;
        const bool ok = open(next, error);
        --redirects;
        return ok;
    }

    void closeRequest() {
        if (request) {
            WinHttpCloseHandle(request);
            request = nullptr;
            // After HANDLE_CLOSING WinHTTP no longer touches the context or our buffers. Should it never come, the
            // context is leaked rather than freed under a pending callback.
            if (WaitForSingleObject(ctx->closed, 10000) != WAIT_OBJECT_0) (void)ctx.release();
        }
        ctx.reset();
        if (sock != INVALID_SOCKET) closesocket(sock);
        sock = INVALID_SOCKET;
        if (sockEvent != WSA_INVALID_EVENT) WSACloseEvent(sockEvent);
        sockEvent = WSA_INVALID_EVENT;
        raw = false;
        pending.clear();
        pendingPos = 0;
        rawHeaders.clear();
    }

    // 0 = completed, 1 = cancelled, 2 = no callback in time (WinHTTP's own timeouts normally fire first).
    int wait() {
        HANDLE handles[2] = {ctx->done, cancel};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, kTimeoutMs + 10000);
        return w == WAIT_OBJECT_0 ? 0 : w == WAIT_OBJECT_0 + 1 ? 1 : 2;
    }

    bool finish(const char* what, std::string& error) {
        const int w = wait();
        if (w == 0 && !ctx->error) return true;
        error = w == 1 ? "cancelled" : w == 2 ? std::format("{} timed out", what) : winHttpError(what, ctx->error);
        return false;
    }

    bool open(const Request& r, std::string& error) {
        closeRequest();
        status = 0;
        contentType.clear();
        contentLength = -1;
        finalUrl = r.url;
        if (cancelled()) {
            error = "cancelled";
            return false;
        }
        UrlParts u;
        if (!crack(r.url, u)) {
            error = "invalid URL";
            return false;
        }
        if (publicOnly && localHost(u.host)) {
            error = std::format("refused: {} is in the local network", toUtf8(u.host));
            return false;
        }
        if (!session) {
            session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  WINHTTP_FLAG_ASYNC);
            if (!session)
                session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                      WINHTTP_FLAG_ASYNC);
            if (!session) {
                error = winHttpError("WinHttpOpen", GetLastError());
                return false;
            }
            WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
            if (WinHttpSetStatusCallback(session, onWinHttpStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                         0) == WINHTTP_INVALID_STATUS_CALLBACK) {
                error = winHttpError("WinHttpSetStatusCallback", GetLastError());
                return false;
            }
        }
        if (!connection || connHost != u.host || connPort != u.port) {
            if (connection) WinHttpCloseHandle(connection);
            connection = WinHttpConnect(session, u.host.c_str(), u.port, 0);
            connHost = u.host;
            connPort = u.port;
            if (!connection) {
                error = winHttpError("WinHttpConnect", GetLastError());
                return false;
            }
        }
        ctx = std::make_unique<RequestContext>();
        request = WinHttpOpenRequest(connection, L"GET", u.pathQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            error = winHttpError("WinHttpOpenRequest", GetLastError());
            return false;
        }
        auto ctxValue = reinterpret_cast<DWORD_PTR>(ctx.get());
        WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &ctxValue, sizeof ctxValue);
        // Radio CDNs redirect https -> http too. Public only: each hop is checked in follow() first.
        DWORD policy = publicOnly ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER : WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        std::wstring headers;
        if (r.icyMetadata) headers += L"Icy-MetaData: 1\r\n";
        if (r.rangeLength > 0)
            headers += std::format(L"Range: bytes={}-{}\r\n", std::max<int64_t>(0, r.rangeOffset), std::max<int64_t>(0, r.rangeOffset) + r.rangeLength - 1);
        if (!WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, ctxValue)) {
            error = winHttpError("WinHttpSendRequest", GetLastError());
            closeRequest();
            return false;
        }
        if (!finish("send", error)) {
            closeRequest();
            return false;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            error = winHttpError("WinHttpReceiveResponse", GetLastError());
            closeRequest();
            return false;
        }
        if (!finish("receive", error)) {
            const bool icyStatusLine = ctx && ctx->error == ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
            closeRequest();
            if (icyStatusLine && !u.secure && !cancelled()) return rawOpen(r, u, error);
            return false;
        }
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        if (publicOnly && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            if (std::string location = toUtf8(queryHeader(request, WINHTTP_QUERY_LOCATION)); !location.empty())
                return follow(r, std::move(location), error);
        }
        contentType = toUtf8(queryHeader(request, WINHTTP_QUERY_CONTENT_TYPE));
        const std::wstring length = queryHeader(request, WINHTTP_QUERY_CONTENT_LENGTH);
        contentLength = length.empty() ? -1 : _wtoi64(length.c_str());
        DWORD urlSize = 0;
        WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &urlSize);
        if (urlSize > 0) {
            std::wstring url(urlSize / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &urlSize)) {
                url.resize(wcsnlen(url.c_str(), url.size()));
                finalUrl = toUtf8(url);
            }
        }
        if (status < 200 || status >= 300) {
            error = std::format("HTTP {}", status);
            closeRequest();
            return false;
        }
        return true;
    }

    // ---- raw socket (Shoutcast v1) -------------------------------------------------------------------------------

    int waitSocket() {   // 0 = event, 1 = cancelled, 2 = timeout
        HANDLE handles[2] = {sockEvent, cancel};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, kTimeoutMs);
        if (w == WAIT_OBJECT_0) return 0;
        return w == WAIT_OBJECT_0 + 1 ? 1 : 2;
    }

    int64_t rawRecv(void* dst, size_t size, std::string& error) {
        if (pendingPos < pending.size()) {
            const size_t n = std::min(size, pending.size() - pendingPos);
            std::memcpy(dst, pending.data() + pendingPos, n);
            pendingPos += n;
            return static_cast<int64_t>(n);
        }
        for (;;) {
            const int r = recv(sock, static_cast<char*>(dst), static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
            if (r >= 0) return r;
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                error = std::format("socket read failed ({})", WSAGetLastError());
                return -1;
            }
            const int w = waitSocket();
            if (w != 0) {
                error = w == 1 ? "cancelled" : "read timed out";
                return -1;
            }
            WSANETWORKEVENTS events{};
            WSAEnumNetworkEvents(sock, sockEvent, &events);   // resets the event
        }
    }

    bool rawOpen(const Request& r, const UrlParts& u, std::string& error) {
        if (!startWinsock()) {
            error = "Winsock unavailable";
            return false;
        }
        const std::string host = toUtf8(u.host);
        const std::string port = std::to_string(u.port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
            error = "host not found";
            return false;
        }
        bool connected = false;
        for (addrinfo* ai = result; ai && !connected && !cancelled(); ai = ai->ai_next) {
            sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock == INVALID_SOCKET) continue;
            sockEvent = WSACreateEvent();
            WSAEventSelect(sock, sockEvent, FD_CONNECT | FD_READ | FD_CLOSE);   // also makes the socket non-blocking
            if (connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
                if (waitSocket() == 0) {
                    WSANETWORKEVENTS events{};
                    WSAEnumNetworkEvents(sock, sockEvent, &events);
                    connected = (events.lNetworkEvents & FD_CONNECT) && events.iErrorCode[FD_CONNECT_BIT] == 0;
                }
            }
            if (!connected) closeRequest();
        }
        freeaddrinfo(result);
        if (!connected) {
            error = cancelled() ? "cancelled" : "cannot connect";
            return false;
        }
        std::string hostHeader = host;
        if (u.port != 80) hostHeader += ":" + port;
        const std::string head = std::format("GET {} HTTP/1.0\r\nHost: {}\r\nUser-Agent: {}\r\nAccept: */*\r\n{}Connection: close\r\n\r\n",
                                             toUtf8(u.pathQuery), hostHeader, kUserAgentAscii, r.icyMetadata ? "Icy-MetaData: 1\r\n" : "");
        for (size_t sent = 0; sent < head.size();) {
            const int n = send(sock, head.data() + sent, static_cast<int>(head.size() - sent), 0);
            if (n > 0) {
                sent += static_cast<size_t>(n);
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                if (WaitForSingleObject(cancel, 20) == WAIT_OBJECT_0) {
                    error = "cancelled";
                    closeRequest();
                    return false;
                }
            } else {
                error = "send failed";
                closeRequest();
                return false;
            }
        }
        // Response header, up to the empty line; what follows is body.
        std::string buf;
        size_t end = std::string::npos;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            char chunk[4096];
            const int64_t n = rawRecv(chunk, sizeof chunk, error);
            if (n <= 0 || buf.size() > kMaxRawHeader) {
                if (n == 0 || buf.size() > kMaxRawHeader) error = "invalid response";
                closeRequest();
                return false;
            }
            buf.append(chunk, static_cast<size_t>(n));
        }
        raw = true;
        pending = buf.substr(end + 4);
        pendingPos = 0;
        const std::string header = buf.substr(0, end);
        size_t lineEnd = header.find("\r\n");
        const std::string statusLine = header.substr(0, lineEnd);
        if (statusLine.starts_with("ICY ")) status = atoi(statusLine.c_str() + 4);
        else if (statusLine.starts_with("HTTP/")) status = atoi(statusLine.c_str() + std::min(statusLine.find(' '), statusLine.size()));
        while (lineEnd != std::string::npos) {
            const size_t next = header.find("\r\n", lineEnd + 2);
            const std::string line = header.substr(lineEnd + 2, next == std::string::npos ? std::string::npos : next - lineEnd - 2);
            lineEnd = next;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            rawHeaders[lowerAscii(line.substr(0, colon))] = value;
        }
        contentType = rawHeaders["content-type"];
        if (status >= 300 && status < 400 && !rawHeaders["location"].empty()) return follow(r, rawHeaders["location"], error);
        if (status < 200 || status >= 300) {
            error = std::format("HTTP {}", status);
            closeRequest();
            return false;
        }
        return true;
    }
};

HttpStream::HttpStream(void* cancelEvent) : impl_(std::make_unique<Impl>(static_cast<HANDLE>(cancelEvent))) {}
HttpStream::~HttpStream() = default;

void HttpStream::setPublicOnly(bool on) { impl_->publicOnly = on; }
bool HttpStream::open(const Request& request, std::string& error) { return impl_->open(request, error); }
int HttpStream::status() const { return impl_->status; }
std::string HttpStream::contentType() const { return impl_->contentType; }
std::string HttpStream::finalUrl() const { return impl_->finalUrl; }
int64_t HttpStream::contentLength() const { return impl_->contentLength; }
void HttpStream::close() { impl_->closeRequest(); }

std::string HttpStream::header(const char* name) const {
    if (impl_->raw) {
        const auto it = impl_->rawHeaders.find(lowerAscii(name));
        return it == impl_->rawHeaders.end() ? std::string() : it->second;
    }
    if (!impl_->request) return {};
    return toUtf8(queryHeader(impl_->request, WINHTTP_QUERY_CUSTOM, toWide(name).c_str()));
}

int64_t HttpStream::read(void* dst, size_t size, std::string& error) {
    Impl& d = *impl_;
    if (d.raw) return d.rawRecv(dst, size, error);
    if (!d.request) {
        error = "not open";
        return -1;
    }
    d.ctx->bytes = 0;
    if (!WinHttpReadData(d.request, dst, static_cast<DWORD>(std::min<size_t>(size, 1 << 20)), nullptr)) {
        error = winHttpError("read", GetLastError());
        return -1;
    }
    if (!d.finish("read", error)) {
        d.closeRequest();   // also waits until WinHTTP is done with `dst`
        return -1;
    }
    return d.ctx->bytes;
}

bool HttpStream::readAll(std::string& body, size_t maxBytes, std::string& error) {
    char chunk[16 * 1024];
    for (;;) {
        const int64_t n = read(chunk, sizeof chunk, error);
        if (n < 0) return false;
        if (n == 0) return true;
        if (body.size() + static_cast<size_t>(n) > maxBytes) {
            error = "response too large";
            close();
            return false;
        }
        body.append(chunk, static_cast<size_t>(n));
    }
}

} // namespace st::audio
