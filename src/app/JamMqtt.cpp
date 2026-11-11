// Listen together: MQTT 3.1.1 over WebSocket (see JamMqtt.h).
#include "app/JamMqtt.h"

#include "core/Log.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <random>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace st::app::jam::mqtt {

namespace {

constexpr const char* kTag = "jam";

void putU16(Bytes& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void putStr(Bytes& out, std::string_view s) {
    putU16(out, static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF)));
    out.insert(out.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 0xFFFF));
}

void putBin(Bytes& out, std::span<const uint8_t> b) {
    putU16(out, static_cast<uint16_t>(std::min<size_t>(b.size(), 0xFFFF)));
    out.insert(out.end(), b.begin(), b.begin() + std::min<size_t>(b.size(), 0xFFFF));
}

// Fixed header (first byte + remaining length) followed by `body`.
Bytes frame(uint8_t first, const Bytes& body) {
    Bytes out;
    out.reserve(body.size() + 5);
    out.push_back(first);
    appendRemainingLength(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Sequential reader of a packet body.
struct Reader {
    std::span<const uint8_t> d;
    size_t i = 0;
    bool bad = false;
    size_t left() const { return d.size() - i; }
    uint8_t u8() {
        if (left() < 1) {
            bad = true;
            return 0;
        }
        return d[i++];
    }
    uint16_t u16() {
        if (left() < 2) {
            bad = true;
            return 0;
        }
        const uint16_t v = static_cast<uint16_t>(d[i] << 8 | d[i + 1]);
        i += 2;
        return v;
    }
    Bytes bin() {
        const uint16_t n = u16();
        if (bad || left() < n) {
            bad = true;
            return {};
        }
        Bytes b(d.begin() + i, d.begin() + i + n);
        i += n;
        return b;
    }
    std::string str() {
        const Bytes b = bin();
        return std::string(b.begin(), b.end());
    }
    Bytes rest() {
        Bytes b(d.begin() + i, d.end());
        i = d.size();
        return b;
    }
};

// Valid UTF-8 without U+0000 (MQTT strings must not hold it).
bool validUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0) return false;
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string randomHex(size_t digits) {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^ static_cast<uint64_t>(steadyMs()));
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < digits; ++i) s.push_back(kDigits[rng() & 15]);
    return s;
}

} // namespace

// ---- Codec ----------------------------------------------------------------------------------------------------------

bool appendRemainingLength(Bytes& out, size_t value) {
    if (value > kMaxRemainingLength) return false;
    do {
        uint8_t b = value % 128;
        value /= 128;
        if (value > 0) b |= 0x80;
        out.push_back(b);
    } while (value > 0);
    return true;
}

Parse decodeRemainingLength(std::span<const uint8_t> data, size_t& value, size_t& used) {
    value = 0;
    size_t mult = 1;
    for (size_t i = 0; i < 4; ++i) {
        if (i >= data.size()) return Parse::NeedMore;
        const uint8_t b = data[i];
        value += (b & 0x7F) * mult;
        if (!(b & 0x80)) {
            used = i + 1;
            // A non-minimal encoding ("0x80 0x00") is tolerated by brokers; we accept it too.
            return Parse::Ok;
        }
        mult *= 128;
    }
    return Parse::Malformed;   // a fifth continuation byte
}

Bytes encodeConnect(const ConnectOptions& o) {
    Bytes body;
    putStr(body, "MQTT");
    body.push_back(4);   // protocol level 3.1.1
    uint8_t flags = 0;
    if (o.cleanSession) flags |= 0x02;
    if (o.will) {
        flags |= 0x04;   // will flag, QoS 0
        if (o.will->retain) flags |= 0x20;
    }
    if (!o.password.empty()) flags |= 0x40;
    if (!o.username.empty()) flags |= 0x80;
    body.push_back(flags);
    putU16(body, o.keepAliveSec);
    putStr(body, o.clientId);
    if (o.will) {
        putStr(body, o.will->topic);
        putBin(body, o.will->payload);
    }
    if (!o.username.empty()) putStr(body, o.username);
    if (!o.password.empty()) putBin(body, std::span(reinterpret_cast<const uint8_t*>(o.password.data()), o.password.size()));
    return frame(0x10, body);
}

