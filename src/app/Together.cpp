// "Birlikte dinle": the listen-together session on the UI thread (see app/Together.h).
#include "app/Together.h"

#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/JamCrypto.h"
#include "app/JamInvite.h"
#include "app/JamMqtt.h"
#include "app/JamProtocol.h"
#include "app/JamSync.h"
#include "app/WinShell.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#define SECURITY_WIN32
#include <security.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "secur32.lib")

namespace st::app::together {

namespace {

namespace fs = std::filesystem;
using namespace st::app::jam;
using json = nlohmann::json;
using player::Player;

constexpr const char* kTag = "jam";
constexpr int64_t kTickMs = 250;
constexpr int64_t kHeartbeatMs = 10'000;
constexpr int64_t kOfflineMs = 35'000;        // no word from a member for this long: offline
constexpr int64_t kPruneMs = 180'000;         // offline members leave the list after this
constexpr int64_t kRepublishMs = 30'000;      // the host's state is re-published at least this often
constexpr int64_t kHostSilenceMs = 35'000;    // a guest that hears nothing from the host tries the next broker
constexpr int64_t kJoinWaitMs = 15'000;       // ... and so does one that finds no session on a broker
constexpr int64_t kPingFastMs = 700, kPingSlowMs = 15'000;
constexpr int kFastPings = 5;
constexpr int kMaxAddsPerAction = 20;
constexpr size_t kMaxNameChars = 40;

enum class Leave { Left, Ended, Kicked, Full, Banned, NotFound, Replaced, Exit };

struct Member {
    std::string device;
    std::string name;
    bool online = true;
    int64_t lastSeen = 0;   // steady ms
    Bytes pk;               // its ECDH key (new invite hand-offs)
};

// Sequence numbers are taken on the UI thread (messages) and on the client's thread (each connection's last will).
struct SeqState {
    std::mutex m;
    SequenceCounter c;
    uint64_t next() {
        std::lock_guard lk(m);
        return c.next();
    }
};

struct Session {
    Role role = Role::None;
    Invite invite;
    Topics topics;
    std::shared_ptr<mqtt::Client> client;
    std::shared_ptr<SeqState> seq;
    Link link = Link::Off;
    std::string brokerUrl;          // in use
    ReplayFilter replay;
    int64_t startedAt = 0;
    int64_t nextHeartbeat = 0;
    // Host
    std::vector<Member> members;    // guests only
    std::vector<std::string> bans;
    bool guestsControl = false;
    uint64_t revision = 0;
    int64_t publishDue = -1;        // steady ms; -1 = nothing pending
    int64_t lastPublish = 0;
    SessionState published;         // what the guests last got (for the drift check)
    bool announced = false;         // "Oturum hazır" was shown
    std::string lastItemId;         // to warn once per unsupported item
    std::unordered_map<std::string, std::string> addedBy;   // track id -> guest name
    // Guest
    std::optional<SessionState> state;
    std::string hostDevice;
    int64_t lastHostMsg = 0;
    int64_t connectedAt = 0;        // the current connection
    int brokerTries = 0;            // brokers tried without finding the session
    ClockEstimator clock;
    int pingsSent = 0;
    int64_t nextPing = 0;
    std::map<uint32_t, int64_t> pings;   // id -> t1
    uint32_t pingId = 0;
    SyncMemory sync;
    int64_t lastReconcile = 0;
    Decision::Hold hold = Decision::Hold::None;
    bool hasDrift = false;
    int64_t driftMs = 0;
    int64_t nextDriftLog = 0;
    std::optional<EcdhKey> myKey;
    bool hostLost = false;
    int64_t lastToastAt = 0;        // throttles "only the host controls" toasts
};

Session g;
Lifetime g_life;                    // renewed when a session ends: the old client's posted events are dropped
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_listeners;
UINT_PTR g_timer = 0;
std::string g_device;
std::string g_inviteFile;           // SHADETUBE_JAM_INVITE_FILE (dev)

// ---- small helpers --------------------------------------------------------------------------------------------------

int64_t now() { return steadyMs(); }

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

fs::path stateFile() { return paths::appData() / L"jam.json"; }
fs::path ownSessionFile() { return paths::appData() / L"jam-own-session.json"; }

json readJson(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return json::object();
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_object() ? j : json::object();
}

void writeJson(const fs::path& p, const json& j) {
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(2);
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

const std::string& deviceId() {
    if (g_device.empty()) {
        json j = readJson(stateFile());
        std::string d = j.value("device", "");
        if (d.size() != 16 || !fromHex(d)) {
            d = toHex(randomBytes(8));
            j["device"] = d;
            writeJson(stateFile(), j);
        }
        g_device = d;
    }
    return g_device;
}

std::string clip(std::string_view s) {
    std::wstring w = toWide(s);
    while (!w.empty() && iswspace(w.back())) w.pop_back();
    while (!w.empty() && iswspace(w.front())) w.erase(w.begin());
    if (w.size() > kMaxNameChars) w.resize(kMaxNameChars);
    return toUtf8(w);
}

std::string windowsFirstName() {
    wchar_t buf[256];
    ULONG n = 256;
    std::wstring name;
    if (GetUserNameExW(NameDisplay, buf, &n) && n > 0) name.assign(buf, n);
    if (name.empty()) {
        DWORD m = 256;
        if (GetUserNameW(buf, &m) && m > 1) name.assign(buf, m - 1);
    }
    // "Berkay AY" -> "Berkay"; "AY, Berkay" (directory form) -> "Berkay".
    if (const size_t comma = name.find(L','); comma != std::wstring::npos) name = name.substr(comma + 1);
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (const size_t sp = name.find(L' '); sp != std::wstring::npos) name.resize(sp);
    return toUtf8(name);
}

}   // namespace

std::string defaultName() {
    if (ctx().session && ctx().session->loggedIn()) {
        const auto& p = ctx().session->profile();
        if (!p.name.empty()) return clip(p.name);
    }
    const std::string w = clip(windowsFirstName());
    return w.empty() ? std::string("ShadeTube") : w;
}

namespace {

std::string myName() {
    const std::string set = clip(Settings::get().jamName);
    return set.empty() ? defaultName() : set;
}

void notify() {
    std::erase_if(g_listeners, [](const auto& l) { return l.first.expired(); });
    auto copy = g_listeners;
    for (auto& [ref, fn] : copy)
        if (!ref.expired()) fn();
    if (auto* p = ctx().player; p && p->onChanged) p->onChanged();   // the player bar shows the session
    if (ctx().window) ctx().window->invalidate();
}

void toastIn(const std::wstring& text, bool error = false) { toast(text, error, true); }

// Message bodies come from other members: never trust their types.
std::string jstr(const json& j, const char* key) {
    const auto it = j.is_object() ? j.find(key) : j.end();
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
int64_t jint(const json& j, const char* key) {
    const auto it = j.is_object() ? j.find(key) : j.end();
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(std::min<uint64_t>(it->get<uint64_t>(), INT64_MAX));
    return 0;
}

Player* player() { return ctx().player; }

// ---- messages -------------------------------------------------------------------------------------------------------

bool send(const std::string& topic, const char* type, json body, bool retain = false) {
    if (!g.client || !g.seq || g.link != Link::Connected) return false;
    Envelope e;
    e.type = type;
    e.device = deviceId();
    e.seq = g.seq->next();
    e.body = std::move(body);
    return g.client->publish(topic, sealEnvelope(g.invite.key, topic, e), retain);
}

bool sendHost(const char* type, json body) { return send(g.topics.host, type, std::move(body)); }
bool sendGuest(const char* type, json body) { return send(g.topics.guest, type, std::move(body)); }

// ---- host: the state ----------------------------------------------------------------------------------------------------

ItemKind kindOf(const catalog::Track& t) {
    if (radio::isStationId(t.id) || catalog::isPodcastId(t.id)) return ItemKind::Unsupported;
    if (auto* p = player(); p && p->localFileFor && !p->localFileFor(t.id).empty()) return ItemKind::File;
    return ItemKind::Song;
}

QueueItem itemFor(const catalog::Track& t, bool current) {
    QueueItem i;
    i.id = t.id;
    i.title = t.name;
    for (const auto& a : t.artists) {
        i.artists.push_back(a.name);
        i.artistIds.push_back(a.id);
    }
    if (std::all_of(i.artistIds.begin(), i.artistIds.end(), [](const std::string& s) { return s.empty(); })) i.artistIds.clear();
    i.album = t.album.name;
    i.albumId = t.album.id;
    i.durationMs = t.durationMs;
    if (const auto* img = catalog::pickImage(t.album.images, 300)) i.cover = img->url;
    i.kind = kindOf(t);
    if (i.kind != ItemKind::Unsupported) {
        std::optional<youtube::Match> m;
        if (current && player()) m = player()->currentMatch();
        if ((!m || m->videoId.empty()) && ctx().matcher) m = ctx().matcher->cachedMatch(t.id);
        i.videoId = m && !m->videoId.empty() ? m->videoId : t.videoId;
    }
    if (auto it = g.addedBy.find(t.id); it != g.addedBy.end()) i.addedBy = it->second;
    return i;
}

HostStatus hostStatus(const Player& p) {
    switch (p.status()) {
    case player::Status::Resolving:
    case player::Status::Buffering: return HostStatus::Loading;
    case player::Status::Playing: return HostStatus::Playing;
    case player::Status::Paused: return HostStatus::Paused;
    case player::Status::Error: return HostStatus::Error;
    case player::Status::Idle: break;
    }
    return HostStatus::Idle;
}

SessionState buildState() {
    SessionState s;
    s.revision = ++g.revision;
    s.hostDevice = deviceId();
    s.hostName = myName();
    s.broker = g.brokerUrl;
    s.guestsControl = g.guestsControl;
    s.bans = g.bans;
    s.members.push_back({deviceId(), s.hostName, true, true});
    for (const auto& m : g.members) s.members.push_back({m.device, m.name, m.online, false});
    if (auto* p = player(); p && p->current()) {
        const int pos = p->currentOrderIndex();
        const auto& order = p->order();
        for (int i = pos; i < static_cast<int>(order.size()) && i <= pos + kQueueWindow; ++i)
            s.queue.push_back(itemFor(p->items()[static_cast<size_t>(order[static_cast<size_t>(i)])], i == pos));
        s.current = 0;
        s.positionMs = p->isLive() ? 0 : p->positionMs();
        s.atHostMs = now();
        s.playing = p->isPlaying() || p->status() == player::Status::Resolving;
        s.status = hostStatus(*p);
        s.queueGen = p->queueGeneration();
        s.contextName = toUtf8(p->context().name);
    } else {
        s.atHostMs = now();
    }
    return s;
}

void publishState() {
    g.publishDue = -1;
    if (g.role != Role::Host || g.link != Link::Connected) return;
    SessionState s = buildState();
    const bool sent = send(g.topics.state, "state", toJson(s), true);
    g.lastPublish = now();
    if (!sent) return;
    const auto* cur = s.currentItem();
    ST_LOG_INFO(kTag, "host: state rev {}: {} at {:.1f} s, {}{}, {} upcoming, {} guest(s)", s.revision,
                cur ? "\"" + cur->title + "\"" : std::string("nothing"), s.positionMs / 1000.0, statusName(s.status),
                cur && !cur->videoId.empty() ? " (video " + cur->videoId + ")" : std::string(),
                s.queue.empty() ? 0 : s.queue.size() - 1, g.members.size());
    g.published = std::move(s);
}

void schedulePublish(int64_t delayMs = 150) {
    if (g.role != Role::Host) return;
    const int64_t due = now() + delayMs;
    if (g.publishDue < 0 || due < g.publishDue) g.publishDue = due;
}

// ---- guest: following -------------------------------------------------------------------------------------------------

catalog::Track trackFor(const QueueItem& i) {
    catalog::Track t;
    t.id = i.id;
    t.name = i.title;
    for (size_t k = 0; k < i.artists.size(); ++k)
        t.artists.push_back({k < i.artistIds.size() ? i.artistIds[k] : std::string{}, i.artists[k]});
    t.album.name = i.album;
    t.album.id = i.albumId;
    if (!i.cover.empty()) t.album.images.push_back({i.cover, 300, 300});
    t.durationMs = static_cast<int>(std::min<int64_t>(i.durationMs, INT32_MAX));
    t.videoId = i.videoId;
    t.playable = true;
    return t;
}

// The host's exact video for a track id ("" = none known), from the last state.
std::string hostVideoFor(const std::string& id) {
    if (!g.state) return {};
    for (const auto& i : g.state->queue)
        if (i.id == id) return i.videoId;
    return {};
}

void pinVideos(const SessionState& s) {
    auto* m = ctx().matcher;
    if (!m) return;
    for (const auto& i : s.queue) {
        if (i.videoId.empty() || i.kind == ItemKind::Unsupported) continue;
        youtube::Match match;
        match.videoId = i.videoId;
        match.title = i.title;
        match.durationSec = static_cast<int>(i.durationMs / 1000);
        match.score = 100;
        match.thumbnailUrl = "https://i.ytimg.com/vi/" + i.videoId + "/mqdefault.jpg";
        m->pinForSession(i.id, std::move(match));
    }
}

std::vector<catalog::Track> tracksFrom(const SessionState& s, size_t first) {
    std::vector<catalog::Track> out;
    for (size_t k = first; k < s.queue.size(); ++k)
        if (s.queue[k].kind != ItemKind::Unsupported) out.push_back(trackFor(s.queue[k]));
    return out;
}

player::PlayContext contextFor(const SessionState& s) {
    return {"jam", i18n::format(tr(L"Birlikte · {}"), {toWide(s.hostName)})};
}

LocalView localView(const Player& p) {
    LocalView v;
    if (const auto* t = p.current()) v.currentId = t->id;
    const int pos = p.currentOrderIndex();
    const auto& order = p.order();
    for (int i = pos + 1; i < static_cast<int>(order.size()) && i <= pos + kQueueWindow + 1; ++i)
        v.upcomingIds.push_back(p.items()[static_cast<size_t>(order[static_cast<size_t>(i)])].id);
    const auto st = p.status();
    v.playing = p.isPlaying() || st == player::Status::Resolving;
    v.buffering = st == player::Status::Resolving || st == player::Status::Buffering;
    v.error = st == player::Status::Error;
    v.positionMs = p.positionMs();
    v.durationMs = p.durationMs();
    return v;
}

void reconcile() {
    auto* p = player();
    if (g.role != Role::Guest || !g.state || !p) return;
    if (g.hostLost) return;   // the host is gone for now: what plays keeps playing until it is back (or the guest leaves)
    const SessionState& s = *g.state;
    // Without a clock sample the host's positions mean nothing here (steady clocks start at boot): wait for a pong,
    // unless the host is paused (its position needs no clock).
    if (!g.clock.ready() && hostAdvancing(s)) {
        g.hold = Decision::Hold::Waiting;
        return;
    }
    const int64_t guestNow = now();
    g.lastReconcile = guestNow;
    const int64_t hostNow = guestNow + g.clock.offsetMs();
    const LocalView lv = localView(*p);
    const Decision d = decide(s, hostNow, guestNow, lv, g.sync);
    const auto prevHold = g.hold;
    g.hold = d.hold;
    g.hasDrift = d.hasDrift && hostAdvancing(s) && lv.playing;
    g.driftMs = d.driftMs;
    if (prevHold != d.hold) {
        notify();
        if (d.hold == Decision::Hold::Failed)
            ST_LOG_WARN(kTag, "guest: \"{}\" could not be played here; holding until the host moves on",
                        s.currentItem() ? s.currentItem()->title : std::string());
    }
    const QueueItem* cur = s.currentItem();
    Player::Unfiltered unfiltered(*p);
    if (d.load == Decision::Load::Queue && cur) {
        pinVideos(s);
        ST_LOG_INFO(kTag, "guest: load \"{}\" at {:.1f} s, {} (host {}, clock offset {} ms, rtt {} ms)", cur->title,
                    d.seekMs / 1000.0, d.autoplay ? "playing" : "paused", statusName(s.status), g.clock.offsetMs(),
                    g.clock.rttMs());
        p->playContextAt(tracksFrom(s, static_cast<size_t>(s.current)), 0, contextFor(s), std::max<int64_t>(0, d.seekMs), d.autoplay);
        return;
    }
    if (d.load == Decision::Load::Jump && cur) {
        pinVideos(s);
        ST_LOG_INFO(kTag, "guest: jump to the next item \"{}\"", cur->title);
        p->jumpTo(p->currentOrderIndex() + 1 + d.jumpIndex);
        if (d.pause) p->pause();
        return;
    }
    if (d.replaceUpcoming) {
        pinVideos(s);
        p->replaceUpcoming(tracksFrom(s, static_cast<size_t>(s.current) + 1), contextFor(s));
        ST_LOG_INFO(kTag, "guest: upcoming items replaced ({} item(s))", s.queue.size() > 0 ? s.queue.size() - 1 : 0);
    }
    if (d.seekMs >= 0) {
        ST_LOG_INFO(kTag, "guest: seek to {:.1f} s (drift {:+} ms, lead {} ms)", d.seekMs / 1000.0, d.driftMs, g.sync.seekLeadMs);
        p->seek(d.seekMs);
    }
    if (d.play) {
        ST_LOG_INFO(kTag, "guest: play (host plays)");
        p->play();
    }
    if (d.pause) {
        ST_LOG_INFO(kTag, "guest: pause ({})", d.hold == Decision::Hold::None ? "host paused" : "holding");
        p->pause();
    }
    if (g.hasDrift && guestNow >= g.nextDriftLog) {
        g.nextDriftLog = guestNow + 5000;
        ST_LOG_INFO(kTag, "guest: following \"{}\": drift {:+} ms at {:.1f} s (clock offset {} ms, rtt {} ms)",
                    cur ? cur->title : std::string(), g.driftMs, lv.positionMs / 1000.0, g.clock.offsetMs(), g.clock.rttMs());
    }
}

// ---- connection -------------------------------------------------------------------------------------------------------

void startTimer();
void stopTimer();
void onMessage(const mqtt::Publish& p, int64_t rxAt);
void onLinkState(mqtt::Client::State s, const std::string& broker, const std::string& detail);

// Hands the client to a worker that flushes what is queued (a goodbye) and closes it.
void dropClient(std::chrono::milliseconds flush = std::chrono::milliseconds(800), bool wait = false) {
    if (!g.client) return;
    auto c = std::move(g.client);
    g.client.reset();
    g.link = Link::Off;
    if (wait) {
        c->stop(flush);
        return;
    }
    background(Priority::Normal, [c, flush]() mutable {
        c->stop(flush);
        c.reset();
    });
}

void connect(const std::vector<std::string>& brokers) {
    dropClient();
    g_life.renew();
    g.topics = topicsFor(g.invite.roomId);
    g.seq = std::make_shared<SeqState>();
    g.replay.clear();
    g.link = Link::Connecting;
    mqtt::Client::Events ev;
    const auto ref = g_life.ref();
    ev.onState = [ref](mqtt::Client::State s, const std::string& b, const std::string& d) {
        Dispatcher::post([ref, s, b, d] {
            if (!ref.expired()) onLinkState(s, b, d);
        });
    };
    ev.onMessage = [ref](const mqtt::Publish& p) {
        const int64_t rxAt = steadyMs();
        Dispatcher::post([ref, p, rxAt] {
            if (!ref.expired()) onMessage(p, rxAt);
        });
    };
    mqtt::Client::Config cfg;
    cfg.brokers = brokers;
    const bool host = g.role == Role::Host;
    cfg.subscriptions = host ? std::vector<std::string>{g.topics.guest} : std::vector<std::string>{g.topics.state, g.topics.host};
    cfg.keepAliveSec = 20;
    // Each connection gets its own last will, numbered past everything that connection sends (JamProtocol).
    cfg.willProvider = [seq = g.seq, key = g.invite.key, topic = host ? g.topics.host : g.topics.guest, dev = deviceId()]() {
        Envelope e;
        e.type = "bye";
        e.device = dev;
        {
            std::lock_guard lk(seq->m);
            seq->c.newEpoch(static_cast<int64_t>(std::time(nullptr)));
            e.seq = seq->c.willSeq();
        }
        e.body = {{"reason", "lost"}};
        return std::optional<mqtt::Will>(mqtt::Will{topic, sealEnvelope(key, topic, e), false});
    };
    g.client = std::make_shared<mqtt::Client>(std::move(ev));
    g.client->start(std::move(cfg));
    ST_LOG_INFO(kTag, "{}: connecting to room {} ({} broker(s), first {})", host ? "host" : "guest", roomTag(g.invite.roomId),
                brokers.size(), brokers.empty() ? std::string() : mqtt::brokerHost(brokers.front()));
}

void sayHello() {
    json body{{"n", myName()}};
    if (g.myKey) body["pk"] = base64url(g.myKey->publicKey());
    sendGuest("hello", std::move(body));
}

void sendPing() {
    const uint32_t id = ++g.pingId;
    const int64_t t1 = now();
    g.pings[id] = t1;
    while (g.pings.size() > 16) g.pings.erase(g.pings.begin());
    sendGuest("ping", {{"id", id}, {"t1", t1}});
    ++g.pingsSent;
    g.nextPing = t1 + (g.pingsSent < kFastPings ? kPingFastMs : kPingSlowMs);
}

void writeInviteFile() {
    if (g_inviteFile.empty() || g.role != Role::Host) return;
    std::ofstream f(fs::path(g_inviteFile), std::ios::binary | std::ios::trunc);
    f << formatInvite(g.invite);
}

void onLinkState(mqtt::Client::State s, const std::string& broker, const std::string& detail) {
    if (g.role == Role::None) return;
    const Link before = g.link;
    switch (s) {
    case mqtt::Client::State::Connecting: g.link = before == Link::Connected || before == Link::Reconnecting ? Link::Reconnecting : Link::Connecting; break;
    case mqtt::Client::State::Waiting: g.link = Link::Reconnecting; break;
    case mqtt::Client::State::Stopped: g.link = Link::Off; break;
    case mqtt::Client::State::Connected: g.link = Link::Connected; break;
    }
    if (!broker.empty()) g.brokerUrl = broker;
    if (s == mqtt::Client::State::Connected) {
        ST_LOG_INFO(kTag, "{}: connected to {}", g.role == Role::Host ? "host" : "guest", mqtt::brokerHost(broker));
        g.connectedAt = now();
        g.nextHeartbeat = now() + kHeartbeatMs;
        if (g.role == Role::Host) {
            const bool moved = g.invite.broker != broker;
            g.invite.broker = broker;   // the invite names the broker the session really is on
            publishState();
            if (moved) writeInviteFile();
            if (!g.announced) {
                g.announced = true;
                if (auto* w = ctx().window)
                    ui::Toasts::show(w, tr(L"Birlikte dinleme oturumu hazır. Davet bağlantısını kopyalayıp arkadaşlarına gönder."),
                                     ui::ToastKind::Success, tr(L"Kopyala"), [] { copyInvite(); });
            }
        } else {
            sayHello();
            g.pingsSent = 0;
            g.nextPing = now();
            sendPing();
        }
    } else if (!detail.empty()) {
        ST_LOG_INFO(kTag, "{}: link {} ({})", g.role == Role::Host ? "host" : "guest", s == mqtt::Client::State::Waiting ? "waiting" : "down",
                    detail);
    }
    if (g.link != before) notify();
}

// ---- guest: own queue ---------------------------------------------------------------------------------------------------

void saveOwnQueue() {
    auto* p = player();
    if (!p) return;
    p->saveSession();
    std::error_code ec;
    const fs::path session = paths::appData() / L"session.json";
    if (fs::exists(session, ec)) fs::copy_file(session, ownSessionFile(), fs::copy_options::overwrite_existing, ec);
    else fs::remove(ownSessionFile(), ec);
    json j = readJson(stateFile());
    j["guestSaved"] = {{"shuffle", p->shuffle()},
                       {"repeat", static_cast<int>(p->repeat())},
                       {"musicSpeed", Settings::get().musicSpeed},
                       {"queue", fs::exists(ownSessionFile(), ec)}};
    writeJson(stateFile(), j);
}

// The files only (exit / a crash recovered at startup): session.json and the playback settings as before the join.
// Returns the saved shuffle / repeat when there was a saved queue.
std::optional<std::pair<bool, RepeatMode>> restoreOwnQueueFiles() {
    json j = readJson(stateFile());
    const auto it = j.find("guestSaved");
    if (it == j.end() || !it->is_object()) return std::nullopt;
    const json saved = *it;
    j.erase("guestSaved");
    writeJson(stateFile(), j);
    std::error_code ec;
    const fs::path session = paths::appData() / L"session.json";
    if (saved.value("queue", false) && fs::exists(ownSessionFile(), ec))
        fs::copy_file(ownSessionFile(), session, fs::copy_options::overwrite_existing, ec);
    else
        fs::remove(session, ec);
    fs::remove(ownSessionFile(), ec);
    auto& s = Settings::get();
    s.musicSpeed = std::clamp(saved.value("musicSpeed", 1.f), 0.5f, 3.f);
    const bool shuffle = saved.value("shuffle", false);
    const RepeatMode repeat = static_cast<RepeatMode>(std::clamp(saved.value("repeat", 0), 0, 2));
    s.shuffle = shuffle;
    s.repeat = repeat;
    s.markDirty();
    return std::make_pair(shuffle, repeat);
}

void restoreOwnQueue() {
    auto* p = player();
    const auto saved = restoreOwnQueueFiles();
    if (!p) return;
    p->restoreSession();   // stops the session's song; the own queue comes back paused where it was
    if (saved) {
        p->setShuffle(saved->first);
        p->setRepeat(saved->second);
    }
    if (p->onChanged) p->onChanged();
}

// ---- filters (Player::commandFilter) ------------------------------------------------------------------------------------

void toastThrottled(const std::wstring& text) {
    if (now() - g.lastToastAt < 2500) return;
    g.lastToastAt = now();
    toast(text);
}

void requestAdd(const std::vector<catalog::Track>& tracks) {
    int n = 0;
    std::wstring first;
    for (const auto& t : tracks) {
        if (n >= kMaxAddsPerAction) break;
        if (!t.playable || t.name.empty()) continue;
        if (radio::isStationId(t.id) || catalog::isPodcastId(t.id)) {
            toastThrottled(tr(L"Podcast ve radyo yayınları birlikte dinlemede paylaşılmaz"));
            continue;
        }
        QueueItem item;
        item.id = t.id;
        item.title = t.name;
        for (const auto& a : t.artists) {
            item.artists.push_back(a.name);
            item.artistIds.push_back(a.id);
        }
        item.album = t.album.name;
        item.albumId = t.album.id;
        item.durationMs = t.durationMs;
        if (const auto* img = catalog::pickImage(t.album.images, 300)) item.cover = img->url;
        // The video this machine knows for it (a pasted YouTube link's `yt:` song needs exactly that one).
        if (auto m = ctx().matcher ? ctx().matcher->cachedMatch(t.id) : std::nullopt; m && t.id.rfind("yt:", 0) == 0)
            item.videoId = m->videoId;
        else if (t.id.rfind("yt:", 0) == 0)
            item.videoId = t.videoId.empty() ? t.id.substr(3) : t.videoId;
        Request r;
        r.op = RequestOp::Add;
        r.item = std::move(item);
        if (!sendGuest("req", toJson(r))) {
            toastThrottled(tr(L"Oturum sahibine şu an ulaşılamıyor; şarkı eklenemedi"));
            break;
        }
        if (n == 0) first = toWide(t.name);
        ++n;
    }
    if (n == 1) toast(i18n::format(tr(L"Oturumun sırasına eklendi: {}"), {first}));
    else if (n > 1) toast(i18n::plural(L"{} şarkı oturumun sırasına eklendi", n));
    if (n > 0) ST_LOG_INFO(kTag, "guest: asked the host to add {} song(s)", n);
}

void requestControl(RequestOp op, int64_t ms = 0, int index = -1, std::string itemId = {}) {
    Request r;
    r.op = op;
    r.ms = ms;
    r.index = index;
    r.itemId = std::move(itemId);
    sendGuest("req", toJson(r));
    ST_LOG_INFO(kTag, "guest: asked the host to {}", opName(op));
}

bool guestFilter(Player::Command c, const Player::CommandArgs& a) {
    using C = Player::Command;
    auto* p = player();
    switch (c) {
    case C::PlayContext: {
        if (!a.tracks || a.tracks->empty()) return true;
        const int i = a.index >= 0 && a.index < static_cast<int>(a.tracks->size()) ? a.index : (p ? std::max(0, p->firstPlayable(*a.tracks)) : 0);
        requestAdd({(*a.tracks)[static_cast<size_t>(i)]});
        return true;
    }
    case C::PlayNext:
    case C::Enqueue:
    case C::InsertAt:
        if (a.tracks) requestAdd(*a.tracks);
        return true;
    case C::Extend: return true;   // endless playback: the host's queue decides
    case C::Play:
    case C::Pause:
    case C::TogglePause:
    case C::Next:
    case C::Previous:
    case C::Seek:
    case C::JumpTo: {
        if (!guestCanControl()) {
            toastThrottled(tr(L"Çalmayı yalnızca oturum sahibi kontrol edebilir"));
            return true;
        }
        const bool hostPlays = g.state && g.state->playing;
        switch (c) {
        case C::Play: requestControl(RequestOp::Play); break;
        case C::Pause: requestControl(RequestOp::Pause); break;
        case C::TogglePause: requestControl(hostPlays ? RequestOp::Pause : RequestOp::Play); break;
        case C::Next: requestControl(RequestOp::Next); break;
        case C::Previous: requestControl(RequestOp::Previous); break;
        case C::Seek: requestControl(RequestOp::Seek, a.ms); break;
        case C::JumpTo:
            if (p && g.state && a.index >= 0 && a.index < static_cast<int>(p->order().size())) {
                const std::string id = p->items()[static_cast<size_t>(p->order()[static_cast<size_t>(a.index)])].id;
                for (size_t k = static_cast<size_t>(g.state->current); k < g.state->queue.size(); ++k)
                    if (g.state->queue[k].id == id) {
                        requestControl(RequestOp::Jump, 0, static_cast<int>(k), id);
                        break;
                    }
            }
            break;
        default: break;
        }
        return true;
    }
    case C::ReplaceUpcoming:
    case C::RemoveAt:
    case C::Move:
    case C::ClearQueue:
    case C::Shuffle:
    case C::Repeat: toastThrottled(tr(L"Birlikte dinlerken çalma sırasını oturum sahibi yönetir")); return true;
    case C::Speed: toastThrottled(tr(L"Birlikte dinlerken çalma hızı değiştirilemez")); return true;
    case C::UseMatch: toastThrottled(tr(L"Birlikte dinlerken oturum sahibinin seçtiği kayıt çalar")); return true;
    }
    return true;
}

bool hostFilter(Player::Command c, const Player::CommandArgs&) {
    if (c == Player::Command::Speed) {
        toastThrottled(tr(L"Birlikte dinlerken çalma hızı değiştirilemez"));
        return true;
    }
    return false;
}

void installFilter() {
    if (auto* p = player())
        p->commandFilter = [](Player::Command c, const Player::CommandArgs& a) {
            if (g.role == Role::Guest) return guestFilter(c, a);
            if (g.role == Role::Host) return hostFilter(c, a);
            return false;
        };
}

void removeFilter() {
    if (auto* p = player()) p->commandFilter = nullptr;
}

// Both sides play at 1x for the session; the guest also follows the host's order (no shuffle / repeat of its own).
void prepareGuestPlayer() {
    auto* p = player();
    if (!p) return;
    Player::Unfiltered u(*p);
    p->pause();
    if (p->shuffle()) p->setShuffle(false);
    if (p->repeat() != RepeatMode::Off) p->setRepeat(RepeatMode::Off);
    auto& s = Settings::get();
    if (std::fabs(s.musicSpeed - 1.f) > 0.01f) {
        s.musicSpeed = 1.f;
        s.markDirty();
    }
}

// ---- ending ---------------------------------------------------------------------------------------------------------

void resetSession() {
    stopTimer();
    removeFilter();
    if (ctx().matcher) ctx().matcher->clearSessionPins();
    g_life.renew();
    g = Session{};
}

void leaveGuest(Leave why, bool quiet = false) {
    if (g.role != Role::Guest) return;
    ST_LOG_INFO(kTag, "guest: leaving room {} ({})", roomTag(g.invite.roomId), static_cast<int>(why));
    if (why == Leave::Left || why == Leave::Exit) sendGuest("bye", {{"reason", "leave"}});
    dropClient(std::chrono::milliseconds(800), why == Leave::Exit);
    resetSession();
    if (why == Leave::Exit) {
        restoreOwnQueueFiles();
        return;
    }
    restoreOwnQueue();
    notify();
    if (quiet) return;
    switch (why) {
    case Leave::Left: toast(tr(L"Birlikte dinleme oturumundan ayrıldın")); break;
    case Leave::Ended: toastIn(tr(L"Oturum sahibi birlikte dinleme oturumunu sonlandırdı")); break;
    case Leave::Kicked: toastIn(tr(L"Oturum sahibi seni birlikte dinleme oturumundan çıkardı"), true); break;
    case Leave::Banned: toastIn(tr(L"Bu oturuma katılamazsın: oturum sahibi seni çıkarmış"), true); break;
    case Leave::Full: toastIn(tr(L"Oturum dolu: en fazla 10 kişi birlikte dinleyebilir"), true); break;
    case Leave::NotFound:
        toastIn(tr(L"Oturum bulunamadı. Bağlantı eskimiş ya da oturum sona ermiş olabilir."), true);
        break;
    case Leave::Replaced:
    case Leave::Exit: break;
    }
}

void endHost(bool exiting) {
    if (g.role != Role::Host) return;
    ST_LOG_INFO(kTag, "host: ending room {}", roomTag(g.invite.roomId));
    sendHost("end", json::object());
    if (g.client && g.link == Link::Connected) g.client->publish(g.topics.state, {}, true);   // clears the retained state
    dropClient(std::chrono::milliseconds(800), exiting);
    resetSession();
    if (exiting) return;
    notify();
    toast(tr(L"Birlikte dinleme oturumu sona erdi"));
}

// ---- host: guests ---------------------------------------------------------------------------------------------------

Member* memberFor(const std::string& device) {
    for (auto& m : g.members)
        if (m.device == device) return &m;
    return nullptr;
}

bool banned(const std::string& device) { return std::find(g.bans.begin(), g.bans.end(), device) != g.bans.end(); }

// Where a guest's song goes: after the songs guests added before it (still to come), else right after the current one.
int insertSlot(const Player& p) {
    const int pos = p.currentOrderIndex();
    int at = pos + 1;
    const auto& order = p.order();
    for (int i = pos + 1; i < static_cast<int>(order.size()) && i <= pos + kQueueWindow; ++i)
        if (g.addedBy.count(p.items()[static_cast<size_t>(order[static_cast<size_t>(i)])].id)) at = i + 1;
    return at;
}

void handleRequest(const Envelope& e, Member& m) {
    auto r = requestFromJson(e.body);
    auto* p = player();
    if (!r || !p) return;
    if (needsControl(r->op) && !g.guestsControl) {
        ST_LOG_INFO(kTag, "host: {} asked to {} without permission", m.name, opName(r->op));
        return;
    }
    switch (r->op) {
    case RequestOp::Add: {
        if (r->item.kind == ItemKind::Unsupported || radio::isStationId(r->item.id) || catalog::isPodcastId(r->item.id)) return;
        catalog::Track t = trackFor(r->item);
        g.addedBy[t.id] = m.name;
        const bool empty = !p->current();
        p->insertAt(insertSlot(*p), {t});
        ST_LOG_INFO(kTag, "host: {} added \"{}\"{}", m.name, t.name, empty ? " (plays now)" : "");
        toast(i18n::format(tr(L"{} sıraya ekledi: {}"), {toWide(m.name), toWide(t.name)}));
        break;
    }
    case RequestOp::Play: p->play(); break;
    case RequestOp::Pause: p->pause(); break;
    case RequestOp::Next: p->next(); break;
    case RequestOp::Previous: p->previous(); break;
    case RequestOp::Seek: p->seek(r->ms); break;
    case RequestOp::Jump: {
        const int slot = p->currentOrderIndex() + r->index;
        if (slot >= 0 && slot < static_cast<int>(p->order().size()) &&
            p->items()[static_cast<size_t>(p->order()[static_cast<size_t>(slot)])].id == r->itemId)
            p->jumpTo(slot);
        break;
    }
    }
    if (r->op != RequestOp::Add) ST_LOG_INFO(kTag, "host: {} -> {}", m.name, opName(r->op));
    schedulePublish(50);
}

void hostMessage(const Envelope& e, int64_t rxAt) {
    if (banned(e.device)) {
        if (e.type == "hello") sendHost("deny", {{"to", e.device}, {"why", "banned"}});
        return;
    }
    Member* m = memberFor(e.device);
    const std::string name = clip(jstr(e.body, "n"));
    if (e.type == "hello" || (e.type == "hb" && !m)) {
        if (!m) {
            if (static_cast<int>(g.members.size()) + 1 >= kMaxMembers) {
                // Room for one more only when an offline member can make way.
                auto off = std::find_if(g.members.begin(), g.members.end(), [](const Member& x) { return !x.online; });
                if (off == g.members.end()) {
                    sendHost("deny", {{"to", e.device}, {"why", "full"}});
                    ST_LOG_INFO(kTag, "host: refused a guest, the session is full");
                    return;
                }
                g.members.erase(off);
            }
            g.members.push_back({e.device, name.empty() ? std::string("?") : name, true, rxAt, {}});
            m = &g.members.back();
            ST_LOG_INFO(kTag, "host: {} joined ({} guest(s))", m->name, g.members.size());
            toast(i18n::format(tr(L"{} birlikte dinlemeye katıldı"), {toWide(m->name)}));
        } else if (!m->online) {
            m->online = true;
            toast(i18n::format(tr(L"{} birlikte dinlemeye katıldı"), {toWide(m->name)}));
        }
        if (const auto pk = fromBase64url(jstr(e.body, "pk")); pk && pk->size() == 64) m->pk = *pk;
        if (!name.empty()) m->name = name;
        m->lastSeen = rxAt;
        m->online = true;
        notify();
        publishState();
        return;
    }
    if (!m) return;   // only members speak
    m->lastSeen = rxAt;
    if (!m->online) {
        m->online = true;
        schedulePublish();
        notify();
    }
    if (e.type == "hb") {
        if (!name.empty() && name != m->name) {
            m->name = name;
            schedulePublish();
            notify();
        }
    } else if (e.type == "ping") {
        sendHost("pong", {{"to", e.device}, {"id", jint(e.body, "id")}, {"t1", jint(e.body, "t1")}, {"t2", rxAt}, {"t3", now()}});
    } else if (e.type == "req") {
        handleRequest(e, *m);
    } else if (e.type == "bye") {
        const bool left = jstr(e.body, "reason") == "leave";
        const std::string who = m->name;
        if (left) std::erase_if(g.members, [&](const Member& x) { return x.device == e.device; });
        else m->online = false;
        ST_LOG_INFO(kTag, "host: {} {}", who, left ? "left" : "lost the connection");
        toast(i18n::format(tr(L"{} birlikte dinlemeden ayrıldı"), {toWide(who)}));
        schedulePublish();
        notify();
    }
}

// ---- guest: host messages -------------------------------------------------------------------------------------------

void joinRoom(const Invite& inv, bool fresh);

void memberToasts(const SessionState& before, const SessionState& after) {
    auto onlineIn = [](const SessionState& s, const std::string& d) {
        for (const auto& m : s.members)
            if (m.device == d) return m.online;
        return false;
    };
    for (const auto& m : after.members)
        if (m.device != deviceId() && !m.host && m.online && !onlineIn(before, m.device))
            toast(i18n::format(tr(L"{} birlikte dinlemeye katıldı"), {toWide(m.name)}));
    for (const auto& m : before.members)
        if (m.device != deviceId() && !m.host && m.online && !onlineIn(after, m.device))
            toast(i18n::format(tr(L"{} birlikte dinlemeden ayrıldı"), {toWide(m.name)}));
}

void guestState(const Envelope& e) {
    auto s = stateFromJson(e.body);
    if (!s) return;
    if (!g.hostDevice.empty() && s->hostDevice != g.hostDevice) return;   // someone else's state in our room
    if (g.state && s->revision < g.state->revision && s->hostDevice == g.state->hostDevice) return;   // stale
    if (s->hostDevice != e.device) return;
    g.hostDevice = s->hostDevice;
    g.lastHostMsg = now();
    g.hostLost = false;
    if (s->banned(deviceId())) {
        leaveGuest(Leave::Banned);
        return;
    }
    if (!s->movedTo.empty() && s->movedTo != g.brokerUrl && mqtt::parseBrokerUrl(s->movedTo)) {
        ST_LOG_INFO(kTag, "guest: the session moved to {}", mqtt::brokerHost(s->movedTo));
        g.client->moveTo(s->movedTo);
        return;
    }
    const bool first = !g.state;
    if (!first) memberToasts(*g.state, *s);
    const auto* cur = s->currentItem();
    if (first || !g.state->currentItem() || !cur || g.state->currentItem()->id != cur->id || g.state->playing != s->playing ||
        g.state->status != s->status)
        ST_LOG_INFO(kTag, "guest: host state rev {}: {} at {:.1f} s, {}", s->revision, cur ? "\"" + cur->title + "\"" : std::string("nothing"),
                    s->positionMs / 1000.0, statusName(s->status));
    g.state = std::move(*s);
    g.brokerTries = 0;
    if (first) {
        ST_LOG_INFO(kTag, "guest: joined {}'s session ({} member(s))", g.state->hostName, g.state->members.size());
        toast(i18n::format(tr(L"{} ile birlikte dinliyorsun"), {toWide(g.state->hostName)}));
    }
    notify();
    reconcile();
}

void guestMessage(const mqtt::Publish& p, const Envelope& e, int64_t rxAt) {
    if (p.topic == g.topics.state) {
        if (e.type == "state") guestState(e);
        return;
    }
    // Host topic: only the host's device (once known) speaks there; pongs and hand-offs are addressed to us.
    if (!g.hostDevice.empty() && e.device != g.hostDevice) return;
    const std::string to = jstr(e.body, "to");
    if (e.type == "pong") {
        if (to != deviceId()) return;
        const auto it = g.pings.find(static_cast<uint32_t>(jint(e.body, "id")));
        if (it == g.pings.end()) return;
        const int64_t t1 = it->second;
        g.pings.erase(it);
        const bool wasReady = g.clock.ready();
        g.clock.add(t1, jint(e.body, "t2"), jint(e.body, "t3"), rxAt);
        if (!wasReady && g.clock.ready())
            ST_LOG_INFO(kTag, "guest: clock offset {} ms (rtt {} ms)", g.clock.offsetMs(), g.clock.rttMs());
        g.lastHostMsg = now();
        return;
    }
    if (g.hostDevice.empty()) return;   // nothing else before the host's state named it
    g.lastHostMsg = now();
    if (e.type == "hb") {
        if (g.hostLost) {
            g.hostLost = false;
            notify();
        }
    } else if (e.type == "end") {
        leaveGuest(Leave::Ended);
    } else if (e.type == "kick") {
        if (jstr(e.body, "d") == deviceId()) leaveGuest(Leave::Kicked);
    } else if (e.type == "deny") {
        if (to != deviceId()) return;
        leaveGuest(jstr(e.body, "why") == "full" ? Leave::Full : Leave::Banned);
    } else if (e.type == "handoff") {
        if (to != deviceId() || !g.myKey) return;
        const auto link = openHandoff(*g.myKey, e.body, deviceId());
        const auto inv = link ? parseInvite(*link) : std::nullopt;
        if (!inv) return;
        ST_LOG_INFO(kTag, "guest: the host moved the session to a new room {}", roomTag(inv->roomId));
        joinRoom(*inv, false);
    } else if (e.type == "move") {
        const std::string b = jstr(e.body, "broker");
        if (mqtt::parseBrokerUrl(b) && b != g.brokerUrl) g.client->moveTo(b);
    } else if (e.type == "bye") {
        g.hostLost = true;
        ST_LOG_INFO(kTag, "guest: the host lost its connection");
        notify();
    }
}

void onMessage(const mqtt::Publish& p, int64_t rxAt) {
    if (g.role == Role::None) return;
    // A cleared retained state (the session ended or moved to a new room): the "end" / hand-off messages say which.
    if (p.payload.empty()) return;
    OpenError why{};
    auto e = openEnvelope(g.invite.key, p.topic, p.payload, &why);
    if (!e) {
        ST_LOG_DEBUG(kTag, "dropped a message that isn't ours ({})", static_cast<int>(why));
        return;
    }
    if (e->device == deviceId()) return;   // our own, echoed by the broker
    if (!g.replay.accept(e->device, e->seq)) {
        ST_LOG_DEBUG(kTag, "dropped a replayed / old message from {}", e->device);
        return;
    }
    if (g.role == Role::Host) {
        if (p.topic == g.topics.guest) hostMessage(*e, rxAt);
    } else {
        guestMessage(p, *e, rxAt);
    }
}

// ---- the tick -----------------------------------------------------------------------------------------------------------

void hostTick() {
    const int64_t t = now();
    if (g.publishDue >= 0 && t >= g.publishDue) publishState();
    if (g.link == Link::Connected) {
        if (t - g.lastPublish >= kRepublishMs) publishState();
        // The published position drifted from where the player really is (a rebuffer, a slow start): say so.
        if (auto* p = player(); p && g.publishDue < 0 && g.published.currentItem() && p->current() &&
                                p->current()->id == g.published.currentItem()->id && hostAdvancing(g.published) &&
                                p->status() == player::Status::Playing) {
            const int64_t expected = expectedPosition(g.published, t);
            if (std::llabs(p->positionMs() - expected) > 400) schedulePublish(0);
        }
        if (t >= g.nextHeartbeat) {
            g.nextHeartbeat = t + kHeartbeatMs;
            sendHost("hb", {{"rev", g.revision}});
        }
    }
    bool changed = false;
    for (auto& m : g.members)
        if (m.online && t - m.lastSeen > kOfflineMs) {
            m.online = false;
            changed = true;
            ST_LOG_INFO(kTag, "host: {} went quiet", m.name);
            toast(i18n::format(tr(L"{} birlikte dinlemeden ayrıldı"), {toWide(m.name)}));
        }
    const size_t before = g.members.size();
    std::erase_if(g.members, [&](const Member& m) { return !m.online && t - m.lastSeen > kPruneMs; });
    if (changed || before != g.members.size()) {
        schedulePublish();
        notify();
    }
}

void guestTick() {
    const int64_t t = now();
    if (!g.state && t - g.startedAt > 90'000) {   // no broker answered with the session for a minute and a half
        leaveGuest(Leave::NotFound);
        return;
    }
    if (g.link == Link::Connected) {
        if (t >= g.nextHeartbeat) {
            g.nextHeartbeat = t + kHeartbeatMs;
            sendGuest("hb", {{"n", myName()}});
        }
        if (t >= g.nextPing) sendPing();
        // No session on this broker (yet): look on the next one; give up after two rounds.
        if (!g.state && t - g.connectedAt > kJoinWaitMs) {
            const int brokers = static_cast<int>(brokerOrder(g.invite.broker, Settings::get().jamBroker).size());
            if (++g.brokerTries >= 2 * std::max(1, brokers)) {
                leaveGuest(Leave::NotFound);
                return;
            }
            ST_LOG_INFO(kTag, "guest: no session on {}, trying the next broker", mqtt::brokerHost(g.brokerUrl));
            g.connectedAt = t;
            g.client->nextBroker();
        }
        // The host went silent (its broker may be down for it): look for it on the next broker.
        if (g.state && t - g.lastHostMsg > kHostSilenceMs && t - g.connectedAt > kHostSilenceMs) {
            if (!g.hostLost) {
                g.hostLost = true;
                notify();
            }
            ST_LOG_INFO(kTag, "guest: nothing from the host for {} s, trying the next broker", (t - g.lastHostMsg) / 1000);
            g.connectedAt = t;
            g.client->nextBroker();
        }
    }
    reconcile();
}

void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (g.role == Role::Host) hostTick();
    else if (g.role == Role::Guest) guestTick();
}

void startTimer() {
    if (!g_timer) g_timer = SetTimer(nullptr, 0, static_cast<UINT>(kTickMs), onTimer);
}

void stopTimer() {
    if (g_timer) KillTimer(nullptr, g_timer);
    g_timer = 0;
}

// ---- joining --------------------------------------------------------------------------------------------------------

void joinRoom(const Invite& inv, bool fresh) {
    if (fresh) {
        saveOwnQueue();
        g = Session{};
        g.role = Role::Guest;
        installFilter();
        prepareGuestPlayer();
        g.myKey = EcdhKey::generate();
    } else {
        // A new room of the same session (hand-off): keep following what plays.
        g.brokerTries = 0;
        g.lastHostMsg = now();
    }
    g.invite = inv;
    g.startedAt = now();
    connect(brokerOrder(inv.broker, Settings::get().jamBroker));
    startTimer();
    notify();
}

void hostLinkTo(const std::string& first) { connect(brokerOrder(first, Settings::get().jamBroker)); }

} // namespace

// =====================================================================================================================
// Public API

View view() {
    View v;
    v.role = g.role;
    v.link = g.link;
    v.broker = g.brokerUrl.empty() ? std::string() : mqtt::brokerHost(g.brokerUrl);
    if (g.role == Role::Host) {
        v.hostName = toWide(myName());
        v.invite = g.invite.broker.empty() ? std::string() : formatInvite(g.invite);
        v.members.push_back({deviceId(), toWide(myName()), true, true, true});
        for (const auto& m : g.members) v.members.push_back({m.device, toWide(m.name), m.online, false, false});
        v.guestsControl = g.guestsControl;
    } else if (g.role == Role::Guest) {
        v.joining = !g.state;
        if (g.state) {
            v.hostName = toWide(g.state->hostName);
            v.guestsControl = g.state->guestsControl;
            for (const auto& m : g.state->members) {
                const bool me = m.device == deviceId();
                v.members.push_back({m.device, me ? toWide(myName()) : toWide(m.name), me || m.online, m.host, me});
            }
        }
    }
    v.online = static_cast<int>(std::count_if(v.members.begin(), v.members.end(), [](const MemberView& m) { return m.online; }));
    return v;
}

Role role() { return g.role; }
bool active() { return g.role != Role::None; }
bool guest() { return g.role == Role::Guest; }
bool guestCanControl() { return g.role == Role::Guest && g.state && g.state->guestsControl; }

std::string displayName() { return myName(); }

std::wstring syncText() {
    if (g.role != Role::Guest) return {};
    if (g.link != Link::Connected && g.state) return tr(L"Bağlantı bekleniyor…");
    if (!g.state) return tr(L"Oturum aranıyor…");
    if (g.hostLost) return tr(L"Oturum sahibine ulaşılamıyor; bekleniyor…");
    switch (g.hold) {
    case Decision::Hold::Unsupported: return tr(L"Oturum sahibi bir podcast ya da radyo dinliyor; bunlar birlikte dinlemede paylaşılmaz.");
    case Decision::Hold::Failed: return tr(L"Bu şarkı burada çalınamadı; oturum sahibi sonrakine geçince devam edilir.");
    case Decision::Hold::Idle: return tr(L"Oturum sahibi şu an bir şey çalmıyor.");
    case Decision::Hold::Waiting: return tr(L"Eşitleniyor…");
    case Decision::Hold::None: break;
    }
    if (g.state->status == HostStatus::Loading) return tr(L"Eşitleniyor…");
    if (!hostAdvancing(*g.state)) return tr(L"Oturum sahibi duraklattı");
    if (!g.hasDrift) return tr(L"Eşitleniyor…");
    wchar_t b[16];
    swprintf(b, 16, L"%.2f", std::fabs(static_cast<double>(g.driftMs)) / 1000.0);
    std::wstring sec = b;
    std::replace(sec.begin(), sec.end(), L'.', i18n::decimalSeparator());
    return i18n::format(tr(L"Eşit · ±{} sn"), {sec});
}

bool syncOk() { return g.role == Role::Guest && g.state && g.hold == Decision::Hold::None && !g.hostLost && g.link == Link::Connected; }

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { g_listeners.emplace_back(std::move(owner), std::move(fn)); }

void startHosting() {
    if (g.role == Role::Host) return;
    if (g.role == Role::Guest) {
        ui::Dialog::confirm(ctx().window, tr(L"Kendi oturumunu başlat"),
                            tr(L"Şu an katıldığın oturumdan ayrılıp kendi birlikte dinleme oturumunu başlatmak istiyor musun?"),
                            tr(L"Ayrıl ve başlat"), [] {
                                leaveGuest(Leave::Left, true);
                                startHosting();
                            });
        return;
    }
    g = Session{};
    g.role = Role::Host;
    g.invite = newInvite("");
    g.startedAt = now();
    installFilter();
    // Both ends play at 1x (a guest can't follow another speed).
    if (auto& s = Settings::get(); std::fabs(s.musicSpeed - 1.f) > 0.01f) {
        s.musicSpeed = 1.f;
        s.markDirty();
        if (auto* p = player()) {
            Player::Unfiltered u(*p);
            p->setSpeed(1.f);
        }
    }
    hostLinkTo("");
    startTimer();
    ST_LOG_INFO(kTag, "host: session started (room {})", roomTag(g.invite.roomId));
    notify();
}

void promptJoin() {
    Dispatcher::post([] {
        auto* d = ui::Dialog::open(ctx().window, tr(L"Birlikte dinleme oturumuna katıl"),
                                   tr(L"Arkadaşının gönderdiği davet bağlantısını yapıştır. Katılınca kendi çalma sıran "
                                      L"saklanır ve oturumdan ayrılınca geri gelir."),
                                   520);
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, L"shadetube://jam/…");
        auto submit = [d, box] {
            const std::string text = toUtf8(box->text());
            if (!parseInvite(text)) {
                box->setError(true);
                toast(tr(L"Bu bir birlikte dinleme davet bağlantısı değil"), true);
                return;
            }
            d->close();
            join(text);
        };
        box->onSubmit = [submit](const std::wstring&) { submit(); };
        box->onChange = [box](const std::wstring&) { box->setError(false); };
        d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
        d->addButton(tr(L"Katıl"), ui::ButtonKind::Primary, submit, false);
        box->focus();
    });
}

bool join(const std::string& inviteText) {
    const auto inv = parseInvite(inviteText);
    if (!inv) return false;
    if (g.role == Role::Host) {
        if (inv->roomId == g.invite.roomId) {
            toast(tr(L"Bu senin oturumunun bağlantısı"));
            return true;
        }
        const Invite copy = *inv;
        ui::Dialog::confirm(ctx().window, tr(L"Başka bir oturuma katıl"),
                            tr(L"Katılmak için kendi birlikte dinleme oturumunu bitirmen gerekiyor. Misafirlerin oturumdan çıkar."),
                            tr(L"Bitir ve katıl"), [copy] {
                                endHost(false);
                                joinRoom(copy, true);
                            });
        return true;
    }
    if (g.role == Role::Guest) {
        if (inv->roomId == g.invite.roomId) {
            toast(tr(L"Bu oturumdasın zaten"));
            return true;
        }
        // Another session: the queue saved at the first join stays the one that comes back.
        sendGuest("bye", {{"reason", "leave"}});
        dropClient();
        removeFilter();
        if (ctx().matcher) ctx().matcher->clearSessionPins();
        g_life.renew();
        Session fresh;
        fresh.role = Role::Guest;
        fresh.myKey = EcdhKey::generate();
        g = std::move(fresh);
        installFilter();
        joinRoom(*inv, false);
        return true;
    }
    ST_LOG_INFO(kTag, "guest: joining room {}", roomTag(inv->roomId));
    joinRoom(*inv, true);
    toast(tr(L"Birlikte dinleme oturumuna bağlanılıyor…"));
    return true;
}

void leave() {
    if (g.role == Role::Guest) {
        leaveGuest(Leave::Left);
    } else if (g.role == Role::Host) {
        const bool guests = std::any_of(g.members.begin(), g.members.end(), [](const Member& m) { return m.online; });
        if (!guests) {
            endHost(false);
            return;
        }
        ui::Dialog::confirm(ctx().window, tr(L"Oturumu bitir"),
                            tr(L"Birlikte dinleme oturumu herkes için sona erer. Misafirler kendi çalma sıralarına döner."),
                            tr(L"Oturumu bitir"), [] { endHost(false); }, true);
    }
}

void endSession() {
    if (g.role == Role::Host) endHost(false);
    else if (g.role == Role::Guest) leaveGuest(Leave::Left);
}

void copyInvite() {
    if (g.role != Role::Host) return;
    const std::string link = formatInvite(g.invite);
    if (link.empty() || g.invite.broker.empty()) {
        toast(tr(L"Bağlantı henüz hazır değil: sunucuya bağlanılıyor"), true);
        return;
    }
    copyText(toWide(link));
    toast(tr(L"Davet bağlantısı kopyalandı"));
}

void rotateInvite() {
    if (g.role != Role::Host) return;
    const std::string broker = g.invite.broker;
    Invite next = newInvite(broker);
    const std::string link = formatInvite(next);
    int handed = 0;
    if (g.link == Link::Connected && !link.empty()) {
        // The new link, sealed for each online guest alone (a removed device reads the room but can't open these).
        const EcdhKey eph = EcdhKey::generate();
        for (const auto& m : g.members)
            if (m.online && m.pk.size() == 64) {
                const json h = sealHandoff(eph, m.pk, m.device, link);
                if (!h.empty() && sendHost("handoff", h)) ++handed;
            }
        g.client->publish(g.topics.state, {}, true);   // the old room's state goes
    }
    ST_LOG_INFO(kTag, "host: new invite: room {} -> {}, {} guest(s) handed over", roomTag(g.invite.roomId), roomTag(next.roomId), handed);
    g.invite = next;
    g.replay.clear();
    hostLinkTo(broker);
    if (!next.broker.empty()) writeInviteFile();
    notify();
    toast(handed > 0 ? i18n::plural(L"Yeni davet bağlantısı hazır; {} misafir yeni odaya taşındı. Eski bağlantı artık çalışmaz.", handed)
                     : std::wstring(tr(L"Yeni davet bağlantısı hazır. Eski bağlantı artık çalışmaz.")));
}

void removeMember(const std::string& device) {
    if (g.role != Role::Host || device == deviceId()) return;
    const Member* m = memberFor(device);
    const std::string name = m ? m->name : std::string("?");
    if (!banned(device)) g.bans.push_back(device);
    std::erase_if(g.members, [&](const Member& x) { return x.device == device; });
    sendHost("kick", {{"d", device}});
    ST_LOG_INFO(kTag, "host: removed {}", name);
    publishState();
    notify();
    if (auto* w = ctx().window)
        ui::Toasts::show(w, i18n::format(tr(L"{} oturumdan çıkarıldı. Bağlantıyı bilen biri yeniden katılamasın diye yeni bir davet bağlantısı oluştur."), {toWide(name)}),
                         ui::ToastKind::Info, tr(L"Yeni bağlantı"), [] { rotateInvite(); });
}

void setGuestsControl(bool on) {
    if (g.role != Role::Host || g.guestsControl == on) return;
    g.guestsControl = on;
    ST_LOG_INFO(kTag, "host: guests {} control playback", on ? "may" : "may not");
    publishState();
    notify();
}

void moveToNextBroker() {
    if (g.role != Role::Host) return;
    const auto order = brokerOrder(g.brokerUrl, Settings::get().jamBroker);
    if (order.size() < 2) return;
    const std::string next = order[1];
    // The old broker keeps a last state that points to the new one (guests still there follow it).
    if (g.link == Link::Connected) {
        SessionState s = buildState();
        s.movedTo = next;
        send(g.topics.state, "state", toJson(s), true);
        sendHost("move", {{"broker", next}});
    }
    ST_LOG_INFO(kTag, "host: moving the session to {}", mqtt::brokerHost(next));
    g.client->moveTo(next);
}

std::wstring addedBy(const catalog::Track& t) {
    if (g.role == Role::Host) {
        if (auto it = g.addedBy.find(t.id); it != g.addedBy.end()) return toWide(it->second);
    } else if (g.role == Role::Guest && g.state) {
        for (const auto& i : g.state->queue)
            if (i.id == t.id) return toWide(i.addedBy);
    }
    return {};
}

namespace {
gfx::Text badgeText(const std::wstring& name) { return gfx::Text(L"+ " + toUpperTr(name), gfx::type::monoBadge); }
}

float addedBadgeWidth(const std::wstring& name) { return std::ceil(std::min(110.f, badgeText(name).measure().w)) + 12; }

float drawAddedBadge(gfx::Canvas& c, float x, float cy, const std::wstring& name) {
    const auto& acc = gfx::accent();
    const float w = addedBadgeWidth(name);
    const gfx::Rect r{x, std::round(cy - 7), w, 14};
    c.fillRounded(r, 2, acc.tint12);
    gfx::Text label = badgeText(name);
    c.text(label, {r.x + 6, r.y, w - 12, r.h}, acc.base, gfx::VAlign::Center);
    return w;
}

std::wstring barLabel() {
    if (g.role == Role::Guest) {
        if (!g.state) return tr(L"Birlikte dinleme · bağlanılıyor…");
        return i18n::format(tr(L"Birlikte dinleniyor · {}"), {toWide(g.state->hostName)});
    }
    if (g.role == Role::Host) {
        const int n = 1 + static_cast<int>(std::count_if(g.members.begin(), g.members.end(), [](const Member& m) { return m.online; }));
        return i18n::format(tr(L"Birlikte dinleniyor · {}"), {i18n::plural(L"{} kişi", n)});
    }
    return {};
}

std::wstring lockedReason() {
    if (g.role != Role::Guest || guestCanControl()) return {};
    return tr(L"Birlikte dinlerken çalmayı oturum sahibi kontrol eder");
}

std::vector<PaletteCommand> paletteCommands() {
    std::vector<PaletteCommand> out;
    if (g.role == Role::None) {
        out.push_back({"jam:start", tr(L"Birlikte dinleme oturumu başlat"), "users", [] { startHosting(); }});
        out.push_back({"jam:join", tr(L"Birlikte dinleme oturumuna katıl…"), "link", [] { promptJoin(); }});
    } else if (g.role == Role::Host) {
        out.push_back({"jam:copy", tr(L"Davet bağlantısını kopyala"), "link", [] { copyInvite(); }});
        out.push_back({"jam:rotate", tr(L"Yeni davet bağlantısı oluştur"), "refresh", [] { rotateInvite(); }});
        out.push_back({"jam:end", tr(L"Birlikte dinleme oturumunu bitir"), "close", [] { leave(); }});
    } else {
        out.push_back({"jam:leave", tr(L"Birlikte dinleme oturumundan ayrıl"), "logout", [] { leave(); }});
    }
    return out;
}

void openForwardedLink(const std::wstring& text) {
    const std::string s = toUtf8(text);
    if (!parseInvite(s)) {
        toast(tr(L"Bu bir birlikte dinleme davet bağlantısı değil"), true);
        return;
    }
    join(s);
}

} // namespace st::app::together

