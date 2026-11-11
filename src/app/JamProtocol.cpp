// Listen together: envelopes, sequence numbers, replay windows, the session state and requests (see JamProtocol.h).
#include "app/JamProtocol.h"

#include "app/JamInvite.h"

#include <algorithm>

namespace st::app::jam {

using json = nlohmann::json;

namespace {

constexpr uint8_t kMagic = 'J';
constexpr uint8_t kWireVersion = 1;
constexpr size_t kMaxPlaintext = 512 * 1024;
constexpr size_t kMaxText = 512;

Bytes aadFor(std::string_view topic) {
    Bytes a{'S', 'T', 'J', '1', '|'};
    a.insert(a.end(), topic.begin(), topic.end());
    return a;
}

std::string str(const json& j, const char* key, size_t max = kMaxText) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    std::string s = it->get<std::string>();
    if (s.size() > max) s.resize(max);
    return s;
}

int64_t num(const json& j, const char* key, int64_t def = 0) {
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    if (it == j.end()) return def;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(std::min<uint64_t>(it->get<uint64_t>(), INT64_MAX));
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    return def;
}

uint64_t unum(const json& j, const char* key) {
    if (!j.is_object()) return 0;
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) return static_cast<uint64_t>(std::max<int64_t>(0, it->get<int64_t>()));
    return 0;
}

bool flag(const json& j, const char* key) {
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::vector<std::string> strings(const json& j, const char* key, size_t maxItems) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (out.size() >= maxItems) break;
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (s.size() > kMaxText) s.resize(kMaxText);
            out.push_back(std::move(s));
        }
    }
    return out;
}

bool deviceId(std::string_view s) {
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

const char* kindName(ItemKind k) { return k == ItemKind::File ? "f" : k == ItemKind::Unsupported ? "x" : "s"; }
ItemKind kindFrom(std::string_view s) { return s == "f" ? ItemKind::File : s == "x" ? ItemKind::Unsupported : ItemKind::Song; }

HostStatus statusFrom(std::string_view s) {
    if (s == "loading") return HostStatus::Loading;
    if (s == "playing") return HostStatus::Playing;
    if (s == "paused") return HostStatus::Paused;
    if (s == "error") return HostStatus::Error;
    return HostStatus::Idle;
}

} // namespace

Topics topicsFor(const Bytes& roomId) {
    Topics t;
    t.base = topicBase(roomId);
    t.state = t.base + "/state";
    t.host = t.base + "/h";
    t.guest = t.base + "/g";
    t.all = t.base + "/#";
    return t;
}

// ---- Envelope -------------------------------------------------------------------------------------------------------

Bytes sealEnvelope(std::span<const uint8_t> key, std::string_view topic, const Envelope& e) {
    const json j{{"v", kProtocolVersion}, {"t", e.type}, {"d", e.device}, {"s", e.seq}, {"b", e.body}};
    const std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
    Bytes wire{kMagic, kWireVersion};
    const Bytes sealed = seal(key, asBytes(text), aadFor(topic));
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    return wire;
}

std::optional<Envelope> openEnvelope(std::span<const uint8_t> key, std::string_view topic, std::span<const uint8_t> wire,
                                     OpenError* why) {
    auto fail = [&](OpenError e) -> std::optional<Envelope> {
        if (why) *why = e;
        return std::nullopt;
    };
    if (wire.size() < 2 + kNonceBytes + kTagBytes || wire[0] != kMagic) return fail(OpenError::Malformed);
    if (wire[1] != kWireVersion) return fail(OpenError::Version);
    if (wire.size() > kMaxPlaintext + 2 + kNonceBytes + kTagBytes) return fail(OpenError::Malformed);
    const auto plain = open(key, wire.subspan(2), aadFor(topic));
    if (!plain) return fail(OpenError::Tampered);
    const json j = json::parse(plain->begin(), plain->end(), nullptr, false);
    if (!j.is_object()) return fail(OpenError::Malformed);
    if (num(j, "v") != kProtocolVersion) return fail(OpenError::Version);
    Envelope e;
    e.type = str(j, "t", 32);
    e.device = str(j, "d", 32);
    e.seq = unum(j, "s");
    if (const auto b = j.find("b"); b != j.end() && b->is_object()) e.body = *b;
    if (e.type.empty() || !deviceId(e.device) || e.seq == 0) return fail(OpenError::Malformed);
    if (why) *why = OpenError::None;
    return e;
}