Bytes encodeSubscribe(uint16_t packetId, const std::vector<std::string>& filters) {
    Bytes body;
    putU16(body, packetId);
    for (const auto& f : filters) {
        putStr(body, f);
        body.push_back(0);   // requested QoS 0
    }
    return frame(0x82, body);
}

Bytes encodePublish(std::string_view topic, std::span<const uint8_t> payload, bool retain) {
    Bytes body;
    body.reserve(topic.size() + payload.size() + 2);
    putStr(body, topic);
    body.insert(body.end(), payload.begin(), payload.end());
    return frame(static_cast<uint8_t>(0x30 | (retain ? 0x01 : 0x00)), body);
}

Bytes encodePuback(uint16_t packetId) {
    Bytes body;
    putU16(body, packetId);
    return frame(0x40, body);
}

Bytes encodePingreq() { return {0xC0, 0x00}; }
Bytes encodePingresp() { return {0xD0, 0x00}; }
Bytes encodeDisconnect() { return {0xE0, 0x00}; }

Bytes encodeConnack(bool sessionPresent, uint8_t returnCode) {
    return {0x20, 0x02, static_cast<uint8_t>(sessionPresent ? 1 : 0), returnCode};
}

Bytes encodeSuback(uint16_t packetId, const std::vector<uint8_t>& codes) {
    Bytes body;
    putU16(body, packetId);
    body.insert(body.end(), codes.begin(), codes.end());
    return frame(0x90, body);
}

void StreamDecoder::feed(std::span<const uint8_t> data) {
    if (bad_) return;
    // Drop what was consumed before growing (keeps the buffer at about one packet).
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 64 * 1024) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Packet> StreamDecoder::next() {
    if (bad_ || buf_.size() - pos_ < 2) return std::nullopt;
    const uint8_t first = buf_[pos_];
    const uint8_t type = first >> 4, flags = first & 0x0F;
    size_t len = 0, used = 0;
    const auto r = decodeRemainingLength(std::span(buf_).subspan(pos_ + 1), len, used);
    if (r == Parse::NeedMore) return std::nullopt;
    if (r == Parse::Malformed || type == 0 || type == 15 || len > max_) {
        bad_ = true;
        return std::nullopt;
    }
    // Fixed flags: PUBLISH carries its own; SUBSCRIBE / UNSUBSCRIBE / PUBREL must be 0010, the rest 0000.
    const bool fixed2 = type == 8 || type == 10 || type == 6;
    if (type != 3 && flags != (fixed2 ? 0x2 : 0x0)) {
        bad_ = true;
        return std::nullopt;
    }
    if (type == 3 && ((flags >> 1) & 3) == 3) {   // QoS 3 does not exist
        bad_ = true;
        return std::nullopt;
    }
    const size_t total = 1 + used + len;
    if (buf_.size() - pos_ < total) return std::nullopt;
    Packet p;
    p.type = static_cast<Type>(type);
    p.flags = flags;
    p.body.assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 1 + used),
                  buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + total));
    pos_ += total;
    return p;
}

std::optional<Connack> parseConnack(const Packet& p) {
    if (p.type != Type::Connack || p.body.size() != 2 || (p.body[0] & 0xFE)) return std::nullopt;
    return Connack{(p.body[0] & 1) != 0, p.body[1]};
}

std::optional<Publish> parsePublish(const Packet& p) {
    if (p.type != Type::Publish) return std::nullopt;
    Reader r{p.body};
    Publish m;
    m.retain = (p.flags & 1) != 0;
    m.qos = (p.flags >> 1) & 3;
    m.dup = (p.flags & 8) != 0;
    if (m.qos > 2) return std::nullopt;
    m.topic = r.str();
    if (m.qos > 0) m.packetId = r.u16();
    if (r.bad || m.topic.empty() || !validUtf8(m.topic) || (m.qos > 0 && m.packetId == 0)) return std::nullopt;
    m.payload = r.rest();
    return m;
}

std::optional<Suback> parseSuback(const Packet& p) {
    if (p.type != Type::Suback) return std::nullopt;
    Reader r{p.body};
    Suback s;
    s.packetId = r.u16();
    s.codes = r.rest();
    if (r.bad || s.codes.empty()) return std::nullopt;
    for (uint8_t c : s.codes)
        if (c > 2 && c != 0x80) return std::nullopt;
    return s;
}

