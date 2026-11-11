#include "app/RemoteServer.h"

#include "core/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

namespace st::app::remote {

namespace {

constexpr const char* kTag = "remote";

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

SOCKET sock(uintptr_t s) { return static_cast<SOCKET>(s); }

bool setNonBlocking(SOCKET s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool privateAddress(const std::string& ip) {
    unsigned a = 0, b = 0;
    if (std::sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return false;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

bool cgnatAddress(const std::string& ip) {   // 100.64.0.0/10: Tailscale and carrier NAT
    unsigned a = 0, b = 0;
    if (std::sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return false;
    return a == 100 && b >= 64 && b <= 127;
}

} // namespace

// ---- Adapter addresses ----------------------------------------------------------------------------------------------

void rankAddresses(std::vector<LanAddress>& list) {
    static const wchar_t* kVirtualWords[] = {L"virtual", L"vmware", L"virtualbox", L"hyper-v", L"vethernet", L"wsl",
                                             L"docker", L"vpn", L"tap-", L"tap adapter", L"wireguard", L"zerotier",
                                             L"tailscale", L"hamachi", L"npcap", L"loopback", L"pseudo", L"teredo",
                                             L"bluetooth", L"vbox", L"parallels", L"openvpn", L"fortinet", L"cisco anyconnect"};
    for (auto& a : list) {
        a.kind = a.ifType == IF_TYPE_IEEE80211 ? LanAddress::Kind::WiFi
                 : a.ifType == IF_TYPE_ETHERNET_CSMACD ? LanAddress::Kind::Ethernet
                                                       : LanAddress::Kind::Other;
        const std::wstring names = lowerW(a.adapter + L" " + a.description);
        a.isVirtual = a.ifType == IF_TYPE_PROP_VIRTUAL || a.ifType == IF_TYPE_TUNNEL || a.ifType == IF_TYPE_PPP;
        for (const wchar_t* w : kVirtualWords)
            if (names.find(w) != std::wstring::npos) a.isVirtual = true;
        int s = 0;
        if (a.kind == LanAddress::Kind::WiFi) s += 30;
        if (a.kind == LanAddress::Kind::Ethernet) s += 25;
        if (a.gateway) s += 40;
        if (privateAddress(a.ip)) s += 10;
        if (cgnatAddress(a.ip)) s -= 20;
        if (a.isVirtual) s -= 60;
        a.score = s;
    }
    std::stable_sort(list.begin(), list.end(), [](const LanAddress& x, const LanAddress& y) { return x.score > y.score; });
}

std::vector<LanAddress> lanAddresses() {
    std::vector<LanAddress> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                      GAA_FLAG_INCLUDE_GATEWAYS,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) {
        ST_LOG_WARN(kTag, "GetAdaptersAddresses failed: {}", rc);
        return out;
    }
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) continue;
            const std::string s = ip;
            if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0 || s == "0.0.0.0") continue;
            LanAddress l;
            l.ip = s;
            l.adapter = a->FriendlyName ? a->FriendlyName : L"";
            l.description = a->Description ? a->Description : L"";
            l.ifType = a->IfType;
            l.gateway = a->FirstGatewayAddress != nullptr;
            out.push_back(std::move(l));
        }
    }
    rankAddresses(out);
    return out;
}

// ---- Server ---------------------------------------------------------------------------------------------------------

struct Server::Shared {
    std::mutex mu;
    bool alive = true;
    SOCKET wake = INVALID_SOCKET;
    sockaddr_in wakeAddr{};
    std::vector<std::pair<uint64_t, Response>> replies;
    std::vector<std::pair<std::string, std::string>> events;   // pending broadcasts, one per name (newest data)
    std::vector<std::string> drops;
    std::vector<std::pair<std::string, std::string>> latest;   // newest data per event name
    std::vector<std::string> streamDevices;

    void poke() {   // caller holds no lock; sending to a closed socket just fails
        const char b = 1;
        sendto(wake, &b, 1, 0, reinterpret_cast<const sockaddr*>(&wakeAddr), sizeof wakeAddr);
    }
};

