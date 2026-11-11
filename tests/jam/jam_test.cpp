// jam_test: checks for "Birlikte dinle" (listen together), the standalone core in app/Jam*.
//
//   mqtt        : remaining-length varint (limits, truncated, 5 bytes), every packet type the client uses byte for
//                 byte (CONNECT with a will / user name / password, SUBSCRIBE, PUBLISH QoS 0 retained, PUBACK, PINGREQ,
//                 PINGRESP, DISCONNECT, CONNACK, SUBACK), the stream decoder (several packets per chunk, byte by byte)
//                 and malformed input (reserved types, wrong fixed flags, QoS 3, oversize, truncated bodies), broker URLs
//   crypto      : SHA-256, hex, base64url vectors (RFC 4648), AES-256-GCM known answer (GCM spec test case 14),
//                 round trip, tampering (nonce, ciphertext, tag, associated data, key), ECDH agreement
//   envelope    : sealed messages per topic, wrong topic / key / version, replay window (duplicates, reordering, too
//                 old), sequence epochs and the last will's number
//   invite      : format / parse round trip, browser trailing slash, quotes, case, ws:// brokers, garbage, topics,
//                 broker order
//   state       : session state / queue item / request JSON round trips, hostile JSON (wrong types, caps), hand-off
//   clock       : offset estimation (symmetric, asymmetric outliers, negative round trips)
//   reconcile   : what a guest does for a host state: load / jump / replace upcoming / seek / play / pause / hold
//                 (unsupported items, waiting for the video, failures), drift threshold, cooldown, seek lead learning
//   protocol    : the shadetube: URL protocol in a test key (SHADETUBE_PROTOCOL_KEY)
//   live        : (`jam_test live [wss://broker...]`) two clients per broker through the real public broker: retained
//                 encrypted state to a late subscriber, a guest request back, clock pings, the last will, clearing the
//                 retained state; prints the connect time and round trips
//
// Runs in a per-process temp profile (SHADETUBE_DATA_DIR) and a per-process test registry key.
#include "app/JamCrypto.h"
#include "app/JamInvite.h"
#include "app/JamMqtt.h"
#include "app/JamProtocol.h"
#include "app/JamSync.h"
#include "core/Log.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace st::app::jam;
namespace mq = st::app::jam::mqtt;
using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static Bytes hex(const char* s) { return *fromHex(s); }
static Bytes bytesOf(std::string_view s) { return Bytes(s.begin(), s.end()); }

static mq::Packet onePacket(const Bytes& wire) {
    mq::StreamDecoder d;
    d.feed(wire);
    auto p = d.next();
    CHECK(p.has_value());
    CHECK(d.buffered() == 0);
    return p ? *p : mq::Packet{};
}

// ---- mqtt ---------------------------------------------------------------------------------------------------------------

static void testVarint() {
    std::printf("mqtt: remaining length\n");
    struct Case {
        size_t value;
        Bytes wire;
    };
    const Case cases[] = {{0, {0x00}},
                          {127, {0x7F}},
                          {128, {0x80, 0x01}},
                          {16383, {0xFF, 0x7F}},
                          {16384, {0x80, 0x80, 0x01}},
                          {2097151, {0xFF, 0xFF, 0x7F}},
                          {2097152, {0x80, 0x80, 0x80, 0x01}},
                          {268435455, {0xFF, 0xFF, 0xFF, 0x7F}}};
    for (const auto& c : cases) {
        Bytes out;
        CHECK(mq::appendRemainingLength(out, c.value));
        CHECK(out == c.wire);
        size_t v = 0, used = 0;
        CHECK(mq::decodeRemainingLength(c.wire, v, used) == mq::Parse::Ok);
        CHECK(v == c.value && used == c.wire.size());
        if (c.wire.size() > 1) {   // every truncation needs more bytes
            CHECK(mq::decodeRemainingLength(std::span(c.wire).first(c.wire.size() - 1), v, used) == mq::Parse::NeedMore);
        }
    }
    Bytes big;
    CHECK(!mq::appendRemainingLength(big, 268435456));
    size_t v = 0, used = 0;
    const Bytes five{0x80, 0x80, 0x80, 0x80, 0x01};
    CHECK(mq::decodeRemainingLength(five, v, used) == mq::Parse::Malformed);
    CHECK(mq::decodeRemainingLength({}, v, used) == mq::Parse::NeedMore);
}

