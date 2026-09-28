// stats_test: checks for app/ListenStats (İstatistikler).
//
//   rule        : the stream rule (30 s, or 50 % of a track shorter than 60 s)
//   recording   : position deltas with pauses, forward/backward seeks, buffering stalls, the tail credit when the
//                 track changes, same-track reloads (continue) vs restarts (new play), stop
//   seeks       : credit never beyond the wall clock (5 s arrow-key taps, a held key, SponsorBlock jumps), Previous
//                 restarting the track with a seek to 0 = a new play
//   aggregation : 7 / 30 day / all-time windows, totals vs streams, distinct counts, top lists (order, merging of
//                 the same song from two sources), recent plays, the current play included provisionally
//   persistence : save -> readFile/adopt round trip, bound of kMaxPlays (newest kept), corrupt file -> .bak and
//                 the previous generation (.old), a locked file is never overwritten, an older save never replaces
//                 a newer one, the async save (worker), plays recorded before the (async) load are merged, clear
//   timing      : save / read / adopt / summarize with 20 000 plays over 3 000 tracks (printed)
//   local time  : civil dates, Windows' dynamic time zones (Berlin's DST switches, Istanbul's history)
//   heatmap/year: hours split at DST changes, years, the year summary (streak, new artists, first stream, months,
//                 weekdays), the current play included
//   import      : Spotify's history formats (extended + account data), podcast / video / audiobook rows skipped,
//                 the ZIP, dedupe (re-import, overlapping formats, plays heard here), plays recorded meanwhile, a
//                 superseded import, removal, clear, broken / locked imported file
//   import timing: 300 000 imported plays - build, load, aggregations (printed, loose budget)
//
// Everything runs in a temp folder (SHADETUBE_DATA_DIR is pointed there too): the user's profile is never touched.
#include "app/HistoryImport.h"
#include "app/ListenStats.h"
#include "core/Dispatcher.h"
#include "core/ThreadPool.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace st;
using namespace st::app;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

static catalog::Track track(std::string id, std::string name, std::string artist, std::string album, int durationMs,
                            std::string artistId = {}, std::string albumId = {}) {
    catalog::Track t;
    t.id = std::move(id);
    t.name = std::move(name);
    t.artists.push_back({std::move(artistId), std::move(artist)});
    t.album.id = std::move(albumId);
    t.album.name = std::move(album);
    t.album.images.push_back({"https://img.example/" + t.album.name + ".jpg", 300, 300});
    t.durationMs = durationMs;
    return t;
}

// Runs the UI-thread dispatcher (st::async continuations) until pred() or the timeout.
template <class Pred>
static bool pumpUntil(Pred pred, int timeoutMs) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    MSG msg;
    while (!pred()) {
        if (GetTickCount64() >= end) return false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred()) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    return true;
}

