#pragma once
// Listen together ("Birlikte dinle", app/Together): a small MQTT 3.1.1 client over secure WebSocket, for the public
// brokers a session meets on (we run no server).
//
// Codec: the packets a QoS 0 client needs — CONNECT (clean session, keep-alive, last will, optional user name /
// password), CONNACK, SUBSCRIBE / SUBACK, PUBLISH (QoS 0 incl. retained; incoming QoS 1 is acknowledged with PUBACK),
// PINGREQ / PINGRESP and DISCONNECT — plus the remaining-length varint and a stream decoder that cuts packets out of
// WebSocket messages (a message may hold several packets or a part of one).
//
// Client: one private thread per client ("st-jam-mqtt") owns the connection, a second one per connection blocks in the
// WebSocket receive. WinHTTP's WebSocket API (`wss://`, subprotocol "mqtt", the system proxy, IPv6 fast fallback).
// Connects to the first broker of its list; a broker that fails twice in a row hands over to the next one (wrapping),
// with a growing pause (1 s doubling to 30 s, with jitter) between attempts. Keep-alive: a PINGREQ every keepAlive
// seconds; no PINGRESP (or nothing at all) within 15 s is a dead connection and reconnects. Publishing is queued
// (latest 256 packets) and sent by the connection thread; while disconnected publish() drops (QoS 0) and returns false.
// Callbacks run on the client's threads: marshal them yourself (the app posts to the UI thread). Every subscription is
// sent again after a reconnect (clean sessions).
//
// Standalone (tests/jam compiles it): the Windows SDK (winhttp) and core/Log only.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::jam::mqtt {

using Bytes = std::vector<uint8_t>;

// ---- Codec ----------------------------------------------------------------------------------------------------------

enum class Type : uint8_t {
    Connect = 1, Connack = 2, Publish = 3, Puback = 4, Subscribe = 8, Suback = 9, Unsubscribe = 10, Unsuback = 11,
    Pingreq = 12, Pingresp = 13, Disconnect = 14,
};

inline constexpr size_t kMaxRemainingLength = 268'435'455;   // 4-byte varint limit

struct Will {
    std::string topic;
    Bytes payload;
    bool retain = false;
};

struct ConnectOptions {
    std::string clientId;
    uint16_t keepAliveSec = 30;
    bool cleanSession = true;
    std::optional<Will> will;
    std::string username, password;   // "" = not sent
};

// Remaining length: 1-4 bytes, 7 bits each, low group first. Appends to `out`; false when `value` is too large.
bool appendRemainingLength(Bytes& out, size_t value);
enum class Parse { Ok, NeedMore, Malformed };
// `used` = bytes of the varint.
Parse decodeRemainingLength(std::span<const uint8_t> data, size_t& value, size_t& used);

Bytes encodeConnect(const ConnectOptions& o);
Bytes encodeSubscribe(uint16_t packetId, const std::vector<std::string>& filters);   // every filter at QoS 0
Bytes encodePublish(std::string_view topic, std::span<const uint8_t> payload, bool retain);   // QoS 0
Bytes encodePuback(uint16_t packetId);
Bytes encodePingreq();
Bytes encodePingresp();
Bytes encodeDisconnect();
Bytes encodeConnack(bool sessionPresent, uint8_t returnCode);                        // (tests / a mock broker)
Bytes encodeSuback(uint16_t packetId, const std::vector<uint8_t>& codes);            // (tests / a mock broker)

struct Packet {
    Type type{};
    uint8_t flags = 0;   // the low nibble of the first byte
    Bytes body;          // variable header + payload
};

// Cuts packets out of a byte stream. Malformed input (reserved type 0 / 15, a varint over 4 bytes, wrong fixed flags
// where the spec fixes them, a packet over `maxPacket`) makes the decoder stop for good (the connection is dropped).
class StreamDecoder {
public:
    explicit StreamDecoder(size_t maxPacket = 1 << 20) : max_(maxPacket) {}
    void feed(std::span<const uint8_t> data);
    // The next whole packet, or nullopt (need more bytes, or malformed()).
    std::optional<Packet> next();
    bool malformed() const { return bad_; }
    size_t buffered() const { return buf_.size() - pos_; }

private:
    Bytes buf_;
    size_t pos_ = 0;
    size_t max_;
    bool bad_ = false;
};

