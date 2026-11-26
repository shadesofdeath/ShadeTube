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
// "move", and the host's last will ("bye"; reason "lost", or "away" when the host's app closes and will resume the
// room after its restart). <base>/g guests -> host: "hello", "hb", "ping", "req", "bye" (also the guest's last will).
//
// Versions: the envelope's "v" stays 1 — a 0.10.0 app drops every envelope with another value, so bumping it would
// turn an old peer away with "session not found". New abilities are announced by the feature level instead ("pv" in
// the state and in the guest's hello, absent = 1) and travel in fields older apps ignore (their parsers read only the
// keys they know). Level 2 (0.11.0): podcast episodes and radio stations are shared — they keep "k":"x" (an older guest
// still holds on them as unsupported instead of searching YouTube for an episode title) and add "pe" / "rs" with what a
// newer guest needs to play them; the host's speed for episodes ("sp"); a host away for a restart ("away").
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

inline constexpr int kProtocolVersion = 1;   // the envelope's "v" (see above: never bumped)
inline constexpr int kFeatureLevel = 2;      // "pv": what this app understands beyond 0.10.0's level 1
inline constexpr int kMaxMembers = 10;     // the host included
// A host whose app closed comes back to the same room within this time (its key is kept with DPAPI); guests wait that
// long (plus a minute for a slow start) before they give up on it.
inline constexpr int64_t kResumeWindowSec = 10 * 60;
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

// File: plays from the host's disk. Episode / Station (level 2): a podcast episode / a live radio station with what a
// guest needs to play it (on the wire "k":"x" + "pe" / "rs"). Unsupported: something a guest can't follow (an episode
// or station from a level-1 host, or one whose data the host didn't have).
enum class ItemKind { Song, File, Unsupported, Episode, Station };
enum class HostStatus { Idle, Loading, Playing, Paused, Error };

struct EpisodeRef {
    std::string feedUrl;      // the show's RSS feed
    std::string guid;         // the item's guid (the episode id is FNV-1a of feed URL + guid)
    std::string url;          // the enclosure (audio), http(s)
    std::string mimeType;
    std::string show;
    int64_t published = 0;    // unix seconds
    bool operator==(const EpisodeRef&) const = default;
};

struct StationRef {
    std::string uuid;         // radio-browser.info stationuuid
    std::string url;          // the stream the host plays, http(s)
    std::string codec;        // "MP3", "AAC", ...
    std::string countryCode;
    std::vector<std::string> tags;
    int bitrate = 0;
    bool hls = false;
    bool operator==(const StationRef&) const = default;
};

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
    EpisodeRef episode;       // kind Episode
    StationRef station;       // kind Station
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
    int featureLevel = 1;               // "pv": the host's feature level (1 = a 0.10.0 host)
    double speed = 1.0;                 // "sp": the current item's speed (podcast episodes; songs always play at 1)
    int64_t awaySince = 0;              // "away": unix seconds since the host's app closed to resume later (0 = here)

    const QueueItem* currentItem() const {
        return current >= 0 && current < static_cast<int>(queue.size()) ? &queue[static_cast<size_t>(current)] : nullptr;
    }
    bool banned(const std::string& device) const;
};

// Live (Station): no position, no seeking — following it means playing the same station.
inline bool isLive(ItemKind k) { return k == ItemKind::Station; }

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

// ---- A host's session across a restart ------------------------------------------------------------------------------
// What a host keeps (app/Together: jam-host.dat, sealed with DPAPI) so that its next start resumes the same room with
// the same key: written when the session starts or changes, every minute while it runs and when the app closes;
// removed when the session ends. A start within kResumeWindowSec of savedAt resumes it.
struct SavedMember {
    std::string device, name;
    Bytes publicKey;                    // its ECDH key (new invite hand-offs)
    bool operator==(const SavedMember&) const = default;
};
struct SavedHost {
    Bytes roomId, key;
    std::string broker;                 // where the session was (tried first)
    uint64_t revision = 0;              // the next state continues above it (guests drop older revisions)
    bool guestsControl = false;
    std::vector<std::string> bans;
    std::vector<SavedMember> members;
    std::vector<std::pair<std::string, std::string>> addedBy;   // track id -> the guest who added it
    int64_t savedAt = 0;                // unix seconds
    bool away = false;                  // the app closed (guests were told it will be back), not a crash
    bool operator==(const SavedHost&) const = default;
};
nlohmann::json toJson(const SavedHost& s);
std::optional<SavedHost> savedHostFromJson(const nlohmann::json& j);
bool resumable(const SavedHost& s, int64_t nowUnix);   // saved within the window (a clock a minute behind is fine)

// ---- New invite hand-off --------------------------------------------------------------------------------------------
// The host's new invite link for one guest: sealed with SHA-256(ECDH(host ephemeral key, guest key) || label), the
// guest's device id as associated data. Body: {"to", "epk" (base64url), "ct" (base64url)}.
nlohmann::json sealHandoff(const EcdhKey& hostEphemeral, std::span<const uint8_t> guestPublic, const std::string& guestDevice,
                           const std::string& link);
std::optional<std::string> openHandoff(const EcdhKey& guestKey, const nlohmann::json& body, const std::string& myDevice);

} // namespace st::app::jam
