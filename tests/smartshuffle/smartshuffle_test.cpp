// Developer test for smart shuffle / "Geliştir" (offline):
//   1. app/SmartMix (compiled in): track keys (remasters / live versions / Turkish İ fold together), exclusions, the
//      candidate filter (unplayable, blocked, in the collection, duplicates; seeded order; artists spread; marked
//      recommended), the queue's insertion points (3-4 own songs apart, never two recommendations in a row, never right
//      after the playing song, only within the window, deterministic, planning again after inserting adds nothing,
//      existing recommendations respected, the run before the playing song counted) and interleave() for "Geliştir".
//   2. A real player::Player on generated WAVs (volume 0, like playback_test): recommendations inserted into the queue
//      keep their flag, dropRecommendations() takes only the upcoming ones out for good (a reshuffle doesn't bring them
//      back, the playing one stays), clearRecommended() turns one into an ordinary item, and the session keeps the flag.
//   smartshuffle_test
// Runs in a temporary profile (SHADETUBE_DATA_DIR) and needs an audio output device for part 2.
#include "app/SmartMix.h"
#include "core/Dispatcher.h"
#include "player/Player.h"
#include "youtube/MatchService.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mix = st::app::smartmix;
using st::catalog::Track;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

Track T(const std::string& id, const std::string& name, const std::string& artist, bool rec = false) {
    Track t;
    t.id = id;
    t.name = name;
    t.artists = {{"", artist}};
    t.durationMs = 1200;
    t.recommended = rec;
    return t;
}

// Applies insertion points to a flag list (as the feature inserts, last point first).
std::vector<bool> applied(std::vector<bool> flags, const std::vector<int>& points) {
    for (auto it = points.rbegin(); it != points.rend(); ++it) flags.insert(flags.begin() + *it, true);
    return flags;
}

// Longest run of own songs / whether two recommendations touch, from `from` on.
void spacing(const std::vector<bool>& flags, int from, int& maxRun, bool& adjacent) {
    maxRun = 0;
    adjacent = false;
    int run = 0;
    for (int i = from; i < static_cast<int>(flags.size()); ++i) {
        if (flags[i]) {
            if (i > from && flags[i - 1]) adjacent = true;
            run = 0;
        } else {
            maxRun = std::max(maxRun, ++run);
        }
    }
}

void testKeys() {
    std::printf("keys and exclusions\n");
    CHECK(mix::trackKey(T("a", "Hello - 2011 Remaster", "Adele")) == mix::trackKey(T("b", "Hello", "ADELE")));
    CHECK(mix::trackKey(T("a", "Hello (Live at the BBC)", "Adele")) == mix::trackKey(T("b", "hello", "Adele")));
    CHECK(mix::trackKey(T("a", "Şımarık", "TARKAN")) == mix::trackKey(T("b", "şımarık", "Tarkan")));
    CHECK(mix::trackKey(T("a", "夜に駆ける", "YOASOBI")) == mix::trackKey(T("b", "夜に駆ける (Live)", "yoasobi")));
    CHECK(mix::trackKey(T("a", "夜に駆ける", "YOASOBI")) != mix::trackKey(T("b", "群青", "YOASOBI")));
    CHECK(mix::trackKey(T("a", "İstanbul", "X")) == mix::trackKey(T("b", "istanbul", "x")));
    CHECK(mix::trackKey(T("a", "Hello", "Adele")) != mix::trackKey(T("b", "Hello", "Lionel Richie")));
    CHECK(mix::trackKey(T("a", "", "Adele")).empty());
    mix::Exclusions ex;
    ex.add(T("spotify:track:1", "Yesterday", "The Beatles"));
    CHECK(ex.contains(T("spotify:track:1", "Other", "Other")));                  // same id
    CHECK(ex.contains(T("mbid-9", "Yesterday - Remastered 2009", "The Beatles")));   // same song elsewhere
    CHECK(!ex.contains(T("mbid-8", "Yesterday", "Boyz II Men")));                 // a cover by someone else
}

