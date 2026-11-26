// blend_test: checks for Blend (app/Blend: taste profiles, the shared playlist, the taste match, the store record), on
// synthetic histories (offline):
//   keys       song / artist keys: 16 hex digits, stable, the same for the same folded name
//   profile    streams only, podcast / radio ids out, recent plays weigh more, the six-month window and its fallback
//              to the whole history, caps, weights 1..100 heaviest first, features credit every artist, the hour
//   json       round trip, hostile input (wrong types, bad keys / member ids, duplicates, caps, non-http covers)
//   mix        shared songs first (by how many, then how much), then one pick per member in turn, whose taste each
//              row is, the 50-song cap, three members, the newest profile per member, the same mix in any input order
//   match      identical = 100, disjoint = 0, symmetric, in between, more members
//   store      the blend record round trip, members who left are not blended
//   timing     a profile from 300 000 plays
//   live       (`blend_test live [wss://broker...]`) two members who are never online together: A shares (retained,
//              sealed) and goes, B comes later, reads A and shares, A comes back and reads B; both build the same mix.
//              A member leaving replaces its profile with a sealed "left" record.
#include "app/Blend.h"
#include "app/JamCrypto.h"
#include "app/JamInvite.h"
#include "app/JamMqtt.h"
#include "app/JamProtocol.h"
#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::app;
using json = nlohmann::json;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

constexpr int64_t kDay = 86'400;
constexpr int64_t kNow = 1'790'000'000;

struct History {
    std::vector<ListenStats::Track> tracks;
    std::vector<ListenStats::Play> plays;
    uint32_t song(const std::string& artist, const std::string& name, std::string feat = {}, std::string id = {}) {
        ListenStats::Track t;
        t.id = id.empty() ? "spotify:track:" + artist + "-" + name : id;
        t.name = name;
        t.artists = {{"a-" + artist, artist}};
        if (!feat.empty()) t.artists.push_back({"a-" + feat, feat});
        t.albumName = artist + " album";
        t.image = "https://i.scdn.co/image/" + artist;
        t.durationMs = 200'000;
        tracks.push_back(t);
        return static_cast<uint32_t>(tracks.size() - 1);
    }
    void play(uint32_t track, int times, int64_t daysAgo, int64_t listenedMs = 180'000) {
        for (int i = 0; i < times; ++i) plays.push_back({track, kNow - daysAgo * kDay - i * 3600, listenedMs, false});
    }
    blend::Profile profile(const std::string& member, const std::string& name) {
        std::sort(plays.begin(), plays.end(), [](const auto& a, const auto& b) { return a.startedAt < b.startedAt; });
        auto p = blend::makeProfile(tracks, plays, kNow);
        p.member = member;
        p.name = name;
        return p;
    }
};

// A member who streams `n` songs of `artists`, the i-th one (n - i) times in the last weeks.
blend::Profile member(const std::string& id, const std::string& name, const std::vector<std::string>& artists, int perArtist,
                      int offset = 0) {
    History h;
    int k = 0;
    for (const auto& a : artists)
        for (int i = 0; i < perArtist; ++i, ++k) h.play(h.song(a, a + " song " + std::to_string(i + offset)), 1 + (perArtist - i), 3 + k % 20);
    return h.profile(id, name);
}

const blend::TasteTrack* findTrack(const blend::Profile& p, const std::string& title) {
    for (const auto& t : p.tracks)
        if (t.title == title) return &t;
    return nullptr;
}

