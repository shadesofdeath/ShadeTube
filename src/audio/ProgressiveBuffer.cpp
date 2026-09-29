#include "audio/ProgressiveBuffer.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <format>
#include <new>

namespace st::audio {

namespace {

constexpr int64_t kChunk = 1 << 20;                // bytes per range request
// Demuxers read the header, then the file's tail (MP4: mfra / trailing index; WebM: cues), then
// the media. The last kTail bytes are therefore fetched by a parallel request at start-up, so the
// head request never has to be aborted and restarted while the decoder opens.
constexpr int64_t kTail = 64 * 1024;
constexpr DWORD kPiece = 64 * 1024;                // bytes per WinHttpReadData (publish granularity)
constexpr int64_t kRestartDistance = 128 * 1024;   // a read further ahead than this restarts the download
constexpr int kMaxRetries = 3;
constexpr DWORD kBackoffMs[kMaxRetries] = {400, 1200, 3000};

constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 "
    L"Safari/537.36 ShadeTube/0.1";

// Per-request completion state shared with the WinHTTP status callback. `done` is signalled for
// every completed asynchronous operation (send, receive, read) or error; `closed` when WinHTTP
// delivers HANDLE_CLOSING, the last callback a request handle ever produces.
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
    if (!ctx) return;  // session / connection handles carry no context
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
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(ctx->closed);
        break;
    default:
        break;
    }
}

struct InternetHandle {
    HINTERNET h = nullptr;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET v) : h(v) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    ~InternetHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string winHttpError(const char* what, DWORD code) {
    return std::format("{} failed (WinHTTP error {})", what, code);
}

// googlevideo: `&range=a-b` query parameter (what YouTube's own players use).
std::wstring withRangeParam(const std::wstring& pathQuery, int64_t first, int64_t last) {
    const auto q = pathQuery.find(L'?');
    std::wstring out = q == std::wstring::npos ? pathQuery + L"?" : pathQuery.substr(0, q + 1);
    bool any = false;
    if (q != std::wstring::npos) {
        size_t pos = q + 1;
        while (pos <= pathQuery.size()) {
            size_t amp = pathQuery.find(L'&', pos);
            if (amp == std::wstring::npos) amp = pathQuery.size();
            const std::wstring param = pathQuery.substr(pos, amp - pos);
            if (!param.empty() && param.rfind(L"range=", 0) != 0) {
                if (any) out.push_back(L'&');
                out += param;
                any = true;
            }
            pos = amp + 1;
        }
    }
    if (any) out.push_back(L'&');
    out += L"range=" + std::to_wstring(first) + L"-" + std::to_wstring(last);
    return out;
}

// `clen=` query parameter of googlevideo URLs = total content length.
int64_t contentLengthFromUrl(const std::string& url) {
    for (const char* key : {"?clen=", "&clen="}) {
        const auto p = url.find(key);
        if (p == std::string::npos) continue;
        int64_t v = 0;
        for (size_t i = p + 6; i < url.size() && url[i] >= '0' && url[i] <= '9'; ++i) v = v * 10 + (url[i] - '0');
        if (v > 0) return v;
    }
    return -1;
}

// Copies out of a mapped view: a vanished network file or an unwritable temporary file surfaces as an in-page error,
// which must fail the read, not the process.
bool guardedCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

