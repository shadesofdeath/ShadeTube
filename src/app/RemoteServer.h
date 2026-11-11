#pragma once
// Phone remote control (app/RemoteControl): the LAN server and the adapter addresses the QR code can point at.
//
// One private thread runs everything: Winsock IPv4 (all interfaces by default), non-blocking sockets and WSAPoll, a
// UDP socket on 127.0.0.1 to wake it. HTTP/1.1 with keep-alive and pipelining (one request at a time per
// connection, in order), RemoteHttp's limits, timeouts for slow requests (408), idle connections and API answers that
// never come (504), at most `maxConnections` connections and `maxStreams` event streams. Requests go to the Router;
// its async answers and the app's broadcasts come back from any thread through a locked outbox. An event stream gets
// the newest "state" / "queue" events at once, a ": ping" comment when it was quiet for a while and is closed when
// the phone stops reading (backlog limit).
//
// start() tries `port`, then the following ports, until one binds (SO_EXCLUSIVEADDRUSE: no other process can share
// it); stop() closes everything and joins the thread. Thread-safe unless noted. Standalone (no app headers).
#include "app/RemoteApi.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st::app::remote {

struct LanAddress {
    std::string ip;              // "192.168.1.20"
    std::wstring adapter;        // friendly name ("Wi-Fi", "Ethernet 2")
    std::wstring description;    // "Intel(R) Wi-Fi 6 AX201 160MHz"
    enum class Kind { WiFi, Ethernet, Other };
    Kind kind = Kind::Other;
    unsigned long ifType = 0;    // IF_TYPE_*
    bool isVirtual = false;      // Hyper-V / VMware / VirtualBox / WSL / Docker / VPN adapters (by type and name)
    bool gateway = false;        // has a default gateway: a real network
    int score = 0;               // rankAddresses(): higher = more likely what a phone on the same network reaches
};
// IPv4 addresses of the adapters that are up (no loopback, no link-local 169.254.x), best first.
std::vector<LanAddress> lanAddresses();
// Fills kind / isVirtual / score from the raw facts (ip, names, ifType, gateway) and sorts best first.
void rankAddresses(std::vector<LanAddress>& list);

class Server {
public:
    struct Options {
        std::string bindAddress = "0.0.0.0";   // tests / SHADETUBE_REMOTE_BIND: "127.0.0.1"
        uint16_t port = 48484;
        int portAttempts = 10;                 // port, port + 1, ... port + attempts - 1
        size_t maxConnections = 32;
        size_t maxStreams = 8;
        int requestTimeoutMs = 10'000;         // a started request must be complete within this
        int idleTimeoutMs = 30'000;            // a keep-alive connection without a request
        int replyTimeoutMs = 25'000;           // an API call the app did not answer -> 504
        int tickMs = 1000;                     // onTick while at least one stream is open
        int heartbeatMs = 15'000;              // ": ping" on a quiet stream
        size_t maxStreamBacklog = 512 * 1024;  // unsent bytes before a stream is dropped
        Limits limits;
    };

    explicit Server(Router& router);   // the router outlives the server
    ~Server();                         // stop()
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts the thread. False with a reason (English, for the log; `inUse` = every port was taken).
    bool start(const Options& options, std::string* error = nullptr, bool* inUse = nullptr);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_.load(); }

    // An event for every open stream; the newest data of each name is also what a new stream starts with ("pos"
    // ticks excepted). Events of a name still waiting for the thread are replaced.
    void broadcast(const std::string& event, std::string data);
    std::string latest(const std::string& event) const;   // "" = none yet
    // A revoked device: its streams and connections are closed.
    void dropDevice(const std::string& deviceId);
    size_t streamCount() const { return streams_.load(); }
    std::vector<std::string> streamDevices() const;       // device ids with an open stream

    // Server thread.
    std::function<void()> onTick;              // every tickMs while a stream is open (a phone is looking)
    std::function<void()> onStreamsChanged;    // a stream opened or closed

private:
    struct Shared;
    struct Conn;
    void run();
    void processOutbox(int64_t now);
    void processInput(Conn& c, int64_t now);
    void dispatch(Conn& c, Request req, int64_t now);
    void write(Conn& c, std::string data);
    void flush(Conn& c);
    void closeConn(Conn& c);
    void streamsChanged();

    Router& router_;
    Options opt_;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{0};
    std::atomic<size_t> streams_{0};
    uintptr_t listen_ = ~uintptr_t(0);
    uintptr_t wake_ = ~uintptr_t(0);
    std::vector<std::unique_ptr<Conn>> conns_;   // server thread
    uint64_t nextId_ = 1;
    bool wsa_ = false;
};

} // namespace st::app::remote