std::optional<uint16_t> parsePuback(const Packet& p) {
    if (p.type != Type::Puback || p.body.size() != 2) return std::nullopt;
    return static_cast<uint16_t>(p.body[0] << 8 | p.body[1]);
}

std::optional<ConnectOptions> parseConnect(const Packet& p) {
    if (p.type != Type::Connect) return std::nullopt;
    Reader r{p.body};
    if (r.str() != "MQTT" || r.u8() != 4) return std::nullopt;
    const uint8_t flags = r.u8();
    if (flags & 0x01) return std::nullopt;   // reserved bit
    ConnectOptions o;
    o.cleanSession = (flags & 0x02) != 0;
    o.keepAliveSec = r.u16();
    o.clientId = r.str();
    if (flags & 0x04) {
        Will w;
        w.topic = r.str();
        w.payload = r.bin();
        w.retain = (flags & 0x20) != 0;
        o.will = std::move(w);
    } else if (flags & 0x38) {
        return std::nullopt;   // will QoS / retain without a will
    }
    if (flags & 0x80) o.username = r.str();
    if (flags & 0x40) {
        const Bytes pw = r.bin();
        o.password.assign(pw.begin(), pw.end());
    }
    if (r.bad || r.left() != 0) return std::nullopt;
    return o;
}

std::optional<Subscribe> parseSubscribe(const Packet& p) {
    if (p.type != Type::Subscribe || p.flags != 0x2) return std::nullopt;
    Reader r{p.body};
    Subscribe s;
    s.packetId = r.u16();
    while (!r.bad && r.left() > 0) {
        std::string f = r.str();
        const uint8_t q = r.u8();
        if (r.bad || f.empty() || q > 2) return std::nullopt;
        s.filters.emplace_back(std::move(f), q);
    }
    if (r.bad || s.filters.empty() || s.packetId == 0) return std::nullopt;
    return s;
}

bool validTopic(std::string_view topic) {
    return !topic.empty() && topic.size() <= 0xFFFF && topic.find_first_of("+#") == std::string_view::npos && validUtf8(topic);
}

const char* connackText(uint8_t rc) {
    switch (rc) {
    case 0: return "accepted";
    case 1: return "unacceptable protocol version";
    case 2: return "client id rejected";
    case 3: return "server unavailable";
    case 4: return "bad user name or password";
    case 5: return "not authorized";
    default: return "unknown return code";
    }
}

// ---- Broker URLs ------------------------------------------------------------------------------------------------------

std::optional<BrokerUrl> parseBrokerUrl(std::string_view url) {
    BrokerUrl b;
    auto lowerStarts = [&](std::string_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) return false;
        return true;
    };
    if (lowerStarts("wss://")) {
        url.remove_prefix(6);
        b.secure = true;
        b.port = 443;
    } else if (lowerStarts("ws://")) {
        url.remove_prefix(5);
        b.secure = false;
        b.port = 80;
    } else {
        return std::nullopt;
    }
    if (url.find_first_of("?#@ \t") != std::string_view::npos) return std::nullopt;
    const size_t slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    b.path = slash == std::string_view::npos ? std::string("/") : std::string(url.substr(slash));
    if (authority.empty()) return std::nullopt;
    if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        const std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        const int p = std::stoi(std::string(port));
        if (p <= 0 || p > 65535) return std::nullopt;
        b.port = static_cast<uint16_t>(p);
        authority = authority.substr(0, colon);
    }
    if (authority.empty() || !std::all_of(authority.begin(), authority.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '[' || c == ']' || c == ':';
        }))
        return std::nullopt;
    b.host = std::string(authority);
    return b;
}

std::string brokerHost(std::string_view url) {
    const auto b = parseBrokerUrl(url);
    return b ? b->host : std::string(url);
}

// ---- Client -----------------------------------------------------------------------------------------------------------

namespace {

// One WebSocket connection. The reader thread blocks in WinHttpWebSocketReceive; abort() closes the socket handle
// from another thread, which makes that call return.
struct Conn {
    HINTERNET session = nullptr, connect = nullptr;
    std::atomic<HINTERNET> ws{nullptr};
    std::atomic<HINTERNET> pending{nullptr};   // the upgrade request while it is being made (abort() closes it)