std::wstring temporaryPath() {
    static std::atomic<uint32_t> counter{0};
    wchar_t dir[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return {};
    return std::format(L"{}ShadeTube-stream-{}-{}.tmp", dir, GetCurrentProcessId(), counter.fetch_add(1));
}

std::wstring queryHeader(HINTERNET req, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

} // namespace

struct ProgressiveBuffer::HttpState {
    InternetHandle session;
    InternetHandle connection;
    std::wstring pathQuery;
    bool secure = false;
    bool googlevideo = false;
};

// ---------------------------------------------------------------------------------------------

ProgressiveBuffer::ProgressiveBuffer(std::string url, std::wstring localPath, int64_t contentLength,
                                     std::atomic<int64_t>* memCounter)
    : url_(std::move(url)), localPath_(std::move(localPath)), memCounter_(memCounter) {
    attention_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (contentLength > 0) length_.store(contentLength);  // allocated by the thread
}

ProgressiveBuffer::~ProgressiveBuffer() {
    cancel();
    release();
    if (attention_) CloseHandle(attention_);
    if (cancelEvent_) CloseHandle(cancelEvent_);
}

void ProgressiveBuffer::start() {
    thread_ = std::thread([this] { run(); });
}

void ProgressiveBuffer::cancel() {
    cancelled_.store(true, std::memory_order_release);
    if (attention_) SetEvent(attention_);
    if (cancelEvent_) SetEvent(cancelEvent_);
    std::lock_guard lock(mutex_);
    cv_.notify_all();
}

void ProgressiveBuffer::release() {
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    if (memCounter_ && allocated_ && !mapped_) memCounter_->fetch_sub(allocated_);
    heap_.reset();
    unmap();
    data_ = nullptr;
    allocated_ = 0;
}

void ProgressiveBuffer::unmap() {
    if (mapped_ && data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    mapping_ = file_ = nullptr;
    mapped_ = false;
}

int64_t ProgressiveBuffer::waitForLength() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return allocated_ > 0 || failed_.load() || cancelled_.load(); });
    return allocated_ > 0 && !cancelled_.load() ? allocated_ : -1;
}

ProgressiveBuffer::ReadStatus ProgressiveBuffer::read(int64_t offset, void* dst, size_t size) {
    if (size == 0) return ReadStatus::Ok;
    std::unique_lock lock(mutex_);
    const uint64_t gen = interruptGen_;
    for (;;) {
        if (cancelled_.load()) return ReadStatus::Cancelled;
        const int64_t end = offset + static_cast<int64_t>(size);
        if (data_ && end <= allocated_ && availableLocked(offset, end)) {
            if (!mapped_) {
                std::memcpy(dst, data_ + offset, size);
                return ReadStatus::Ok;
            }
            if (guardedCopy(dst, data_ + offset, size)) return ReadStatus::Ok;
            error_ = "read error (file no longer readable)";
            failed_.store(true, std::memory_order_release);
            cv_.notify_all();
            return ReadStatus::Failed;
        }
        if (failed_.load()) return ReadStatus::Failed;
        if (interruptGen_ != gen) return ReadStatus::Interrupted;
        if (allocated_ > 0) {
            if (end > allocated_) return ReadStatus::Failed;  // caller must clamp to length()
            // First missing byte the main request could serve (the reserved tail is in flight).
            const int64_t missing = firstMissingLocked(offset);
            if (missing >= 0 && missing < end) {
                priority_ = missing;
                // Restart the download here unless the active request will reach it shortly.
                if (reqPos_ < 0 || missing < reqPos_ || missing - reqPos_ > kRestartDistance) {
                    if (reqPos_ >= 0) restart_ = true;
                    SetEvent(attention_);
                }
            }
        }
        waiters_.fetch_add(1);
        cv_.wait(lock);
        waiters_.fetch_sub(1);
    }
}

void ProgressiveBuffer::interruptWaiters() {
    std::lock_guard lock(mutex_);
    ++interruptGen_;
    cv_.notify_all();
}

float ProgressiveBuffer::fraction() const {
    const int64_t len = length_.load(std::memory_order_acquire);
    if (len <= 0) return 0.f;
    return std::clamp(static_cast<float>(static_cast<double>(downloaded_.load()) / static_cast<double>(len)), 0.f, 1.f);
}

std::string ProgressiveBuffer::errorMessage() const {
    std::lock_guard lock(mutex_);
    return error_;
}

// ---- range bookkeeping ------------------------------------------------------------------------