struct Server::Conn {
    SOCKET s = INVALID_SOCKET;
    uint64_t id = 0;
    std::string peer;
    std::string in;
    std::string out;
    size_t outPos = 0;
    int64_t lastActivity = 0;
    int64_t requestStart = 0;    // first byte of an unfinished request, 0 = none
    bool awaiting = false;       // an async API answer is pending
    int64_t awaitSince = 0;
    bool awaitHead = false, awaitKeepAlive = false;
    bool stream = false;
    std::string deviceId;        // stream / last authenticated device (dropDevice)
    int64_t lastSend = 0;
    bool closeAfterWrite = false;
    bool dead = false;
    size_t pending() const { return out.size() - outPos; }
};

Server::Server(Router& router) : router_(router) {}

Server::~Server() { stop(); }

bool Server::start(const Options& options, std::string* error, bool* inUse) {
    stop();
    opt_ = options;
    if (inUse) *inUse = false;
    auto failWith = [&](std::string why) {
        if (error) *error = why;
        ST_LOG_WARN(kTag, "{}", why);
        if (listen_ != ~uintptr_t(0)) closesocket(sock(listen_));
        if (wake_ != ~uintptr_t(0)) closesocket(sock(wake_));
        listen_ = wake_ = ~uintptr_t(0);
        if (wsa_) WSACleanup();
        wsa_ = false;
        return false;
    };
    WSADATA wsa{};
    if (const int rc = WSAStartup(MAKEWORD(2, 2), &wsa); rc != 0) return failWith("WSAStartup failed: " + std::to_string(rc));
    wsa_ = true;

    in_addr bindAddr{};
    if (inet_pton(AF_INET, opt_.bindAddress.c_str(), &bindAddr) != 1) return failWith("bad bind address " + opt_.bindAddress);

    // The listener: the first free port from opt_.port on.
    SOCKET ls = INVALID_SOCKET;
    int lastErr = 0;
    uint16_t bound = 0;
    for (int i = 0; i < std::max(1, opt_.portAttempts) && ls == INVALID_SOCKET; ++i) {
        const int port = opt_.port + i;
        if (port > 65535) break;
        SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET) return failWith("socket() failed: " + std::to_string(WSAGetLastError()));
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr = bindAddr;
        addr.sin_port = htons(static_cast<u_short>(port));
        if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 && listen(s, SOMAXCONN) == 0 && setNonBlocking(s)) {
            ls = s;
            bound = static_cast<uint16_t>(port);
        } else {
            lastErr = WSAGetLastError();
            closesocket(s);
            if (lastErr != WSAEADDRINUSE && lastErr != WSAEACCES) break;   // another error: no point in trying on
        }
    }
    if (ls == INVALID_SOCKET) {
        const bool taken = lastErr == WSAEADDRINUSE || lastErr == WSAEACCES;
        if (inUse) *inUse = taken;
        return failWith(taken ? "ports " + std::to_string(opt_.port) + "+" + std::to_string(opt_.portAttempts - 1) + " are in use"
                              : "bind failed: " + std::to_string(lastErr));
    }
    listen_ = static_cast<uintptr_t>(ls);

    // The wake-up socket: datagrams to itself on 127.0.0.1.
    SOCKET ws = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (ws == INVALID_SOCKET) return failWith("wake socket failed: " + std::to_string(WSAGetLastError()));
    wake_ = static_cast<uintptr_t>(ws);
    sockaddr_in wa{};
    wa.sin_family = AF_INET;
    wa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int waLen = sizeof wa;
    if (bind(ws, reinterpret_cast<const sockaddr*>(&wa), sizeof wa) != 0 ||
        getsockname(ws, reinterpret_cast<sockaddr*>(&wa), &waLen) != 0 || !setNonBlocking(ws))
        return failWith("wake socket bind failed: " + std::to_string(WSAGetLastError()));

    shared_ = std::make_shared<Shared>();
    shared_->wake = ws;
    shared_->wakeAddr = wa;
    port_ = bound;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    ST_LOG_INFO(kTag, "listening on {}:{}", opt_.bindAddress, bound);
    return true;
}