// Plays `seconds` of real playback from the current position, sampling every 2 s like App's housekeeping.
struct Sim {
    ListenStats& s;
    int64_t wall = 1'000'000;   // monotonic ms
    int64_t pos = 0;            // player position ms
    int64_t unix() const { return 1'700'000'000 + (wall - 1'000'000) / 1000; }
    void sample(bool playing) { s.progress(pos, playing, 0, wall, unix()); }
    void play(int seconds) {
        for (int i = 0; i < seconds; i += 2) {
            const int step = std::min(2, seconds - i);
            wall += step * 1000;
            pos += step * 1000;
            sample(true);
        }
    }
    void pause(int seconds) {
        sample(false);   // the pause itself (playerChanged)
        for (int i = 0; i < seconds; i += 2) {
            wall += 2000;
            sample(false);
        }
    }
    void resume() { sample(true); }
    void seekTo(int64_t ms) {   // seek() fires playerChanged at once: position = the target
        wall += 100;
        pos = ms;
        sample(true);
    }
    // n seeks of `jumpMs`, `everyMs` apart, the engine rebuffering after each (no audio in between).
    void jumps(int n, int64_t jumpMs, int64_t everyMs) {
        for (int i = 0; i < n; ++i) {
            wall += everyMs;
            pos += jumpMs;
            sample(true);
        }
    }
};

// ---------------------------------------------------------------------------------------------------------
static void testRule() {
    std::printf("stream rule:\n");
    CHECK(!listen::counts(29'999, 200'000));
    CHECK(listen::counts(30'000, 200'000));
    CHECK(listen::counts(30'000, 0));        // unknown duration: 30 s
    CHECK(!listen::counts(29'000, 0));
    CHECK(listen::counts(20'000, 40'000));   // short track: 50 %
    CHECK(!listen::counts(19'999, 40'000));
    CHECK(listen::counts(29'500, 59'000));
    CHECK(!listen::counts(29'000, 59'000));
    CHECK(!listen::counts(29'999, 60'000));  // 60 s is not "shorter than 60 s"
    CHECK(listen::counts(5'000, 10'000));
}

static void testRecording() {
    std::printf("recording (pauses, seeks, tail, reloads):\n");
    ListenStats s(g_dir / L"rec.json");
    s.load();
    Sim sim{s};
    const auto a = track("spotify:track:a", "Şımarık", "Tarkan", "Ölürüm Sana", 234'000, "spotify:artist:tarkan", "spotify:album:olurum");
    const auto b = track("spotify:track:b", "Kuzu Kuzu", "Tarkan", "Karma", 223'000, "spotify:artist:tarkan", "spotify:album:karma");
    s.trackStarted(a, 0, 1'700'000'000, sim.wall);
    CHECK(s.active());
    sim.wall += 700;   // resolving + buffering: no position yet
    sim.sample(false);
    sim.play(40);
    CHECK(s.currentListenedMs() == 40'000);
    sim.pause(60);     // a minute paused: nothing counts
    CHECK(s.currentListenedMs() == 40'000);
    sim.resume();
    sim.play(10);
    CHECK(s.currentListenedMs() == 50'000);
    // Forward seek +60 s right after a sample: only the 100 ms of audio before the jump counts (+1/40 jitter).
    sim.seekTo(sim.pos + 60'000);
    CHECK(s.currentListenedMs() == 50'102);
    sim.play(4);
    CHECK(s.currentListenedMs() == 54'102);
    // Seek detected by the housekeeping sample instead (no playerChanged): at most the elapsed 2 s count.
    sim.wall += 2000;
    sim.pos += 30'000;
    sim.sample(true);
    CHECK(s.currentListenedMs() == 56'152);
    // Backward seek (not back to the top: the same play).
    sim.seekTo(60'000);
    CHECK(s.currentListenedMs() == 56'252);
    CHECK(s.playCount() == 0);
    sim.play(6);
    CHECK(s.currentListenedMs() == 62'252);
    // Buffering stall: wall runs, position does not -> nothing; then it resumes normally.
    for (int i = 0; i < 5; ++i) {
        sim.wall += 2000;
        sim.sample(false);
    }
    sim.resume();
    sim.play(2);
    CHECK(s.currentListenedMs() == 64'252);
    // Player re-resolves the same track at the same spot ("Yanlış eşleşme?"): same play.
    sim.wall += 300;
    s.trackStarted(a, sim.pos, 1'700'000'100, sim.wall);
    CHECK(s.playCount() == 0);
    sim.play(2);
    CHECK(s.currentListenedMs() == 66'252);
    // Next track 1.5 s after the last sample while playing: the tail (1.5 s) is credited to A.
    sim.wall += 1500;
    s.trackStarted(b, 0, 1'700'000'200, sim.wall);
    CHECK(s.playCount() == 1);
    CHECK(s.plays().back().listenedMs == 67'752);
    CHECK(s.plays().back().startedAt == 1'700'000'000);
    CHECK(s.dirty());
    // B: 20 s, then repeat (same track from the top) -> A new play for B.
    sim.pos = 0;
    sim.play(20);
    sim.pos = 0;
    s.trackStarted(b, 0, 1'700'000'300, sim.wall);   // restart: position 0
    CHECK(s.playCount() == 2);
    CHECK(s.plays().back().listenedMs == 20'000);
    // Skipped right away (< 1 s) -> not stored.
    s.trackStarted(a, 0, 1'700'000'400, sim.wall + 300);
    CHECK(s.playCount() == 2);
    // Paused then stopped: no tail credit.
    sim.wall += 300;
    sim.pos = 0;
    sim.play(8);
    sim.pause(4);
    s.stop(sim.wall + 10'000);
    CHECK(!s.active());
    CHECK(s.playCount() == 3);
    CHECK(s.plays().back().listenedMs == 8'000);
    // A tail never runs past the track's end.
    const auto shortTrack = track("mb:short", "Intro", "Duman", "Belki Alışman Lazım", 20'000);
    s.trackStarted(shortTrack, 0, 1'700'000'500, sim.wall);
    sim.pos = 0;
    sim.play(18);
    s.stop(sim.wall + 4000);   // ended 4 s after the last sample, but only 2 s were left
    CHECK(s.plays().back().listenedMs == 20'000);
    // The duration learned from the decoder fills an unknown catalog duration (short track -> 50 % rule).
    auto preview = track("preview:x", "Aşkın Olayım", "Simge", "", 0);
    s.trackStarted(preview, 0, 1'700'000'600, sim.wall);
    sim.wall += 2000;
    s.progress(2000, true, 50'000, sim.wall, sim.unix());
    bool found = false;
    for (const auto& t : s.tracks())
        if (t.id == "preview:x") found = t.durationMs == 50'000;
    CHECK(found);
}

static void testSeeks() {
    std::printf("seeks (arrow keys, held key, SponsorBlock, Previous restart):\n");
    ListenStats s(g_dir / L"seek.json");
    s.load();
    Sim sim{s};
    const auto a = track("sp:a", "Kısa Kısa", "Altın Gün", "Gece", 303'000);
    s.trackStarted(a, 0, sim.unix(), sim.wall);
    sim.play(10);
    CHECK(s.currentListenedMs() == 10'000);
    // Right arrow (+5 s) tapped six times ~300 ms apart: each tap credits the 300 ms (+1/40 jitter), never the 5 s.
    sim.jumps(6, 5'000, 300);
    CHECK(s.currentListenedMs() == 10'000 + 6 * 307);
    // Held key: auto-repeat +5 s every 33 ms skims 200 s of the track: 33 ms each.
    int64_t before = s.currentListenedMs();
    sim.jumps(40, 5'000, 33);
    CHECK(s.currentListenedMs() - before == 40 * 33);
    // SponsorBlock-style skip: 1 s of audio, then a +4 s jump inside the same sample interval: only the 1 s.
    before = s.currentListenedMs();
    sim.jumps(1, 1'000 + 4'000, 1'000);
    CHECK(s.currentListenedMs() - before == 1'025);
    CHECK(!listen::counts(s.currentListenedMs(), 303'000));   // ~14 s of real audio: no stream
    CHECK(s.playCount() == 0);

    // Previous past 3 s restarts the track with seek(0) (no track change): that play ends, a new one starts.
    ListenStats r(g_dir / L"restart.json");
    r.load();
    Sim rs{r};
    const auto song = track("sp:b", "Yolcu", "Altın Gün", "Gece", 285'000);
    r.trackStarted(song, 0, rs.unix(), rs.wall);
    rs.play(200);
    rs.wall += 500;
    rs.pos = 0;
    rs.sample(true);   // seek(0) notifies at once
    CHECK(r.playCount() == 1 && !r.plays().empty() && r.plays().back().listenedMs == 200'500);   // + the 500 ms before it
    CHECK(r.active() && r.currentListenedMs() == 0);
    rs.play(40);
    // A seek back that is not to the top of the track stays the same play.
    rs.seekTo(20'000);
    CHECK(r.playCount() == 1);
    rs.play(10);
    r.stop(rs.wall);
    CHECK(r.playCount() == 2 && r.plays().back().listenedMs == 50'100);
    CHECK(r.plays().size() == 2 && r.plays()[1].startedAt > r.plays()[0].startedAt);
    const auto sum = r.summarize(StatsPeriod::All, rs.unix());
    CHECK(sum.streams == 2 && !sum.topTracks.empty() && sum.topTracks[0].streams == 2);
}

static void testAggregation() {
    std::printf("aggregation:\n");
    ListenStats s(g_dir / L"agg.json");
    s.load();
    const int64_t now = 1'760'000'000;
    const int64_t day = 86400;
    const auto a = track("spotify:track:a", "Şımarık", "Tarkan", "Ölürüm Sana", 234'000, "spotify:artist:tarkan", "spotify:album:olurum");
    const auto aLocal = track("local:1", "ŞIMARIK", "tarkan", "Ölürüm Sana", 0);   // same song from a local file
    const auto b = track("mb:b", "Gülpembe", "Barış Manço", "Sahibinden İhtiyar", 280'000, "11111111-2222-3333-4444-555555555555");
    const auto c = track("mb:c", "Bohemian Rhapsody", "Queen", "A Night at the Opera", 354'000);
    auto duet = track("spotify:track:d", "Yolla", "Tarkan", "Yolla", 208'000, "spotify:artist:tarkan");
    duet.artists.push_back({"spotify:artist:x", "Sezen Aksu"});

    s.addPlay(a, now - 1 * day, 200'000);
    s.addPlay(aLocal, now - 2 * day, 150'000);
    s.addPlay(a, now - 3 * day, 10'000);        // skipped: time counts, stream does not
    s.addPlay(b, now - 2 * day, 280'000);
    s.addPlay(c, now - 10 * day, 354'000);
    s.addPlay(c, now - 12 * day, 300'000);
    s.addPlay(c, now - 20 * day, 100'000);
    s.addPlay(b, now - 40 * day, 280'000);
    s.addPlay(duet, now - 5 * day, 60'000);
    s.addPlay(a, now - 400 * day, 60'000);
    s.addPlay(a, now - 1 * day, 500);           // < 1 s: ignored
    CHECK(s.playCount() == 10);
    CHECK(s.tracks().size() == 4);              // a + aLocal merged

    const auto w = s.summarize(StatsPeriod::Week, now);
    CHECK(w.periodStart == now - 7 * day);
    CHECK(w.listenedMs == 200'000 + 150'000 + 10'000 + 280'000 + 60'000);
    CHECK(w.streams == 4);
    CHECK(w.distinctTracks == 3);
    CHECK(w.distinctArtists == 3);             // Tarkan, Barış Manço, Sezen Aksu
    CHECK(w.topTracks.size() == 3);
    CHECK(!w.topTracks.empty() && w.topTracks[0].track.name == "Şımarık" && w.topTracks[0].streams == 2 &&
          w.topTracks[0].listenedMs == 350'000);
    CHECK(!w.topTracks.empty() && w.topTracks[0].track.id == "spotify:track:a");   // the first sighting's id
    CHECK(w.topTracks.size() > 1 && w.topTracks[1].track.name == "Gülpembe");    // 1 stream, more time than Yolla
    CHECK(!w.topArtists.empty() && w.topArtists[0].artist.name == "Tarkan" && w.topArtists[0].streams == 3 &&
          w.topArtists[0].artist.id == "spotify:artist:tarkan");
    CHECK(!w.topAlbums.empty() && w.topAlbums[0].album.name == "Ölürüm Sana" && w.topAlbums[0].streams == 2 &&
          w.topAlbums[0].album.id == "spotify:album:olurum" && w.topAlbums[0].artist == "Tarkan");
    CHECK(w.recent.size() == 4);
    CHECK(w.recent.size() == 4 && w.recent[0].startedAt == now - 1 * day && w.recent[3].startedAt == now - 5 * day);

    const auto m = s.summarize(StatsPeriod::Month, now);
    CHECK(m.streams == 7);
    CHECK(m.topTracks.size() > 0 && m.topTracks[0].track.name == "Bohemian Rhapsody" && m.topTracks[0].streams == 3);
    CHECK(m.distinctArtists == 4);

    const auto all = s.summarize(StatsPeriod::All, now);
    CHECK(all.periodStart == now - 400 * day);
    CHECK(all.streams == 9);
    // 3 streams each: Bohemian Rhapsody wins on time (754 s vs 410 s).
    CHECK(all.topTracks.size() > 1 && all.topTracks[0].track.name == "Bohemian Rhapsody" && all.topTracks[0].streams == 3 &&
          all.topTracks[1].track.name == "Şımarık" && all.topTracks[1].streams == 3);
    const auto top2 = s.summarize(StatsPeriod::All, now, 2, 3);
    CHECK(top2.topTracks.size() == 2 && top2.recent.size() == 3 && top2.distinctTracks == 4);

    // The current play is included once it counts (and then it is the most recent).
    Sim sim{s};
    s.trackStarted(b, 0, now - 60, sim.wall);
    sim.play(20);
    CHECK(s.summarize(StatsPeriod::Week, now).streams == 4);
    CHECK(s.summarize(StatsPeriod::Week, now).listenedMs == w.listenedMs + 20'000);
    sim.play(20);
    const auto live = s.summarize(StatsPeriod::Week, now);
    CHECK(live.streams == 5);
    CHECK(!live.recent.empty() && live.recent[0].track.name == "Gülpembe" && live.recent[0].startedAt == now - 60);

    // A soundtrack: one album id, a different first artist on every track -> one album ("various artists"). A download
    // of another song of it without the id joins through name + first artist.
    ListenStats v(g_dir / L"various.json");
    v.load();
    for (int i = 0; i < 5; ++i)
        v.addPlay(track("sp:ost" + std::to_string(i), "Parça " + std::to_string(i), "Sanatçı " + std::to_string(i),
                        "Ayla (Film Müziği)", 200'000, {}, "spotify:album:ayla"),
                  now - 100 + i, 60'000);
    v.addPlay(track("dl:9", "Parça 9", "Sanatçı 1", "Ayla (Film Müziği)", 200'000), now - 50, 60'000);
    const auto vs = v.summarize(StatsPeriod::All, now);
    CHECK(vs.topAlbums.size() == 1);
    CHECK(!vs.topAlbums.empty() && vs.topAlbums[0].streams == 6 && vs.topAlbums[0].variousArtists &&
          vs.topAlbums[0].album.id == "spotify:album:ayla");
    CHECK(!w.topAlbums.empty() && !w.topAlbums[0].variousArtists);   // a regular album stays one artist's

    // An empty store.
    ListenStats empty(g_dir / L"empty.json");
    empty.load();
    const auto e = empty.summarize(StatsPeriod::All, now);
    CHECK(e.periodStart == 0 && e.streams == 0 && e.listenedMs == 0 && e.topTracks.empty() && e.recent.empty());
}

static void testPersistence() {
    std::printf("persistence:\n");
    const auto file = g_dir / L"listening.json";
    const int64_t now = 1'760'000'000;
    const auto a = track("spotify:track:a", "Şımarık", "Tarkan", "Ölürüm Sana", 234'000, "spotify:artist:tarkan", "spotify:album:olurum");
    const auto b = track("mb:b", "Gülpembe", "Barış Manço", "Sahibinden İhtiyar", 280'000);
    {
        ListenStats s(file);
        s.load();
        CHECK(s.loaded());
        CHECK(s.save());   // nothing yet: an empty history
        s.addPlay(a, now - 100, 200'000);
        s.addPlay(b, now - 50, 40'000);
        s.addPlay(a, now - 10, 5'000);
        Sim sim{s};
        s.trackStarted(b, 0, now - 5, sim.wall);   // the current play is saved too (so far)
        sim.play(12);
        CHECK(s.dirty());
        CHECK(s.save());
        CHECK(!s.dirty());
    }
    CHECK(fs::exists(file));
    CHECK(!fs::exists(fs::path(file).concat(L".tmp")));
    {
        const auto snap = ListenStats::readFile(file);
        CHECK(!snap.corrupt);
        CHECK(snap.index.tracks.size() == 2);
        CHECK(snap.plays.size() == 4);
        ListenStats s(file);
        s.adopt(snap);
        CHECK(s.playCount() == 4);
        CHECK(!s.dirty());
        const auto sum = s.summarize(StatsPeriod::All, now);
        CHECK(sum.listenedMs == 200'000 + 40'000 + 5'000 + 12'000);
        CHECK(sum.streams == 2);
        CHECK(!sum.topTracks.empty() && sum.topTracks[0].track.name == "Şımarık" &&
              sum.topTracks[0].track.artists.size() == 1 && sum.topTracks[0].track.artists[0].id == "spotify:artist:tarkan" &&
              sum.topTracks[0].track.album.id == "spotify:album:olurum" && sum.topTracks[0].track.durationMs == 234'000 &&
              !sum.topTracks[0].track.album.images.empty() && sum.topTracks[0].track.album.images[0].url == "https://img.example/Ölürüm Sana.jpg");
        // A second adopt is ignored (only the first load applies).
        s.adopt(snap);
        CHECK(s.playCount() == 4);
    }
    // Plays recorded before the async load finished are merged on top, and the store becomes dirty.
    {
        ListenStats s(file);
        s.addPlay(a, now + 10, 60'000);
        Sim sim{s};
        s.trackStarted(a, 0, now + 100, sim.wall);
        sim.play(6);
        CHECK(!s.save());   // not loaded yet: never clobbers the file
        s.adopt(ListenStats::readFile(file));
        CHECK(s.playCount() == 5);
        CHECK(s.dirty());
        CHECK(s.active() && s.currentListenedMs() == 6'000);
        CHECK(s.tracks().size() == 2);   // merged, not duplicated
        CHECK(s.summarize(StatsPeriod::All, now + 200).topTracks[0].streams == 2);
    }
    // Bound: the newest kMaxPlays are kept.
    {
        const auto big = g_dir / L"big.json";
        ListenStats s(big);
        s.load();
        const size_t n = listen::kMaxPlays + 10;
        for (size_t i = 0; i < n; ++i)
            s.addPlay(track("t" + std::to_string(i % 50), "Song " + std::to_string(i % 50), "Artist " + std::to_string(i % 7), "Album", 200'000),
                      now - static_cast<int64_t>(n - i) * 60, 40'000);
        CHECK(s.save());
        const auto snap = ListenStats::readFile(big);
        CHECK(snap.plays.size() == listen::kMaxPlays);
        CHECK(!snap.plays.empty() && snap.plays.front().startedAt == now - static_cast<int64_t>(listen::kMaxPlays) * 60);
        CHECK(!snap.plays.empty() && snap.plays.back().startedAt == now - 60);
    }
    // Escaping: quotes, backslashes, control characters and invalid UTF-8 still give a file that parses.
    {
        const auto esc = g_dir / L"esc.json";
        ListenStats s(esc);
        s.load();
        s.addPlay(track("x", "A \"quoted\" \\ name\t\x01", "Art\xC3\x28ist", "Al\xFF" "bum", 200'000), now, 40'000);
        CHECK(s.save());
        const auto snap = ListenStats::readFile(esc);
        CHECK(!snap.corrupt && snap.plays.size() == 1 && snap.index.tracks.size() == 1);
        if (snap.index.tracks.size() == 1) {
            const auto& t = snap.index.tracks[0];
            CHECK(t.name == "A \"quoted\" \\ name\t\x01");
            CHECK(t.artists.size() == 1 && t.artists[0].name == "Art\xEF\xBF\xBD(ist");
            CHECK(t.albumName == "Al\xEF\xBF\xBD" "bum");
        }
        // The same song twice in a file (e.g. written by an older version) merges into one entry.
        const auto dup = g_dir / L"dup.json";
        std::ofstream(dup, std::ios::binary) << R"({"v":1,"tracks":[{"n":"Beni Çok Sev","a":[["","Tarkan"]]},{"n":"BENİ ÇOK SEV","id":"sp:1","a":[["sp:a","tarkan"]],"d":250000}],"plays":[[0,10,40000],[1,20,40000]]})";
        const auto d = ListenStats::readFile(dup);
        CHECK(d.index.tracks.size() == 1 && d.plays.size() == 2 && d.plays[0].track == 0 && d.plays[1].track == 0);
        CHECK(d.index.tracks.size() == 1 && d.index.tracks[0].id == "sp:1" && d.index.tracks[0].durationMs == 250'000 &&
              d.index.artists.size() == 1 && d.index.artists[0].id == "sp:a");
    }
    // Generations: every save keeps the previous file as .old; a missing or broken main file falls back to it.
    {
        const auto gen = g_dir / L"gen.json";
        const auto old = fs::path(gen).concat(L".old");
        ListenStats s(gen);
        s.load();
        s.addPlay(a, now - 100, 200'000);
        CHECK(s.save());
        CHECK(!fs::exists(old));
        s.addPlay(b, now - 50, 40'000);
        CHECK(s.save());
        CHECK(fs::exists(old) && ListenStats::readFile(old).plays.size() == 1);
        CHECK(ListenStats::readFile(gen).plays.size() == 2);
        // Broken main (a crash while replacing it): .old is loaded, the broken file kept as .bak.
        std::ofstream(gen, std::ios::binary | std::ios::trunc) << std::string(64, '\0');
        auto snap = ListenStats::readFile(gen);
        CHECK(snap.fromOld && snap.corrupt && !snap.keepFile && snap.plays.size() == 1);
        CHECK(fs::exists(fs::path(gen).concat(L".bak")));
        ListenStats r(gen);
        r.adopt(std::move(snap));
        CHECK(r.playCount() == 1 && r.dirty());   // put back in place by the next save
        CHECK(r.save() && ListenStats::readFile(gen).plays.size() == 1 && !ListenStats::readFile(gen).fromOld);
        // Missing main (between the two renames): .old again.
        fs::remove(gen);
        CHECK(ListenStats::readFile(gen).fromOld);
    }
    // A file that can't be read (locked by a scanner / backup, I/O error) is never overwritten.
    {
        const auto lockedFile = g_dir / L"locked.json";
        {
            ListenStats s(lockedFile);
            s.load();
            s.addPlay(a, now - 100, 200'000);
            CHECK(s.save());
        }
        const auto sizeBefore = fs::file_size(lockedFile);
        HANDLE h = CreateFileW(lockedFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        auto snap = ListenStats::readFile(lockedFile);   // retries ~400 ms, then gives up
        CloseHandle(h);
        CHECK(snap.keepFile && snap.plays.empty());
        ListenStats s(lockedFile);
        s.adopt(std::move(snap));
        CHECK(s.readOnly());
        s.addPlay(b, now - 10, 40'000);
        CHECK(!s.save());
        CHECK(fs::file_size(lockedFile) == sizeBefore && ListenStats::readFile(lockedFile).plays.size() == 1);
        s.clear(now);   // an explicit clear lifts it
        CHECK(!s.readOnly() && s.save());
    }
    // An older save never replaces a newer one (a worker write finishing after the exit save).
    {
        const auto order = g_dir / L"order.json";
        ListenStats s(order);
        s.load();
        s.addPlay(a, now - 100, 200'000);
        const auto older = s.makeSaveJob();
        s.addPlay(b, now - 50, 40'000);
        const auto newer = s.makeSaveJob();
        CHECK(ListenStats::write(newer));
        CHECK(ListenStats::write(older));   // skipped, not an error
        CHECK(ListenStats::readFile(order).plays.size() == 2);
    }
    // The async save: serialized and written on a worker, the UI thread only snapshots.
    {
        const auto asyncFile = g_dir / L"async.json";
        ListenStats s(asyncFile);
        s.load();
        s.addPlay(a, now - 100, 200'000);
        s.addPlay(b, now - 50, 40'000);
        s.saveAsync();
        CHECK(!s.dirty());
        CHECK(pumpUntil([&] { return fs::exists(asyncFile) && ListenStats::readFile(asyncFile).plays.size() == 2; }, 5000));
        CHECK(!s.dirty());
    }
    // Corrupt file: kept as .bak, the store starts empty.
    {
        const auto bad = g_dir / L"bad.json";
        std::ofstream(bad, std::ios::binary) << "{\"v\":1,\"tracks\":[{\"n\":\"x\"";
        const auto snap = ListenStats::readFile(bad);
        CHECK(snap.corrupt && snap.plays.empty());
        CHECK(fs::exists(fs::path(bad).concat(L".bak")));
        // Invalid rows are skipped, valid ones kept.
        const auto odd = g_dir / L"odd.json";
        std::ofstream(odd, std::ios::binary) << R"({"v":1,"tracks":[{"n":"A","a":[["","X"]]}],"plays":[[0,100,40000],[5,100,40000],[0,50,10],"x",[0,200,50000]]})";
        const auto o = ListenStats::readFile(odd);
        CHECK(!o.corrupt && o.plays.size() == 2 && o.plays[0].startedAt == 100);
    }
    // Clear: everything goes, the current track keeps recording from now.
    {
        ListenStats s(file);
        s.load();
        Sim sim{s};
        s.trackStarted(b, 0, now, sim.wall);
        sim.play(40);
        CHECK(s.playCount() > 0);
        s.clear(now + 40);
        CHECK(s.playCount() == 0 && s.active() && s.currentListenedMs() == 0);
        CHECK(s.save());
        sim.play(4);
        const auto sum = s.summarize(StatsPeriod::All, now + 50);
        CHECK(sum.listenedMs == 4'000 && sum.streams == 0);
        CHECK(ListenStats::readFile(file).plays.empty());
    }
    // Default location = the profile folder.
    CHECK(ListenStats().file() == g_dir / L"listening.json");
}

static void testTiming() {
    std::printf("timing (20 000 plays, 3 000 tracks):\n");
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); };
    const auto file = g_dir / L"timing.json";
    const int64_t now = 1'760'000'000;
    {
        ListenStats s(file);
        s.load();
        for (int i = 0; i < 20'000; ++i) {
            const int k = (i * 7919) % 3000;
            auto t = track("spotify:track:" + std::to_string(k), "Şarkı numarası " + std::to_string(k),
                           "Sanatçı " + std::to_string(k % 400), "Albüm " + std::to_string(k % 900), 180'000 + k,
                           "spotify:artist:" + std::to_string(k % 400), "spotify:album:" + std::to_string(k % 900));
            t.album.images[0].url = "https://i.scdn.co/image/ab67616d00001e02" + std::to_string(100000000 + k);
            s.addPlay(t, now - (20'000 - i) * 120, 30'000 + (i % 200) * 1000);
        }
        auto t0 = clk::now();
        const auto job = s.makeSaveJob();
        std::printf("  snapshot    %7.1f ms  (UI thread)\n", ms(t0));
        t0 = clk::now();
        CHECK(ListenStats::write(job));
        std::printf("  write       %7.1f ms  (worker: serialize + flushed write, %.0f KB)\n", ms(t0), fs::file_size(file) / 1024.0);
        t0 = clk::now();
        const auto sum = s.summarize(StatsPeriod::All, now);
        std::printf("  summarize   %7.1f ms  (all time)\n", ms(t0));
        CHECK(sum.streams == 20'000);
    }
    auto t0 = clk::now();
    auto snap = ListenStats::readFile(file);
    std::printf("  readFile    %7.1f ms  (worker)\n", ms(t0));
    ListenStats s(file);
    t0 = clk::now();
    s.adopt(std::move(snap));
    std::printf("  adopt       %7.1f ms  (UI thread)\n", ms(t0));
    CHECK(s.playCount() == 20'000 && s.tracks().size() == 3000);
    t0 = clk::now();
    const auto w = s.summarize(StatsPeriod::Week, now);
    std::printf("  summarize   %7.1f ms  (7 days: %d streams)\n", ms(t0), w.streams);
}

// ---------------------------------------------------------------------------------------------------------
// Local time: civil dates, the dynamic time zone clock (historic DST rules), heatmap bucketing, year summaries.

static int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    return listen::daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

// Europe/Berlin by hand (the EU rule since 1996): +2 from the last Sunday of March 01:00 UTC to the last Sunday of
// October 01:00 UTC, else +1.
static int64_t euLocal(int64_t u) {
    int y, m, d;
    listen::civil(listen::dayNumber(u), y, m, d);
    auto lastSunday = [](int year, int month) {
        int64_t day = listen::daysFromCivil(year, month, 31);
        while (listen::weekday(day) != 6) --day;
        return day * 86400 + 3600;
    };
    const bool summer = u >= lastSunday(y, 3) && u < lastSunday(y, 10);
    return u + (summer ? 7200 : 3600);
}

static void testLocalTime() {
    std::printf("local time (civil dates, time zones, DST):\n");
    CHECK(listen::daysFromCivil(1970, 1, 1) == 0);
    CHECK(listen::weekday(0) == 3);                                   // Thursday
    CHECK(listen::weekday(listen::daysFromCivil(2024, 1, 1)) == 0);   // Monday
    CHECK(listen::weekday(listen::daysFromCivil(2024, 3, 31)) == 6);  // Sunday
    CHECK(listen::weekday(-1) == 2);                                  // 1969-12-31: Wednesday
    for (int64_t day : {-800'000LL, -1LL, 0LL, 11'016LL, 19'782LL, 20'000LL, 2'932'896LL}) {
        int y, m, d;
        listen::civil(day, y, m, d);
        CHECK(listen::daysFromCivil(y, m, d) == day);
    }
    int y, m, d;
    listen::civil(listen::daysFromCivil(2000, 2, 29), y, m, d);
    CHECK(y == 2000 && m == 2 && d == 29);
    CHECK(listen::dayNumber(-1) == -1 && listen::dayNumber(86399) == 0 && listen::dayNumber(86400) == 1);

    // Windows' dynamic zone data: Berlin's DST switches, and Istanbul's history (DST until 2016, then +3 all year).
    const LocalClock berlin = timeZoneClock(L"W. Europe Standard Time");
    CHECK(berlin(utc(2024, 3, 31, 0, 30)) - utc(2024, 3, 31, 0, 30) == 3600);
    CHECK(berlin(utc(2024, 3, 31, 1, 30)) - utc(2024, 3, 31, 1, 30) == 7200);
    CHECK(berlin(utc(2024, 10, 27, 0, 30)) - utc(2024, 10, 27, 0, 30) == 7200);
    CHECK(berlin(utc(2024, 10, 27, 1, 30)) - utc(2024, 10, 27, 1, 30) == 3600);
    CHECK(berlin(utc(2019, 7, 1)) - utc(2019, 7, 1) == 7200);
    for (int64_t u = utc(2023, 1, 1); u < utc(2025, 1, 1); u += 3 * 3600 + 17)   // agrees with the EU rule all along
        if (berlin(u) != euLocal(u)) {
            CHECK(berlin(u) == euLocal(u));
            break;
        }
    const LocalClock istanbul = timeZoneClock(L"Turkey Standard Time");
    CHECK(istanbul(utc(2015, 1, 15)) - utc(2015, 1, 15) == 2 * 3600);
    CHECK(istanbul(utc(2015, 7, 15)) - utc(2015, 7, 15) == 3 * 3600);
    CHECK(istanbul(utc(2020, 1, 15)) - utc(2020, 1, 15) == 3 * 3600);
    // The system clock answers (whatever this PC's zone is) within +-14 h.
    const LocalClock sys = systemLocalClock();
    CHECK(std::abs(sys(utc(2024, 6, 1)) - utc(2024, 6, 1)) <= 14 * 3600);
}

static void testHeatmapAndYears() {
    std::printf("heatmap and year summary:\n");
    const LocalClock clock = euLocal;
    ListenStats s(g_dir / L"year.json");
    s.load();
    const auto a = track("spotify:track:a", "Şımarık", "Tarkan", "Ölürüm Sana", 234'000, "spotify:artist:tarkan");
    const auto b = track("mb:b", "Gülpembe", "Barış Manço", "Sahibinden İhtiyar", 280'000);
    const auto c = track("mb:c", "Firuze", "Sezen Aksu", "Firuze", 215'000);
    // Spring forward: 00:30 UTC Sunday = 01:30 local; one hour of listening = 01:30-02:00 and 03:00-03:30 local.
    s.addPlay(c, utc(2024, 3, 31, 0, 30), 3'600'000);
    // Fall back: 00:30 UTC = 02:30 (+2), at 01:00 UTC the clock shows 02:00 again: the whole hour is in 02:xx.
    s.addPlay(c, utc(2024, 10, 27, 0, 30), 3'600'000);
    StatsHeatmap h = s.heatmap(StatsPeriod::All, utc(2025, 1, 1), clock);
    CHECK(h.at(6, 1) == 1'800'000 && h.at(6, 2) == 3'600'000 && h.at(6, 3) == 1'800'000);
    CHECK(h.totalMs == 7'200'000 && h.maxMs == 3'600'000);
    int64_t sum = 0;
    for (int64_t v : h.ms) sum += v;
    CHECK(sum == h.totalMs);

    // 2023: Tarkan and Barış Manço. 2024: Tarkan again, Sezen Aksu new (and Barış Manço, not new).
    s.addPlay(a, utc(2023, 5, 10, 18), 200'000);
    s.addPlay(b, utc(2023, 6, 1, 12), 280'000);
    // 23:30 UTC on Dec 31 is 00:30 on Jan 1 locally: 2024's first stream.
    s.addPlay(a, utc(2023, 12, 31, 23, 30), 200'000);
    s.addPlay(b, utc(2024, 1, 1, 10), 10'000);        // not a stream (the time counts)
    s.addPlay(b, utc(2024, 1, 2, 10), 280'000);
    s.addPlay(a, utc(2024, 1, 3, 10), 200'000);
    s.addPlay(a, utc(2024, 1, 3, 11), 200'000);
    s.addPlay(c, utc(2024, 1, 5, 20), 215'000);       // a gap on the 4th: the first streak is Jan 1-3
    s.addPlay(c, utc(2024, 1, 6, 20), 215'000);
    s.addPlay(c, utc(2024, 1, 7, 20), 215'000);
    s.addPlay(c, utc(2024, 1, 8, 20), 215'000);       // Jan 5-8: 4 days, the longest
    const auto years = s.years(clock);
    CHECK(years.size() == 2 && years[0] == 2024 && years[1] == 2023);

    const YearSummary y = s.summarizeYear(2024, clock);
    CHECK(y.year == 2024);
    CHECK(y.streams == 10);   // the two DST hours count too
    CHECK(y.listenedMs == 2 * 3'600'000 + 200'000 + 10'000 + 280'000 + 400'000 + 4 * 215'000);
    CHECK(y.distinctTracks == 3 && y.distinctArtists == 3);
    CHECK(y.newArtists == 1);                          // Sezen Aksu
    CHECK(y.firstStream && y.firstStream->track.name == "Şımarık" && y.firstStream->startedAt == utc(2023, 12, 31, 23, 30));
    CHECK(y.longestStreak == 4 && y.streakFrom == utc(2024, 1, 5, 20) && y.streakTo == utc(2024, 1, 8, 20));
    CHECK(y.activeDays == 3 + 4 + 2);                  // Jan 1-3, Jan 5-8, Mar 31, Oct 27
    CHECK(y.topMonth == 2 || y.topMonth == 9);         // March and October: one hour each
    CHECK(y.monthMs[0] == 200'000 + 10'000 + 280'000 + 400'000 + 4 * 215'000);
    CHECK(y.topWeekday == 6);                          // the two Sunday hours
    CHECK(!y.topTracks.empty() && y.topTracks[0].track.name == "Firuze" && y.topTracks[0].streams == 6);
    CHECK(y.topArtists.size() == 3 && y.topArtists[0].artist.name == "Sezen Aksu");
    CHECK(y.heatmap.totalMs == y.listenedMs);
    const YearSummary old = s.summarizeYear(2023, clock);
    CHECK(old.streams == 2 && old.newArtists == 2 && old.longestStreak == 1 && old.topMonth == 5);
    CHECK(old.firstStream && old.firstStream->startedAt == utc(2023, 5, 10, 18));
    const YearSummary none = s.summarizeYear(2019, clock);
    CHECK(none.streams == 0 && none.listenedMs == 0 && !none.firstStream && none.topMonth == -1 && none.longestStreak == 0);
    // The current play counts provisionally (Jan 4 fills the gap: Jan 1-8 in a row).
    Sim sim{s};
    s.trackStarted(a, 0, utc(2024, 1, 4, 9), sim.wall);
    sim.play(40);
    const YearSummary live = s.summarizeYear(2024, clock);
    CHECK(live.streams == 11 && live.longestStreak == 8 && live.activeDays == 10);
    // 7-day heatmap: from that window on (the two DST hours come later in the year).
    const StatsHeatmap week = s.heatmap(StatsPeriod::Week, utc(2024, 1, 9), clock);
    CHECK(week.totalMs == 280'000 + 400'000 + 4 * 215'000 + 40'000 + 2 * 3'600'000);
}

// ---------------------------------------------------------------------------------------------------------
// Spotify history import: parsing, ZIP, deduplication, persistence of the imported file.

static std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static size_t countImported(const std::vector<ListenStats::Play>& plays) {
    size_t n = 0;
    for (const auto& p : plays) n += p.imported ? 1 : 0;
    return n;
}

static void testImport() {
    std::printf("import (Spotify history files, ZIP, dedupe, persistence):\n");
    const fs::path fx = ST_STATS_FIXTURES;
    CHECK(history::parseUtc("2024-01-02T20:04:40Z") == utc(2024, 1, 2, 20, 4, 40));
    CHECK(history::parseUtc("2024-01-02 20:04") == utc(2024, 1, 2, 20, 4));
    CHECK(history::parseUtc("2024-01-02T20:04:40.123Z") == utc(2024, 1, 2, 20, 4, 40));
    CHECK(history::parseUtc("2024-01-02T23:04:40+03:00") == utc(2024, 1, 2, 20, 4, 40));
    CHECK(history::parseUtc("2024-13-02 20:04") == -1 && history::parseUtc("") == -1 && history::parseUtc("2024-01-02") == -1);
    CHECK(history::isHistoryEntry("Spotify Extended Streaming History/Streaming_History_Audio_2023-2024_0.json"));
    CHECK(history::isHistoryEntry("MyData/endsong_3.json"));
    CHECK(history::isHistoryEntry("Spotify Account Data\\StreamingHistory_music_0.json"));
    CHECK(history::isHistoryEntry("MyData/StreamingHistory0.json"));
    CHECK(!history::isHistoryEntry("Spotify Extended Streaming History/Streaming_History_Video_2024.json"));
    CHECK(!history::isHistoryEntry("Spotify Account Data/StreamingHistory_podcast_0.json"));
    CHECK(!history::isHistoryEntry("Spotify Account Data/YourLibrary.json"));
    CHECK(!history::isHistoryEntry("ReadMeFirst_ExtendedStreamingHistory.pdf"));

    // One document of each format.
    {
        std::vector<ListenStats::ImportRow> rows;
        int skipped = 0;
        CHECK(history::parse(readText(fx / L"Streaming_History_Audio_2023-2024_0.json"), rows, skipped) == history::Format::Extended);
        CHECK(rows.size() == 7 && skipped == 3);   // podcast, no song, audiobook
        if (!rows.empty()) {
            CHECK(rows[0].track.name == "Şımarık" && rows[0].track.artists.size() == 1 && rows[0].track.artists[0].name == "Tarkan");
            CHECK(rows[0].track.albumName == "Ölürüm Sana" && rows[0].track.id == "spotify:track:0000000000000000000001");
            CHECK(rows[0].endedAt == utc(2023, 12, 31, 20, 3, 20) && rows[0].listenedMs == 200'000);
        }
        const size_t n = rows.size();
        CHECK(history::parse(readText(fx / L"StreamingHistory_music_0.json"), rows, skipped) == history::Format::Account);
        CHECK(rows.size() == n + 3 && rows.back().track.name == "Firuze" && rows.back().track.id.rfind("import:", 0) == 0 &&
              rows.back().endedAt == utc(2024, 1, 5, 9, 15));
        CHECK(history::parse(readText(fx / L"StreamingHistory_podcast_0.json"), rows, skipped) == history::Format::Account);
        CHECK(rows.size() == n + 3 && skipped == 4);
        CHECK(history::parse(R"([{"id":1},{"name":"x"}])", rows, skipped) == history::Format::Unknown);
        CHECK(history::parse(R"({"ts":"x"})", rows, skipped) == history::Format::Unknown);
        bool threw = false;
        try {
            history::parse("{not json", rows, skipped);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw && rows.size() == n + 3);
    }
    // From the ZIP (deflate, written by PowerShell's Compress-Archive): the audio + music files; video, podcast and
    // the read-me are left out. Plus a file that isn't one and two that can't be read: reported, the rest imported.
    history::Result zip;
    {
        const auto junk = g_dir / L"notes.json";
        std::ofstream(junk, std::ios::binary) << R"({"hello":"world"})";
        int calls = 0, lastDone = -1, lastTotal = -1;
        zip = history::read({fx / L"my_spotify_data.zip", junk, g_dir / L"missing.zip", g_dir / L"notes.txt"},
                            [&](int done, int total) {
                                ++calls;
                                lastDone = done;
                                lastTotal = total;
                            });
        CHECK(zip.documents == 2 && zip.rows.size() == 10 && zip.skippedRows == 3);
        CHECK(zip.unrecognized == 1);             // notes.json
        CHECK(zip.errors.size() == 2);            // missing.zip, notes.txt
        CHECK(calls == 4 && lastDone == 3 && lastTotal == 3);
    }

    const auto file = g_dir / L"import.json";
    const auto importedFile = ListenStats::importedFileFor(file);
    CHECK(importedFile == g_dir / L"import-imported.json");
    ListenStats s(file);
    s.load();
    CHECK(s.canImport() && s.importedCount() == 0);
    // A play heard here at the same time as one in the files is not doubled.
    const auto kuzu = track("spotify:track:0000000000000000000003", "Kuzu Kuzu", "Tarkan", "Karma", 0, "spotify:artist:tarkan", "spotify:album:karma");
    s.addPlay(kuzu, utc(2024, 1, 3, 21, 0), 180'000);
    CHECK(s.save());
    {
        auto base = s.beginImport();
        CHECK(s.importing() && !s.canImport() && base.plays.size() == 1 && base.importedFile == importedFile);
        auto r = ListenStats::buildImport(base, zip.rows);
        // 10 rows: a 0.5 s one is too short; Kuzu Kuzu (both files) = the play heard here; the account file's
        // Gülpembe ends 40 s before the extended one with the same length = the same play.
        CHECK(r.ok && r.rows == 10 && r.tooShort == 1 && r.duplicates == 3 && r.added == 6);
        CHECK(fs::exists(importedFile));
        CHECK(s.finishImport(std::move(r)));
        CHECK(!s.importing() && s.canImport());
        CHECK(s.importedCount() == 6 && countImported(s.plays()) == 6 && s.playCount() == 7);
    }
    {
        const auto all = s.summarize(StatsPeriod::All, utc(2024, 12, 1));
        // Şımarık x2, Gülpembe 280 s (+ a 4 s skip), Uzun Kayıt, Firuze (imported) + Kuzu Kuzu (here).
        CHECK(all.streams == 6);
        CHECK(!all.topTracks.empty() && all.topTracks[0].track.name == "Şımarık" && all.topTracks[0].streams == 2);
        CHECK(!all.topArtists.empty() && all.topArtists[0].artist.name == "Tarkan" && all.topArtists[0].streams == 3 &&
              all.topArtists[0].artist.id == "spotify:artist:tarkan");   // merged with the play heard here
    }
    // Importing the same data again adds nothing; the imported file stays as it was.
    {
        const auto before = readText(importedFile);
        auto base = s.beginImport();
        auto r = ListenStats::buildImport(base, zip.rows);
        CHECK(r.ok && r.added == 0 && r.duplicates == 9 && r.tooShort == 1);
        CHECK(!s.finishImport(std::move(r)));
        CHECK(s.importedCount() == 6 && !s.importing() && readText(importedFile) == before);
    }
    // Regular saves never touch the imported file; the main file holds only this PC's plays.
    {
        const auto before = readText(importedFile);
        s.addPlay(kuzu, utc(2024, 6, 1, 12), 180'000);
        CHECK(s.save());
        CHECK(readText(importedFile) == before);
        const auto j = nlohmann::json::parse(readText(file));
        CHECK(j["plays"].size() == 2);
        const auto snap = ListenStats::readFile(file);
        CHECK(snap.plays.size() == 8 && countImported(snap.plays) == 6 && !snap.importedKeepFile);
        CHECK(std::is_sorted(snap.plays.begin(), snap.plays.end(), [](const auto& x, const auto& y) { return x.startedAt < y.startedAt; }));
        ListenStats r(file);
        r.adopt(snap);
        CHECK(r.importedCount() == 6 && r.playCount() == 8 && !r.dirty());
        CHECK(r.summarize(StatsPeriod::All, utc(2024, 12, 1)).streams == 7);
    }
    // Plays stored while an import runs survive it; a superseded import's result is dropped.
    {
        auto base = s.beginImport();
        s.addPlay(kuzu, utc(2024, 7, 1, 12), 180'000);
        std::vector<ListenStats::ImportRow> rows(1);
        rows[0].track.name = "Yeni Şarkı";
        rows[0].track.artists.push_back({"", "Yeni Sanatçı"});
        rows[0].endedAt = utc(2022, 3, 3, 3);
        rows[0].listenedMs = 120'000;
        auto r = ListenStats::buildImport(base, rows);
        CHECK(r.ok && r.added == 1);
        CHECK(s.finishImport(std::move(r)));
        CHECK(s.importedCount() == 7 && s.playCount() == 10);
        bool found = false;
        for (const auto& p : s.plays())
            if (!p.imported && p.startedAt == utc(2024, 7, 1, 12)) found = true;
        CHECK(found);

        // Superseded (a clear or a failure cancelled it) after its worker wrote the file: the store's state is written
        // back by the next save.
        auto base2 = s.beginImport();
        rows[0].endedAt = utc(2022, 4, 4, 4);
        auto r2 = ListenStats::buildImport(base2, rows);
        s.cancelImport();
        CHECK(!s.finishImport(std::move(r2)) && s.importedCount() == 7 && s.dirty());
        CHECK(countImported(ListenStats::readFile(file).plays) == 8);
        CHECK(s.save() && countImported(ListenStats::readFile(file).plays) == 7);
    }
    // Removing the imported plays empties the file on the next save; this PC's plays stay.
    {
        s.removeImported();
        CHECK(s.importedCount() == 0 && countImported(s.plays()) == 0 && s.playCount() == 3 && s.dirty());
        CHECK(s.save());
        const auto snap = ListenStats::readFile(file);
        CHECK(snap.plays.size() == 3 && countImported(snap.plays) == 0);
        CHECK(fs::exists(fs::path(importedFile).concat(L".old")));   // the previous one is kept once
    }
    // Clear drops both histories.
    {
        auto base = s.beginImport();
        CHECK(s.finishImport(ListenStats::buildImport(base, zip.rows)) && s.importedCount() == 6);
        s.clear(utc(2024, 12, 1));
        CHECK(s.playCount() == 0 && s.importedCount() == 0);
        CHECK(s.save());
        CHECK(ListenStats::readFile(file).plays.empty());
    }
    // A broken imported file: kept as .bak, the previous copy used, this PC's history unaffected. A locked one:
    // nothing imported this session and never replaced.
    {
        const auto f2 = g_dir / L"broken.json";
        ListenStats b(f2);
        b.load();
        b.addPlay(kuzu, utc(2024, 1, 1), 180'000);
        auto base = b.beginImport();
        CHECK(b.finishImport(ListenStats::buildImport(base, zip.rows)));
        auto base2 = b.beginImport();
        std::vector<ListenStats::ImportRow> one(1, zip.rows.back());
        one[0].endedAt += 86400;
        CHECK(b.finishImport(ListenStats::buildImport(base2, one)));   // 8 imported now, the first 7 in .old
        CHECK(b.save());
        const auto imp = ListenStats::importedFileFor(f2);
        std::ofstream(imp, std::ios::binary | std::ios::trunc) << "{\"v\":1,\"tracks\":[";
        const auto snap = ListenStats::readFile(f2);
        CHECK(snap.importedCorrupt && !snap.importedKeepFile && countImported(snap.plays) == 7 && snap.plays.size() == 8);
        CHECK(fs::exists(fs::path(imp).concat(L".bak")));
        HANDLE h = CreateFileW(imp.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto locked = ListenStats::readFile(f2);
        CloseHandle(h);
        CHECK(locked.importedKeepFile && locked.plays.size() == 1);
        ListenStats l(f2);
        l.adopt(locked);
        CHECK(!l.canImport() && l.beginImport().importedFile.empty());
    }
}

// 300 000 imported plays over 30 000 songs: import, save / load, aggregations (printed; the budget is loose).
static void testImportTiming() {
    std::printf("timing (import of 300 000 plays, 30 000 songs):\n");
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); };
    const auto file = g_dir / L"bigimport.json";
    std::vector<ListenStats::ImportRow> rows;
    rows.reserve(300'000);
    const int64_t first = utc(2016, 1, 1);
    for (int i = 0; i < 300'000; ++i) {
        const int k = static_cast<int>((static_cast<int64_t>(i) * 7919) % 30'000);
        ListenStats::ImportRow r;
        r.track.id = "spotify:track:" + std::to_string(1'000'000 + k);
        r.track.name = "Şarkı " + std::to_string(k);
        r.track.artists.push_back({"", "Sanatçı " + std::to_string(k % 3'000)});
        r.track.albumName = "Albüm " + std::to_string(k % 9'000);
        r.endedAt = first + static_cast<int64_t>(i) * 1000 + 240;   // ~86 plays a day for 9.5 years
        r.listenedMs = 20'000 + (i % 220) * 1000;
        rows.push_back(std::move(r));
    }
    // A realistic document for the parser: one Spotify file of 16 000 rows (~12 MB).
    {
        std::string doc = "[";
        for (int i = 0; i < 16'000; ++i) {
            if (i) doc += ',';
            doc += R"({"ts":"2023-05-0)" + std::to_string(1 + i % 9) +
                   R"(T12:00:00Z","platform":"windows","ms_played":180000,"conn_country":"TR","ip_addr":"0.0.0.0","master_metadata_track_name":"Şarkı )" +
                   std::to_string(i) +
                   R"(","master_metadata_album_artist_name":"Sanatçı","master_metadata_album_album_name":"Albüm","spotify_track_uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","episode_name":null,"episode_show_name":null,"spotify_episode_uri":null,"audiobook_title":null,"audiobook_uri":null,"audiobook_chapter_uri":null,"audiobook_chapter_title":null,"reason_start":"trackdone","reason_end":"trackdone","shuffle":false,"skipped":false,"offline":false,"offline_timestamp":null,"incognito_mode":false})";
        }
        doc += "]";
        std::vector<ListenStats::ImportRow> parsed;
        int skipped = 0;
        const auto t0 = clk::now();
        CHECK(history::parse(doc, parsed, skipped) == history::Format::Extended && parsed.size() == 16'000);
        std::printf("  parse       %7.1f ms  (one 16 000-row file, %.1f MB)\n", ms(t0), doc.size() / 1048576.0);
    }
    ListenStats s(file);
    s.load();
    auto t0 = clk::now();
    auto base = s.beginImport();
    auto r = ListenStats::buildImport(base, std::move(rows));
    const double build = ms(t0);
    std::printf("  build       %7.1f ms  (worker: dedupe + write, %.1f MB)\n", build,
                fs::file_size(ListenStats::importedFileFor(file)) / 1048576.0);
    CHECK(r.ok && r.added == 300'000);
    t0 = clk::now();
    CHECK(s.finishImport(std::move(r)));
    std::printf("  install     %7.1f ms  (UI thread)\n", ms(t0));
    CHECK(s.importedCount() == 300'000);
    t0 = clk::now();
    s.addPlay(track("x", "Yeni", "Biri", "Bir", 200'000), utc(2025, 9, 1), 60'000);
    const auto job = s.makeSaveJob();
    std::printf("  snapshot    %7.1f ms  (UI thread: a save with 300 000 imported plays around)\n", ms(t0));
    CHECK(job.plays.size() == 1 && ListenStats::write(job));
    t0 = clk::now();
    auto snap = ListenStats::readFile(file);
    const double read = ms(t0);
    std::printf("  readFile    %7.1f ms  (worker)\n", read);
    CHECK(snap.plays.size() == 300'001);
    ListenStats l(file);
    l.adopt(std::move(snap));
    const LocalClock clock = systemLocalClock();
    t0 = clk::now();
    const auto all = l.summarize(StatsPeriod::All, utc(2025, 9, 2));
    const double sumAll = ms(t0);
    std::printf("  summarize   %7.1f ms  (all time, UI thread)\n", sumAll);
    CHECK(all.streams > 0 && all.distinctTracks == 30'001);
    t0 = clk::now();
    const auto ys = l.years(clock);
    std::printf("  years       %7.1f ms  (%zu years)\n", ms(t0), ys.size());
    CHECK(ys.size() >= 10);
    t0 = clk::now();
    const auto y = l.summarizeYear(2020, clock);
    const double year = ms(t0);
    std::printf("  year        %7.1f ms  (2020: %d streams)\n", year, y.streams);
    CHECK(y.streams > 20'000);
    t0 = clk::now();
    const auto h = l.heatmap(StatsPeriod::All, utc(2025, 9, 2), clock);
    const double heat = ms(t0);
    std::printf("  heatmap     %7.1f ms  (all time)\n", heat);
    CHECK(h.totalMs > 0);
#ifdef NDEBUG
    CHECK(build + read < 4'000 && sumAll + year + heat < 400);
#else
    CHECK(build + read < 30'000 && sumAll + year + heat < 4'000);
#endif
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // progress stays visible when redirected (and if a check hangs)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    // Per process: two checkouts running their tests at the same time must not share (and wipe) one folder.
    g_dir = fs::path(tmp) / (L"shadetube_stats_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()
    ThreadPool pool(2);   // saveAsync
    ThreadPool::setShared(&pool);
    Dispatcher::init();

    testRule();
    testRecording();
    testSeeks();
    testAggregation();
    testPersistence();
    testTiming();
    testLocalTime();
    testHeatmapAndYears();
    testImport();
    testImportTiming();

    Dispatcher::shutdown();
    ThreadPool::setShared(nullptr);

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall stats checks passed\n");
    return 0;
}