    void abort() {
        if (HINTERNET h = pending.exchange(nullptr)) WinHttpCloseHandle(h);
        if (HINTERNET h = ws.exchange(nullptr)) WinHttpCloseHandle(h);
    }
    ~Conn() {
        abort();
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

std::string winError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "name not resolved";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection error";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    case ERROR_INVALID_HANDLE: return "closed";
    default: return std::format("error {}", e);
    }
}

// Opens a WebSocket to the broker (blocking, bounded by the WinHTTP timeouts). `err` says why it failed.
std::shared_ptr<Conn> openSocket(const BrokerUrl& b, const std::shared_ptr<Conn>& conn, std::string& err) {
    conn->session = WinHttpOpen(L"ShadeTube", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!conn->session)
        conn->session = WinHttpOpen(L"ShadeTube", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!conn->session) {
        err = "WinHttpOpen: " + winError(GetLastError());
        return nullptr;
    }
    // Hosts that publish IPv6 addresses this network can't reach would otherwise wait for the IPv6 connect timeout.
    BOOL on = TRUE;
    WinHttpSetOption(conn->session, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &on, sizeof on);
    WinHttpSetTimeouts(conn->session, 10'000, 10'000, 10'000, 15'000);
    conn->connect = WinHttpConnect(conn->session, widen(b.host).c_str(), b.port, 0);
    if (!conn->connect) {
        err = "WinHttpConnect: " + winError(GetLastError());
        return nullptr;
    }
    HINTERNET req = WinHttpOpenRequest(conn->connect, L"GET", widen(b.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, b.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        err = "WinHttpOpenRequest: " + winError(GetLastError());
        return nullptr;
    }
    conn->pending = req;
    auto fail = [&](const char* what) {
        err = std::string(what) + ": " + winError(GetLastError());
        if (HINTERNET h = conn->pending.exchange(nullptr)) WinHttpCloseHandle(h);
        return nullptr;
    };
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return fail("upgrade option");
    if (!WinHttpAddRequestHeaders(req, L"Sec-WebSocket-Protocol: mqtt", static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD))
        return fail("headers");
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return fail("send");
    if (!WinHttpReceiveResponse(req, nullptr)) return fail("response");
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        err = std::format("HTTP {} instead of a WebSocket upgrade", status);
        if (HINTERNET h = conn->pending.exchange(nullptr)) WinHttpCloseHandle(h);
        return nullptr;
    }
    // Past the handshake the socket is quiet between keep-alive pings: allow a minute before a receive gives up
    // (the client's own keep-alive notices a dead connection much sooner).
    DWORD minute = 60'000;
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &minute, sizeof minute);
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!ws) return fail("upgrade");
    WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &minute, sizeof minute);
    conn->ws = ws;
    if (HINTERNET h = conn->pending.exchange(nullptr)) WinHttpCloseHandle(h);   // the socket lives on its own
    if (!conn->ws.load()) {   // aborted meanwhile
        err = "cancelled";
        return nullptr;
    }
    return conn;
}

} // namespace

struct Client::Impl : std::enable_shared_from_this<Impl> {
    Events events;
    mutable std::mutex m;
    std::condition_variable cv;
    Config config;
    uint64_t configGen = 0;   // bumped by start(): the connection loop restarts
    bool stopping = false;
    std::chrono::milliseconds flush{0};
    bool skip = false;        // nextBroker()
    size_t brokerIndex = 0;
    int failuresHere = 0;
    int backoffStep = 0;
    std::deque<Bytes> outbox;
    State state = State::Stopped;
    std::string currentBroker;
    Stats stats;
    std::thread thread;
    // Current connection (control thread + reader thread).
    std::shared_ptr<Conn> conn;
    bool readerDead = false;
    std::string readerWhy;
    std::optional<Connack> connack;
    int64_t lastRx = 0;

    void setState(State s, const std::string& broker, const std::string& detail) {
        {
            std::lock_guard lk(m);
            state = s;
            if (!broker.empty()) currentBroker = broker;
        }
        cv.notify_all();
        if (events.onState) events.onState(s, broker, detail);
    }