struct Connack {
    bool sessionPresent = false;
    uint8_t returnCode = 0;   // 0 accepted; 1 bad protocol, 2 id rejected, 3 unavailable, 4 bad credentials, 5 not authorized
};
struct Publish {
    std::string topic;
    Bytes payload;
    bool retain = false, dup = false;
    uint8_t qos = 0;
    uint16_t packetId = 0;   // QoS 1 / 2 only
};
struct Suback {
    uint16_t packetId = 0;
    std::vector<uint8_t> codes;   // 0..2 granted QoS, 0x80 failure
};
struct Subscribe {
    uint16_t packetId = 0;
    std::vector<std::pair<std::string, uint8_t>> filters;
};

std::optional<Connack> parseConnack(const Packet& p);
std::optional<Publish> parsePublish(const Packet& p);
std::optional<Suback> parseSuback(const Packet& p);
std::optional<ConnectOptions> parseConnect(const Packet& p);   // (tests / a mock broker)
std::optional<Subscribe> parseSubscribe(const Packet& p);       // (tests / a mock broker)
std::optional<uint16_t> parsePuback(const Packet& p);

// A topic name we may publish to: 1..65535 bytes of UTF-8 without '+', '#' or U+0000.
bool validTopic(std::string_view topic);
const char* connackText(uint8_t returnCode);   // English, for logs

// ---- Broker URLs ----------------------------------------------------------------------------------------------------

struct BrokerUrl {
    bool secure = true;
    std::string host;
    uint16_t port = 443;
    std::string path = "/";
};
// "wss://host[:port][/path]" (also ws:// for a broker on the local network); no user info, query or fragment.
std::optional<BrokerUrl> parseBrokerUrl(std::string_view url);
std::string brokerHost(std::string_view url);   // "broker.emqx.io" (for the UI); the url itself when it doesn't parse

// ---- Client -----------------------------------------------------------------------------------------------------------

class Client {
public:
    enum class State { Stopped, Connecting, Connected, Waiting };
    struct Config {
        std::vector<std::string> brokers;        // tried in order (see the header comment)
        std::vector<std::string> subscriptions;  // topic filters, QoS 0
        std::optional<Will> will;
        uint16_t keepAliveSec = 20;
        std::string clientIdPrefix = "st-";      // + 16 random hex digits per connection
        int failuresBeforeNext = 2;              // failed attempts on one broker before the next one is tried
    };
    struct Events {
        // Connection state changes; `broker` = the URL in use, `detail` = why (English, for logs).
        std::function<void(State state, const std::string& broker, const std::string& detail)> onState;
        std::function<void(const Publish& message)> onMessage;
    };

    explicit Client(Events events);
    ~Client();   // stop() without flushing
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Starts (or restarts with a new configuration: the current connection is closed first). Returns at once.
    void start(Config config);
    // Sends what is queued (up to `flush`), a DISCONNECT, closes and joins the threads. Blocks up to about flush + 1 s.
    void stop(std::chrono::milliseconds flush = std::chrono::milliseconds(0));
    // Queues a QoS 0 publish. False (dropped) when not connected or the topic is invalid.
    bool publish(std::string topic, Bytes payload, bool retain = false);
    // Leaves the current broker for the next one in the list (now, without waiting for failures).
    void nextBroker();
    // Puts `url` first and connects to it.
    void moveTo(const std::string& url);
    // Drops the connection without a DISCONNECT, as a lost network would (the broker publishes the last will); the
    // client reconnects as after any drop. Tests.
    void abortConnection();

    State state() const;
    std::string broker() const;   // the URL of the current / last attempt
    struct Stats {
        int connects = 0, failures = 0;
        uint64_t packetsSent = 0, packetsReceived = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace st::app::jam::mqtt
