// sync_test: checks for app/SyncRules (download sync: "Çevrimdışı kullanılabilir").
//
//   store     : sync.json round trip (rules, status, attempts), tolerant parsing (unknown / broken entries,
//               duplicates), a missing file = no rules (an old profile), a damaged file -> sync.json.bad + empty
//   filter    : downloadable (radio stations, local files, podcast episodes, unplayable), ruleTrackIds (order, once)
//   plan      : done / queued / downloading skipped (by anyone), blocked skipped, the retry policy for failed downloads
//               (1 h, 6 h, then given up), duplicates across one listing, the storage cap (estimates, used bytes)
//   drop      : wanted ids across rules, sync downloads no rule wants (queued -> cancel, downloaded -> removed only
//               when asked), the user's own downloads never, a suspicious empty listing
//   progress  : done / active / failed counts of a rule
//   schedule  : ruleDue (never listed, backoff, change debounce + least gap, pending relist, periodic intervals per
//               kind, clock going back), backoff steps (429: at least 10 min)
//
// Runs in a temp folder: the user's profile is never touched.
#include "app/SyncRules.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace st::app::sync;
namespace fs = std::filesystem;
using st::catalog::Track;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static Track track(std::string id, int durationMs = 200000) {
    Track t;
    t.id = std::move(id);
    t.name = "Song " + t.id;
    t.artists.push_back({"artist-1", "Artist"});
    t.durationMs = durationMs;
    return t;
}

using Items = std::unordered_map<std::string, ItemInfo>;
static ItemLookup lookupOf(const Items& items) {
    return [&items](const std::string& id) {
        const auto it = items.find(id);
        return it == items.end() ? ItemInfo{} : it->second;
    };
}
static ItemInfo info(ItemInfo::St st, bool synced = true, int64_t size = 0) {
    ItemInfo i;
    i.state = st;
    i.synced = synced;
    i.sizeBytes = size;
    return i;
}

static std::vector<std::string> ids(const std::vector<Track>& tracks) {
    std::vector<std::string> out;
    for (const auto& t : tracks) out.push_back(t.id);
    return out;
}

static void testStore(const fs::path& dir) {
    std::printf("store\n");
    State s;
    Rule a;
    a.id = "spotify:playlist:abc";
    a.kind = Kind::Playlist;
    a.name = "Gece Sürüşü";
    a.images = {{"https://i.example/a.jpg", 300, 300}};
    a.addedAt = 1700000000;
    a.lastSync = 1700000500;
    a.trackIds = {"spotify:track:1", "spotify:track:2"};
    Rule b;
    b.id = kLocalLikedId;
    b.kind = Kind::Liked;
    b.name = "Beğenilen Şarkılar";
    b.error = ListError::RateLimited;
    b.errorDetail = "HTTP 429";
    b.failures = 2;
    b.retryAt = 1700001000;
    b.changedAt = 1700000999;   // not persisted
    s.rules = {a, b};
    s.attempts["spotify:track:2"] = {2, 1700000400};

    const fs::path file = dir / "sync.json";
    CHECK(saveState(s, file));
    CHECK(!fs::exists(dir / "sync.json.tmp"));
    const State r = loadState(file);
    CHECK(r.rules.size() == 2);
    if (r.rules.size() == 2) {
        const Rule& x = r.rules[0];
        CHECK(x.id == a.id && x.kind == Kind::Playlist && x.name == a.name);
        CHECK(x.images.size() == 1 && x.images[0].url == "https://i.example/a.jpg" && x.images[0].width == 300);
        CHECK(x.addedAt == a.addedAt && x.lastSync == a.lastSync && x.trackIds == a.trackIds);
        CHECK(x.error == ListError::None && x.failures == 0);
        const Rule& y = r.rules[1];
        CHECK(y.id == kLocalLikedId && y.kind == Kind::Liked);
        CHECK(y.error == ListError::RateLimited && y.errorDetail == "HTTP 429" && y.failures == 2 && y.retryAt == 1700001000);
        CHECK(y.changedAt == 0);
    }
    CHECK(r.attempts.size() == 1 && r.attempts.at("spotify:track:2").count == 2 &&
          r.attempts.at("spotify:track:2").last == 1700000400);
    CHECK(findRule(r, "liked") != nullptr && findRule(r, "nope") == nullptr);
    CHECK(isSpotifyRule(r.rules[0]) && !isSpotifyRule(r.rules[1]));

    // An older profile has no sync.json: no rules, nothing breaks.
    const State none = loadState(dir / "missing.json");
    CHECK(none.rules.empty() && none.attempts.empty());

    // Tolerant parsing: junk entries, a duplicate id, a wrong error code, bad attempts.
    const auto j = nlohmann::json::parse(R"({
        "version": 1,
        "rules": [ 5, {"n": "no id"}, {"id": "album-mbid", "k": "album", "tracks": ["a", 7, "b"], "err": 9},
                   {"id": "album-mbid", "k": "playlist"}, {"id": "x", "k": "weird"} ],
        "attempts": {"a": [1, 100], "b": "x", "c": [1], "d": [-3, 5]}
    })");
    const State t = stateFromJson(j);
    CHECK(t.rules.size() == 2);
    if (t.rules.size() == 2) {
        CHECK(t.rules[0].kind == Kind::Album && t.rules[0].trackIds == std::vector<std::string>({"a", "b"}));
        CHECK(t.rules[0].error == ListError::None);
        CHECK(t.rules[1].id == "x" && t.rules[1].kind == Kind::Playlist);
    }
    CHECK(t.attempts.size() == 2 && t.attempts.at("a").count == 1 && t.attempts.at("d").count == 0);

    // A damaged file: kept as .bad, empty state.
    const fs::path broken = dir / "broken.json";
    {
        std::ofstream f(broken, std::ios::binary);
        f << "{ \"rules\": [ {\"id\": ";
    }
    const State d = loadState(broken);
    CHECK(d.rules.empty());
    CHECK(fs::exists(dir / "broken.json.bad"));
}

