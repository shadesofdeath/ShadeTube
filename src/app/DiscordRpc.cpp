#include "app/DiscordRpc.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace st::app {

using json = nlohmann::json;

namespace {

constexpr const char* kTag = "discord";
constexpr int32_t kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3, kOpPong = 4;
constexpr int64_t kReplyTimeoutMs = 5'000;
constexpr uint32_t kMaxFrameBytes = 64 * 1024;
constexpr int kFirstBackoffMs = 5'000, kMaxBackoffMs = 60'000;
constexpr size_t kRateCount = 5;            // Discord: 5 SET_ACTIVITY per 20 s
constexpr int64_t kRateWindowMs = 20'000;
constexpr int64_t kSameTimestampSlackMs = 2'000;

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string dumpJson(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string jstr(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// Discord validates text fields as 2..128 UTF-16 units: cut long strings (with an ellipsis), pad 1-char ones.
std::string fitText(const std::string& s) {
    size_t i = 0, units = 0, cut = std::string::npos;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        len = std::min(len, s.size() - i);
        const size_t u = len == 4 ? 2 : 1;
        if (cut == std::string::npos && units + u > 127) cut = i;
        units += u;
        i += len;
    }
    if (units > 128) return s.substr(0, cut) + "\xE2\x80\xA6";   // "…"
    if (units == 1) return s + "\xE2\x80\x8B";                     // + zero-width space
    return s;
}

struct Activity {
    std::string title, artist, album, image;
    int64_t start = 0, end = 0;
};

bool nearlySame(const Activity& a, const Activity& b) {
    auto close = [](int64_t x, int64_t y) { return (x == 0) == (y == 0) && std::llabs(x - y) <= kSameTimestampSlackMs; };
    return a.title == b.title && a.artist == b.artist && a.album == b.album && a.image == b.image && close(a.start, b.start) &&
           close(a.end, b.end);
}

json buildActivity(const Activity& a) {
    json act = {{"type", 2}, {"details", fitText(a.title)}};   // 2 = "Listening to <app name>"
    if (!a.artist.empty()) act["state"] = fitText(a.artist);
    if (a.start > 0) {
        json ts = {{"start", a.start}};
        if (a.end > a.start) ts["end"] = a.end;
        act["timestamps"] = std::move(ts);
    }
    // Discord accepts an https URL as the asset (proxied); asset keys are limited to 256 characters.
    if (a.image.rfind("https://", 0) == 0 && a.image.size() <= 256) {
        json assets = {{"large_image", a.image}};
        if (!a.album.empty()) assets["large_text"] = fitText(a.album);
        act["assets"] = std::move(assets);
    }
    return act;
}

// "ERROR" event payload: {evt:"ERROR", data:{code, message}} -> "code: message".
std::string errorMessage(const json& j) {
    if (!j.is_object()) return {};
    const auto d = j.find("data");
    if (d == j.end() || !d->is_object()) return {};
    int code = 0;
    if (const auto c = d->find("code"); c != d->end() && c->is_number_integer()) code = c->get<int>();
    return std::to_string(code) + ": " + jstr(*d, "message");
}

struct Frame {
    int32_t op = -1;
    std::string payload;
};

} // namespace

// ============================================================================================================

struct DiscordRpc::Impl {
    explicit Impl(std::wstring pipeBase) : base(std::move(pipeBase)) {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl() {
        {
            std::lock_guard lk(m);
            stop = true;
        }
        if (stopEvent) SetEvent(stopEvent);
        cv.notify_all();
        if (thread.joinable()) thread.join();
        closePipe();
        if (ioEvent) CloseHandle(ioEvent);
        if (stopEvent) CloseHandle(stopEvent);
    }

    // ---- shared state (guarded by m) -------------------------------------------------------------------
    mutable std::mutex m;
    mutable std::condition_variable cv;
    std::string appId;
    std::optional<Activity> activity;
    uint64_t version = 0;      // bumped on every wanted-state change
    uint64_t handled = 0;      // last version the worker fully applied
    bool backingOff = false;   // Discord unreachable; waiting for the next attempt
    bool stop = false;
    bool started = false;
    Stats stats;

    // ---- worker-only state -------------------------------------------------------------------------------
    std::wstring base;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE ioEvent = nullptr;
    std::string connectedAppId;
    std::string lastAttemptAppId;
    bool remoteHasActivity = false;
    bool loggedUnavailable = false;
    std::string lastWarn;      // last handshake failure logged at WARN (repeats go to DEBUG)
    int backoffMs = 0;
    int64_t nextAttemptAt = 0;
    std::deque<int64_t> sendTimes;
    uint64_t nonce = 0;
    std::thread thread;

    // Caller holds m.
    void ensureThreadLocked() {
        if (started || stop || appId.empty() || !stopEvent || !ioEvent) return;
        started = true;
        thread = std::thread([this] { run(); });
    }

    void markHandled(uint64_t ver) {
        {
            std::lock_guard lk(m);
            handled = std::max(handled, ver);
        }
        cv.notify_all();
    }

    void setBackingOff(bool on) {
        {
            std::lock_guard lk(m);
            backingOff = on;
        }
        cv.notify_all();
    }

    // Sleeps up to ms; wakes early on stop or when the wanted state moves past `ver`.
    void sleepFor(int64_t ms, uint64_t ver) {
        std::unique_lock lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(std::max<int64_t>(ms, 1)), [&] { return stop || version != ver; });
    }

