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
//
// Everything runs in a temp folder (SHADETUBE_DATA_DIR is pointed there too): the user's profile is never touched.
#include "app/ListenStats.h"
#include "core/Dispatcher.h"
#include "core/ThreadPool.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / L"shadetube_stats_test";
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