static void testFilter() {
    std::printf("filter\n");
    CHECK(downloadable(track("spotify:track:1")));
    CHECK(downloadable(track("0f5c1d3e-1111-2222-3333-444455556666")));
    CHECK(downloadable(track("import:abc")));
    CHECK(!downloadable(track("")));
    CHECK(!downloadable(track("radio:9617a958-0601-11e8-ae97-52543be04c81")));
    CHECK(!downloadable(track("local:1234abcd")));
    CHECK(!downloadable(track("podcast:77")));
    Track u = track("spotify:track:9");
    u.playable = false;
    CHECK(!downloadable(u));

    const std::vector<Track> list{track("a"), track("local:x"), track("b"), track("a"), track("c")};
    CHECK(ruleTrackIds(list) == std::vector<std::string>({"a", "b", "c"}));

    // Estimates: 200 s at 320 kbps = 8 MB + 64 KB; the original stream ~130 kbps; unknown duration = 3.5 min.
    CHECK(estimateBytes(200000, 320) == 200000LL * 320 / 8 + 65536);
    CHECK(estimateBytes(200000, 0) == 200000LL * 130 / 8 + 65536);
    CHECK(estimateBytes(0, 320) == estimateBytes(210000, 320));
}

static void testPlan() {
    std::printf("plan\n");
    const int64_t now = 1'800'000'000;
    const std::vector<Track> tracks{track("t1"), track("t2"), track("t3"), track("t4"), track("t5"),
                                    track("t6"), track("t2"), track("local:9"), track("t7")};
    Items items;
    items["t1"] = info(ItemInfo::St::Done, false, 5'000'000);   // the user downloaded it
    items["t2"] = info(ItemInfo::St::Queued);
    items["t3"] = info(ItemInfo::St::Downloading);
    items["t4"] = info(ItemInfo::St::Failed);                   // attempts below
    items["t5"] = info(ItemInfo::St::Failed);
    items["t6"] = info(ItemInfo::St::Failed);
    std::unordered_map<std::string, Attempt> attempts;
    attempts["t4"] = {1, now - 2 * 3600};    // 1 attempt, 2 h ago -> retry
    attempts["t5"] = {2, now - 3600};        // 2 attempts, 1 h ago -> waits (6 h)
    attempts["t6"] = {3, now - 30 * 86400};  // gave up
    const auto blocked = [](const Track& t) { return t.id == "t7"; };
    Limits lim;
    Plan p = plan(tracks, lookupOf(items), blocked, attempts, lim, now);
    CHECK(ids(p.toQueue) == std::vector<std::string>({"t4"}));
    CHECK(p.waitingRetry == 1 && p.gaveUp == 1 && p.skippedCap == 0);
    CHECK(p.queuedBytes == estimateBytes(200000, 320));

    // Retry schedule.
    CHECK(retryDue({0, 0}, now));
    CHECK(retryDue({1, now - 3600}, now) && !retryDue({1, now - 3599}, now));
    CHECK(retryDue({2, now - 6 * 3600}, now) && !retryDue({2, now - 3600}, now));
    CHECK(!retryDue({3, 0}, now));
    // A failed download sync knows nothing about (the user's own, or attempts pruned) is retried once.
    Items failedUnknown;
    failedUnknown["n1"] = info(ItemInfo::St::Failed, false);
    p = plan({track("n1")}, lookupOf(failedUnknown), {}, {}, lim, now);
    CHECK(p.toQueue.size() == 1);

    // New tracks, in collection order, each once.
    Items empty;
    p = plan({track("x1"), track("x2"), track("x1"), track("x3")}, lookupOf(empty), {}, {}, lim, now);
    CHECK(ids(p.toQueue) == std::vector<std::string>({"x1", "x2", "x3"}));

    // Storage cap: room for two 8 MB songs after 10 MB used.
    lim.capBytes = 10'000'000 + 2 * estimateBytes(200000, 320) + 1000;
    lim.usedBytes = 10'000'000;
    p = plan({track("c1"), track("c2"), track("c3"), track("c4")}, lookupOf(empty), {}, {}, lim, now);
    CHECK(ids(p.toQueue) == std::vector<std::string>({"c1", "c2"}));
    CHECK(p.skippedCap == 2);
    // A short track still fits after a long one did not.
    lim.capBytes = 10'000'000 + estimateBytes(60000, 320) + 10;
    p = plan({track("long", 600000), track("short", 60000)}, lookupOf(empty), {}, {}, lim, now);
    CHECK(ids(p.toQueue) == std::vector<std::string>({"short"}) && p.skippedCap == 1);
    // Cap already exceeded: nothing.
    lim.usedBytes = lim.capBytes + 1;
    p = plan({track("z")}, lookupOf(empty), {}, {}, lim, now);
    CHECK(p.toQueue.empty() && p.skippedCap == 1);
    // Passthrough estimate is smaller than MP3 320.
    lim = {};
    lim.mp3Kbps = 0;
    p = plan({track("m")}, lookupOf(empty), {}, {}, lim, now);
    CHECK(p.queuedBytes == estimateBytes(200000, 0));
}