void ProgressiveBuffer::addRangeLocked(int64_t begin, int64_t end) {
    if (end <= begin) return;
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), std::pair{begin, begin});
    if (it != ranges_.begin() && std::prev(it)->second >= begin) --it;
    // Merge every range overlapping/adjacent to [begin, end).
    auto last = it;
    while (last != ranges_.end() && last->first <= end) {
        begin = std::min(begin, last->first);
        end = std::max(end, last->second);
        ++last;
    }
    it = ranges_.erase(it, last);
    ranges_.insert(it, {begin, end});
}

bool ProgressiveBuffer::availableLocked(int64_t begin, int64_t end) const {
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), std::pair{begin, INT64_MAX});
    if (it == ranges_.begin()) return false;
    --it;
    return it->first <= begin && it->second >= end;
}

int64_t ProgressiveBuffer::firstMissingLocked(int64_t from) const {
    const int64_t len = allocated_;
    for (;;) {
        if (from >= len) return -1;
        for (const auto& [b, e] : ranges_) {
            if (e <= from) continue;
            if (b > from) break;
            from = e;
        }
        if (from >= len) return -1;
        if (reservedBegin_ >= 0 && from >= reservedBegin_ && from < reservedEnd_) {
            from = reservedEnd_;  // in flight on the side request
            continue;
        }
        return from;
    }
}

int64_t ProgressiveBuffer::nextRangeStartLocked(int64_t pos) const {
    int64_t next = allocated_;
    for (const auto& [b, e] : ranges_) {
        if (b > pos) {
            next = b;
            break;
        }
    }
    if (reservedBegin_ > pos) next = std::min(next, reservedBegin_);
    return next;
}

// ---- download thread --------------------------------------------------------------------------

void ProgressiveBuffer::run() {
    SetThreadDescription(GetCurrentThread(), L"st-audio-download");
    try {
        if (!localPath_.empty()) runLocal();
        else runHttp();
    } catch (const std::exception& e) {
        fail(std::string("download: ") + e.what(), false);
    } catch (...) {
        fail("download: unknown error", false);
    }
    finished_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    cv_.notify_all();
}

bool ProgressiveBuffer::allocate(int64_t length) {
    if (length <= 0 || length > (int64_t(1) << 32)) {
        fail(std::format("invalid content length {}", length), false);
        return false;
    }
    if (length > kMapThreshold && mapTemporary(length)) return true;
    if (length > (int64_t(1) << 31)) {   // only a mapping holds more than 2 GB
        fail(std::format("content too large for memory ({} bytes)", length), false);
        return false;
    }
    std::unique_ptr<uint8_t[]> block(new (std::nothrow) uint8_t[static_cast<size_t>(length)]);
    if (!block) {
        fail("out of memory", false);
        return false;
    }
    std::lock_guard lock(mutex_);
    heap_ = std::move(block);
    data_ = heap_.get();
    allocated_ = length;
    length_.store(length, std::memory_order_release);
    if (memCounter_) memCounter_->fetch_add(length);
    cv_.notify_all();
    return true;
}

bool ProgressiveBuffer::mapTemporary(int64_t length) {
    const std::wstring path = temporaryPath();
    if (path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    size.QuadPart = length;
    HANDLE mapping = nullptr;
    void* view = nullptr;
    if (SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file)) {
        mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr);
        if (mapping) view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(length));
    }
    if (!view) {
        ST_LOG_WARN("audio", "temporary file mapping failed (error {}): buffering in memory", GetLastError());
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);   // deleted on close
        return false;
    }
    std::lock_guard lock(mutex_);
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<uint8_t*>(view);
    mapped_ = true;
    allocated_ = length;
    length_.store(length, std::memory_order_release);
    ST_LOG_INFO("audio", "long stream ({} MB): buffered in a temporary file", length >> 20);
    cv_.notify_all();
    return true;
}