void testCandidates() {
    std::printf("candidates\n");
    std::vector<Track> pool;
    for (int i = 0; i < 12; ++i) pool.push_back(T("p" + std::to_string(i), "Song " + std::to_string(i), i < 6 ? "A" : "B"));
    Track unplayable = T("u", "Gone", "C");
    unplayable.playable = false;
    pool.push_back(unplayable);
    pool.push_back(T("", "No id", "C"));
    pool.push_back(T("dup", "Song 3 - Live", "A"));            // same song as p3
    pool.push_back(T("inlist", "Already Here", "D"));
    pool.push_back(T("blocked", "Blocked Song", "E"));
    mix::Exclusions ex;
    ex.add(T("x", "Already Here", "D"));
    auto blocked = [](const Track& t) { return t.id == "blocked"; };
    const auto a = mix::candidates(pool, ex, blocked, 42);
    const auto b = mix::candidates(pool, ex, blocked, 42);
    const auto c = mix::candidates(pool, ex, blocked, 7);
    CHECK(a.size() == 12);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i) same = a[i].id == b[i].id;
    CHECK(same);   // deterministic for a seed
    bool differs = false;
    for (size_t i = 0; i < a.size() && i < c.size(); ++i) differs = differs || a[i].id != c[i].id;
    CHECK(differs);
    std::set<std::string> ids;
    bool allRec = true, spread = true;
    for (size_t i = 0; i < a.size(); ++i) {
        ids.insert(a[i].id);
        allRec = allRec && a[i].recommended;
        if (i > 0 && a[i].artists[0].name == a[i - 1].artists[0].name) spread = false;
    }
    CHECK(ids.size() == 12 && !ids.contains("u") && !ids.contains("dup") && !ids.contains("inlist") && !ids.contains("blocked"));
    CHECK(allRec);
    CHECK(spread);   // 6 + 6: the artists alternate
}

void testInsertionPoints() {
    std::printf("insertion points\n");
    // A plain 20-song queue, playing the first.
    std::vector<bool> flags(20, false);
    const auto points = mix::insertionPoints(flags, 0, 99);
    CHECK(!points.empty());
    CHECK(points.front() >= 2);   // never right after the playing song
    const auto after = applied(flags, points);
    int maxRun = 0;
    bool adjacent = true;
    spacing(after, 0, maxRun, adjacent);
    CHECK(maxRun <= mix::kMaxGap);
    CHECK(!adjacent);
    bool minGap = true;   // every recommendation after at least kMinGap own songs (counting the playing one)
    for (int i = 0, run = 0; i < static_cast<int>(after.size()); ++i) {
        if (after[i]) {
            minGap = minGap && run >= mix::kMinGap;
            run = 0;
        } else {
            ++run;
        }
    }
    CHECK(minGap);
    CHECK(mix::insertionPoints(flags, 0, 99) == points);   // deterministic
    CHECK(mix::insertionPoints(after, 0, 99).empty());     // planned again: nothing new
    CHECK(mix::insertionPoints(after, 0, 5).empty());      // ... whatever the seed
    // Existing recommendations 4 songs apart are enough.
    std::vector<bool> spaced;
    for (int i = 0; i < 25; ++i) spaced.push_back(i % 5 == 4);   // 4 own songs, a recommendation, 4 own songs...
    CHECK(mix::insertionPoints(spaced, 0, 3).empty());
    // Only the window is planned.
    std::vector<bool> longQueue(200, false);
    const auto w = mix::insertionPoints(longQueue, 10, 1, 24);
    CHECK(!w.empty() && w.back() <= 10 + 1 + 24);
    // The own songs just before the playing one count: 3 of them + the playing one -> the next slot must not be skipped
    // for long (first point at pos + 2 at the latest pos + 2).
    std::vector<bool> hist(30, false);
    const auto h = mix::insertionPoints(hist, 10, 11);
    CHECK(!h.empty() && h.front() == 12);
    // A recommendation playing now resets the count.
    std::vector<bool> recNow(30, false);
    recNow[10] = true;
    const auto r = mix::insertionPoints(recNow, 10, 11);
    CHECK(!r.empty() && r.front() >= 10 + 1 + mix::kMinGap);
    // Nothing to plan at the end of a queue / outside it.
    CHECK(mix::insertionPoints(std::vector<bool>(5, false), 4, 1).empty());
    CHECK(mix::insertionPoints(std::vector<bool>(5, false), -1, 1).empty());
}

void testInterleave() {
    std::printf("interleave (Geliştir)\n");
    std::vector<Track> own, recs;
    for (int i = 0; i < 17; ++i) own.push_back(T("o" + std::to_string(i), "Own " + std::to_string(i), "X"));
    for (int i = 0; i < 3; ++i) recs.push_back(T("r" + std::to_string(i), "Rec " + std::to_string(i), "Y"));
    const auto out = mix::interleave(own, recs, 5);
    CHECK(out.size() == own.size() + recs.size());
    std::vector<bool> flags;
    std::vector<std::string> ownOrder, recOrder;
    for (const auto& t : out) {
        flags.push_back(t.recommended);
        (t.recommended ? recOrder : ownOrder).push_back(t.id);
    }
    int maxRun = 0;
    bool adjacent = true;
    spacing(flags, 0, maxRun, adjacent);
    CHECK(!adjacent);
    CHECK(!flags.front());
    CHECK((recOrder == std::vector<std::string>{"r0", "r1", "r2"}));
    bool ownKept = ownOrder.size() == own.size();
    for (size_t i = 0; ownKept && i < own.size(); ++i) ownKept = ownOrder[i] == own[i].id;
    CHECK(ownKept);
    CHECK(mix::interleave(std::vector<Track>(own.begin(), own.begin() + 2), recs, 5).size() == 2);   // too short
    CHECK(mix::interleave(own, {}, 5).size() == own.size());
}