static void testDrop() {
    std::printf("drop\n");
    Rule a, b;
    a.id = "A";
    a.trackIds = {"1", "2", "3"};
    b.id = "B";
    b.trackIds = {"3", "4"};
    const auto wanted = wantedIds({a, b});
    CHECK(wanted.size() == 4 && wanted.contains("4") && !wanted.contains("5"));

    const std::vector<std::pair<std::string, ItemInfo>> items{
        {"1", info(ItemInfo::St::Done)},                 // wanted
        {"5", info(ItemInfo::St::Done, true, 100)},      // sync download no rule wants
        {"6", info(ItemInfo::St::Done, false, 100)},     // the user's own: never
        {"7", info(ItemInfo::St::Queued)},               // queued by sync, not wanted -> cancel
        {"8", info(ItemInfo::St::Failed)},               // failed sync item -> cancel
        {"9", info(ItemInfo::St::Downloading)},          // downloading -> cancel
        {"10", info(ItemInfo::St::Queued, false)},       // the user's queued download: never
    };
    Drop d = dropped(items, wanted, false);
    CHECK(d.remove.empty());
    CHECK(d.cancel.size() == 3);
    d = dropped(items, wanted, true);
    CHECK(d.remove == std::vector<std::string>({"5"}));
    CHECK(d.cancel.size() == 3);
    // Removing rule B: "4" is no longer wanted, "3" still is (rule A).
    const auto wantedA = wantedIds({a});
    d = dropped({{"3", info(ItemInfo::St::Done)}, {"4", info(ItemInfo::St::Done)}}, wantedA, true);
    CHECK(d.remove == std::vector<std::string>({"4"}));

    CHECK(suspiciousShrink(120, 0));
    CHECK(!suspiciousShrink(0, 0) && !suspiciousShrink(120, 3) && !suspiciousShrink(5, 6));
}