void SequenceCounter::newEpoch(int64_t unixSeconds) {
    const uint64_t e = static_cast<uint64_t>(std::max<int64_t>(1, unixSeconds));
    epoch_ = std::max(e, epoch_ + 1);
    counter_ = 0;
}

uint64_t SequenceCounter::next() {
    if (epoch_ == 0) newEpoch(1);
    if (++counter_ >= 0x800000) newEpoch(static_cast<int64_t>(epoch_ + 1));   // never reaches the will's number
    if (counter_ == 0) counter_ = 1;
    return epoch_ << 24 | counter_;
}

bool ReplayWindow::accept(uint64_t seq) {
    if (seq == 0) return false;
    if (!any_) {
        any_ = true;
        top_ = seq;
        bits_ = 1;
        return true;
    }
    if (seq > top_) {
        const uint64_t shift = seq - top_;
        bits_ = shift >= 64 ? 1 : (bits_ << shift) | 1;
        top_ = seq;
        return true;
    }
    const uint64_t back = top_ - seq;
    if (back >= 64) return false;   // older than the window
    const uint64_t bit = 1ull << back;
    if (bits_ & bit) return false;  // seen
    bits_ |= bit;
    return true;
}

// ---- Session state --------------------------------------------------------------------------------------------------

bool SessionState::banned(const std::string& device) const {
    return std::find(bans.begin(), bans.end(), device) != bans.end();
}

const char* statusName(HostStatus s) {
    switch (s) {
    case HostStatus::Loading: return "loading";
    case HostStatus::Playing: return "playing";
    case HostStatus::Paused: return "paused";
    case HostStatus::Error: return "error";
    case HostStatus::Idle: break;
    }
    return "idle";
}

json toJson(const QueueItem& i) {
    json j{{"i", i.id}, {"n", i.title}, {"a", i.artists}, {"d", i.durationMs}, {"k", kindName(i.kind)}};
    if (!i.artistIds.empty()) j["ai"] = i.artistIds;
    if (!i.album.empty()) j["al"] = i.album;
    if (!i.albumId.empty()) j["ali"] = i.albumId;
    if (!i.cover.empty()) j["c"] = i.cover;
    if (!i.videoId.empty()) j["v"] = i.videoId;
    if (!i.addedBy.empty()) j["by"] = i.addedBy;
    return j;
}

std::optional<QueueItem> itemFromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    QueueItem i;
    i.id = str(j, "i");
    i.title = str(j, "n");
    i.artists = strings(j, "a", 16);
    i.artistIds = strings(j, "ai", 16);
    i.album = str(j, "al");
    i.albumId = str(j, "ali");
    i.durationMs = std::clamp<int64_t>(num(j, "d"), 0, 24ll * 3600 * 1000);
    i.cover = str(j, "c", 2048);
    i.videoId = str(j, "v", 32);
    i.addedBy = str(j, "by", 64);
    i.kind = kindFrom(str(j, "k", 4));
    if (i.id.empty() || i.title.empty()) return std::nullopt;
    return i;
}

json toJson(const SessionState& s) {
    json members = json::array();
    for (const auto& m : s.members) members.push_back({{"d", m.device}, {"n", m.name}, {"o", m.online}, {"h", m.host}});
    json queue = json::array();
    for (const auto& i : s.queue) queue.push_back(toJson(i));
    json j{{"rev", s.revision}, {"hd", s.hostDevice}, {"hn", s.hostName}, {"br", s.broker}, {"gc", s.guestsControl},
           {"bans", s.bans}, {"m", std::move(members)}, {"q", std::move(queue)}, {"cur", s.current}, {"pos", s.positionMs},
           {"at", s.atHostMs}, {"pl", s.playing}, {"st", statusName(s.status)}, {"qg", s.queueGen}, {"ctx", s.contextName}};
    if (!s.movedTo.empty()) j["mv"] = s.movedTo;
    return j;
}