void testKeys() {
    std::printf("keys\n");
    const std::string k = blend::keyOf(L"song\x1f" L"artist");
    CHECK(k.size() == 16 && k.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(k == blend::keyOf(L"song\x1f" L"artist") && k != blend::keyOf(L"song\x1f" L"other"));
    // The same song from two sources (different ids, case) has one key.
    ListenStats::Track a, b;
    a.id = "spotify:track:1";
    a.name = "Gece";
    a.artists = {{"", "Duman"}};
    b.id = "local:2";
    b.name = "GECE";
    b.artists = {{"", "duman"}};
    CHECK(blend::keyOf(ListenStats::keysFor(a).track) == blend::keyOf(ListenStats::keysFor(b).track));
}

void testProfile() {
    std::printf("profile\n");
    History h;
    const uint32_t top = h.song("Duman", "Bu Akşam", "Teoman");
    const uint32_t mid = h.song("Duman", "Elleri Ellerime");
    const uint32_t old = h.song("Mor ve Ötesi", "Bir Derdim Var");
    const uint32_t skipped = h.song("Sezen Aksu", "Firuze");
    const uint32_t pod = h.song("Podcast", "Bölüm 1", {}, "podcast:0123456789abcdef");
    const uint32_t radio = h.song("Radyo", "Canlı", {}, "radio:abc");
    for (int i = 0; i < 20; ++i) h.play(h.song("Filler " + std::to_string(i), "Song " + std::to_string(i)), 1, 10 + i);
    h.play(top, 10, 2);
    h.play(mid, 10, 120);       // as often, but four months ago: weighs less
    h.play(old, 30, 400);       // outside the six months
    h.play(skipped, 20, 1, 8'000);   // 8 s each: never a stream
    h.play(pod, 10, 1);
    h.play(radio, 10, 1);
    const auto p = h.profile("0011223344556677", "Berkay");
    CHECK(p.windowDays == 180 && p.at % 3600 == 0 && p.at <= kNow && kNow - p.at < 3600);
    CHECK(!p.tracks.empty() && p.tracks[0].title == "Bu Akşam" && p.tracks[0].weight == 100);
    const auto* m = findTrack(p, "Elleri Ellerime");
    CHECK(m && m->weight < 50 && m->weight >= 1);
    CHECK(!findTrack(p, "Bir Derdim Var") && !findTrack(p, "Firuze") && !findTrack(p, "Bölüm 1") && !findTrack(p, "Canlı"));
    CHECK(std::is_sorted(p.tracks.begin(), p.tracks.end(), [](const auto& a, const auto& b) { return a.weight > b.weight; }));
    CHECK(p.tracks[0].cover == "https://i.scdn.co/image/Duman" && p.tracks[0].artists.size() == 2 && p.tracks[0].durationMs == 200'000);
    // Artists: Duman first; Teoman credited through the feature.
    CHECK(!p.artists.empty() && p.artists[0].name == "Duman" && p.artists[0].weight == 100);
    CHECK(std::any_of(p.artists.begin(), p.artists.end(), [](const auto& a) { return a.name == "Teoman"; }));
    CHECK(std::none_of(p.artists.begin(), p.artists.end(), [](const auto& a) { return a.name == "Mor ve Ötesi" || a.name == "Podcast"; }));
    CHECK(blend::usable(p));
    // Caps.
    History big;
    for (int i = 0; i < 300; ++i) big.play(big.song("Artist " + std::to_string(i % 80), "S" + std::to_string(i)), 1 + i % 7, i % 90);
    const auto bp = big.profile("0011223344556677", "B");
    CHECK(bp.tracks.size() == blend::kProfileTracks && bp.artists.size() == blend::kProfileArtists);
    for (const auto& t : bp.tracks) CHECK(t.weight >= 1 && t.weight <= 100);
    // Little in the window: the whole history instead.
    History sparse;
    for (int i = 0; i < 12; ++i) sparse.play(sparse.song("Old", "Old " + std::to_string(i)), 3, 500 + i);
    sparse.play(sparse.song("New", "New"), 2, 3);
    const auto sp = sparse.profile("0011223344556677", "S");
    CHECK(sp.windowDays == 0 && sp.tracks.size() == 13);
    // Nothing: an empty profile that isn't usable.
    History none;
    const auto np = none.profile("0011223344556677", "N");
    CHECK(np.tracks.empty() && !blend::usable(np));
    // The same history in another track order gives the same profile (ties broken by key).
    History t1, t2;
    for (int i = 0; i < 10; ++i) t1.play(t1.song("Tie", "T" + std::to_string(i)), 2, 5);
    for (int i = 9; i >= 0; --i) t2.play(t2.song("Tie", "T" + std::to_string(i)), 2, 5);
    const auto a1 = t1.profile("0011223344556677", "x"), a2 = t2.profile("0011223344556677", "x");
    CHECK(a1.tracks.size() == a2.tracks.size());
    bool same = a1.tracks.size() == a2.tracks.size();
    for (size_t i = 0; same && i < a1.tracks.size(); ++i) same = a1.tracks[i].key == a2.tracks[i].key;
    CHECK(same);
}

void testJson() {
    std::printf("json\n");
    const auto p = member("0011223344556677", "Berkay", {"Duman", "Teoman"}, 8);
    const auto back = blend::profileFromJson(json::parse(blend::toJson(p).dump()));
    CHECK(back && *back == p);
    // Only what is listed in the profile travels: no counts, no times beyond the hour, no ids of this PC.
    const std::string text = blend::toJson(p).dump();
    CHECK(text.find("listened") == std::string::npos && text.find("startedAt") == std::string::npos);
    json j = blend::toJson(p);
    CHECK(!blend::profileFromJson(json::array()));
    json bad = j;
    bad["v"] = 2;
    CHECK(!blend::profileFromJson(bad));
    bad = j;
    bad["m"] = "../../etc";
    CHECK(!blend::profileFromJson(bad));
    bad = j;
    bad["t"][0]["k"] = "zz";                       // a bad key: that row goes
    bad["t"][1]["k"] = j["t"][2]["k"];             // a duplicate key: the second one goes
    bad["t"][3]["c"] = "file:///C:/x.jpg";         // not http(s): no cover
    bad["t"][4]["w"] = 1000;                       // clamped
    bad["t"][5]["a"] = "nope";                     // wrong type: no artists
    bad["n"] = std::string(500, 'x');
    for (int i = 0; i < 100; ++i) bad["t"].push_back(j["t"][6]);   // more duplicates
    const auto odd = blend::profileFromJson(bad);
    CHECK(odd && odd->tracks.size() == p.tracks.size() - 2 && odd->tracks.size() <= blend::kProfileTracks);
    CHECK(odd && odd->name.size() <= 120);
    if (odd) {
        CHECK(std::none_of(odd->tracks.begin(), odd->tracks.end(), [](const auto& t) { return t.cover.rfind("file:", 0) == 0; }));
        CHECK(std::all_of(odd->tracks.begin(), odd->tracks.end(), [](const auto& t) { return t.weight >= 1 && t.weight <= 100; }));
    }
}

void testMix() {
    std::printf("mix\n");
    // A and B share Duman (3 songs each in common); A also likes Teoman, B likes Sezen Aksu.
    auto a = member("aaaaaaaaaaaaaaaa", "Ayşe", {"Duman", "Teoman"}, 6);
    auto b = member("bbbbbbbbbbbbbbbb", "Berkay", {"Duman", "Sezen Aksu"}, 6);
    // Both lists hold "Duman song 0..5": six shared songs.
    const auto m = blend::build({b, a});
    CHECK(m.members.size() == 2 && m.members[0] == "aaaaaaaaaaaaaaaa" && m.names[0] == "Ayşe");
    CHECK(m.shared == 6);
    for (int i = 0; i < m.shared; ++i) CHECK(m.rows[static_cast<size_t>(i)].from.size() == 2);
    // Shared ones are heaviest first.
    CHECK(m.rows.size() > 6 && m.rows[0].track.name == "Duman song 0");
    // Then alternating: A, B, A, B...
    bool alternates = true;
    for (size_t i = static_cast<size_t>(m.shared); i + 1 < m.rows.size() && i < 14; ++i)
        alternates = alternates && m.rows[i].from.size() == 1 && m.rows[i].from[0] == static_cast<int>((i - m.shared) % 2);
    CHECK(alternates);
    CHECK(m.rows[static_cast<size_t>(m.shared)].track.name == "Teoman song 0");
    CHECK(m.rows[static_cast<size_t>(m.shared) + 1].track.name == "Sezen Aksu song 0");
    // No song twice, every row playable with an id.
    std::set<std::string> ids;
    for (const auto& r : m.rows) CHECK(ids.insert(r.track.id).second && r.track.playable && !r.track.name.empty());
    // The same profiles in any order (and on the other PC) give the same mix.
    CHECK(blend::sameMix(blend::build({a, b}), m));
    // The newest profile of a member wins; an older copy is ignored.
    auto oldA = member("aaaaaaaaaaaaaaaa", "Ayşe", {"Metallica"}, 6);
    oldA.at = a.at - 86400;
    CHECK(blend::sameMix(blend::build({oldA, b, a}), m));
    // 50 songs at most; when one member runs out, the other goes on.
    auto bigA = member("aaaaaaaaaaaaaaaa", "Ayşe", {"X1", "X2", "X3", "X4", "X5"}, 10);
    auto smallB = member("bbbbbbbbbbbbbbbb", "Berkay", {"Y"}, 5);
    const auto capped = blend::build({bigA, smallB});
    CHECK(capped.rows.size() == blend::kMaxRows && capped.shared == 0);
    CHECK(std::count_if(capped.rows.begin(), capped.rows.end(), [](const auto& r) { return r.from[0] == 1; }) == 5);
    // Three members: a song all three love comes before one two of them love.
    auto c = member("cccccccccccccccc", "Can", {"Duman", "Teoman"}, 3, 0);
    const auto m3 = blend::build({a, b, c});
    CHECK(m3.members.size() == 3 && !m3.rows.empty() && m3.rows[0].from.size() == 3);
    // One member: just their songs, no match.
    const auto solo = blend::build({a});
    CHECK(solo.match == -1 && solo.rows.size() == a.tracks.size() && solo.shared == 0);
    CHECK(blend::build({}).rows.empty());
}

void testMatch() {
    std::printf("match\n");
    auto a = member("aaaaaaaaaaaaaaaa", "A", {"Duman", "Teoman"}, 6);
    auto a2 = a;
    a2.member = "bbbbbbbbbbbbbbbb";
    auto distant = member("cccccccccccccccc", "C", {"Metallica", "Slayer"}, 6);
    auto half = member("dddddddddddddddd", "D", {"Duman", "Sezen Aksu"}, 6);
    CHECK(blend::tasteMatch({a, a2}) == 100);
    CHECK(blend::tasteMatch({a, distant}) == 0);
    const int m = blend::tasteMatch({a, half});
    std::printf("  half the taste in common: %d %%\n", m);
    CHECK(m > 40 && m < 100);
    CHECK(blend::tasteMatch({half, a}) == m);
    CHECK(blend::tasteMatch({a}) == -1);
    const int three = blend::tasteMatch({a, half, distant});
    CHECK(three > 0 && three < m);
}

void testStore() {
    std::printf("store\n");
    blend::Blend b;
    b.link = "shadetube://blend/AQA";
    b.me = "aaaaaaaaaaaaaaaa";
    b.mine = member("aaaaaaaaaaaaaaaa", "Ayşe", {"Duman"}, 5);
    b.others = {member("bbbbbbbbbbbbbbbb", "Berkay", {"Teoman"}, 5), member("cccccccccccccccc", "Can", {"Sezen"}, 5)};
    b.left = {"cccccccccccccccc"};
    b.creator = true;
    b.createdAt = kNow - 3600;
    b.syncedAt = kNow;
    b.othersChangedAt = kNow - 60;
    const auto back = blend::blendFromJson(json::parse(blend::toJson(b).dump()));
    CHECK(back && back->link == b.link && back->me == b.me && back->mine == b.mine && back->others == b.others);
    CHECK(back && back->left == b.left && back->creator && back->createdAt == b.createdAt && back->syncedAt == kNow);
    const auto ps = blend::profilesOf(b);
    CHECK(ps.size() == 2 && ps[0].member == b.me && ps[1].member == "bbbbbbbbbbbbbbbb");
    blend::Blend pending = b;
    pending.mine.reset();
    CHECK(blend::profilesOf(pending).size() == 1);
    json bad = blend::toJson(b);
    bad["me"] = "x";
    CHECK(!blend::blendFromJson(bad));
    bad = blend::toJson(b);
    bad["mine"]["m"] = "bbbbbbbbbbbbbbbb";   // a "mine" that isn't me is dropped
    CHECK(blend::blendFromJson(bad) && !blend::blendFromJson(bad)->mine);
}

void testTiming() {
    std::printf("timing\n");
    History h;
    std::mt19937 rng(7);
    for (int i = 0; i < 20'000; ++i) h.song("Artist " + std::to_string(i % 2'000), "Song " + std::to_string(i));
    for (int i = 0; i < 300'000; ++i)
        h.plays.push_back({static_cast<uint32_t>(rng() % 20'000), kNow - static_cast<int64_t>(rng() % (3 * 365)) * kDay, 150'000, false});
    std::sort(h.plays.begin(), h.plays.end(), [](const auto& a, const auto& b) { return a.startedAt < b.startedAt; });
    const auto t0 = std::chrono::steady_clock::now();
    const auto p = blend::makeProfile(h.tracks, h.plays, kNow);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  profile from 300 000 plays over 20 000 songs: %lld ms\n", static_cast<long long>(ms));
    CHECK(p.tracks.size() == blend::kProfileTracks && ms < 5000);
    auto other = p;
    other.member = "bbbbbbbbbbbbbbbb";
    const auto t1 = std::chrono::steady_clock::now();
    const auto m = blend::build({p, other});
    const auto ms2 = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t1).count();
    std::printf("  mix of two profiles: %lld us\n", static_cast<long long>(ms2));
    CHECK(m.shared == 50);
}

// ---- live --------------------------------------------------------------------------------------------------------

namespace mq = st::app::jam::mqtt;

struct Inbox {
    std::mutex m;
    std::condition_variable cv;
    std::vector<mq::Publish> msgs;
    bool connected = false;
};

// One member's visit to the broker: connect, share `mine` (retained, sealed), collect what is there for a few seconds.
std::vector<mq::Publish> visit(const std::string& broker, const std::string& base, const jam::Bytes& key, const std::string& member,
                               const std::string& type, const json& body, int collectMs) {
    Inbox in;
    mq::Client::Events ev;
    ev.onState = [&in](mq::Client::State s, const std::string&, const std::string&) {
        std::lock_guard lk(in.m);
        in.connected = s == mq::Client::State::Connected;
        in.cv.notify_all();
    };
    ev.onMessage = [&in](const mq::Publish& p) {
        std::lock_guard lk(in.m);
        in.msgs.push_back(p);
        in.cv.notify_all();
    };
    mq::Client c(ev);
    mq::Client::Config cfg;
    cfg.brokers = {broker};
    cfg.subscriptions = {base + "/#"};
    cfg.failuresBeforeNext = 3;
    const auto t0 = std::chrono::steady_clock::now();
    c.start(cfg);
    {
        std::unique_lock lk(in.m);
        if (!in.cv.wait_for(lk, std::chrono::seconds(30), [&] { return in.connected; })) {
            std::printf("  FAIL %s could not connect\n", member.c_str());
            ++g_failures;
            return {};
        }
    }
    std::printf("  %s connected in %lld ms\n", member.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    jam::SequenceCounter seq;
    seq.newEpoch(std::time(nullptr));
    const std::string topic = base + "/p/" + member;
    CHECK(c.publish(topic, jam::sealEnvelope(key, topic, jam::Envelope{type, member, seq.next(), body}), true));
    std::this_thread::sleep_for(std::chrono::milliseconds(collectMs));
    c.stop(std::chrono::milliseconds(800));
    std::lock_guard lk(in.m);
    return in.msgs;
}

// The other members' newest records ("taste" / "left") from what a visit collected.
std::vector<std::pair<std::string, json>> records(const std::vector<mq::Publish>& msgs, const std::string& base, const jam::Bytes& key,
                                                  const std::string& me) {
    std::vector<std::pair<std::string, json>> out;
    for (const auto& p : msgs) {
        if (p.payload.empty() || p.topic.rfind(base + "/p/", 0) != 0) continue;
        const std::string who = p.topic.substr(base.size() + 3);
        const auto e = jam::openEnvelope(key, p.topic, p.payload);
        if (!e || e->device != who || who == me) continue;
        out.emplace_back(e->type, e->body);
    }
    return out;
}

bool liveBroker(const std::string& broker) {
    std::printf("live: %s\n", broker.c_str());
    const int before = g_failures;
    const jam::Invite inv = jam::newInvite(broker);
    const std::string base = jam::blendTopicBase(inv.roomId);
    auto a = member("aaaaaaaaaaaaaaaa", "Ayşe", {"Duman", "Teoman"}, 6);
    auto b = member("bbbbbbbbbbbbbbbb", "Berkay", {"Duman", "Sezen Aksu"}, 6);
    // A creates the blend, shares and leaves.
    auto seenByA = visit(broker, base, inv.key, a.member, "taste", blend::toJson(a), 2'000);
    CHECK(records(seenByA, base, inv.key, a.member).empty());
    std::printf("  (A is offline now; B opens the link 5 s later)\n");
    std::this_thread::sleep_for(std::chrono::seconds(5));
    // B: finds A's retained profile, shares its own, leaves.
    auto seenByB = visit(broker, base, inv.key, b.member, "taste", blend::toJson(b), 4'000);
    const auto forB = records(seenByB, base, inv.key, b.member);
    CHECK(forB.size() == 1 && forB[0].first == "taste");
    std::optional<blend::Profile> aAtB = forB.empty() ? std::nullopt : blend::profileFromJson(forB[0].second);
    CHECK(aAtB && *aAtB == a);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    // A comes back: B's profile is there.
    seenByA = visit(broker, base, inv.key, a.member, "taste", blend::toJson(a), 4'000);
    const auto forA = records(seenByA, base, inv.key, a.member);
    CHECK(forA.size() == 1 && forA[0].first == "taste");
    std::optional<blend::Profile> bAtA = forA.empty() ? std::nullopt : blend::profileFromJson(forA[0].second);
    CHECK(bAtA && *bAtA == b);
    if (aAtB && bAtA) {
        const auto mixA = blend::build({a, *bAtA}), mixB = blend::build({*aAtB, b});
        CHECK(blend::sameMix(mixA, mixB));
        std::printf("  both built the same %zu-song mix (%d shared, match %d %%)\n", mixA.rows.size(), mixA.shared, mixA.match);
    }
    // Without the key the retained records are useless.
    for (const auto& p : seenByA)
        if (!p.payload.empty()) CHECK(!jam::openEnvelope(jam::randomBytes(32), p.topic, p.payload));
    // B leaves: its record becomes "left"; A sees that on its next visit.
    visit(broker, base, inv.key, b.member, "left", json::object(), 1'500);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    seenByA = visit(broker, base, inv.key, a.member, "left", json::object(), 4'000);   // A leaves too (cleans up)
    const auto last = records(seenByA, base, inv.key, a.member);
    CHECK(last.size() == 1 && last[0].first == "left");
    return g_failures == before;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path dir = std::filesystem::path(tmp) / (L"shadetube-blend-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", dir.c_str());
    st::log::init();
    const bool live = argc > 1 && std::strcmp(argv[1], "live") == 0;
    int rc = 0;
    if (live) {
        std::vector<std::string> brokers;
        for (int i = 2; i < argc; ++i) brokers.emplace_back(argv[i]);
        if (brokers.empty()) brokers = jam::defaultBrokers();
        int ok = 0;
        for (const auto& b : brokers)
            if (liveBroker(b)) ++ok;
        std::printf("live: %d of %zu broker(s) passed\n", ok, brokers.size());
        rc = ok > 0 ? 0 : 1;
    } else {
        testKeys();
        testProfile();
        testJson();
        testMix();
        testMatch();
        testStore();
        testTiming();
    }
    st::log::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (!live && g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\n%s\n", live ? (rc == 0 ? "live checks passed" : "live checks FAILED") : "all checks passed");
    return rc;
}