    bool connected() const { return pipe != INVALID_HANDLE_VALUE; }

    void closePipe() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }

    void disconnect() {
        const bool was = connected();
        closePipe();
        connectedAppId.clear();
        remoteHasActivity = false;   // Discord drops a presence when its connection closes
        if (was) {
            std::lock_guard lk(m);
            stats.connected = false;
        }
    }

    // ---- overlapped pipe I/O (every wait also watches stopEvent) -----------------------------------------
    bool io(bool write, char* buf, DWORD n, int64_t deadline) {
        DWORD done = 0;
        while (done < n) {
            OVERLAPPED ov{};
            ov.hEvent = ioEvent;
            ResetEvent(ioEvent);
            const BOOL ok = write ? WriteFile(pipe, buf + done, n - done, nullptr, &ov) : ReadFile(pipe, buf + done, n - done, nullptr, &ov);
            if (!ok) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    const int64_t left = deadline - steadyMs();
                    const HANDLE waits[2] = {ioEvent, stopEvent};
                    const DWORD w = left > 0 ? WaitForMultipleObjects(2, waits, FALSE, static_cast<DWORD>(left)) : WAIT_TIMEOUT;
                    if (w != WAIT_OBJECT_0) {
                        CancelIoEx(pipe, &ov);
                        DWORD ignored = 0;
                        GetOverlappedResult(pipe, &ov, &ignored, TRUE);   // wait for the cancel: ov lives on our stack
                        return false;
                    }
                } else if (err != ERROR_MORE_DATA) {
                    return false;   // broken pipe / no data: Discord went away
                }
            }
            DWORD got = 0;
            if (!GetOverlappedResult(pipe, &ov, &got, FALSE) && GetLastError() != ERROR_MORE_DATA) return false;
            if (got == 0) return false;
            done += got;
        }
        return true;
    }

    bool writeFrame(int32_t op, const std::string& payload) {
        if (!connected() || payload.size() > kMaxFrameBytes) return false;
        std::string buf(8 + payload.size(), '\0');
        const auto len = static_cast<int32_t>(payload.size());
        std::memcpy(buf.data(), &op, 4);   // x64 is little endian, as the protocol wants
        std::memcpy(buf.data() + 4, &len, 4);
        std::memcpy(buf.data() + 8, payload.data(), payload.size());
        return io(true, buf.data(), static_cast<DWORD>(buf.size()), steadyMs() + kReplyTimeoutMs);
    }

    bool readFrame(Frame& f, int64_t deadline) {
        if (!connected()) return false;
        char header[8];
        if (!io(false, header, 8, deadline)) return false;
        int32_t len = 0;
        std::memcpy(&f.op, header, 4);
        std::memcpy(&len, header + 4, 4);
        if (len < 0 || static_cast<uint32_t>(len) > kMaxFrameBytes) return false;
        f.payload.assign(static_cast<size_t>(len), '\0');
        return len == 0 || io(false, f.payload.data(), static_cast<DWORD>(len), deadline);
    }

    static std::string closeText(const Frame& f) {
        const json j = json::parse(f.payload, nullptr, false);
        int code = 0;
        if (j.is_object())
            if (const auto c = j.find("code"); c != j.end() && c->is_number_integer()) code = c->get<int>();
        return "Discord closed the connection (code " + std::to_string(code) + "): " + jstr(j, "message");
    }

    void logClose(const Frame& f) { ST_LOG_WARN(kTag, "{}", closeText(f)); }

    // Handshake failures repeat every backoff cycle (e.g. a wrong application id): WARN once, then DEBUG.
    void warnOnce(const std::string& msg) {
        if (msg == lastWarn) {
            ST_LOG_DEBUG(kTag, "{}", msg);
            return;
        }
        lastWarn = msg;
        ST_LOG_WARN(kTag, "{}", msg);
    }

    // ---- protocol ------------------------------------------------------------------------------------------
    bool connect(const std::string& id) {
        {
            std::lock_guard lk(m);
            ++stats.connectAttempts;
        }
        for (int i = 0; i < 10 && !connected(); ++i) {
            const std::wstring name = base + std::to_wstring(i);
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        }
        if (!connected()) {
            if (!loggedUnavailable) ST_LOG_DEBUG(kTag, "Discord is not running (no IPC pipe); will retry quietly");
            loggedUnavailable = true;
            return false;
        }
        if (!writeFrame(kOpHandshake, dumpJson(json{{"v", 1}, {"client_id", id}}))) {
            disconnect();
            return false;
        }
        const int64_t deadline = steadyMs() + kReplyTimeoutMs;
        for (;;) {
            Frame f;
            if (!readFrame(f, deadline)) {
                warnOnce("handshake: no reply from Discord");
                disconnect();
                return false;
            }
            if (f.op == kOpPing) {
                writeFrame(kOpPong, f.payload);
                continue;
            }
            if (f.op == kOpClose) {   // e.g. 4000 "Invalid Client ID"
                warnOnce(closeText(f));
                disconnect();
                return false;
            }
            if (f.op != kOpFrame) continue;
            const json j = json::parse(f.payload, nullptr, false);
            const std::string evt = jstr(j, "evt");
            if (evt == "READY") break;
            if (evt == "ERROR") {
                warnOnce("handshake rejected: " + errorMessage(j));
                disconnect();
                return false;
            }
        }
        connectedAppId = id;
        loggedUnavailable = false;
        lastWarn.clear();
        {
            std::lock_guard lk(m);
            stats.connected = true;
        }
        ST_LOG_INFO(kTag, "connected to Discord");
        return true;
    }

    int64_t throttleWaitMs() {
        const int64_t now = steadyMs();
        while (!sendTimes.empty() && now - sendTimes.front() >= kRateWindowMs) sendTimes.pop_front();
        if (sendTimes.size() < kRateCount) return 0;
        return sendTimes.front() + kRateWindowMs - now;
    }

    bool send(const std::optional<Activity>& act) {
        const std::string n = "st-" + std::to_string(++nonce);
        json args = {{"pid", static_cast<int64_t>(GetCurrentProcessId())}};
        args["activity"] = act ? buildActivity(*act) : json(nullptr);
        const json cmd = {{"cmd", "SET_ACTIVITY"}, {"args", std::move(args)}, {"nonce", n}};
        if (!writeFrame(kOpFrame, dumpJson(cmd))) return false;
        sendTimes.push_back(steadyMs());
        {
            std::lock_guard lk(m);
            ++stats.framesSent;
        }
        const int64_t deadline = steadyMs() + kReplyTimeoutMs;
        for (;;) {
            Frame f;
            if (!readFrame(f, deadline)) return false;   // no reply: stream state unknown -> reconnect
            if (f.op == kOpPing) {
                if (!writeFrame(kOpPong, f.payload)) return false;
                continue;
            }
            if (f.op == kOpClose) {
                logClose(f);
                return false;
            }
            if (f.op != kOpFrame) continue;
            const json j = json::parse(f.payload, nullptr, false);
            if (jstr(j, "nonce") != n) continue;   // a stale reply / event
            if (jstr(j, "evt") == "ERROR") ST_LOG_WARN(kTag, "SET_ACTIVITY rejected: {}", errorMessage(j));
            return true;   // applied (or rejected for good: don't loop on it)
        }
    }

    // Idle while connected: answer pings, notice Discord closing/quitting.
    void service() {
        if (!connected()) return;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
                ST_LOG_INFO(kTag, "Discord connection lost");
                lostConnection();
                return;
            }
            if (avail < 8) return;
            Frame f;
            if (!readFrame(f, steadyMs() + 1'000)) {
                lostConnection();
                return;
            }
            if (f.op == kOpPing) {
                if (!writeFrame(kOpPong, f.payload)) return lostConnection();
            } else if (f.op == kOpClose) {
                logClose(f);
                return lostConnection();
            }
        }
    }

    // The presence vanished with the connection: re-apply the wanted state once Discord is back.
    void lostConnection() {
        disconnect();
        backoffMs = 0;
        nextAttemptAt = steadyMs() + 3'000;
        std::lock_guard lk(m);
        if (activity) handled = 0;
    }

    void step(const std::string& id, const std::optional<Activity>& act, uint64_t ver) {
        if (id.empty()) {
            disconnect();
            setBackingOff(false);
            markHandled(ver);
            return;
        }
        if (connected() && connectedAppId != id) disconnect();
        if (!act && !remoteHasActivity) {   // nothing on screen to clear: stay disconnected (lazy)
            markHandled(ver);
            return;
        }
        if (!connected()) {
            if (id != lastAttemptAppId) {   // a new id deserves an immediate attempt
                lastAttemptAppId = id;
                backoffMs = 0;
                nextAttemptAt = 0;
                loggedUnavailable = false;
                lastWarn.clear();
            }
            const int64_t now = steadyMs();
            if (now < nextAttemptAt) {
                sleepFor(nextAttemptAt - now, ver);
                return;
            }
            if (!connect(id)) {
                backoffMs = backoffMs ? std::min(backoffMs * 2, kMaxBackoffMs) : kFirstBackoffMs;
                nextAttemptAt = steadyMs() + backoffMs;
                setBackingOff(true);
                return;
            }
            backoffMs = 0;
            setBackingOff(false);
        }
        if (const int64_t wait = throttleWaitMs(); wait > 0) {
            sleepFor(wait, ver);
            return;
        }
        if (send(act)) {
            remoteHasActivity = act.has_value();
            markHandled(ver);
        } else {
            disconnect();
            nextAttemptAt = steadyMs() + 2'000;
        }
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"st-discord");
        for (;;) {
            std::string id;
            std::optional<Activity> act;
            uint64_t ver = 0;
            {
                std::unique_lock lk(m);
                const auto pending = [&] { return stop || version != handled; };
                if (!pending()) {
                    if (connected()) cv.wait_for(lk, std::chrono::seconds(5), pending);
                    else cv.wait(lk, pending);
                }
                if (stop) break;
                if (version == handled) {   // idle tick while connected
                    lk.unlock();
                    try {
                        service();
                    } catch (...) {
                    }
                    continue;
                }
                id = appId;
                act = activity;
                ver = version;
            }
            try {
                step(id, act, ver);
            } catch (const std::exception& e) {   // never let the thread die (e.g. std::bad_alloc)
                ST_LOG_WARN(kTag, "worker: {}", e.what());
                disconnect();
                markHandled(ver);
            }
        }
        disconnect();
    }
};