bool ProgressiveBuffer::mapLocal(void* file, int64_t length) {
    if (length <= 0) return false;
    HANDLE mapping = CreateFileMappingW(static_cast<HANDLE>(file), nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!view) {
        if (mapping) CloseHandle(mapping);
        return false;
    }
    std::lock_guard lock(mutex_);
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<uint8_t*>(const_cast<void*>(view));
    mapped_ = true;
    allocated_ = length;
    length_.store(length, std::memory_order_release);
    addRangeLocked(0, length);
    downloaded_.store(length);
    cv_.notify_all();
    return true;
}

void ProgressiveBuffer::fail(std::string message, bool expired) {
    ST_LOG_WARN("audio", "buffer failed: {}", message);
    std::lock_guard lock(mutex_);
    error_ = std::move(message);
    expired_.store(expired);
    failed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool ProgressiveBuffer::backoff(int attempt) {
    const ULONGLONG deadline = GetTickCount64() + kBackoffMs[std::clamp(attempt - 1, 0, kMaxRetries - 1)];
    while (!cancelled_.load()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return true;
        WaitForSingleObject(attention_, static_cast<DWORD>(deadline - now));
    }
    return false;
}

void ProgressiveBuffer::runLocal() {
    // Shared for deletion too: a synced or downloaded file may be removed while it plays (the mapping stays valid).
    HANDLE file = CreateFileW(localPath_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail("cannot open " + toUtf8(localPath_), false);
        return;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    // The file cache holds the pages: no copy, no private memory (and instant availability).
    if (mapLocal(file, size.QuadPart)) return;
    if (allocate(size.QuadPart)) {
        int64_t pos = 0;
        while (pos < size.QuadPart && !cancelled_.load()) {
            const DWORD want = static_cast<DWORD>(std::min<int64_t>(kChunk, size.QuadPart - pos));
            DWORD got = 0;
            if (!ReadFile(file, data_ + pos, want, &got, nullptr) || got == 0) {
                fail("read error: " + toUtf8(localPath_), false);
                break;
            }
            {
                std::lock_guard lock(mutex_);
                addRangeLocked(pos, pos + got);
            }
            pos += got;
            downloaded_.store(pos);
            cv_.notify_all();
        }
    }
    CloseHandle(file);
}

void ProgressiveBuffer::runHttp() {
    HttpState http;
    const std::wstring wideUrl = toWide(url_);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        fail("invalid URL", true);
        return;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    http.pathQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) http.pathQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (http.pathQuery.empty()) http.pathQuery = L"/";
    http.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    http.googlevideo = host.find(L"googlevideo.com") != std::wstring::npos;

    http.session.h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!http.session.h)  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+; fall back
        http.session.h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!http.session.h) {
        fail(winHttpError("WinHttpOpen", GetLastError()), false);
        return;
    }
    DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(http.session.h, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof http2);
    WinHttpSetTimeouts(http.session.h, 10000, 10000, 10000, 15000);
    if (WinHttpSetStatusCallback(http.session.h, onWinHttpStatus,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        fail(winHttpError("WinHttpSetStatusCallback", GetLastError()), false);
        return;
    }
    http.connection.h = WinHttpConnect(http.session.h, host.c_str(), parts.nPort, 0);
    if (!http.connection.h) {
        fail(winHttpError("WinHttpConnect", GetLastError()), false);
        return;
    }

    // Length known up front (manifest) or from googlevideo's clen= parameter; else the first
    // request probes it (Content-Range / Content-Length).
    int64_t known = length_.load();
    if (known <= 0) known = contentLengthFromUrl(url_);
    if (known > 0 && !allocate(known)) return;

    // Parallel tail request (see kTail). Joined before this function returns.
    std::thread side;
    struct JoinSide {
        std::thread& t;
        ~JoinSide() {
            if (t.joinable()) t.join();
        }
    } joinSide{side};
    {
        std::lock_guard lock(mutex_);
        if (allocated_ > 4 * kTail) {
            reservedBegin_ = allocated_ - kTail;
            reservedEnd_ = allocated_;
        }
    }
    if (reservedBegin_ >= 0) {
        side = std::thread([this, &http, begin = reservedBegin_, end = reservedEnd_] {
            int64_t got = 0;
            std::string error;
            for (int attempt = 0; attempt < 2 && !cancelled_.load(); ++attempt)
                if (fetch(http, begin, end, true, got, error) != Fetch::Retry) break;
            std::lock_guard lock(mutex_);
            reservedBegin_ = reservedEnd_ = -1;  // anything still missing is fetched by the main loop
            SetEvent(attention_);
        });
    }

    int attempt = 0;
    while (!cancelled_.load()) {
        int64_t start = 0, end = -1;
        bool waitForSide = false;
        {
            std::lock_guard lock(mutex_);
            if (allocated_ > 0) {
                start = firstMissingLocked(priority_);
                if (start < 0) start = firstMissingLocked(0);
                if (start < 0) {
                    if (reservedBegin_ < 0) break;  // complete
                    waitForSide = true;             // only the in-flight tail is left
                } else {
                    end = std::min(start + kChunk, nextRangeStartLocked(start));
                }
            }
            restart_ = false;
            reqPos_ = waitForSide ? -1 : start;
        }
        if (waitForSide) {
            WaitForSingleObject(attention_, 200);
            continue;
        }
        int64_t got = 0;
        std::string error;
        const Fetch result = fetch(http, start, end, false, got, error);
        {
            std::lock_guard lock(mutex_);
            reqPos_ = -1;
        }
        switch (result) {
        case Fetch::Ok:
        case Fetch::Restart:
            attempt = 0;
            break;
        case Fetch::Cancelled:
            return;
        case Fetch::Fatal:
            fail(error, expired_.load());
            return;
        case Fetch::Retry:
            if (got > 0) attempt = 0;
            if (++attempt > kMaxRetries) {
                fail(error, false);
                return;
            }
            ST_LOG_WARN("audio", "range {}-{} failed ({}), retry {}/{}", start, end, error, attempt, kMaxRetries);
            if (!backoff(attempt)) return;
            break;
        }
    }
}