// ---- the real Player ------------------------------------------------------------------------------------------------
void writeWav(const fs::path& path, uint32_t ms) {
    constexpr uint32_t rate = 44100, channels = 2;
    const uint32_t frames = rate * ms / 1000;
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(6000 * std::sin(2 * 3.14159265358979 * 330 * i / rate));
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + data);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(channels * 2);
    u16(16);
    f.write("data", 4);
    u32(data);
    f.write(reinterpret_cast<const char*>(pcm.data()), data);
}

template <class F>
bool pumpUntil(F done, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (!done()) {
        if (GetTickCount() - start > timeoutMs) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return true;
}

std::vector<std::string> upcomingIds(const st::player::Player& p) {
    std::vector<std::string> ids;
    for (int i = p.currentOrderIndex() + 1; i < static_cast<int>(p.order().size()); ++i) ids.push_back(p.items()[p.order()[i]].id);
    return ids;
}

void testPlayer(const fs::path& tmp) {
    std::printf("player: recommendations in the queue (local WAVs, volume 0)\n");
    st::Dispatcher::init();
    const fs::path wav = tmp / L"tone.wav";
    writeWav(wav, 8000);
    st::youtube::MatchService matcher{YoutubeExplode::YoutubeClient()};
    {
        st::player::Player p(matcher);
        p.setVolume(0.f);
        p.setShuffle(false);
        p.localFileFor = [&](const std::string&) { return wav.wstring(); };
        std::vector<Track> own;
        for (int i = 0; i < 12; ++i) own.push_back(T("own" + std::to_string(i), "Own " + std::to_string(i), "X"));
        p.playContext(own, 0, {"playlist:local:test", L"Test"});
        CHECK(pumpUntil([&] { return p.current() != nullptr; }, 5000));
        // What the feature does: plan, then insert from the last point.
        std::vector<bool> flags;
        for (int item : p.order()) flags.push_back(p.items()[item].recommended);
        const auto points = mix::insertionPoints(flags, p.currentOrderIndex(), 17);
        CHECK(points.size() >= 2);
        for (size_t k = points.size(); k-- > 0;) {
            Track r = T("rec" + std::to_string(k), "Rec " + std::to_string(k), "Y", true);
            CHECK(p.insertAt(points[k], {r}) == 1);
        }
        const size_t withRecs = p.order().size();
        CHECK(withRecs == own.size() + points.size());
        int recCount = 0;
        for (const auto& id : upcomingIds(p)) recCount += id.rfind("rec", 0) == 0;
        CHECK(recCount == static_cast<int>(points.size()));
        // Session round trip keeps the flag.
        p.pause();
        p.saveSession();
        {
            st::player::Player restored(matcher);
            restored.restoreSession();
            int restoredRecs = 0;
            for (const auto& t : restored.items()) restoredRecs += t.recommended ? 1 : 0;
            CHECK(restoredRecs == static_cast<int>(points.size()));
        }
        // One becomes the user's own.
        p.clearRecommended("rec0");
        bool rec0Own = false;
        for (const auto& t : p.items())
            if (t.id == "rec0") rec0Own = !t.recommended;
        CHECK(rec0Own);
        // Dropping: only the (still flagged) upcoming recommendations go, for good.
        const int dropped = p.dropRecommendations();
        CHECK(dropped == static_cast<int>(points.size()) - 1);
        const auto left = upcomingIds(p);
        CHECK(std::count_if(left.begin(), left.end(), [](const std::string& id) { return id.rfind("rec", 0) == 0; }) == 1);
        CHECK(p.current() && p.current()->id == "own0");
        CHECK(p.items().size() == own.size() + 1);   // compacted: rec0 (now own) stays
        p.setShuffle(true);                          // a reshuffle brings nothing back
        CHECK(p.items().size() == own.size() + 1 && p.order().size() == own.size() + 1);
        CHECK(p.dropRecommendations() == 0);
        p.setShuffle(false);
        pumpUntil([] { return false; }, 200);
    }
    st::Dispatcher::shutdown();
}

} // namespace

int wmain() {
    const fs::path tmp = fs::temp_directory_path() / (L"shadetube_smartshuffle_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (tmp / L"profile").c_str());   // never the user's profile
    testKeys();
    testCandidates();
    testInsertionPoints();
    testInterleave();
    testPlayer(tmp);
    fs::remove_all(tmp, ec);
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