// ============================================================================================================

DiscordRpc::DiscordRpc() : DiscordRpc(L"\\\\.\\pipe\\discord-ipc-") {}

DiscordRpc::DiscordRpc(std::wstring pipeBaseName) : impl_(std::make_unique<Impl>(std::move(pipeBaseName))) {}

DiscordRpc::~DiscordRpc() = default;

void DiscordRpc::setAppId(std::string clientId) {
    try {
        while (!clientId.empty() && std::isspace(static_cast<unsigned char>(clientId.back()))) clientId.pop_back();
        while (!clientId.empty() && std::isspace(static_cast<unsigned char>(clientId.front()))) clientId.erase(clientId.begin());
        {
            std::lock_guard lk(impl_->m);
            if (clientId == impl_->appId) return;
            impl_->appId = std::move(clientId);
            ++impl_->version;
            impl_->backingOff = false;
            impl_->ensureThreadLocked();
        }
        impl_->cv.notify_all();
    } catch (...) {
    }
}

void DiscordRpc::setActivity(const std::string& title, const std::string& artist, const std::string& album,
                             const std::string& imageUrl, int64_t startUnixMs, int64_t endUnixMs) {
    if (title.empty()) return clear();
    try {
        Activity a{title, artist, album, imageUrl, std::max<int64_t>(0, startUnixMs), std::max<int64_t>(0, endUnixMs)};
        {
            std::lock_guard lk(impl_->m);
            if (impl_->activity && nearlySame(*impl_->activity, a)) return;
            impl_->activity = std::move(a);
            ++impl_->version;
            impl_->ensureThreadLocked();
        }
        impl_->cv.notify_all();
    } catch (...) {
    }
}

void DiscordRpc::clear() {
    {
        std::lock_guard lk(impl_->m);
        if (!impl_->activity) return;
        impl_->activity.reset();
        ++impl_->version;
    }
    impl_->cv.notify_all();
}

DiscordRpc::Stats DiscordRpc::stats() const {
    std::lock_guard lk(impl_->m);
    return impl_->stats;
}

bool DiscordRpc::waitIdle(int timeoutMs) const {
    std::unique_lock lk(impl_->m);
    return impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
        return !impl_->started || impl_->stop || impl_->handled == impl_->version || impl_->backingOff;
    });
}

} // namespace st::app