std::optional<SessionState> stateFromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    SessionState s;
    s.revision = unum(j, "rev");
    s.hostDevice = str(j, "hd", 32);
    s.hostName = str(j, "hn", 64);
    s.broker = str(j, "br", 256);
    s.movedTo = str(j, "mv", 256);
    s.guestsControl = flag(j, "gc");
    s.bans = strings(j, "bans", 256);
    if (const auto m = j.find("m"); m != j.end() && m->is_array())
        for (const auto& v : *m) {
            if (s.members.size() >= 32) break;
            Member mem{str(v, "d", 32), str(v, "n", 64), flag(v, "o"), flag(v, "h")};
            if (deviceId(mem.device)) s.members.push_back(std::move(mem));
        }
    if (const auto q = j.find("q"); q != j.end() && q->is_array())
        for (const auto& v : *q) {
            if (s.queue.size() > static_cast<size_t>(kQueueWindow) + 1) break;
            if (auto item = itemFromJson(v)) s.queue.push_back(std::move(*item));
        }
    s.current = static_cast<int>(std::clamp<int64_t>(num(j, "cur"), 0, std::max<int64_t>(0, static_cast<int64_t>(s.queue.size()) - 1)));
    s.positionMs = std::max<int64_t>(0, num(j, "pos"));
    s.atHostMs = num(j, "at");
    s.playing = flag(j, "pl");
    s.status = statusFrom(str(j, "st", 16));
    s.queueGen = unum(j, "qg");
    s.contextName = str(j, "ctx", 256);
    if (!deviceId(s.hostDevice) || s.revision == 0) return std::nullopt;
    return s;
}

// ---- Requests -------------------------------------------------------------------------------------------------------

const char* opName(RequestOp op) {
    switch (op) {
    case RequestOp::Add: return "add";
    case RequestOp::Play: return "play";
    case RequestOp::Pause: return "pause";
    case RequestOp::Next: return "next";
    case RequestOp::Previous: return "prev";
    case RequestOp::Seek: return "seek";
    case RequestOp::Jump: return "jump";
    }
    return "add";
}

json toJson(const Request& r) {
    json j{{"op", opName(r.op)}};
    if (r.op == RequestOp::Add) j["item"] = toJson(r.item);
    if (r.op == RequestOp::Seek) j["ms"] = r.ms;
    if (r.op == RequestOp::Jump) {
        j["index"] = r.index;
        j["id"] = r.itemId;
    }
    return j;
}

std::optional<Request> requestFromJson(const json& j) {
    const std::string op = str(j, "op", 8);
    Request r;
    if (op == "add") r.op = RequestOp::Add;
    else if (op == "play") r.op = RequestOp::Play;
    else if (op == "pause") r.op = RequestOp::Pause;
    else if (op == "next") r.op = RequestOp::Next;
    else if (op == "prev") r.op = RequestOp::Previous;
    else if (op == "seek") r.op = RequestOp::Seek;
    else if (op == "jump") r.op = RequestOp::Jump;
    else return std::nullopt;
    if (r.op == RequestOp::Add) {
        const auto it = j.find("item");
        auto item = it == j.end() ? std::nullopt : itemFromJson(*it);
        if (!item) return std::nullopt;
        item->addedBy.clear();   // the host names the guest itself
        r.item = std::move(*item);
    }
    if (r.op == RequestOp::Seek) r.ms = std::max<int64_t>(0, num(j, "ms"));
    if (r.op == RequestOp::Jump) {
        r.index = static_cast<int>(std::clamp<int64_t>(num(j, "index", -1), -1, kQueueWindow + 1));
        r.itemId = str(j, "id");
        if (r.index < 0 || r.itemId.empty()) return std::nullopt;
    }
    return r;
}

// ---- Hand-off -------------------------------------------------------------------------------------------------------

namespace {
constexpr std::string_view kHandoffLabel = "shadetube-jam-handoff-v1";
}

json sealHandoff(const EcdhKey& hostEphemeral, std::span<const uint8_t> guestPublic, const std::string& guestDevice,
                 const std::string& link) {
    const auto key = hostEphemeral.derive(guestPublic, kHandoffLabel);
    if (!key) return {};
    const Bytes ct = seal(*key, asBytes(link), asBytes(guestDevice));
    return {{"to", guestDevice}, {"epk", base64url(hostEphemeral.publicKey())}, {"ct", base64url(ct)}};
}

std::optional<std::string> openHandoff(const EcdhKey& guestKey, const json& body, const std::string& myDevice) {
    if (str(body, "to", 32) != myDevice) return std::nullopt;
    const auto epk = fromBase64url(str(body, "epk", 128));
    const auto ct = fromBase64url(str(body, "ct", 2048));
    if (!epk || !ct) return std::nullopt;
    const auto key = guestKey.derive(*epk, kHandoffLabel);
    if (!key) return std::nullopt;
    const auto plain = open(*key, *ct, asBytes(myDevice));
    if (!plain) return std::nullopt;
    return std::string(plain->begin(), plain->end());
}

} // namespace st::app::jam