void Server::stop() {
    if (!thread_.joinable()) return;
    running_ = false;
    if (shared_) shared_->poke();
    thread_.join();
    {
        std::lock_guard lock(shared_->mu);
        shared_->alive = false;
        shared_->replies.clear();
        shared_->events.clear();
        shared_->streamDevices.clear();
    }
    for (auto& c : conns_)
        if (c->s != INVALID_SOCKET) closesocket(c->s);
    conns_.clear();
    closesocket(sock(listen_));
    closesocket(sock(wake_));
    listen_ = wake_ = ~uintptr_t(0);
    shared_->wake = INVALID_SOCKET;
    port_ = 0;
    streams_ = 0;
    if (wsa_) WSACleanup();
    wsa_ = false;
    ST_LOG_INFO(kTag, "stopped");
}

void Server::broadcast(const std::string& event, std::string data) {
    auto s = shared_;
    if (!s) return;
    {
        std::lock_guard lock(s->mu);
        if (!s->alive) return;
        if (event != "pos") {
            auto it = std::find_if(s->latest.begin(), s->latest.end(), [&](const auto& e) { return e.first == event; });
            if (it == s->latest.end()) s->latest.emplace_back(event, data);
            else it->second = data;
        }
        auto it = std::find_if(s->events.begin(), s->events.end(), [&](const auto& e) { return e.first == event; });
        if (it == s->events.end()) s->events.emplace_back(event, std::move(data));
        else it->second = std::move(data);
    }
    s->poke();
}

std::string Server::latest(const std::string& event) const {
    auto s = shared_;
    if (!s) return {};
    std::lock_guard lock(s->mu);
    for (const auto& [name, data] : s->latest)
        if (name == event) return data;
    return {};
}

void Server::dropDevice(const std::string& deviceId) {
    auto s = shared_;
    if (!s) return;
    {
        std::lock_guard lock(s->mu);
        if (!s->alive) return;
        s->drops.push_back(deviceId);
    }
    s->poke();
}

std::vector<std::string> Server::streamDevices() const {
    auto s = shared_;
    if (!s) return {};
    std::lock_guard lock(s->mu);
    return s->streamDevices;
}

void Server::write(Conn& c, std::string data) {
    if (c.outPos > 0 && c.outPos == c.out.size()) {
        c.out.clear();
        c.outPos = 0;
    }
    c.out += data;
    flush(c);
}

void Server::flush(Conn& c) {
    while (c.pending() > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(c.pending(), 256 * 1024));
        const int n = send(c.s, c.out.data() + c.outPos, chunk, 0);
        if (n > 0) {
            c.outPos += static_cast<size_t>(n);
            c.lastSend = steadyMs();
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) break;
        c.dead = true;
        return;
    }
    if (c.pending() == 0) {
        c.out.clear();
        c.outPos = 0;
        if (c.closeAfterWrite) {
            shutdown(c.s, SD_SEND);
            c.dead = true;
        }
    } else if (c.stream && c.pending() > opt_.maxStreamBacklog) {
        ST_LOG_INFO(kTag, "a phone stopped reading its event stream: closed");
        c.dead = true;
    }
}

void Server::closeConn(Conn& c) {
    if (c.s != INVALID_SOCKET) closesocket(c.s);
    c.s = INVALID_SOCKET;
}

void Server::streamsChanged() {
    size_t n = 0;
    std::vector<std::string> devices;
    for (const auto& c : conns_)
        if (c->stream && !c->dead) {
            ++n;
            devices.push_back(c->deviceId);
        }
    const bool changed = n != streams_.load();
    streams_ = n;
    {
        std::lock_guard lock(shared_->mu);
        shared_->streamDevices = std::move(devices);
    }
    if (changed && onStreamsChanged) onStreamsChanged();
}

void Server::dispatch(Conn& c, Request req, int64_t now) {
    const bool head = req.method == "HEAD";
    const bool keep = req.keepAlive();
    std::weak_ptr<Shared> weak = shared_;
    const uint64_t id = c.id;
    Reply reply = [weak, id](Response r) {
        auto s = weak.lock();
        if (!s) return;
        {
            std::lock_guard lock(s->mu);
            if (!s->alive) return;
            s->replies.emplace_back(id, std::move(r));
        }
        s->poke();
    };
    Outcome o = router_.handle(req, c.peer, now, unixNow(), std::move(reply));
    switch (o.kind) {
    case Outcome::Kind::Respond:
        if (!keep) c.closeAfterWrite = true;
        write(c, serialize(o.response, keep, head));
        break;
    case Outcome::Kind::Async:
        c.awaiting = true;
        c.awaitSince = now;
        c.awaitHead = head;
        c.awaitKeepAlive = keep;
        break;
    case Outcome::Kind::Stream: {
        size_t open = 0;
        for (const auto& other : conns_)
            if (other->stream && !other->dead) ++open;
        if (open >= opt_.maxStreams) {
            c.closeAfterWrite = true;
            write(c, serialize(Response::error(503), false));
            break;
        }
        c.stream = true;
        c.deviceId = o.device.id;
        c.in.clear();
        std::string first = sseHead();
        {
            std::lock_guard lock(shared_->mu);
            for (const auto& [name, data] : shared_->latest) first += sseEvent(name, data);
        }
        write(c, std::move(first));
        streamsChanged();
        break;
    }
    }
}