    void readerLoop(std::shared_ptr<Conn> c) {
        SetThreadDescription(GetCurrentThread(), L"st-jam-mqtt-rx");
        StreamDecoder dec;
        Bytes buf(64 * 1024);
        std::string why;
        for (;;) {
            HINTERNET ws = c->ws.load();
            if (!ws) {
                why = "closed";
                break;
            }
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD e = WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &read, &type);
            if (e != NO_ERROR) {
                why = "receive: " + winError(e);
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                why = "closed by the broker";
                break;
            }
            dec.feed(std::span(buf.data(), read));
            bool stop = false;
            while (auto p = dec.next()) {
                {
                    std::lock_guard lk(m);
                    lastRx = steadyMs();
                    ++stats.packetsReceived;
                }
                if (!handle(*p)) {
                    why = "unexpected packet";
                    stop = true;
                    break;
                }
            }
            if (stop) break;
            if (dec.malformed()) {
                why = "malformed packet";
                break;
            }
        }
        {
            std::lock_guard lk(m);
            readerDead = true;
            readerWhy = why;
        }
        cv.notify_all();
    }

    // Reader thread. False = protocol violation (drop the connection).
    bool handle(const Packet& p) {
        switch (p.type) {
        case Type::Connack: {
            const auto ack = parseConnack(p);
            if (!ack) return false;
            {
                std::lock_guard lk(m);
                connack = ack;
            }
            cv.notify_all();
            return true;
        }
        case Type::Publish: {
            auto msg = parsePublish(p);
            if (!msg) return false;
            if (msg->qos == 1) {
                {
                    std::lock_guard lk(m);
                    outbox.push_back(encodePuback(msg->packetId));
                }
                cv.notify_all();
            }
            if (events.onMessage) events.onMessage(*msg);
            return true;
        }
        case Type::Suback: {
            const auto ack = parseSuback(p);
            if (!ack) return false;
            if (std::any_of(ack->codes.begin(), ack->codes.end(), [](uint8_t c) { return c == 0x80; }))
                ST_LOG_WARN(kTag, "broker refused a subscription");
            return true;
        }
        case Type::Pingresp:
        case Type::Puback:
        case Type::Unsuback: return true;
        default: return false;   // a broker never sends CONNECT / SUBSCRIBE / PINGREQ / DISCONNECT ...
        }
    }

    bool sendRaw(const std::shared_ptr<Conn>& c, const Bytes& data) {
        HINTERNET ws = c->ws.load();
        if (!ws) return false;
        const DWORD e = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                             const_cast<uint8_t*>(data.data()), static_cast<DWORD>(data.size()));
        if (e != NO_ERROR) return false;
        std::lock_guard lk(m);
        ++stats.packetsSent;
        return true;
    }

    // Waits up to `ms` unless something wakes the loop: stop, a new configuration or a broker skip.
    void pause(int64_t ms, uint64_t gen) {
        std::unique_lock lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return stopping || configGen != gen || skip; });
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"st-jam-mqtt");
        std::mt19937 rng(std::random_device{}());
        for (;;) {
            Config cfg;
            uint64_t gen = 0;
            std::string url;
            {
                std::lock_guard lk(m);
                if (stopping) break;
                cfg = config;
                gen = configGen;
                skip = false;
                if (cfg.brokers.empty()) break;
                url = cfg.brokers[brokerIndex % cfg.brokers.size()];
                outbox.clear();   // QoS 0: nothing survives a reconnect (the app re-publishes on Connected)
                readerDead = false;
                readerWhy.clear();
                connack.reset();
            }
            setState(State::Connecting, url, {});
            std::string err;
            bool wasConnected = false;
            auto c = std::make_shared<Conn>();
            {
                std::lock_guard lk(m);
                conn = c;
            }
            const auto b = parseBrokerUrl(url);
            if (!b) err = "invalid broker URL";
            else if (!openSocket(*b, c, err)) {
            } else {
                std::thread reader([self = shared_from_this(), c] { self->readerLoop(c); });
                // CONNECT -> CONNACK (10 s).
                ConnectOptions co;
                co.clientId = cfg.clientIdPrefix + randomHex(16);
                co.keepAliveSec = cfg.keepAliveSec;
                co.cleanSession = true;
                co.will = cfg.will;
                bool accepted = false;
                if (!sendRaw(c, encodeConnect(co))) {
                    err = "CONNECT not sent";
                } else {
                    std::unique_lock lk(m);
                    cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return connack.has_value() || readerDead || stopping || configGen != gen || skip; });
                    if (connack && connack->returnCode == 0) accepted = true;
                    else if (connack) err = std::string("CONNACK: ") + connackText(connack->returnCode);
                    else if (readerDead) err = readerWhy;
                    else if (!stopping && configGen == gen && !skip) err = "no CONNACK";
                    else err = "interrupted";
                }
                if (accepted && !cfg.subscriptions.empty() && !sendRaw(c, encodeSubscribe(1, cfg.subscriptions))) {
                    accepted = false;
                    err = "SUBSCRIBE not sent";
                }
                if (accepted) {
                    wasConnected = true;
                    {
                        std::lock_guard lk(m);
                        failuresHere = 0;
                        backoffStep = 0;
                        lastRx = steadyMs();
                        ++stats.connects;
                    }
                    setState(State::Connected, url, {});
                    err = session(c, cfg, gen);
                }
                c->abort();
                reader.join();
            }
            {
                std::lock_guard lk(m);
                conn.reset();
            }
            c.reset();
            bool stopNow = false, restart = false, skipped = false;
            {
                std::lock_guard lk(m);
                stopNow = stopping;
                restart = configGen != gen;
                skipped = skip;
                if (restart) {
                    brokerIndex = 0;
                    failuresHere = 0;
                    backoffStep = 0;
                } else if (skipped) {
                    ++brokerIndex;
                    failuresHere = 0;
                }
            }
            if (stopNow) break;
            if (restart || skipped) continue;
            // A connection that was up and dropped is retried at once; failed attempts count towards the next broker.
            int64_t wait = 0;
            {
                std::lock_guard lk(m);
                ++stats.failures;
                if (!wasConnected) {
                    if (++failuresHere >= std::max(1, cfg.failuresBeforeNext)) {
                        ++brokerIndex;
                        failuresHere = 0;
                    }
                    const int step = std::min(backoffStep++, 5);
                    wait = static_cast<int64_t>(std::min(30'000.0, 1000.0 * (1 << step)) * std::uniform_real_distribution<>(0.8, 1.2)(rng));
                } else {
                    wait = 500;
                }
            }
            ST_LOG_WARN(kTag, "{} {}: {}; retrying in {} ms", wasConnected ? "connection to" : "could not connect to",
                        brokerHost(url), err.empty() ? std::string("closed") : err, wait);
            setState(State::Waiting, url, err);
            pause(wait, gen);
        }
        setState(State::Stopped, {}, {});
    }

    // The connected phase: sends the outbox, keeps the connection alive. Returns why it ended ("" = stop / restart).
    std::string session(const std::shared_ptr<Conn>& c, const Config& cfg, uint64_t gen) {
        const int64_t keepAlive = std::max<int64_t>(5, cfg.keepAliveSec) * 1000;
        int64_t lastPing = steadyMs();
        for (;;) {
            std::deque<Bytes> send;
            bool quit = false;
            std::string why;
            {
                std::unique_lock lk(m);
                const int64_t now = steadyMs();
                const int64_t untilPing = std::max<int64_t>(0, lastPing + keepAlive - now);
                cv.wait_for(lk, std::chrono::milliseconds(std::min<int64_t>(untilPing, 1000)), [&] {
                    return !outbox.empty() || stopping || configGen != gen || skip || readerDead;
                });
                send.swap(outbox);
                if (readerDead) {
                    quit = true;
                    why = readerWhy;
                } else if (stopping || configGen != gen || skip) {
                    quit = true;
                } else if (steadyMs() - lastRx > keepAlive + 15'000) {
                    quit = true;
                    why = "keep-alive timeout";
                }
            }
            for (const auto& pkt : send)
                if (!sendRaw(c, pkt)) {
                    quit = true;
                    if (why.empty()) why = "send failed";
                    break;
                }
            if (quit) {
                bool flushNow = false;
                std::chrono::milliseconds flushFor{0};
                {
                    std::lock_guard lk(m);
                    flushNow = stopping;
                    flushFor = flush;
                }
                if (flushNow && why.empty()) {
                    // stop(): what was queued until now goes out, then a clean DISCONNECT (no last will).
                    const int64_t until = steadyMs() + flushFor.count();
                    for (;;) {
                        std::deque<Bytes> rest;
                        {
                            std::lock_guard lk(m);
                            rest.swap(outbox);
                        }
                        if (rest.empty() || steadyMs() > until) break;
                        for (const auto& pkt : rest) sendRaw(c, pkt);
                    }
                    sendRaw(c, encodeDisconnect());
                } else if (why.empty()) {
                    sendRaw(c, encodeDisconnect());   // restart / broker change: leave cleanly
                }
                return why;
            }
            if (steadyMs() - lastPing >= keepAlive) {
                if (!sendRaw(c, encodePingreq())) return "ping failed";
                lastPing = steadyMs();
            }
        }
    }
};