static void testProgress() {
    std::printf("progress\n");
    Rule r;
    r.trackIds = {"1", "2", "3", "4", "5", "6"};
    Items items;
    items["1"] = info(ItemInfo::St::Done);
    items["2"] = info(ItemInfo::St::Done, false);
    items["3"] = info(ItemInfo::St::Queued);
    items["4"] = info(ItemInfo::St::Downloading);
    items["5"] = info(ItemInfo::St::Failed);
    const Progress p = progress(r, lookupOf(items));
    CHECK(p.total == 6 && p.done == 2 && p.active == 2 && p.failed == 1);
}

static void testSchedule() {
    std::printf("schedule\n");
    const int64_t now = 1'800'000'000;
    Rule pl;
    pl.id = "spotify:playlist:x";
    pl.kind = Kind::Playlist;
    Rule liked;
    liked.id = kSpotifyLikedId;
    liked.kind = Kind::Liked;
    Rule album;
    album.id = "spotify:album:y";
    album.kind = Kind::Album;
    Rule local;
    local.id = "local:abc";
    local.kind = Kind::Playlist;

    CHECK(ruleDue(pl, now, false));   // never listed
    pl.retryAt = now + 10;
    CHECK(!ruleDue(pl, now, false));  // backing off
    pl.retryAt = 0;

    pl.lastSync = now - 10 * 60;
    CHECK(!ruleDue(pl, now, false));
    CHECK(ruleDue(pl, now + 35 * 60, false));   // 45 min periodic
    // A change: debounced, then at least a minute after the last listing.
    pl.changedAt = now - 2;
    CHECK(!ruleDue(pl, now, false));
    pl.changedAt = now - 6;
    CHECK(ruleDue(pl, now, false));
    pl.lastSync = now - 30;
    pl.changedAt = now - 10;
    CHECK(!ruleDue(pl, now, false));            // listed 30 s ago
    CHECK(ruleDue(pl, now + 31, false));
    // Pending tracks: listed again after 30 min.
    pl.changedAt = 0;
    pl.lastSync = now - 31 * 60;
    CHECK(ruleDue(pl, now, true) && !ruleDue(pl, now, false));
    // The clock went back.
    pl.lastSync = now + 3600;
    CHECK(ruleDue(pl, now, false));

    CHECK(relistInterval(pl) == 45 * 60 && relistInterval(liked) == 6 * 3600 && relistInterval(album) == 24 * 3600);
    CHECK(relistInterval(local) == 45 * 60);
    CHECK(minRelistGap(liked) == 5 * 60 && minRelistGap(pl) == 60 && minRelistGap(local) == 0);
    liked.lastSync = now - 3600;
    CHECK(!ruleDue(liked, now, false));
    liked.changedAt = now - 60;
    CHECK(ruleDue(liked, now, false));
    liked.lastSync = now - 120;
    CHECK(!ruleDue(liked, now, false));   // likes keep coming: every 5 min at most
    local.lastSync = now - 1;
    local.changedAt = now - 5;
    CHECK(!ruleDue(local, now, false));   // changedAt older than the listing
    local.changedAt = now;
    CHECK(ruleDue(local, now + 5, false));

    CHECK(backoffSeconds(1, false) == 60 && backoffSeconds(2, false) == 300 && backoffSeconds(3, false) == 900);
    CHECK(backoffSeconds(4, false) == 3600 && backoffSeconds(40, false) == 3600 && backoffSeconds(0, false) == 60);
    CHECK(backoffSeconds(1, true) == 600 && backoffSeconds(4, true) == 3600);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / L"shadetube_sync_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    testStore(dir);
    testFilter();
    testPlan();
    testDrop();
    testProgress();
    testSchedule();

    fs::remove_all(dir, ec);
    if (g_failures) {
        std::printf("\n%d of %d check(s) FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("\nall %d sync checks passed\n", g_checks);
    return 0;
}