int ProgressiveBuffer::waitOp(void* doneEvent, bool side) {
    // The side request must not consume the main loop's auto-reset attention event.
    HANDLE handles[2] = {static_cast<HANDLE>(doneEvent), static_cast<HANDLE>(side ? cancelEvent_ : attention_)};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) return 0;
        if (cancelled_.load()) return 1;
        if (side) continue;
        std::lock_guard lock(mutex_);
        if (restart_) return 2;
    }
}

ProgressiveBuffer::Fetch ProgressiveBuffer::fetch(HttpState& http, int64_t start, int64_t end, bool side,
                                                  int64_t& got, std::string& error) {
    got = 0;
    const bool probe = end < 0;
    const bool useParam = http.googlevideo && !probe;
    const std::wstring path = useParam ? withRangeParam(http.pathQuery, start, end - 1) : http.pathQuery;
    std::wstring headers;
    if (!useParam) {
        const int64_t last = probe ? start + kChunk - 1 : end - 1;
        headers = L"Range: bytes=" + std::to_wstring(start) + L"-" + std::to_wstring(last) + L"\r\n";
    }

    RequestContext ctx;
    HINTERNET req = WinHttpOpenRequest(http.connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, http.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        error = winHttpError("WinHttpOpenRequest", GetLastError());
        return Fetch::Retry;
    }
    auto ctxValue = reinterpret_cast<DWORD_PTR>(&ctx);
    WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &ctxValue, sizeof ctxValue);
    // Closing is the only way to abort an asynchronous request. HANDLE_CLOSING is the last callback,
    // after which WinHTTP no longer writes into our buffer nor touches `ctx`.
    auto close = [&] {
        WinHttpCloseHandle(req);
        WaitForSingleObject(ctx.closed, 10000);
    };
    auto finishWait = [&](const char* what) -> Fetch {
        switch (waitOp(ctx.done, side)) {
        case 1: close(); return Fetch::Cancelled;
        case 2: close(); return Fetch::Restart;
        default: break;
        }
        if (ctx.error) {
            error = winHttpError(what, ctx.error);
            close();
            return Fetch::Retry;
        }
        return Fetch::Ok;
    };

    if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, ctxValue)) {
        error = winHttpError("WinHttpSendRequest", GetLastError());
        close();
        return Fetch::Retry;
    }
    if (auto r = finishWait("send"); r != Fetch::Ok) return r;
    if (!WinHttpReceiveResponse(req, nullptr)) {
        error = winHttpError("WinHttpReceiveResponse", GetLastError());
        close();
        return Fetch::Retry;
    }
    if (auto r = finishWait("receive"); r != Fetch::Ok) return r;

    DWORD status = 0, statusSize = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status == 403 || status == 404 || status == 410) {
        error = std::format("HTTP {} (stream URL expired?)", status);
        expired_.store(true);
        close();
        return Fetch::Fatal;
    }
    if (status != 200 && status != 206) {
        error = std::format("HTTP {}", status);
        close();
        return status == 416 ? Fetch::Fatal : Fetch::Retry;
    }
    const bool wholeBody = status == 200 && !useParam;  // server ignored the Range header
    if (wholeBody && start > 0) {
        error = "server does not support range requests";
        close();
        return Fetch::Fatal;
    }
    if (allocated_ <= 0) {  // probe: learn the total length (only this thread writes allocated_)
        int64_t total = -1;
        const std::wstring range = queryHeader(req, WINHTTP_QUERY_CONTENT_RANGE);  // "bytes 0-1023/12345"
        if (const auto slash = range.rfind(L'/'); status == 206 && slash != std::wstring::npos)
            total = _wtoi64(range.c_str() + slash + 1);
        if (total <= 0) total = _wtoi64(queryHeader(req, WINHTTP_QUERY_CONTENT_LENGTH).c_str());
        if (total <= 0) {
            error = "unknown content length";
            close();
            return Fetch::Fatal;
        }
        if (!allocate(total)) {
            close();
            return Fetch::Cancelled;  // fail() already recorded the reason
        }
    }
    const int64_t limit = wholeBody ? allocated_ : std::min(probe ? start + kChunk : end, allocated_);

    Fetch result = Fetch::Ok;
    int64_t pos = start;
    for (;;) {
        int64_t stop;
        {
            std::lock_guard lock(mutex_);
            if (!side && restart_) {
                result = Fetch::Restart;
                break;
            }
            // Never overwrite published bytes (readers may be copying them right now).
            stop = std::min(limit, nextRangeStartLocked(pos));
        }
        if (pos >= stop) break;
        const DWORD want = static_cast<DWORD>(std::min<int64_t>(kPiece, stop - pos));
        ctx.bytes = 0;
        if (!WinHttpReadData(req, data_ + pos, want, nullptr)) {
            error = winHttpError("WinHttpReadData", GetLastError());
            result = Fetch::Retry;
            break;
        }
        if (auto r = finishWait("read"); r != Fetch::Ok) return r;  // closed by finishWait
        if (ctx.bytes == 0) {
            if (pos < stop) {
                error = "connection closed early";
                result = Fetch::Retry;
            }
            break;
        }
        {
            std::lock_guard lock(mutex_);
            addRangeLocked(pos, pos + ctx.bytes);
            if (!side) reqPos_ = pos + ctx.bytes;
        }
        pos += ctx.bytes;
        got += ctx.bytes;
        downloaded_.fetch_add(ctx.bytes);
        cv_.notify_all();
        if (cancelled_.load()) {
            result = Fetch::Cancelled;
            break;
        }
    }
    close();
    return result;
}

} // namespace st::audio
