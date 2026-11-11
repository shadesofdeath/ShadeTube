#pragma once
// Listen together ("Birlikte dinle", app/Together): the messages of a session and the host's session state.
//
// Every message is an Envelope sealed with the room key (AES-256-GCM, the topic as associated data):
//   wire = 'J' | 0x01 | nonce (12) | ciphertext | tag (16), plaintext = {"v":1,"t":type,"d":device,"s":seq,"b":body}.
// `d` is the sender's device id (random, kept in jam.json), `s` its sequence number: receivers keep a replay window
// per device and drop repeated or old numbers (ReplayFilter), and the host ignores devices it removed.
//
// Topics (JamInvite topicBase): <base>/state retained, host -> all: the SessionState ("state"); an empty retained
// message clears it when the session ends. <base>/h host -> all: "hb", "pong", "end", "kick", "deny", "handoff",
// "move", and the host's last will ("bye"). <base>/g guests -> host: "hello", "hb", "ping", "req", "bye" (also the
// guest's last will).
//
// Standalone (tests/jam compiles it): app/JamCrypto, nlohmann/json.
#include "app/JamCrypto.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::app::jam {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kMaxMembers = 10;     // the host included
inline constexpr int kQueueWindow = 50;    // upcoming items in the state (after the current one)

struct Topics {
    std::string base, state, host, guest, all;   // all = "<base>/#"
};
Topics topicsFor(const Bytes& roomId);

// ---- Envelope -------------------------------------------------------------------------------------------------------

struct Envelope {
    std::string type;
    std::string device;            // 16 hex digits
    uint64_t seq = 0;
    nlohmann::json body = nlohmann::json::object();
};

enum class OpenError { None, Malformed, Tampered, Version };
Bytes sealEnvelope(std::span<const uint8_t> key, std::string_view topic, const Envelope& e);
// nullopt (and `why`) for anything that isn't a well-formed envelope sealed with `key` for `topic`.
std::optional<Envelope> openEnvelope(std::span<const uint8_t> key, std::string_view topic, std::span<const uint8_t> wire,
                                     OpenError* why = nullptr);

// Sequence numbers of this device: (epoch << 24) | counter, the epoch being the unix second of the connection (always
// growing) and the counter below 2^23. The last will takes 2^23: above everything that connection sent, and 2^23 below
// the next epoch's first number, far outside a receiver's replay window: a will the broker delivers after the device
// reconnected is refused.
class SequenceCounter {
public:
    void newEpoch(int64_t unixSeconds);
    uint64_t next();
    uint64_t willSeq() const { return epoch_ << 24 | 0x800000; }
    uint64_t epoch() const { return epoch_; }

private:
    uint64_t epoch_ = 0;
    uint64_t counter_ = 0;
};

// Anti-replay for one sender: the highest number seen and a 64-entry window below it (each number accepted once;
// anything older than the window is refused).
class ReplayWindow {
public:
    bool accept(uint64_t seq);
    uint64_t highest() const { return top_; }

private:
    uint64_t top_ = 0;
    uint64_t bits_ = 0;   // bit i = top_ - i seen
    bool any_ = false;
};

class ReplayFilter {
public:
    bool accept(const std::string& device, uint64_t seq) { return windows_[device].accept(seq); }
    void forget(const std::string& device) { windows_.erase(device); }
    void clear() { windows_.clear(); }

private:
    std::unordered_map<std::string, ReplayWindow> windows_;
};

// ---- Session state --------------------------------------------------------------------------------------------------

enum class ItemKind { Song, File, Unsupported };   // File: plays from the host's disk; Unsupported: podcast / radio
enum class HostStatus { Idle, Loading, Playing, Paused, Error };

struct QueueItem {
    std::string id;
    std::string title;
    std::vector<std::string> artists, artistIds;
    std::string album, albumId;
    int64_t durationMs = 0;
    std::string cover;        // image URL
    std::string videoId;      // the YouTube video the host plays it from ("" = not known)
    std::string addedBy;      // the guest who queued it ("" = the host / the list)
    ItemKind kind = ItemKind::Song;
    bool operator==(const QueueItem&) const = default;
};

struct Member {
    std::string device, name;
    bool online = true;
    bool host = false;
    bool operator==(const Member&) const = default;
};

struct SessionState {
    uint64_t revision = 0;
    std::string hostDevice, hostName;
    std::string broker;                 // where the host is connected
    std::string movedTo;                // the last state left on a broker the room moved away from
    bool guestsControl = false;         // "Misafirler çalmayı kontrol edebilir"
    std::vector<std::string> bans;      // removed devices
    std::vector<Member> members;        // the host first
    std::vector<QueueItem> queue;       // the current item, then up to kQueueWindow upcoming ones
    int current = 0;                    // index of the current item in `queue`
    int64_t positionMs = 0;             // the host's position at atHostMs
    int64_t atHostMs = 0;               // the host's steady clock (ms)
    bool playing = false;               // the host plays or is about to (Loading)
    HostStatus status = HostStatus::Idle;
    uint64_t queueGen = 0;
    std::string contextName;            // what the host plays from ("Gece Sürüşü")

    const QueueItem* currentItem() const {
        return current >= 0 && current < static_cast<int>(queue.size()) ? &queue[static_cast<size_t>(current)] : nullptr;
    }
    bool banned(const std::string& device) const;
};

nlohmann::json toJson(const QueueItem& item);
std::optional<QueueItem> itemFromJson(const nlohmann::json& j);
nlohmann::json toJson(const SessionState& s);
std::optional<SessionState> stateFromJson(const nlohmann::json& j);
const char* statusName(HostStatus s);

// ---- Guest requests -------------------------------------------------------------------------------------------------

enum class RequestOp { Add, Play, Pause, Next, Previous, Seek, Jump };
struct Request {
    RequestOp op = RequestOp::Add;
    QueueItem item;          // Add
    int64_t ms = 0;          // Seek
    int index = -1;          // Jump: index in the state's queue
    std::string itemId;      // Jump: the id at that index (stale requests are refused)
};
nlohmann::json toJson(const Request& r);
std::optional<Request> requestFromJson(const nlohmann::json& j);
const char* opName(RequestOp op);
// Guest-controlled ops (everything but Add) need SessionState::guestsControl.
inline bool needsControl(RequestOp op) { return op != RequestOp::Add; }

// ---- New invite hand-off --------------------------------------------------------------------------------------------
// The host's new invite link for one guest: sealed with SHA-256(ECDH(host ephemeral key, guest key) || label), the
// guest's device id as associated data. Body: {"to", "epk" (base64url), "ct" (base64url)}.
nlohmann::json sealHandoff(const EcdhKey& hostEphemeral, std::span<const uint8_t> guestPublic, const std::string& guestDevice,
                           const std::string& link);
std::optional<std::string> openHandoff(const EcdhKey& guestKey, const nlohmann::json& body, const std::string& myDevice);

} // namespace st::app::jam