Client::Client(Events events) : impl_(std::make_shared<Impl>()) { impl_->events = std::move(events); }

Client::~Client() { stop(); }

void Client::start(Config config) {
    bool spawn = false;
    {
        std::lock_guard lk(impl_->m);
        impl_->config = std::move(config);
        ++impl_->configGen;
        impl_->stopping = false;
        impl_->brokerIndex = 0;
        spawn = !impl_->thread.joinable();
        // Restart with the new configuration now: an attempt in progress is cut short, an open connection leaves
        // with a DISCONNECT (the connection thread wakes on the new generation).
        if (!spawn && impl_->conn && !impl_->conn->ws.load()) impl_->conn->abort();
    }
    impl_->cv.notify_all();
    if (spawn) impl_->thread = std::thread([self = impl_] { self->run(); });
}

void Client::stop(std::chrono::milliseconds flush) {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard lk(impl_->m);
        if (!impl_->thread.joinable()) return;
        impl_->stopping = true;
        impl_->flush = flush;
        c = impl_->conn;
    }
    impl_->cv.notify_all();
    // A connection attempt in progress is cut short; an open one sends what is queued (up to `flush`) and a
    // DISCONNECT first. That is bounded: a send still stalled after flush + 1.5 s is aborted.
    if (c && !c->ws.load()) c->abort();
    std::thread watchdog;
    if (c)
        watchdog = std::thread([self = impl_, c, wait = flush + std::chrono::milliseconds(1500)] {
            std::unique_lock lk(self->m);
            if (!self->cv.wait_for(lk, wait, [&] { return self->state == State::Stopped; })) {
                lk.unlock();
                c->abort();
            }
        });
    impl_->thread.join();
    {
        std::lock_guard lk(impl_->m);
        impl_->state = State::Stopped;
    }
    impl_->cv.notify_all();
    if (watchdog.joinable()) watchdog.join();
}