void Server::processInput(Conn& c, int64_t now) {
    while (!c.dead && !c.awaiting && !c.stream && !c.closeAfterWrite && !c.in.empty()) {
        Request req;
        const ParseResult pr = parseRequest(c.in, req, opt_.limits);
        if (pr.status == ParseStatus::Incomplete) {
            if (c.requestStart == 0) c.requestStart = now;
            return;
        }
        if (pr.status == ParseStatus::Error) {
            c.closeAfterWrite = true;
            c.in.clear();
            write(c, serialize(Response::error(pr.error), false));
            return;
        }
        c.in.erase(0, pr.consumed);
        c.requestStart = c.in.empty() ? 0 : now;
        dispatch(c, std::move(req), now);
    }
}

void Server::processOutbox(int64_t now) {
    std::vector<std::pair<uint64_t, Response>> replies;
    std::vector<std::pair<std::string, std::string>> events;
    std::vector<std::string> drops;
    {
        std::lock_guard lock(shared_->mu);
        replies.swap(shared_->replies);
        events.swap(shared_->events);
        drops.swap(shared_->drops);
    }
    for (auto& [id, response] : replies) {
        auto it = std::find_if(conns_.begin(), conns_.end(), [id = id](const auto& c) { return c->id == id; });
        if (it == conns_.end()) continue;   // the phone went away meanwhile
        Conn& c = **it;
        if (!c.awaiting || c.dead) continue;
        c.awaiting = false;
        c.lastActivity = now;
        if (!c.awaitKeepAlive) c.closeAfterWrite = true;
        write(c, serialize(response, c.awaitKeepAlive, c.awaitHead));
        processInput(c, now);   // a pipelined request waiting behind it
    }
    if (!events.empty()) {
        std::string frames;
        for (const auto& [name, data] : events) frames += sseEvent(name, data);
        for (auto& c : conns_)
            if (c->stream && !c->dead) write(*c, frames);
    }
    bool dropped = false;
    for (const auto& device : drops)
        for (auto& c : conns_)
            if (c->deviceId == device && !c->dead) {
                c->dead = true;
                dropped = dropped || c->stream;
            }
    if (dropped) streamsChanged();
}