static void testPackets() {
    std::printf("mqtt: packets\n");
    // CONNECT, minimal: client id "a", keep-alive 30, clean session.
    mq::ConnectOptions co;
    co.clientId = "a";
    co.keepAliveSec = 30;
    CHECK(mq::encodeConnect(co) == hex("100d00044d5154540402001e000161"));
    // CONNECT with a will (retained), user name and password.
    co.clientId = "st-0123456789abcdef";
    co.keepAliveSec = 20;
    co.will = mq::Will{"shadetube/jam/v1/x/g", Bytes{1, 2, 3, 0, 255}, true};
    co.username = "user";
    co.password = "pw";
    const Bytes connect = mq::encodeConnect(co);
    CHECK(connect[0] == 0x10);
    CHECK(connect[2 + 7] == (0x02 | 0x04 | 0x20 | 0x40 | 0x80));   // flags byte after "MQTT" + level
    const auto pc = mq::parseConnect(onePacket(connect));
    CHECK(pc.has_value());
    if (pc) {
        CHECK(pc->clientId == co.clientId && pc->keepAliveSec == 20 && pc->cleanSession);
        CHECK(pc->will && pc->will->topic == co.will->topic && pc->will->payload == co.will->payload && pc->will->retain);
        CHECK(pc->username == "user" && pc->password == "pw");
    }
    // A will without retain, no credentials.
    co.will->retain = false;
    co.username.clear();
    co.password.clear();
    const auto pc2 = mq::parseConnect(onePacket(mq::encodeConnect(co)));
    CHECK(pc2 && pc2->will && !pc2->will->retain && pc2->username.empty());

    // SUBSCRIBE: packet id 1, "a/#" at QoS 0.
    const Bytes sub = mq::encodeSubscribe(1, {"a/#"});
    CHECK(sub == hex("820800010003612f2300"));
    const auto ps = mq::parseSubscribe(onePacket(sub));
    CHECK(ps && ps->packetId == 1 && ps->filters.size() == 1 && ps->filters[0].first == "a/#" && ps->filters[0].second == 0);
    const auto ps2 = mq::parseSubscribe(onePacket(mq::encodeSubscribe(7, {"x/state", "x/h", "x/g"})));
    CHECK(ps2 && ps2->filters.size() == 3 && ps2->filters[2].first == "x/g");

    // PUBLISH QoS 0, retained and not.
    const Bytes payload{0, 1, 2, 0xFE, 0xFF};
    const Bytes pub = mq::encodePublish("t/x", payload, true);
    CHECK(pub == hex("310a0003742f78000102feff"));
    const auto pp = mq::parsePublish(onePacket(pub));
    CHECK(pp && pp->topic == "t/x" && pp->payload == payload && pp->retain && pp->qos == 0 && !pp->dup);
    const auto pp2 = mq::parsePublish(onePacket(mq::encodePublish("t", {}, false)));
    CHECK(pp2 && pp2->topic == "t" && pp2->payload.empty() && !pp2->retain);   // empty payload = clears a retained one
    // An incoming QoS 1 publish (a broker may send one): packet id parsed, PUBACK echoes it.
    const auto q1 = mq::parsePublish(onePacket(hex("3207" "0001" "61" "1234" "6869")));
    CHECK(q1 && q1->qos == 1 && q1->packetId == 0x1234 && q1->topic == "a" && q1->payload == bytesOf("hi"));
    CHECK(mq::encodePuback(0x1234) == hex("40021234"));
    CHECK(mq::parsePuback(onePacket(mq::encodePuback(9))) == 9);
    // A large payload (3-byte remaining length).
    const Bytes large(100'000, 0xAB);
    const auto pl = mq::parsePublish(onePacket(mq::encodePublish("big", large, false)));
    CHECK(pl && pl->payload == large);

    CHECK(mq::encodePingreq() == hex("c000"));
    CHECK(mq::encodePingresp() == hex("d000"));
    CHECK(mq::encodeDisconnect() == hex("e000"));
    CHECK(onePacket(mq::encodePingresp()).type == mq::Type::Pingresp);
    CHECK(onePacket(mq::encodeDisconnect()).type == mq::Type::Disconnect);

    const auto ca = mq::parseConnack(onePacket(mq::encodeConnack(true, 0)));
    CHECK(ca && ca->sessionPresent && ca->returnCode == 0);
    const auto cr = mq::parseConnack(onePacket(hex("20020005")));
    CHECK(cr && !cr->sessionPresent && cr->returnCode == 5);
    CHECK(!mq::parseConnack(onePacket(hex("20020200"))));   // reserved acknowledge flags
    const auto sa = mq::parseSuback(onePacket(mq::encodeSuback(1, {0x00, 0x80})));
    CHECK(sa && sa->packetId == 1 && sa->codes == std::vector<uint8_t>({0x00, 0x80}));
    CHECK(!mq::parseSuback(onePacket(hex("9003000105"))));  // 5 is no return code
    CHECK(std::string(mq::connackText(4)) == "bad user name or password");
}

static void testDecoder() {
    std::printf("mqtt: stream decoder\n");
    // Three packets in one chunk, then the same stream one byte at a time.
    Bytes stream = mq::encodeConnack(false, 0);
    const Bytes pub = mq::encodePublish("a/b", bytesOf("hello"), false);
    stream.insert(stream.end(), pub.begin(), pub.end());
    const Bytes ping = mq::encodePingresp();
    stream.insert(stream.end(), ping.begin(), ping.end());
    {
        mq::StreamDecoder d;
        d.feed(stream);
        int n = 0;
        while (auto p = d.next()) ++n;
        CHECK(n == 3 && !d.malformed());
    }
    {
        mq::StreamDecoder d;
        std::vector<mq::Type> types;
        for (uint8_t b : stream) {
            d.feed(std::span(&b, 1));
            while (auto p = d.next()) types.push_back(p->type);
        }
        CHECK(types == std::vector<mq::Type>({mq::Type::Connack, mq::Type::Publish, mq::Type::Pingresp}));
    }
    auto malformed = [](const Bytes& wire, size_t max = 1 << 20) {
        mq::StreamDecoder d(max);
        d.feed(wire);
        return !d.next() && d.malformed();
    };
    CHECK(malformed(hex("0000")));                  // reserved type 0
    CHECK(malformed(hex("f000")));                  // reserved type 15
    CHECK(malformed(hex("8000")));                  // SUBSCRIBE without its fixed 0010 flags
    CHECK(malformed(hex("c100")));                  // PINGREQ with flags
    CHECK(malformed(hex("3600")));                  // PUBLISH QoS 3
    CHECK(malformed(hex("308080808001")));          // five-byte remaining length
    CHECK(malformed(hex("30ff7f"), 1000));          // over the size limit
    {
        mq::StreamDecoder d;
        d.feed(hex("3003000474"));   // a topic longer than the packet: whole packet, but no valid PUBLISH
        auto p = d.next();
        CHECK(p && !mq::parsePublish(*p));
        CHECK(!d.next() && !d.malformed());
    }
    {
        mq::StreamDecoder d;
        d.feed(hex("300a0003"));   // incomplete: waits
        CHECK(!d.next() && !d.malformed());
    }
    CHECK(!mq::parsePublish(onePacket(hex("30020000"))));   // empty topic
    CHECK(!mq::parseConnect(onePacket(hex("100d00044d5154540302001e000161"))));   // protocol level 3
    CHECK(!mq::parseConnect(onePacket(hex("100d00044d5154540403001e000161"))));   // reserved connect flag
    CHECK(!mq::parseSubscribe(onePacket(hex("82020001"))));   // no filters
    CHECK(mq::validTopic("shadetube/jam/v1/abc/state"));
    CHECK(!mq::validTopic("a/+/b") && !mq::validTopic("a/#") && !mq::validTopic(""));
}

static void testBrokerUrls() {
    std::printf("mqtt: broker URLs\n");
    auto b = mq::parseBrokerUrl("wss://broker.emqx.io:8084/mqtt");
    CHECK(b && b->secure && b->host == "broker.emqx.io" && b->port == 8084 && b->path == "/mqtt");
    b = mq::parseBrokerUrl("WSS://test.mosquitto.org");
    CHECK(b && b->port == 443 && b->path == "/");
    b = mq::parseBrokerUrl("ws://192.168.1.20:9001");
    CHECK(b && !b->secure && b->port == 9001 && b->host == "192.168.1.20");
    CHECK(!mq::parseBrokerUrl("https://broker.emqx.io/mqtt"));
    CHECK(!mq::parseBrokerUrl("wss://"));
    CHECK(!mq::parseBrokerUrl("wss://host:99999/x"));
    CHECK(!mq::parseBrokerUrl("wss://user@host/x"));
    CHECK(!mq::parseBrokerUrl("wss://host/x?y=1"));
    CHECK(!mq::parseBrokerUrl("wss://ho st/x"));
    CHECK(mq::brokerHost("wss://broker.hivemq.com:8884/mqtt") == "broker.hivemq.com");
}

// ---- crypto -------------------------------------------------------------------------------------------------------------

static void testCrypto() {
    std::printf("crypto\n");
    CHECK(toHex(sha256(std::string_view("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256(std::string_view(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(hex("00ff10Ab")) == "00ff10ab");
    CHECK(!fromHex("abc") && !fromHex("zz"));
    const std::pair<const char*, const char*> b64[] = {{"", ""},         {"f", "Zg"},         {"fo", "Zm8"},
                                                       {"foo", "Zm9v"},  {"foob", "Zm9vYg"}, {"fooba", "Zm9vYmE"},
                                                       {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, enc] : b64) {
        CHECK(base64url(asBytes(plain)) == enc);
        const auto back = fromBase64url(enc);
        CHECK(back && std::string(back->begin(), back->end()) == plain);
    }
    CHECK(base64url(Bytes{0xFB, 0xFF, 0xBF}) == "-_-_");   // the URL-safe alphabet
    CHECK(fromBase64url("-_-_") == Bytes({0xFB, 0xFF, 0xBF}));
    CHECK(fromBase64url("Zm9v=="));                        // padding tolerated
    CHECK(!fromBase64url("Zm9v+") && !fromBase64url("Z") && !fromBase64url("Zm9vY$"));
    CHECK(!fromBase64url("Zh"));                           // non-canonical trailing bits
    CHECK(randomBytes(32).size() == 32 && randomBytes(16) != randomBytes(16));

    // AES-256-GCM known answer: K = 0^256, IV = 0^96, P = 0^128 (GCM spec test case 14).
    const Bytes zeroKey(32, 0);
    Bytes kat(12, 0);
    const Bytes ct = hex("cea7403d4d606b6e074ec5d3baf39d18"), tag = hex("d0d1c8a799996bf0265b98b5d48ab919");
    kat.insert(kat.end(), ct.begin(), ct.end());
    kat.insert(kat.end(), tag.begin(), tag.end());
    const auto katPlain = open(zeroKey, kat, {});
    CHECK(katPlain && *katPlain == Bytes(16, 0));

    const Bytes key = randomBytes(32), aad = bytesOf("shadetube/jam/v1/x/state");
    const Bytes msg = bytesOf("{\"t\":\"state\",\"pos\":123456}");
    const Bytes sealed = seal(key, msg, aad);
    CHECK(sealed.size() == msg.size() + kNonceBytes + kTagBytes);
    CHECK(open(key, sealed, aad) == msg);
    CHECK(seal(key, msg, aad) != sealed);                  // a fresh nonce every time
    for (size_t at : {size_t(0), kNonceBytes + 3, sealed.size() - 1}) {   // nonce, ciphertext, tag
        Bytes t = sealed;
        t[at] ^= 0x01;
        CHECK(!open(key, t, aad));
    }
    CHECK(!open(key, sealed, bytesOf("shadetube/jam/v1/x/g")));   // another topic
    CHECK(!open(randomBytes(32), sealed, aad));                  // another key
    CHECK(!open(key, std::span(sealed).first(20), aad));         // truncated
    const Bytes empty = seal(key, {}, aad);
    CHECK(open(key, empty, aad) == Bytes{});
    bool threw = false;
    try {
        seal(Bytes(16, 1), msg, aad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // ECDH P-256: both sides derive the same key; a different label or peer doesn't.
    auto a = EcdhKey::generate(), b = EcdhKey::generate(), c = EcdhKey::generate();
    const Bytes pa = a.publicKey(), pb = b.publicKey();
    CHECK(pa.size() == 64 && pb.size() == 64);
    const auto kab = a.derive(pb, "label"), kba = b.derive(pa, "label");
    CHECK(kab && kba && *kab == *kba && kab->size() == 32);
    CHECK(*a.derive(pb, "other") != *kab);
    CHECK(*c.derive(pb, "label") != *kab);
    CHECK(!a.derive(Bytes(63, 1), "label"));
    CHECK(!a.derive(Bytes(64, 1), "label"));   // not a point on the curve
}

// ---- envelope ---------------------------------------------------------------------------------------------------------

static void testEnvelope() {
    std::printf("envelope\n");
    const Bytes key = randomBytes(32);
    Envelope e;
    e.type = "req";
    e.device = "0123456789abcdef";
    e.seq = 42;
    e.body = {{"op", "next"}, {"name", "Ayşe"}};
    const Bytes wire = sealEnvelope(key, "r/g", e);
    OpenError why{};
    auto back = openEnvelope(key, "r/g", wire, &why);
    CHECK(back && why == OpenError::None);
    CHECK(back && back->type == "req" && back->device == e.device && back->seq == 42 && back->body["name"] == "Ayşe");
    CHECK(!openEnvelope(key, "r/h", wire, &why) && why == OpenError::Tampered);
    CHECK(!openEnvelope(randomBytes(32), "r/g", wire, &why) && why == OpenError::Tampered);
    Bytes v2 = wire;
    v2[1] = 2;
    CHECK(!openEnvelope(key, "r/g", v2, &why) && why == OpenError::Version);
    CHECK(!openEnvelope(key, "r/g", Bytes{'J', 1, 2, 3}, &why) && why == OpenError::Malformed);
    Bytes flipped = wire;
    flipped[wire.size() / 2] ^= 0x80;
    CHECK(!openEnvelope(key, "r/g", flipped, &why) && why == OpenError::Tampered);
    // Sealed JSON that isn't an envelope (no device id).
    Envelope bad = e;
    bad.device = "nothex";
    CHECK(!openEnvelope(key, "r/g", sealEnvelope(key, "r/g", bad), &why) && why == OpenError::Malformed);
    // Large sequence numbers survive JSON exactly.
    e.seq = (uint64_t(1'800'000'000) << 24) | 0x800000;
    back = openEnvelope(key, "r/g", sealEnvelope(key, "r/g", e));
    CHECK(back && back->seq == e.seq);

    // Replay window.
    ReplayWindow w;
    CHECK(w.accept(100) && !w.accept(100));
    CHECK(w.accept(103) && w.accept(101) && w.accept(102) && !w.accept(101));   // reordering within the window
    CHECK(w.accept(200) && !w.accept(136) && w.accept(137));                    // 64 below the top: still in the window
    CHECK(!w.accept(0));
    ReplayFilter f;
    CHECK(f.accept("aaaaaaaaaaaaaaaa", 5) && f.accept("bbbbbbbbbbbbbbbb", 5) && !f.accept("aaaaaaaaaaaaaaaa", 5));
    f.forget("aaaaaaaaaaaaaaaa");
    CHECK(f.accept("aaaaaaaaaaaaaaaa", 5));

    // Sequence numbers: the last will is accepted after the connection's messages, then refused once a newer one speaks.
    SequenceCounter seq;
    seq.newEpoch(1'000);
    const uint64_t m1 = seq.next(), m2 = seq.next(), will = seq.willSeq();
    CHECK(m2 == m1 + 1 && will > m2);
    ReplayWindow r;
    CHECK(r.accept(m1) && r.accept(m2) && r.accept(will));
    seq.newEpoch(1'000);   // the same second: the epoch still grows
    const uint64_t n1 = seq.next();
    CHECK(n1 > will && r.accept(n1));
    ReplayWindow late;     // a will delivered after the device reconnected is stale
    CHECK(late.accept(n1) && !late.accept(will));
}

// ---- invite -----------------------------------------------------------------------------------------------------------

static void testInvite() {
    std::printf("invite\n");
    const Invite inv = newInvite("wss://broker.emqx.io:8084/mqtt");
    CHECK(inv.roomId.size() == 16 && inv.key.size() == 32);
    const std::string link = formatInvite(inv);
    CHECK(link.rfind("shadetube://jam/", 0) == 0);
    CHECK(link.size() < 130);
    CHECK(link.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:/") == std::string::npos);
    CHECK(parseInvite(link) == inv);
    CHECK(parseInvite(link + "/") == inv);                       // browsers append a slash
    CHECK(parseInvite("  \"" + link + "\"\r\n") == inv);
    CHECK(parseInvite("<" + link + ">") == inv);
    std::string upper = link;
    for (size_t i = 0; i < 16; ++i) upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    CHECK(parseInvite(upper) == inv);
    CHECK(looksLikeInvite(" " + link) && !looksLikeInvite("https://open.spotify.com/track/x"));
    const Invite lan = newInvite("ws://192.168.1.5:9001/mqtt");
    CHECK(parseInvite(formatInvite(lan)) == lan);
    // Garbage: wrong version, cut short, bad characters, a bad broker, extra flags.
    CHECK(!parseInvite("shadetube://jam/"));
    CHECK(!parseInvite(link.substr(0, link.size() - 30)));
    CHECK(!parseInvite(link + "$"));
    CHECK(!parseInvite("shadetube://other/" + link.substr(16)));
    auto payload = *fromBase64url(link.substr(16));
    payload[0] = 2;
    CHECK(!parseInvite("shadetube://jam/" + base64url(payload)));
    payload[0] = 1;
    payload[1] = 0x80;
    CHECK(!parseInvite("shadetube://jam/" + base64url(payload)));
    payload[1] = 0;
    payload.back() = ' ';
    CHECK(!parseInvite("shadetube://jam/" + base64url(payload)));
    CHECK(formatInvite(newInvite("https://x/y")).empty());
    Invite shortKey = inv;
    shortKey.key.resize(16);
    CHECK(formatInvite(shortKey).empty());

    const std::string base = topicBase(inv.roomId);
    CHECK(base.rfind("shadetube/jam/v1/", 0) == 0 && base.size() == 17 + 64);
    CHECK(base.find(toHex(inv.roomId)) == std::string::npos);   // the room id itself never shows
    const Topics t = topicsFor(inv.roomId);
    CHECK(t.state == base + "/state" && t.host == base + "/h" && t.guest == base + "/g" && t.all == base + "/#");
    CHECK(roomTag(inv.roomId).size() == 8);

    const auto order = brokerOrder("wss://test.mosquitto.org:8081/mqtt", "wss://my.broker:443/mqtt");
    CHECK(order.size() == 4 && order[0] == "wss://test.mosquitto.org:8081/mqtt" && order[1] == "wss://my.broker:443/mqtt");
    CHECK(brokerOrder("", "not a url").size() == defaultBrokers().size());
}

// ---- state ------------------------------------------------------------------------------------------------------------

static QueueItem item(const std::string& id, int64_t durMs = 200'000, ItemKind kind = ItemKind::Song, std::string video = "dQw4w9WgXcQ") {
    QueueItem i;
    i.id = id;
    i.title = "Song " + id;
    i.artists = {"Artist"};
    i.artistIds = {"spotify:artist:1"};
    i.album = "Album";
    i.albumId = "spotify:album:1";
    i.durationMs = durMs;
    i.cover = "https://i.scdn.co/image/x";
    i.videoId = std::move(video);
    i.kind = kind;
    return i;
}

static SessionState hostState() {
    SessionState s;
    s.revision = 7;
    s.hostDevice = "00112233445566ff";
    s.hostName = "Berkay";
    s.broker = "wss://broker.emqx.io:8084/mqtt";
    s.guestsControl = true;
    s.bans = {"ffeeddccbbaa0099"};
    s.members = {{"00112233445566ff", "Berkay", true, true}, {"0102030405060708", "Ayşe", false, false}};
    s.queue = {item("spotify:track:a"), item("local:1", 180'000, ItemKind::File, ""), item("podcast:2", 0, ItemKind::Unsupported, "")};
    s.queue[1].addedBy = "Ayşe";
    s.current = 0;
    s.positionMs = 61'500;
    s.atHostMs = 123'456'789;
    s.playing = true;
    s.status = HostStatus::Playing;
    s.queueGen = 3;
    s.contextName = "Gece Sürüşü";
    return s;
}

static void testState() {
    std::printf("state\n");
    const SessionState s = hostState();
    const auto back = stateFromJson(json::parse(toJson(s).dump()));
    CHECK(back.has_value());
    if (back) {
        CHECK(back->revision == 7 && back->hostDevice == s.hostDevice && back->hostName == "Berkay" && back->broker == s.broker);
        CHECK(back->guestsControl && back->bans == s.bans && back->members == s.members);
        CHECK(back->queue == s.queue && back->current == 0 && back->positionMs == 61'500 && back->atHostMs == 123'456'789);
        CHECK(back->playing && back->status == HostStatus::Playing && back->queueGen == 3 && back->contextName == "Gece Sürüşü");
        CHECK(back->banned("ffeeddccbbaa0099") && !back->banned("0102030405060708"));
    }
    // Hostile / broken JSON: wrong types give defaults, a state without its host is refused, caps hold.
    json j = toJson(s);
    j["pos"] = "soon";
    j["pl"] = 1;
    j["m"][0]["n"] = 5;
    j["q"].push_back(json{{"i", 3}});
    for (int i = 0; i < 80; ++i) j["q"].push_back(toJson(item("spotify:track:x" + std::to_string(i))));
    const auto odd = stateFromJson(j);
    CHECK(odd && odd->positionMs == 0 && !odd->playing && odd->members[0].name.empty());
    CHECK(odd && odd->queue.size() <= static_cast<size_t>(kQueueWindow) + 2);
    json noHost = toJson(s);
    noHost["hd"] = "xyz";
    CHECK(!stateFromJson(noHost));
    CHECK(!stateFromJson(json::array()));
    json cur = toJson(s);
    cur["cur"] = 99;
    CHECK(stateFromJson(cur)->current == 2);   // clamped into the queue

    // Requests.
    Request add;
    add.op = RequestOp::Add;
    add.item = item("spotify:track:z");
    add.item.addedBy = "spoofed";
    const auto ra = requestFromJson(json::parse(toJson(add).dump()));
    CHECK(ra && ra->op == RequestOp::Add && ra->item.id == "spotify:track:z" && ra->item.addedBy.empty());
    Request seek;
    seek.op = RequestOp::Seek;
    seek.ms = 90'000;
    const auto rs = requestFromJson(toJson(seek));
    CHECK(rs && rs->op == RequestOp::Seek && rs->ms == 90'000 && needsControl(rs->op));
    Request jump;
    jump.op = RequestOp::Jump;
    jump.index = 2;
    jump.itemId = "podcast:2";
    const auto rj = requestFromJson(toJson(jump));
    CHECK(rj && rj->index == 2 && rj->itemId == "podcast:2");
    CHECK(!requestFromJson(json{{"op", "jump"}, {"index", 1}}));
    CHECK(!requestFromJson(json{{"op", "dance"}}));
    CHECK(!requestFromJson(json{{"op", "add"}}));
    CHECK(!needsControl(RequestOp::Add));

    // Hand-off of a new invite to one guest.
    auto host = EcdhKey::generate(), guest = EcdhKey::generate(), other = EcdhKey::generate();
    const std::string link = formatInvite(newInvite("wss://broker.emqx.io:8084/mqtt"));
    const json h = sealHandoff(host, guest.publicKey(), "0102030405060708", link);
    CHECK(openHandoff(guest, h, "0102030405060708") == link);
    CHECK(!openHandoff(guest, h, "0807060504030201"));   // addressed to someone else
    CHECK(!openHandoff(other, h, "0102030405060708"));   // a removed member's own key can't open it
    json tampered = h;
    tampered["to"] = "0807060504030201";
    CHECK(!openHandoff(guest, tampered, "0807060504030201"));
}

// ---- clock ------------------------------------------------------------------------------------------------------------

static void testClock() {
    std::printf("clock\n");
    // Host clock = guest clock + 5000; symmetric 40 ms one way, 2 ms host processing.
    ClockEstimator c;
    CHECK(!c.ready() && c.offsetMs() == 0);
    for (int i = 0; i < 5; ++i) {
        const int64_t t1 = 1000 + i * 300;
        c.add(t1, t1 + 40 + 5000, t1 + 42 + 5000, t1 + 82);
    }
    CHECK(c.ready() && c.offsetMs() == 5000 && c.rttMs() == 80);
    // Slow, asymmetric round trips (a 900 ms stall on the way back) don't move the estimate.
    for (int i = 0; i < 3; ++i) {
        const int64_t t1 = 5000 + i * 300;
        c.add(t1, t1 + 40 + 5000, t1 + 42 + 5000, t1 + 82 + 900);
    }
    CHECK(c.offsetMs() == 5000);
    const int before = c.samples();
    c.add(100, 50, 60, 90);   // negative round trip: dropped
    CHECK(c.samples() == before);
    // Old samples age out; a new offset (host clock adjusted) wins once the faster half holds it.
    for (int i = 0; i < 9; ++i) c.add(9000 + i, 9000 + i + 20 - 300, 9000 + i + 21 - 300, 9000 + i + 41);
    CHECK(c.samples() == static_cast<int>(ClockEstimator::kMaxSamples) && c.offsetMs() == -300);
    c.reset();
    CHECK(!c.ready());
}

// ---- reconcile --------------------------------------------------------------------------------------------------------

static void testReconcile() {
    std::printf("reconcile\n");
    SessionState s = hostState();
    s.queue = {item("A"), item("B"), item("C")};
    s.positionMs = 30'000;
    s.atHostMs = 10'000;
    const int64_t hostNow = 12'000;   // 2 s after the sample: expected 32 s
    CHECK(expectedPosition(s, hostNow) == 32'000);
    {
        SessionState p = s;
        p.playing = false;
        p.status = HostStatus::Paused;
        CHECK(expectedPosition(p, hostNow) == 30'000);
        p.playing = true;
        p.status = HostStatus::Playing;
        CHECK(expectedPosition(p, 10'000 + 400'000) == 200'000);   // clamped to the length
    }

    // Nothing local yet: the host's queue at the host's position (+ the load lead), playing.
    {
        SyncMemory mem;
        LocalView l;
        auto d = decide(s, hostNow, 0, l, mem);
        CHECK(d.load == Decision::Load::Queue && d.autoplay && d.seekMs == 32'000 + mem.loadLeadMs);
        CHECK(mem.attemptId == "A" && mem.attempts == 1 && mem.pending == SyncMemory::Pending::Load);
        // It started 1.2 s later, 500 ms behind: measured 0.8 s into playback, the load lead grows by half of that.
        l = LocalView{"A", {"B", "C"}, true, false, false, 32'700, 200'000};
        d = decide(s, hostNow + 1'200, 1'200, l, mem);   // expected 33.2 s: -500, starts the 0.8 s wait
        CHECK(d.seekMs < 0 && mem.pending == SyncMemory::Pending::Load);
        l.positionMs = 33'500;
        d = decide(s, hostNow + 2'000, 2'000, l, mem);   // expected 34.0 s: still -500
        CHECK(mem.pending == SyncMemory::Pending::None && mem.loadLeadMs == 950 && d.seekMs < 0);
    }
    // Host paused: loaded paused at its position.
    {
        SessionState p = s;
        p.playing = false;
        p.status = HostStatus::Paused;
        SyncMemory mem;
        auto d = decide(p, hostNow, 0, LocalView{}, mem);
        CHECK(d.load == Decision::Load::Queue && !d.autoplay && d.seekMs == 30'000);
    }
    // Same item: drift beyond 1 s seeks (once per cool-down); a steady smaller offset gets a few fine corrections.
    {
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, false, false, 33'500, 200'000};
        auto d = decide(s, hostNow, 50'000, l, mem);
        CHECK(d.load == Decision::Load::None && !d.replaceUpcoming && d.hasDrift && d.driftMs == 1500);
        CHECK(d.seekMs == 32'000 + mem.seekLeadMs && mem.pending == SyncMemory::Pending::Seek);
        mem.pending = SyncMemory::Pending::None;   // (no lead learning in this case)
        d = decide(s, hostNow, 51'000, l, mem);    // 1 s later, still off: cooling down
        CHECK(d.seekMs < 0);
        l.positionMs = 32'200;                     // 200 ms: close enough
        d = decide(s, hostNow, 60'000, l, mem);
        CHECK(d.seekMs < 0 && d.driftMs == 200 && !d.play && !d.pause);
        l.positionMs = 32'400;                     // 400 ms, steady, settled: a fine correction
        d = decide(s, hostNow, 61'000, l, mem);
        CHECK(d.seekMs == 32'000 + mem.seekLeadMs && mem.fineSeeks == 1);
        for (int i = 0; i < 5; ++i) decide(s, hostNow, 70'000 + i * 5'000, l, mem);
        CHECK(mem.fineSeeks == SyncTuning{}.maxFineSeeks);   // capped per song
    }
    // Same item, local paused while the host plays: play, lined up.
    {
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, false, false, false, 20'000, 200'000};
        auto d = decide(s, hostNow, 0, l, mem);
        CHECK(d.play && d.seekMs == 32'000 + mem.seekLeadMs);
        d = decide(s, hostNow, 50, l, mem);   // the player hasn't reported it yet: nothing twice
        CHECK(!d.play && d.seekMs < 0);
        l.positionMs = 32'050;   // already there: just play
        d = decide(s, hostNow, 10'000, l, mem);
        CHECK(d.play && d.seekMs < 0);
    }
    // Host paused, local playing: pause and go to its position.
    {
        SessionState p = s;
        p.playing = false;
        p.status = HostStatus::Paused;
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, false, false, 45'000, 200'000};
        auto d = decide(p, hostNow, 0, l, mem);
        CHECK(d.pause && d.seekMs == 30'000);
        d = decide(p, hostNow, 100, l, mem);   // still reported playing a moment later: not paused twice
        CHECK(!d.pause && d.seekMs < 0);
    }
    // The host moved on to the item local plays next: jump (prepared, gapless); unless local is about to get there.
    {
        SessionState n = s;
        n.queue.erase(n.queue.begin());
        n.positionMs = 300;
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, false, false, 120'000, 200'000};
        auto d = decide(n, n.atHostMs + 100, 0, l, mem);
        CHECK(d.load == Decision::Load::Jump && d.jumpIndex == 0);
        l.positionMs = 199'200;
        d = decide(n, n.atHostMs + 100, 0, l, mem);
        CHECK(d.load == Decision::Load::None && d.hold == Decision::Hold::Waiting);
        // Something else entirely: the whole queue again.
        LocalView other{"Z", {"Y"}, true, false, false, 10'000, 200'000};
        d = decide(n, n.atHostMs + 100, 0, other, mem);
        CHECK(d.load == Decision::Load::Queue);
    }
    // The upcoming items changed (a guest added a song): replace them, keep playing.
    {
        SyncMemory mem;
        LocalView l{"A", {"C"}, true, false, false, 32'000, 200'000};
        auto d = decide(s, hostNow, 0, l, mem);
        CHECK(d.replaceUpcoming && d.load == Decision::Load::None && d.seekMs < 0);
    }
    // Buffering: no seeks.
    {
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, true, false, 0, 200'000};
        CHECK(decide(s, hostNow, 0, l, mem).seekMs < 0);
    }
    // Unsupported items (podcast / radio) and an idle host: hold, paused.
    {
        SessionState u = s;
        u.queue[0].kind = ItemKind::Unsupported;
        SyncMemory mem;
        LocalView l{"Q", {}, true, false, false, 0, 0};
        auto d = decide(u, hostNow, 0, l, mem);
        CHECK(d.hold == Decision::Hold::Unsupported && d.pause && d.load == Decision::Load::None);
        SessionState idle = s;
        idle.queue.clear();
        CHECK(decide(idle, hostNow, 0, l, mem).hold == Decision::Hold::Idle);
    }
    // The host hasn't found the song's video yet: wait instead of matching it ourselves.
    {
        SessionState w = s;
        w.queue[0].videoId.clear();
        w.status = HostStatus::Loading;
        SyncMemory mem;
        auto d = decide(w, hostNow, 0, LocalView{}, mem);
        CHECK(d.hold == Decision::Hold::Waiting && d.load == Decision::Load::None);
        w.queue[0].kind = ItemKind::File;   // a file of the host: guests look it up by name right away
        d = decide(w, hostNow, 0, LocalView{}, mem);
        CHECK(d.load == Decision::Load::Queue && !d.autoplay);
    }
    // Host rebuffering mid-song: keep playing.
    {
        SessionState b = s;
        b.status = HostStatus::Loading;
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, false, false, 31'000, 200'000};
        auto d = decide(b, hostNow, 0, l, mem);
        CHECK(!d.pause && d.seekMs < 0);
    }
    // Failures: the item fails twice -> held (paused) until the host moves on.
    {
        SyncMemory mem;
        auto d = decide(s, hostNow, 0, LocalView{}, mem);   // attempt 1
        CHECK(d.load == Decision::Load::Queue);
        LocalView err{"A", {"B", "C"}, false, false, true, 0, 0};
        d = decide(s, hostNow, 0, err, mem);                  // counted once, however often it is seen
        d = decide(s, hostNow, 0, err, mem);
        CHECK(mem.attempts == 1 + 1 || mem.attempts == 2);
        CHECK(d.hold == Decision::Hold::Failed && mem.failed.count("A"));
        LocalView moved{"B", {"C"}, true, false, false, 1000, 200'000};
        d = decide(s, hostNow, 0, moved, mem);
        CHECK(d.hold == Decision::Hold::Failed && d.pause);   // the player moved on by itself: held
        SessionState next = s;
        next.queue.erase(next.queue.begin());
        d = decide(next, hostNow, 0, moved, mem);             // the host moved on: followed again
        CHECK(d.hold == Decision::Hold::None);
    }
    {
        // Loads that never start count too.
        SyncMemory mem;
        decide(s, hostNow, 0, LocalView{}, mem);
        LocalView gone{"B", {"C"}, false, false, false, 0, 0};   // advanced past A after an error
        auto d = decide(s, hostNow, 0, gone, mem);               // attempt 2
        CHECK(d.load == Decision::Load::Jump || d.load == Decision::Load::Queue);
        d = decide(s, hostNow, 0, gone, mem);
        CHECK(d.hold == Decision::Hold::Failed);
    }
    // Seek lead learning: 400 ms behind after the seek -> aim 200 ms further next time.
    {
        SyncMemory mem;
        LocalView l{"A", {"B", "C"}, true, false, false, 34'000, 200'000};
        auto d = decide(s, hostNow, 0, l, mem);
        CHECK(d.seekMs == 32'100 && mem.pending == SyncMemory::Pending::Seek);
        l.positionMs = 32'000 + 1'000 - 400;   // a second later, 400 ms behind
        d = decide(s, hostNow + 1'000, 1'000, l, mem);
        CHECK(mem.pending == SyncMemory::Pending::None && mem.seekLeadMs == 300);
        learnSeekLead(mem, -10'000);
        CHECK(mem.seekLeadMs == 1500);
        learnSeekLead(mem, 10'000);
        CHECK(mem.seekLeadMs == 0);
        learnLoadLead(mem, -20'000);
        CHECK(mem.loadLeadMs == 4000);
    }
    // No seeks into the last seconds of a track.
    {
        SyncMemory mem;
        SessionState e = s;
        e.positionMs = 198'900;
        LocalView l{"A", {"B", "C"}, true, false, false, 196'000, 200'000};
        CHECK(decide(e, e.atHostMs, 0, l, mem).seekMs < 0);
    }
}

// ---- URL protocol -----------------------------------------------------------------------------------------------------

static void testProtocol() {
    std::printf("protocol\n");
    CHECK(protocolKeyOverridden());
    const std::wstring key = protocolKeyPath();
    CHECK(key.find(L"ShadeTubeTest") != std::wstring::npos);
    const std::filesystem::path exe = L"C:\\Program Files\\Test Ş\\ShadeTube.exe";
    CHECK(protocolCommand(exe) == L"\"C:\\Program Files\\Test Ş\\ShadeTube.exe\" --open-link \"%1\"");
    CHECK(registerUrlProtocol(exe, key));
    CHECK(!registerUrlProtocol(exe, key));   // unchanged: nothing written
    CHECK(registeredCommand(key) == protocolCommand(exe));
    CHECK(registerUrlProtocol(L"C:\\Other\\ShadeTube.exe", key));   // a moved exe is rewritten
    CHECK(unregisterUrlProtocol(key) && registeredCommand(key).empty());
    CHECK(unregisterUrlProtocol(key));   // already gone
}

// ---- live -------------------------------------------------------------------------------------------------------------

namespace {

struct Inbox {
    std::mutex m;
    std::condition_variable cv;
    std::vector<mq::Publish> msgs;
    mq::Client::State state = mq::Client::State::Stopped;
    std::string broker;
    void push(const mq::Publish& p) {
        {
            std::lock_guard lk(m);
            msgs.push_back(p);
        }
        cv.notify_all();
    }
    void setState(mq::Client::State s, const std::string& b) {
        {
            std::lock_guard lk(m);
            state = s;
            if (!b.empty()) broker = b;
        }
        cv.notify_all();
    }
    bool waitState(mq::Client::State s, int ms) {
        std::unique_lock lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return state == s; });
    }
    // The first message matching `pred` (removed), waiting up to `ms`.
    std::optional<mq::Publish> take(const std::function<bool(const mq::Publish&)>& pred, int ms) {
        std::unique_lock lk(m);
        std::optional<mq::Publish> out;
        cv.wait_for(lk, std::chrono::milliseconds(ms), [&] {
            for (auto it = msgs.begin(); it != msgs.end(); ++it)
                if (pred(*it)) {
                    out = *it;
                    msgs.erase(it);
                    return true;
                }
            return false;
        });
        return out;
    }
};

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

mq::Client::Events eventsFor(Inbox& in) {
    mq::Client::Events ev;
    ev.onState = [&in](mq::Client::State s, const std::string& b, const std::string&) { in.setState(s, b); };
    ev.onMessage = [&in](const mq::Publish& p) { in.push(p); };
    return ev;
}

bool liveBroker(const std::string& url) {
    std::printf("live: %s\n", url.c_str());
    const int failuresBefore = g_failures;
    const Invite inv = newInvite(url);
    const Topics t = topicsFor(inv.roomId);
    const std::string hostDev = toHex(randomBytes(8)), guestDev = toHex(randomBytes(8));
    SequenceCounter hostSeq, guestSeq;
    hostSeq.newEpoch(std::time(nullptr));
    guestSeq.newEpoch(std::time(nullptr));

    // Host: connects, publishes the retained (sealed) state.
    Inbox hostIn;
    mq::Client host(eventsFor(hostIn));
    mq::Client::Config hc;
    hc.brokers = {url};
    hc.subscriptions = {t.guest};
    hc.failuresBeforeNext = 3;
    const int64_t t0 = nowMs();
    host.start(hc);
    if (!hostIn.waitState(mq::Client::State::Connected, 30'000)) {
        std::printf("  FAIL could not connect within 30 s\n");
        ++g_failures;
        return false;
    }
    std::printf("  host connected in %lld ms\n", static_cast<long long>(nowMs() - t0));
    SessionState s = hostState();
    s.hostDevice = hostDev;
    s.broker = url;
    s.queue = {item("spotify:track:live")};
    Envelope st{"state", hostDev, hostSeq.next(), toJson(s)};
    CHECK(host.publish(t.state, sealEnvelope(inv.key, t.state, st), true));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // retained by the broker before the guest subscribes

    // Guest: subscribes later and still gets the state (retained); its will goes to the guest topic.
    Inbox guestIn;
    mq::Client guest(eventsFor(guestIn));
    mq::Client::Config gc;
    gc.brokers = {url};
    gc.subscriptions = {t.all};
    gc.failuresBeforeNext = 3;
    Envelope will{"bye", guestDev, guestSeq.willSeq(), {{"reason", "lost"}}};
    gc.will = mq::Will{t.guest, sealEnvelope(inv.key, t.guest, will), false};
    const int64_t g0 = nowMs();
    guest.start(gc);
    CHECK(guestIn.waitState(mq::Client::State::Connected, 30'000));
    std::printf("  guest connected in %lld ms\n", static_cast<long long>(nowMs() - g0));
    auto retained = guestIn.take([&](const mq::Publish& p) { return p.topic == t.state; }, 10'000);
    CHECK(retained.has_value());
    if (retained) {
        CHECK(retained->retain);
        OpenError why{};
        const auto e = openEnvelope(inv.key, t.state, retained->payload, &why);
        CHECK(e && e->type == "state");
        const auto got = e ? stateFromJson(e->body) : std::nullopt;
        CHECK(got && got->queue.size() == 1 && got->queue[0].id == "spotify:track:live" && got->hostDevice == hostDev);
        CHECK(!openEnvelope(randomBytes(32), t.state, retained->payload));   // useless without the key
    }

    // Guest -> host: a request, and clock pings answered by the host.
    Request add;
    add.op = RequestOp::Add;
    add.item = item("spotify:track:added");
    const int64_t r0 = nowMs();
    CHECK(guest.publish(t.guest, sealEnvelope(inv.key, t.guest, Envelope{"req", guestDev, guestSeq.next(), toJson(add)})));
    auto req = hostIn.take([&](const mq::Publish& p) { return p.topic == t.guest; }, 10'000);
    CHECK(req.has_value());
    if (req) {
        const auto e = openEnvelope(inv.key, t.guest, req->payload);
        const auto r = e ? requestFromJson(e->body) : std::nullopt;
        CHECK(r && r->item.id == "spotify:track:added");
        std::printf("  guest -> host request in %lld ms\n", static_cast<long long>(nowMs() - r0));
    }
    ClockEstimator clock;
    for (int i = 0; i < 5; ++i) {
        const int64_t t1 = nowMs();
        guest.publish(t.guest, sealEnvelope(inv.key, t.guest, Envelope{"ping", guestDev, guestSeq.next(), {{"t1", t1}}}));
        auto ping = hostIn.take([&](const mq::Publish& p) { return p.topic == t.guest; }, 10'000);
        if (!ping) break;
        const auto pe = openEnvelope(inv.key, t.guest, ping->payload);
        if (!pe || pe->type != "ping") continue;
        const int64_t t2 = nowMs() + 1'000'000;   // the "host clock": this machine's + 1000 s
        host.publish(t.host, sealEnvelope(inv.key, t.host,
                                          Envelope{"pong", hostDev, hostSeq.next(),
                                                   {{"to", guestDev}, {"t1", pe->body["t1"]}, {"t2", t2}, {"t3", nowMs() + 1'000'000}}}));
        auto pong = guestIn.take([&](const mq::Publish& p) { return p.topic == t.host; }, 10'000);
        if (!pong) break;
        const auto po = openEnvelope(inv.key, t.host, pong->payload);
        if (po) clock.add(po->body["t1"].get<int64_t>(), po->body["t2"].get<int64_t>(), po->body["t3"].get<int64_t>(), nowMs());
    }
    CHECK(clock.samples() == 5);
    std::printf("  clock: offset %lld ms (expected 1000000 +- rtt/2), rtt %lld ms\n", static_cast<long long>(clock.offsetMs()),
                static_cast<long long>(clock.rttMs()));
    CHECK(std::llabs(clock.offsetMs() - 1'000'000) <= std::max<int64_t>(50, clock.rttMs()));

    // The guest's connection drops: the broker publishes its last will to the host.
    guest.abortConnection();
    auto lost = hostIn.take(
        [&](const mq::Publish& p) {
            const auto e = p.topic == t.guest ? openEnvelope(inv.key, t.guest, p.payload) : std::nullopt;
            return e && e->type == "bye";
        },
        20'000);
    CHECK(lost.has_value());
    if (lost) {
        const auto e = openEnvelope(inv.key, t.guest, lost->payload);
        CHECK(e && e->seq == guestSeq.willSeq() && e->body["reason"] == "lost");
    }
    // It reconnects by itself; a goodbye published right before stop() still arrives (flushed, then a clean close).
    CHECK(guestIn.waitState(mq::Client::State::Connecting, 5'000) || guestIn.waitState(mq::Client::State::Connected, 1));
    CHECK(guestIn.waitState(mq::Client::State::Connected, 30'000));
    guestSeq.newEpoch(std::time(nullptr) + 1);
    CHECK(guest.publish(t.guest, sealEnvelope(inv.key, t.guest, Envelope{"bye", guestDev, guestSeq.next(), {{"reason", "leave"}}})));
    guest.stop(std::chrono::milliseconds(500));
    auto bye = hostIn.take(
        [&](const mq::Publish& p) {
            const auto e = p.topic == t.guest ? openEnvelope(inv.key, t.guest, p.payload) : std::nullopt;
            return e && e->type == "bye" && e->body["reason"] == "leave";
        },
        10'000);
    CHECK(bye.has_value());

    // Ending: an empty retained message clears the state for later guests.
    CHECK(host.publish(t.state, {}, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Inbox lateIn;
    mq::Client late(eventsFor(lateIn));
    mq::Client::Config lc = gc;
    lc.will.reset();
    late.start(lc);
    CHECK(lateIn.waitState(mq::Client::State::Connected, 30'000));
    CHECK(!lateIn.take([&](const mq::Publish& p) { return p.topic == t.state && !p.payload.empty(); }, 4'000));
    late.stop(std::chrono::milliseconds(500));
    const auto stats = host.stats();
    host.stop(std::chrono::milliseconds(1000));
    std::printf("  host: %d connect(s), %llu packet(s) sent, %llu received\n", stats.connects,
                static_cast<unsigned long long>(stats.packetsSent), static_cast<unsigned long long>(stats.packetsReceived));
    return g_failures == failuresBefore;
}

} // namespace

static int runLive(int argc, char** argv) {
    std::vector<std::string> brokers;
    for (int i = 2; i < argc; ++i) brokers.emplace_back(argv[i]);
    if (brokers.empty()) brokers = defaultBrokers();
    int ok = 0;
    for (const auto& b : brokers)
        if (liveBroker(b)) ++ok;
    std::printf("live: %d of %zu broker(s) passed\n", ok, brokers.size());
    return ok > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // A per-process temp profile and registry key: nothing of the user's is touched.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path dir = std::filesystem::path(tmp) / (L"shadetube-jam-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", dir.c_str());
    const std::wstring key = L"Software\\ShadeTubeTest\\jam_test_" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"SHADETUBE_PROTOCOL_KEY", (L"HKCU\\" + key).c_str());
    st::log::init();

    const bool live = argc > 1 && std::strcmp(argv[1], "live") == 0;
    int rc = 0;
    if (live) {
        rc = runLive(argc, argv);
    } else {
        testVarint();
        testPackets();
        testDecoder();
        testBrokerUrls();
        testCrypto();
        testEnvelope();
        testInvite();
        testState();
        testClock();
        testReconcile();
        testProtocol();
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest");   // only when empty
    st::log::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\n%s\n", live ? (rc == 0 ? "live checks passed" : "live checks FAILED") : "all checks passed");
    return rc;
}