namespace st::app {

void initTogether() {
    using namespace together;
    auto* p = ctx().player;
    // A guest's own queue left behind by a crash (or a kill) comes back first: the player restores session.json next.
    if (const auto saved = restoreOwnQueueFiles()) {
        ST_LOG_INFO(kTag, "restored the own queue saved when joining a session (the app ended as a guest)");
        if (p) {
            p->setShuffle(saved->first);
            p->setRepeat(saved->second);
        }
    }
    (void)deviceId();
    if (p) {
        // A guest plays the host's exact video: its own download / local copy of the song might be another recording.
        auto previous = p->localFileFor;
        p->localFileFor = [previous](const std::string& id) -> std::wstring {
            if (g.role == Role::Guest && !hostVideoFor(id).empty()) return {};
            return previous ? previous(id) : std::wstring{};
        };
    }
    ctx().playerChangedHooks.push_back([] {
        if (g.role == Role::Host) {
            schedulePublish();
            // Podcasts and radio are not shared: say so once per item.
            if (auto* pl = ctx().player; pl && pl->current() && pl->current()->id != g.lastItemId) {
                g.lastItemId = pl->current()->id;
                if (radio::isStationId(g.lastItemId) || catalog::isPodcastId(g.lastItemId))
                    toast(tr(L"Podcast ve radyo yayınları birlikte dinlemede paylaşılmaz; misafirler bekler"));
            }
        } else if (g.role == Role::Guest) {
            // Follow at once (a finished song, a failed load); the 250 ms tick does the rest.
            if (now() - g.lastReconcile >= 100)
                Dispatcher::post([] {
                    if (g.role == Role::Guest) reconcile();
                });
        }
    });
    ctx().persistHooks.push_back([] {
        // Exit (or Windows ending the session): the host ends it for everyone, a guest says goodbye; the guest's own
        // queue goes back to session.json for the next start.
        if (g.role == Role::Host) endHost(true);
        else if (g.role == Role::Guest) leaveGuest(Leave::Exit);
    });

    // The shadetube: URL protocol, for the real profile only (a sandbox leaves it alone unless a test key is given).
    if (!winshell::registrationAllowed() && !protocolKeyOverridden()) {
        ST_LOG_INFO(kTag, "sandbox profile: shadetube: URL protocol left alone");
    } else {
        background(Priority::Low, [exe = winshell::currentExe(), key = protocolKeyPath()] {
            if (registerUrlProtocol(exe, key)) ST_LOG_INFO(kTag, "shadetube: URL protocol registered");
        });
    }

    // Dev: a host and a guest in two sandbox profiles.
    g_inviteFile = toUtf8(envVar(L"SHADETUBE_JAM_INVITE_FILE"));
    if (envVar(L"SHADETUBE_JAM_HOST") == L"1") Dispatcher::post([] { startHosting(); });
    if (const std::wstring link = envVar(L"SHADETUBE_JAM_JOIN"); !link.empty())
        Dispatcher::post([text = toUtf8(link)] {
            if (!join(text)) ST_LOG_WARN(kTag, "SHADETUBE_JAM_JOIN is not an invite link");
        });
}

} // namespace st::app