bool Client::publish(std::string topic, Bytes payload, bool retain) {
    if (!validTopic(topic)) return false;
    {
        std::lock_guard lk(impl_->m);
        if (impl_->state != State::Connected || impl_->stopping) return false;
        if (impl_->outbox.size() >= 256) impl_->outbox.pop_front();
        impl_->outbox.push_back(encodePublish(topic, payload, retain));
    }
    impl_->cv.notify_all();
    return true;
}

void Client::nextBroker() {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard lk(impl_->m);
        impl_->skip = true;
        c = impl_->conn;
    }
    impl_->cv.notify_all();
    if (c && !c->ws.load()) c->abort();   // still connecting: give up on it now
}

void Client::abortConnection() {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard lk(impl_->m);
        c = impl_->conn;
    }
    if (c) c->abort();
}

void Client::moveTo(const std::string& url) {
    Config cfg;
    {
        std::lock_guard lk(impl_->m);
        cfg = impl_->config;
    }
    std::erase(cfg.brokers, url);
    cfg.brokers.insert(cfg.brokers.begin(), url);
    start(std::move(cfg));
}

Client::State Client::state() const {
    std::lock_guard lk(impl_->m);
    return impl_->state;
}

std::string Client::broker() const {
    std::lock_guard lk(impl_->m);
    return impl_->currentBroker;
}

Client::Stats Client::stats() const {
    std::lock_guard lk(impl_->m);
    return impl_->stats;
}

} // namespace st::app::jam::mqtt