void Server::run() {
    SetThreadDescription(GetCurrentThread(), L"st-remote");
    int64_t nextTick = 0;
    std::vector<WSAPOLLFD> fds;
    while (running_) {
        int64_t now = steadyMs();
        // What to wait for, and for how long.
        fds.clear();
        fds.push_back({sock(wake_), POLLRDNORM, 0});
        const bool accepting = conns_.size() < opt_.maxConnections;   // full: new phones wait in the backlog
        if (accepting) fds.push_back({sock(listen_), POLLRDNORM, 0});
        const size_t base = fds.size();   // fds[base + i] is conns_[i]
        int64_t deadline = INT64_MAX;
        const bool streaming = streams_.load() > 0;
        if (streaming) {
            if (nextTick == 0) nextTick = now + opt_.tickMs;
            deadline = std::min(deadline, nextTick);
        } else {
            nextTick = 0;
        }
        for (const auto& c : conns_) {
            SHORT ev = 0;
            if (!c->closeAfterWrite && c->in.size() < opt_.limits.maxHeaderBytes + opt_.limits.maxBodyBytes + 4) ev |= POLLRDNORM;
            if (c->pending() > 0) ev |= POLLWRNORM;
            fds.push_back({c->s, ev, 0});
            if (c->stream) deadline = std::min(deadline, c->lastSend + opt_.heartbeatMs);
            else if (c->awaiting) deadline = std::min(deadline, c->awaitSince + opt_.replyTimeoutMs);
            else if (c->requestStart) deadline = std::min(deadline, c->requestStart + opt_.requestTimeoutMs);
            else deadline = std::min(deadline, c->lastActivity + opt_.idleTimeoutMs);
        }
        const INT timeout = deadline == INT64_MAX ? -1 : static_cast<INT>(std::clamp<int64_t>(deadline - now, 0, 60'000));
        const int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout);
        if (!running_) break;
        if (n == SOCKET_ERROR) {
            ST_LOG_WARN(kTag, "WSAPoll failed: {}", WSAGetLastError());
            Sleep(50);
            continue;
        }
        now = steadyMs();

        if (fds[0].revents & POLLRDNORM) {   // drain the wake-ups
            char buf[64];
            while (recv(sock(wake_), buf, sizeof buf, 0) > 0) {
            }
        }
        processOutbox(now);

        if (accepting && (fds[1].revents & (POLLRDNORM | POLLERR))) {
            for (;;) {
                sockaddr_in from{};
                int fromLen = sizeof from;
                SOCKET s = accept(sock(listen_), reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (s == INVALID_SOCKET) break;
                if (conns_.size() >= opt_.maxConnections || !setNonBlocking(s)) {
                    closesocket(s);
                    break;
                }
                BOOL noDelay = TRUE;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
                auto c = std::make_unique<Conn>();
                c->s = s;
                c->id = nextId_++;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                c->peer = ip;
                c->lastActivity = c->lastSend = now;
                conns_.push_back(std::move(c));
            }
        }

        // The connections that were polled (accepted ones come next round).
        const size_t polled = fds.size() - base;
        for (size_t i = 0; i < polled && i < conns_.size(); ++i) {
            Conn& c = *conns_[i];
            const SHORT re = fds[base + i].revents;
            if (c.dead || re == 0) continue;
            if (re & POLLNVAL) {
                c.dead = true;
                continue;
            }
            if (re & (POLLRDNORM | POLLHUP | POLLERR)) {
                char buf[8192];
                for (;;) {
                    const int got = recv(c.s, buf, sizeof buf, 0);
                    if (got > 0) {
                        c.lastActivity = now;
                        if (c.stream) continue;   // a stream never sends anything we need
                        c.in.append(buf, static_cast<size_t>(got));
                        if (c.in.size() > opt_.limits.maxHeaderBytes + opt_.limits.maxBodyBytes + 4) break;
                        continue;
                    }
                    if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                        // Closed by the phone (browsers never half-close): whatever was pending is dropped.
                        c.dead = true;
                        c.in.clear();
                    }
                    break;
                }
                processInput(c, now);
            }
            if (!c.dead && (re & POLLWRNORM)) flush(c);
        }

        // Timeouts, heartbeats.
        for (auto& cp : conns_) {
            Conn& c = *cp;
            if (c.dead) continue;
            if (c.stream) {
                if (now - c.lastSend >= opt_.heartbeatMs) write(c, sseComment("ping"));
            } else if (c.awaiting) {
                if (now - c.awaitSince >= opt_.replyTimeoutMs) {
                    c.awaiting = false;
                    c.closeAfterWrite = true;
                    write(c, serialize(Response::error(504), false));
                }
            } else if (c.requestStart) {
                if (now - c.requestStart >= opt_.requestTimeoutMs) {
                    c.closeAfterWrite = true;
                    c.in.clear();
                    write(c, serialize(Response::error(408), false));
                }
            } else if (c.pending() == 0 && now - c.lastActivity >= opt_.idleTimeoutMs) {
                c.dead = true;
            }
        }

        if (streams_.load() > 0 && nextTick && now >= nextTick) {
            nextTick = now + opt_.tickMs;
            if (onTick) onTick();
        }

        // Closed connections go.
        bool streamGone = false;
        for (auto& c : conns_)
            if (c->dead) {
                streamGone = streamGone || c->stream;
                closeConn(*c);
            }
        std::erase_if(conns_, [](const auto& c) { return c->dead; });
        if (streamGone) streamsChanged();
    }
}

} // namespace st::app::remote
