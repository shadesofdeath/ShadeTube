// Developer test for video mode (src/video, youtube/VideoStreams):
//   video_test                 offline: picture stream choice on manifest fixtures, the still-picture check, the A/V
//                              scheduler against a fake clock and a fake decoder (1x / 0.5x / 2x / 3x, pause, seeks
//                              both ways, a decoder that falls behind, the end of the video), and a generated H.264 MP4
//                              (Media Foundation's encoder) decoded in software and on the GPU (DXVA + the video
//                              processor), frame by frame against the colors it was made of, before and after seeks
//   video_test live [<id>...]  + YouTube: the chosen stream of each video (and whether it is a still picture), the
//                              songs MatchService matches for a few tracks, and one stream played on the GPU for a few
//                              seconds against a real-time clock (frames shown / dropped, the download window)
// Runs in its own profile and temp folder (<exe dir>\video-test-<pid>, removed at the end).
#include "app/OfficialVideoCache.h"
#include "audio/AudioEngine.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "video/Fmp4Index.h"
#include "video/AudioAlign.h"
#include "video/Soundtrack.h"
#include "video/SyncScheduler.h"
#include "video/VideoDecoder.h"
#include "video/VideoPlayer.h"
#include "youtube/MatchService.h"
#include "youtube/OfficialVideo.h"
#include "youtube/VideoStreams.h"

#include <YoutubeExplode/YoutubeClient.hpp>

#include <windows.h>
#include <shellapi.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace yte = YoutubeExplode;
using Microsoft::WRL::ComPtr;
using st::video::FrameTime;
using st::video::SyncScheduler;
using st::youtube::VideoCandidate;

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

double nowMs() { return st::audio::AudioEngine::qpcNowMs(); }

// ---- stream choice ----------------------------------------------------------------------------------------------------

VideoCandidate cand(int itag, const char* container, const char* codec, int w, int h, int fps, int64_t kbps, bool muxed = false,
                    bool upscaled = false) {
    VideoCandidate c;
    c.itag = itag;
    c.container = container;
    c.codec = codec;
    c.width = w;
    c.height = h;
    c.fps = fps;
    c.bitrate = kbps * 1000;
    c.contentLength = kbps * 1000 / 8 * 200;   // a 200 s video
    c.muxed = muxed;
    c.upscaled = upscaled;
    return c;
}

// A typical music video manifest (itags as YouTube serves them).
std::vector<VideoCandidate> musicVideo() {
    return {cand(18, "mp4", "avc1.42001E", 640, 360, 30, 500, true),
            cand(160, "mp4", "avc1.4d400c", 256, 144, 30, 110),
            cand(133, "mp4", "avc1.4d4015", 426, 240, 30, 240),
            cand(134, "mp4", "avc1.4d401e", 640, 360, 30, 600),
            cand(135, "mp4", "avc1.4d401f", 854, 480, 30, 1100),
            cand(136, "mp4", "avc1.4d401f", 1280, 720, 30, 2300),
            cand(298, "mp4", "avc1.4d4020", 1280, 720, 60, 3500),
            cand(137, "mp4", "avc1.640028", 1920, 1080, 30, 4300),
            cand(299, "mp4", "avc1.64002a", 1920, 1080, 60, 6000),
            cand(244, "webm", "vp9", 854, 480, 30, 800),
            cand(247, "webm", "vp9", 1280, 720, 30, 1500),
            cand(248, "webm", "vp9", 1920, 1080, 30, 2700),
            cand(271, "webm", "vp9", 2560, 1440, 30, 9000),
            cand(398, "mp4", "av01.0.05M.08", 1280, 720, 30, 1300),
            cand(399, "mp4", "av01.0.08M.08", 1920, 1080, 30, 2400)};
}

void testSelection() {
    std::printf("stream choice\n");
    const st::youtube::VideoDecoders none{}, all{true, true};
    const auto mv = musicVideo();
    auto pick = [&](int rows, const st::youtube::VideoDecoders& d) { return st::youtube::pickVideoStream(mv, rows, d); };
    CHECK(pick(720, none) && pick(720, none)->itag == 136);     // H.264 720p30 before 720p60
    CHECK(pick(1080, none) && pick(1080, none)->itag == 137);
    CHECK(pick(480, none) && pick(480, none)->itag == 135);
    CHECK(pick(1080, all) && pick(1080, all)->itag == 137);     // H.264 reaches the cap: VP9 / AV1 not needed
    CHECK(pick(240, none) && pick(240, none)->itag == 133);     // a cap under 360 still works
    // Only 60 fps at a size: taken.
    {
        std::vector<VideoCandidate> v{cand(298, "mp4", "avc1.4d4020", 1280, 720, 60, 3500), cand(135, "mp4", "avc1.4d401f", 854, 480, 30, 1100)};
        const auto p = st::youtube::pickVideoStream(v, 720, none);
        CHECK(p && p->itag == 298);
    }
    // No H.264 above 360p (seen on some new uploads): VP9 / AV1 only with a decoder.
    {
        std::vector<VideoCandidate> v{cand(18, "mp4", "avc1.42001E", 640, 360, 30, 500, true), cand(134, "mp4", "avc1.4d401e", 640, 360, 30, 600),
                                      cand(247, "webm", "vp9", 1280, 720, 30, 1500), cand(398, "mp4", "av01.0.05M.08", 1280, 720, 30, 1300)};
        auto p = st::youtube::pickVideoStream(v, 720, none);
        CHECK(p && p->itag == 134);
        p = st::youtube::pickVideoStream(v, 720, {true, false});
        CHECK(p && p->itag == 247);
        p = st::youtube::pickVideoStream(v, 720, {false, true});
        CHECK(p && p->itag == 398);
        p = st::youtube::pickVideoStream(v, 360, all);   // the cap is reached by H.264
        CHECK(p && p->itag == 134);
    }
    // Upscaled ("Super Resolution") after the original at the same size.
    {
        std::vector<VideoCandidate> v{cand(136, "mp4", "avc1.4d401f", 1280, 720, 30, 2600, false, true), cand(136, "mp4", "avc1.4d401f", 1280, 720, 30, 2000)};
        v[1].itag = 999;
        const auto p = st::youtube::pickVideoStream(v, 720, none);
        CHECK(p && p->itag == 999);
    }
    // Portrait: the smaller side is the quality.
    {
        std::vector<VideoCandidate> v{cand(136, "mp4", "avc1.4d401f", 720, 1280, 30, 2000), cand(137, "mp4", "avc1.640028", 1080, 1920, 30, 4000)};
        const auto p = st::youtube::pickVideoStream(v, 720, none);
        CHECK(p && p->itag == 136);
    }
    // Only the muxed MP4, only undecodable streams, nothing.
    {
        std::vector<VideoCandidate> v{cand(18, "mp4", "avc1.42001E", 640, 360, 30, 500, true)};
        auto p = st::youtube::pickVideoStream(v, 720, none);
        CHECK(p && p->itag == 18 && p->muxed);
        v = {cand(247, "webm", "vp9", 1280, 720, 30, 1500)};
        CHECK(!st::youtube::pickVideoStream(v, 720, none));
        CHECK(!st::youtube::pickVideoStream({}, 720, all));
    }
    CHECK(st::youtube::codecOf("avc1.640028") == st::youtube::VideoCodec::H264);
    CHECK(st::youtube::codecOf("vp09.00.40.08") == st::youtube::VideoCodec::Vp9);
    CHECK(st::youtube::codecOf("av01.0.08M.08") == st::youtube::VideoCodec::Av1);
}

void testStatic() {
    std::printf("still picture check\n");
    auto at = [](int w, int h, int64_t bytes) {
        VideoCandidate c = cand(136, "mp4", "avc1.4d401f", w, h, 30, 0);
        c.contentLength = bytes;
        return c;
    };
    // 200 s: an Art Track's 720p stream of ~1 MB (40 kbps) vs a music video's 50 MB (2 Mbps).
    CHECK(st::youtube::looksStatic(at(1280, 720, 1'000'000), 200));
    CHECK(!st::youtube::looksStatic(at(1280, 720, 50'000'000), 200));
    CHECK(st::youtube::looksStatic(at(1920, 1080, 4'000'000), 200));    // 160 kbps at 1080p: static (threshold 450)
    CHECK(st::youtube::looksStatic(at(1280, 720, 3'500'000), 200));     // 140 kbps: an "Official Audio" still
    CHECK(!st::youtube::looksStatic(at(1280, 720, 8'350'000), 200));    // 334 kbps: a lyric video
    CHECK(!st::youtube::looksStatic(at(854, 480, 12'000'000), 200));    // 480 kbps at 480p: a real video
    CHECK(st::youtube::looksStatic(at(640, 360, 800'000), 200));        // 32 kbps at 360p (floor 45)
    CHECK(!st::youtube::looksStatic(at(1280, 720, 0), 200));            // unknown size: shown
    CHECK(!st::youtube::looksStatic(at(1280, 720, 1'000'000), 0));      // unknown duration: shown
    CHECK(st::youtube::averageKbps(at(1280, 720, 25'000'000), 200) == 1000);
}

// ---- fragmented MP4 index ------------------------------------------------------------------------------------------

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}
void box(std::vector<uint8_t>& v, const char* type, const std::vector<uint8_t>& body) {
    put32(v, static_cast<uint32_t>(8 + body.size()));
    v.insert(v.end(), type, type + 4);
    v.insert(v.end(), body.begin(), body.end());
}

void testFmp4Index() {
    std::printf("fragmented MP4 index (sidx)\n");
    // ftyp, moov, sidx (timescale 1000, 4 segments of 5 s, 1000 / 2000 / 1500 / 500 bytes), then the media.
    std::vector<uint8_t> f;
    box(f, "ftyp", {'d', 'a', 's', 'h', 0, 0, 0, 0});
    box(f, "moov", std::vector<uint8_t>(40, 0));
    const size_t initEnd = f.size();
    std::vector<uint8_t> sidx;
    put32(sidx, 0);       // version 0, flags
    put32(sidx, 1);       // reference_ID
    put32(sidx, 1000);    // timescale
    put32(sidx, 0);       // earliest_presentation_time
    put32(sidx, 0);       // first_offset
    put32(sidx, 4);       // reserved + reference_count
    for (uint32_t size : {1000u, 2000u, 1500u, 500u}) {
        put32(sidx, size);
        put32(sidx, 5000);
        put32(sidx, 0x90000000u);
    }
    box(f, "sidx", sidx);
    const int64_t mediaStart = static_cast<int64_t>(f.size());
    const int64_t length = mediaStart + 5000;
    const auto idx = st::video::parseFmp4Index(f, length);
    CHECK(idx.valid() && idx.segments.size() == 4);
    CHECK(idx.init.size() == initEnd);
    if (idx.segments.size() == 4) {
        CHECK(idx.segments[0].offset == mediaStart && idx.segments[1].offset == mediaStart + 1000);
        CHECK(idx.segments[3].offset == mediaStart + 4500 && idx.segments[2].startUs == 10'000'000);
        CHECK(idx.segmentAt(0) == 0 && idx.segmentAt(4'999'999) == 0 && idx.segmentAt(5'000'000) == 1);
        CHECK(idx.segmentAt(17'000'000) == 3 && idx.segmentAt(90'000'000) == 3);
    }
    CHECK(!st::video::parseFmp4Index(std::span<const uint8_t>(f.data(), initEnd + 10), length).valid());   // sidx cut off
    CHECK(!st::video::parseFmp4Index(f, mediaStart + 100).valid());                                       // sizes past the end
    // An edit list like YouTube's (media time 512 at timescale 12800 = 40 ms): renamed to "free" in the init boxes.
    {
        std::vector<uint8_t> mdhd, elst, edts, mdia, trak, moov, g;
        put32(mdhd, 0);                 // version 0
        put32(mdhd, 0);
        put32(mdhd, 0);
        put32(mdhd, 12800);             // timescale
        put32(mdhd, 0);
        put32(mdhd, 0x55c40000);
        box(mdia, "mdhd", mdhd);
        put32(elst, 0);
        put32(elst, 1);                 // entry_count
        put32(elst, 0x299c00);          // segment_duration
        put32(elst, 512);               // media_time
        put32(elst, 0x00010000);        // rate
        box(edts, "elst", elst);
        box(trak, "edts", edts);
        box(trak, "mdia", mdia);
        box(moov, "trak", trak);
        box(g, "ftyp", {'d', 'a', 's', 'h', 0, 0, 0, 0});
        box(g, "moov", moov);
        const size_t init = g.size();
        box(g, "sidx", sidx);
        const auto e = st::video::parseFmp4Index(g, static_cast<int64_t>(g.size()) + 5000);
        CHECK(e.valid() && e.editUs == 40'000);
        CHECK(e.init.size() == init);
        const std::string text(e.init.begin(), e.init.end());
        CHECK(text.find("edts") == std::string::npos && text.find("free") != std::string::npos && text.find("elst") != std::string::npos);
    }
    std::vector<uint8_t> plain;
    box(plain, "ftyp", {'i', 's', 'o', 'm', 0, 0, 0, 0});
    box(plain, "moov", std::vector<uint8_t>(40, 0));
    box(plain, "mdat", std::vector<uint8_t>(100, 0));
    CHECK(!st::video::parseFmp4Index(plain, static_cast<int64_t>(plain.size())).valid());   // not fragmented
}

// ---- A/V scheduler --------------------------------------------------------------------------------------------------

// A decoder stand-in: frames every `frameUs` from its position, up to `capacity` queued, `perVblank` decoded per vblank;
// a seek lands on the key frame before the target (every `gopUs`) and the frames before the target are skipped after
// `seekVblanks` vblanks.
struct FakeDecoder {
    int64_t frameUs = 33'333, gopUs = 2'000'000, lengthUs = 120'000'000;
    int capacity = 6, perVblank = 4, seekVblanks = 6;
    std::vector<FrameTime> queue;
    int64_t next = 0, discardUntil = 0;
    int waitVblanks = 0;
    bool ended = false;
    void seek(int64_t us) {
        queue.clear();
        next = us / gopUs * gopUs;
        next = next / frameUs * frameUs;
        discardUntil = us;
        waitVblanks = seekVblanks;
        ended = false;
    }
    void skipUntil(int64_t us) {
        discardUntil = std::max(discardUntil, us);
        std::erase_if(queue, [&](const FrameTime& f) { return f.ptsUs + f.durUs <= us; });
    }
    void step() {
        if (waitVblanks > 0) {
            --waitVblanks;
            return;
        }
        for (int i = 0; i < perVblank && static_cast<int>(queue.size()) < capacity; ++i) {
            if (next >= lengthUs) {
                ended = true;
                return;
            }
            const int64_t pts = next;
            next += frameUs;
            if (pts + frameUs <= discardUntil) {
                --i;   // the way from the key frame costs nothing here
                continue;
            }
            queue.push_back({pts, frameUs});
        }
    }
};

struct SimResult {
    int vblanks = 0, synced = 0, offBy = 0;   // vblanks after the first frame; within one frame of the clock; worse
    int64_t worstUs = 0;
    uint64_t shown = 0, dropped = 0, resyncs = 0;
};

// Plays `seconds` of display at 60 Hz. `clockAt(t)` gives the audio clock (us) and rate at display time t (ms).
template <class ClockFn>
SimResult simulate(FakeDecoder& dec, SyncScheduler& s, double seconds, ClockFn clockAt, double settleMs = 400) {
    SimResult r;
    int64_t shownPts = INT64_MIN;
    int64_t shownDur = 0;
    double lastJumpAt = 0;   // the start settles like a seek (the decoder's first frames arrive late)
    int64_t lastClock = 0;
    double lastT = 0;
    const double vblank = 1000.0 / 60.0;
    for (double t = 0; t < seconds * 1000; t += vblank) {
        dec.step();
        const auto [clock, rate] = clockAt(t);
        if (t > 0 && std::llabs(clock - lastClock - static_cast<int64_t>((t - lastT) * 1000 * rate)) > 300'000) lastJumpAt = t;
        lastClock = clock;
        lastT = t;
        const auto p = s.pick(dec.queue, clock, rate, t, dec.ended);
        if (p.resync) {
            dec.seek(p.resyncUs);
            continue;
        }
        if (p.show >= 0) {
            shownPts = dec.queue[static_cast<size_t>(p.show)].ptsUs;
            shownDur = dec.queue[static_cast<size_t>(p.show)].durUs;
        }
        dec.queue.erase(dec.queue.begin(), dec.queue.begin() + std::min<size_t>(dec.queue.size(), static_cast<size_t>(p.consume)));
        if (p.catchUpUs >= 0) dec.skipUntil(p.catchUpUs);
        if (shownPts == INT64_MIN || t - lastJumpAt < settleMs) continue;   // a seek settles first
        ++r.vblanks;
        // In sync: the frame on screen is the one whose interval holds the clock (one frame of slack either way).
        const int64_t err = clock < shownPts ? shownPts - clock : std::max<int64_t>(0, clock - (shownPts + shownDur));
        r.worstUs = std::max(r.worstUs, err);
        if (err <= dec.frameUs) ++r.synced;
        else ++r.offBy;
    }
    r.shown = s.stats().shown;
    r.dropped = s.stats().dropped;
    r.resyncs = s.stats().resyncs;
    return r;
}

void report(const char* what, const SimResult& r) {
    std::printf("  %-28s %d vblanks, %d in sync, %d off (worst %.1f ms); shown %llu, dropped %llu, resyncs %llu\n", what, r.vblanks,
                r.synced, r.offBy, r.worstUs / 1000.0, static_cast<unsigned long long>(r.shown),
                static_cast<unsigned long long>(r.dropped), static_cast<unsigned long long>(r.resyncs));
}

void testScheduler() {
    std::printf("A/V scheduler (fake clock, 60 Hz display, 30 fps video)\n");
    for (double rate : {1.0, 0.5, 2.0, 3.0}) {
        FakeDecoder dec;
        dec.perVblank = 6;
        dec.capacity = 6;
        SyncScheduler s;
        dec.seek(10'000'000);
        s.restart(10'000'000);
        const auto r = simulate(dec, s, 8, [&](double t) {
            return std::pair<int64_t, double>{10'000'000 + static_cast<int64_t>(t * 1000 * rate), rate};
        });
        char label[64];
        std::snprintf(label, sizeof label, "%.1fx", rate);
        report(label, r);
        CHECK(r.vblanks > 400 && r.offBy == 0);
        CHECK(r.resyncs == 0);
        if (rate <= 1.0) CHECK(r.dropped <= 4);   // only the catch-up after the start's seek
        if (rate > 2.5) CHECK(r.dropped > 0);   // 90 frames a second on a 60 Hz display
    }
    {
        // Pause at 3 s for 2 s: the picture holds, nothing is resynced; a seek forward at 6 s and back at 9 s.
        FakeDecoder dec;
        SyncScheduler s;
        dec.seek(0);
        s.restart(0);
        const auto r = simulate(dec, s, 12, [](double t) {
            int64_t c;
            double rate = 1;
            if (t < 3000) c = static_cast<int64_t>(t * 1000);
            else if (t < 5000) c = 3'000'000, rate = 0;
            else if (t < 6000) c = 3'000'000 + static_cast<int64_t>((t - 5000) * 1000);
            else if (t < 9000) c = 60'000'000 + static_cast<int64_t>((t - 6000) * 1000);
            else c = 20'000'000 + static_cast<int64_t>((t - 9000) * 1000);
            return std::pair<int64_t, double>{c, rate};
        });
        report("pause + seeks", r);
        CHECK(r.resyncs == 2);
        CHECK(r.offBy == 0);
    }
    {
        // The first frame after a seek comes before its time: shown at once (the old picture is the wrong scene).
        FakeDecoder dec;
        SyncScheduler s;
        dec.seek(0);
        s.restart(0);
        for (int i = 0; i < 10; ++i) {
            dec.step();
            const auto p = s.pick(dec.queue, i * 16'667, 1, i * 16.667, false);
            dec.queue.erase(dec.queue.begin(), dec.queue.begin() + p.consume);
            if (p.catchUpUs >= 0) dec.skipUntil(p.catchUpUs);
        }
        const auto p = s.pick(dec.queue, 30'000'000, 1, 11 * 16.667, false);   // a seek far ahead
        CHECK(p.resync && p.resyncUs >= 30'000'000 && p.resyncUs <= 30'300'000);
        dec.seek(p.resyncUs);
        int64_t firstShown = -1;
        for (int i = 12; i < 40 && firstShown < 0; ++i) {
            dec.step();
            const int64_t clock = 30'000'000 + static_cast<int64_t>((i - 11) * 16'667);
            const auto q = s.pick(dec.queue, clock, 1, i * 16.667, false);
            if (q.show >= 0) firstShown = dec.queue[static_cast<size_t>(q.show)].ptsUs;
            dec.queue.erase(dec.queue.begin(), dec.queue.begin() + q.consume);
            if (q.catchUpUs >= 0) dec.skipUntil(q.catchUpUs);
        }
        std::printf("  first frame after the seek: %.3f s (aimed at %.3f s)\n", firstShown / 1e6, p.resyncUs / 1e6);
        CHECK(firstShown >= p.resyncUs - 33'334 && firstShown < p.resyncUs + 100'000);
    }
    {
        // A decoder that can't keep up (one frame every second vblank at 3x = 1/3 of what is needed): it skips ahead
        // to the clock instead of showing ever older frames.
        FakeDecoder dec;
        dec.perVblank = 1;
        dec.seekVblanks = 2;
        SyncScheduler s;
        dec.seek(0);
        s.restart(0);
        int step = 0;
        uint64_t resyncs = 0;
        int64_t worstLag = 0;
        int64_t shown = INT64_MIN;
        for (double t = 0; t < 10000; t += 16.667, ++step) {
            if (step % 2 == 0) dec.step();
            const int64_t clock = static_cast<int64_t>(t * 1000 * 3);
            const auto p = s.pick(dec.queue, clock, 3, t, dec.ended);
            if (p.resync) {
                dec.seek(p.resyncUs);
                ++resyncs;
                continue;
            }
            if (p.show >= 0) shown = dec.queue[static_cast<size_t>(p.show)].ptsUs;
            dec.queue.erase(dec.queue.begin(), dec.queue.begin() + p.consume);
            if (p.catchUpUs >= 0) dec.skipUntil(p.catchUpUs);
            if (shown != INT64_MIN && t > 2000) worstLag = std::max(worstLag, clock - shown);
        }
        std::printf("  slow decoder at 3x: %llu resyncs in 10 s, worst lag %.2f s\n", static_cast<unsigned long long>(resyncs),
                    worstLag / 1e6);
        CHECK(resyncs <= 1);   // it catches up by skipping, not by seeking
        CHECK(worstLag < 1'500'000);
    }
    {
        // The video ends before the song: the last frame stays, no resync loop.
        FakeDecoder dec;
        dec.lengthUs = 2'000'000;
        SyncScheduler s;
        dec.seek(0);
        s.restart(0);
        const auto r = simulate(dec, s, 6, [](double t) { return std::pair<int64_t, double>{static_cast<int64_t>(t * 1000), 1.0}; });
        CHECK(dec.ended);
        CHECK(r.resyncs == 0);
        CHECK(s.shownPts() >= 1'900'000);
    }
}

// ---- a generated MP4, decoded --------------------------------------------------------------------------------------

constexpr int kW = 640, kH = 360, kFps = 30, kFrames = 150;   // 5 s

// Frame i: three bands whose levels encode i (survives H.264 well): R = (i % 8), G = (i / 8) % 8, B = 4 (on a 32 grid).
uint32_t colorOf(int i, int band) {
    const int v = band == 0 ? i % 8 : band == 1 ? (i / 8) % 8 : 4;
    return static_cast<uint32_t>(v * 32 + 16);
}

bool writeTestVideo(const fs::path& path) {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), &writer))) return false;
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, 1'500'000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    if (FAILED(writer->AddStream(out.Get(), &stream))) return false;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->SetInputMediaType(stream, in.Get(), nullptr)) || FAILED(writer->BeginWriting())) return false;
    const LONGLONG dur = 10'000'000 / kFps;
    for (int i = 0; i < kFrames; ++i) {
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(kW * kH * 4, &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        for (int y = 0; y < kH; ++y) {
            const int band = y * 3 / kH;
            // RGB32 is bottom-up by default: row 0 in memory is the last row of the picture.
            uint32_t* row = reinterpret_cast<uint32_t*>(p + static_cast<size_t>(kH - 1 - y) * kW * 4);
            const uint32_t v = colorOf(i, band);
            for (int x = 0; x < kW; ++x) row[x] = 0xFF000000u | v << 16 | v << 8 | v;
        }
        buf->Unlock();
        buf->SetCurrentLength(kW * kH * 4);
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(i * dur);
        s->SetSampleDuration(dur);
        if (FAILED(writer->WriteSample(stream, s.Get()))) return false;
    }
    return SUCCEEDED(writer->Finalize());
}

int decodeIndex(const uint8_t* bgra, int stride, int w, int h) {
    auto level = [&](int band) {
        const int y = (band * 2 + 1) * h / 6;
        const uint8_t* px = bgra + static_cast<size_t>(y) * stride + static_cast<size_t>(w / 2) * 4;
        return static_cast<int>(std::lround((px[1] - 16) / 32.0));   // green channel (gray bands)
    };
    const int r = level(0), g = level(1);
    if (r < 0 || r > 7 || g < 0 || g > 7) return -1;
    return g * 8 + r;
}

// Waits (bounded) until the decoder has frames or finished.
bool waitFrames(st::video::VideoDecoder& d, std::vector<st::video::VideoDecoder::Frame>& out, int want, int timeoutMs = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        d.peek(out);
        if (static_cast<int>(out.size()) >= want || d.state() == st::video::DecoderState::Ended ||
            d.state() == st::video::DecoderState::Failed)
            return !out.empty();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    d.peek(out);
    return !out.empty();
}

void testSoftwareDecode(const fs::path& file) {
    std::printf("software decoding of the generated MP4\n");
    st::video::Source src;
    src.localPath = file.wstring();
    src.mimeType = "video/mp4";
    st::video::VideoDecoder d(src, nullptr, nullptr, 0);
    std::vector<st::video::VideoDecoder::Frame> q;
    int matched = 0, total = 0;
    int64_t lastPts = -1;
    bool ordered = true;
    while (waitFrames(d, q, 1)) {
        for (const auto& f : q) {
            const auto fmt = d.format();
            const int idx = decodeIndex(d.slot(f.slot).pixels.data(), fmt.texWidth * 4, fmt.width, fmt.height);
            const int expected = static_cast<int>(std::lround(f.ptsUs * kFps / 1e6)) % 64;
            if (idx == expected) ++matched;
            if (f.ptsUs <= lastPts) ordered = false;
            lastPts = f.ptsUs;
            ++total;
        }
        d.take(static_cast<int>(q.size()));
        if (d.state() == st::video::DecoderState::Ended) {
            d.peek(q);
            if (q.empty()) break;
        }
    }
    const auto fmt = d.format();
    std::printf("  %d frames, %d with the right colors, %dx%d, %.2f fps, %s\n", total, matched, fmt.width, fmt.height, fmt.fps,
                fmt.hardware ? "hardware" : "software");
    CHECK(total == kFrames);
    CHECK(matched == total);
    CHECK(ordered);
    CHECK(fmt.width == kW && fmt.height == kH && std::fabs(fmt.fps - kFps) < 0.01);
    // Seek to 3.0 s: the first frame is frame 90.
    d.seek(3'000'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(waitFrames(d, q, 1));
    if (!q.empty()) {
        const int idx = decodeIndex(d.slot(q[0].slot).pixels.data(), fmt.texWidth * 4, fmt.width, fmt.height);
        std::printf("  after seek to 3.0 s: first frame %.3f s, colors of frame %d\n", q[0].ptsUs / 1e6, idx);
        CHECK(q[0].ptsUs >= 2'966'000 && q[0].ptsUs <= 3'034'000);
        CHECK(idx == 90 % 64);
    }
    // Seek back to 1 s.
    d.seek(1'000'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(waitFrames(d, q, 1) && q[0].ptsUs >= 966'000 && q[0].ptsUs <= 1'034'000);
}

// Reads back the bitmap the player draws (BGRA) and returns the frame index its colors encode.
int bitmapIndex(ID2D1Bitmap1* bmp, const D2D1_RECT_F& src) {
    auto* dc = st::gfx::Device::get().resourceContext();
    const auto size = bmp->GetPixelSize();
    ComPtr<ID2D1Bitmap1> cpu;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, bmp, nullptr))) return -2;
    D2D1_MAPPED_RECT m{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return -3;
    const int idx = decodeIndex(m.bits, static_cast<int>(m.pitch), static_cast<int>(src.right), static_cast<int>(src.bottom));
    static int dumps = 0;
    if (idx < 0 && dumps++ < 3) {
        const int w = static_cast<int>(src.right), h = static_cast<int>(src.bottom);
        for (int band = 0; band < 3; ++band) {
            const uint8_t* px = m.bits + static_cast<size_t>((band * 2 + 1) * h / 6) * m.pitch + static_cast<size_t>(w / 2) * 4;
            std::printf("    band %d: B %d G %d R %d A %d\n", band, px[0], px[1], px[2], px[3]);
        }
    }
    cpu->Unmap();
    return idx;
}

// The player as the views use it, against a clock that runs in real time from `startUs`.
struct PlayRun {
    int paints = 0, matched = 0, checked = 0;
    uint64_t shown = 0, dropped = 0, resyncs = 0;
    bool hardware = false;
    int64_t worstUs = 0;
    double slowestMs = 0;   // the longest paint (frame() between BeginDraw / EndDraw)
};

PlayRun play(st::video::VideoPlayer& p, int64_t startUs, double rate, double seconds, bool checkColors) {
    PlayRun r;
    auto* dc = st::gfx::Device::get().resourceContext();
    const double t0 = nowMs();
    while (nowMs() - t0 < seconds * 1000) {
        const double t = nowMs();
        const int64_t clock = startUs + static_cast<int64_t>((t - t0) * 1000 * rate);
        dc->BeginDraw();
        ID2D1Bitmap1* bmp = p.frame(dc, clock, rate, t);
        dc->EndDraw();
        ++r.paints;
        r.slowestMs = std::max(r.slowestMs, nowMs() - t);
        if (bmp && p.stats().shown > 0 && !p.lost()) {
            const int64_t err = std::llabs(p.stats().lastErrorUs);
            if (t - t0 > 500) r.worstUs = std::max(r.worstUs, err);
            if (checkColors && r.paints % 7 == 0) {
                const int idx = bitmapIndex(bmp, p.sourceRect());
                // The picture must be the frame for the clock (give or take one).
                const int expected = static_cast<int>(std::lround((clock / 1e6) * kFps));
                ++r.checked;
                bool hit = false;
                for (int d = -2; d <= 1 && !hit; ++d) hit = idx == ((expected + d) % 64 + 64) % 64;
                if (hit) ++r.matched;
                else if (r.checked - r.matched <= 3) std::printf("    picture of frame %d, clock at frame %d\n", idx, expected);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    r.shown = p.stats().shown;
    r.dropped = p.stats().dropped;
    r.resyncs = p.stats().resyncs;
    r.hardware = p.hardware();
    return r;
}

// Luma at the middle of each band of a decoded NV12 slot, read back through a staging texture.
void slotLuma(ID3D11Texture2D* tex, int w, int h, int out[3]) {
    auto* d3d = st::gfx::Device::get().d3d();
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> ctx;
    d3d->GetImmediateContext(&ctx);
    out[0] = out[1] = out[2] = -1;
    if (FAILED(d3d->CreateTexture2D(&d, nullptr, &staging))) return;
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    for (int band = 0; band < 3; ++band)
        out[band] = static_cast<const uint8_t*>(m.pData)[static_cast<size_t>((band * 2 + 1) * h / 6) * m.RowPitch + w / 2];
    ctx->Unmap(staging.Get(), 0);
}

void testGpuDecode(const fs::path& file) {
    std::printf("GPU decoding (DXVA) + video processor of the generated MP4\n");
    st::video::Source src;
    src.localPath = file.wstring();
    src.mimeType = "video/mp4";
    {
        // The decoder alone: its NV12 slots hold the picture (luma of the bands).
        auto& dev = st::gfx::Device::get();
        ComPtr<ID2D1Multithread> lock;
        dev.d2dFactory()->QueryInterface(IID_PPV_ARGS(&lock));
        st::video::VideoDecoder d(src, dev.d3d(), lock.Get(), 1'000'000);
        std::vector<st::video::VideoDecoder::Frame> q;
        CHECK(waitFrames(d, q, 1));
        if (!q.empty() && d.slot(q[0].slot).texture) {
            int y[3];
            slotLuma(d.slot(q[0].slot).texture.Get(), kW, kH, y);
            std::printf("  NV12 slot of %.3f s: luma %d / %d / %d\n", q[0].ptsUs / 1e6, y[0], y[1], y[2]);
            CHECK(y[2] > 100 && y[2] < 180);   // the B band: gray 144 in limited range
        }
    }
    {
        st::video::VideoPlayer p(src, 0);
        const auto r = play(p, 0, 1.0, 3.0, true);
        std::printf("  1x: %d paints (slowest %.1f ms), %llu shown, %llu dropped, %llu resyncs, colors %d/%d, %s, worst pick error %.1f ms\n",
                    r.paints, r.slowestMs, static_cast<unsigned long long>(r.shown), static_cast<unsigned long long>(r.dropped),
                    static_cast<unsigned long long>(r.resyncs), r.matched, r.checked, r.hardware ? "DXVA" : "software",
                    r.worstUs / 1000.0);
        CHECK(r.hardware);
        CHECK(r.shown >= 80);
        CHECK(r.checked > 0 && r.matched >= r.checked * 9 / 10);
        CHECK(r.worstUs <= 34'000);
        std::printf("  slots + display texture: %zu KB\n", p.gpuBytes() / 1024);
    }
    {
        st::video::VideoPlayer p(src, 1'000'000);   // 2x from 1 s
        const auto r = play(p, 1'000'000, 2.0, 1.5, true);
        std::printf("  2x: %llu shown, %llu dropped, %llu resyncs, colors %d/%d\n", static_cast<unsigned long long>(r.shown),
                    static_cast<unsigned long long>(r.dropped), static_cast<unsigned long long>(r.resyncs), r.matched, r.checked);
        CHECK(r.shown >= 15);   // the frames skipped while it caught up after opening are not counted
        CHECK(r.checked > 0 && r.matched >= r.checked * 8 / 10);
    }
    {
        // Software decoding through the same player (no DXVA).
        st::video::VideoPlayer p(src, 0, true);
        const auto r = play(p, 0, 1.0, 1.5, true);
        std::printf("  software player: %llu shown, colors %d/%d\n", static_cast<unsigned long long>(r.shown), r.matched, r.checked);
        CHECK(!r.hardware && r.shown >= 5 && r.matched >= r.checked * 9 / 10);   // a busy CPU decodes slower: fewer, still in sync
    }
    {
        // Idle release: the player is marked lost and holds nothing; the device comes back for the next one.
        st::video::VideoPlayer p(src, 0);
        play(p, 0, 1.0, 0.5, false);
        st::gfx::Device::get().release();
        CHECK(p.lost());
        CHECK(!st::gfx::Device::get().ready());
        st::gfx::Device::get().ensure();
        st::video::VideoPlayer again(src, 500'000);
        const auto r = play(again, 500'000, 1.0, 1.0, false);
        CHECK(r.shown > 3 && r.hardware);
    }
}

// ---- official video: alignment on synthetic songs ----------------------------------------------------------------------

constexpr int kRate = st::video::kSoundtrackRate;

// A made-up song: kick / snare / hats / tones on a seeded pattern at ~118 BPM, every bar its own (so nothing repeats
// unless a part is copied on purpose). `tempo` stretches the event times, `jitterMs` moves each event at random (a
// live take), `timbre` changes the tones' pitch (another performance).
std::vector<float> makeSong(uint32_t seed, double seconds, double tempo = 1.0, double jitterMs = 0, double timbre = 1.0) {
    std::vector<float> out(static_cast<size_t>(seconds * kRate), 0.f);
    std::mt19937 rng(seed), jit(seed ^ 0x9e3779b9u);
    std::uniform_real_distribution<double> u(0, 1);
    std::normal_distribution<double> jn(0, 1);
    auto add = [&](double at, auto&& voice, double len) {
        const long s0 = std::lround(at * kRate);
        const long n = static_cast<long>(len * kRate);
        for (long i = 0; i < n; ++i) {
            const long k = s0 + i;
            if (k < 0 || k >= static_cast<long>(out.size())) continue;
            out[static_cast<size_t>(k)] += static_cast<float>(voice(static_cast<double>(i) / kRate));
        }
    };
    const double step = 60.0 / 118 / 4;   // a 16th note
    std::mt19937 noise(seed * 7 + 1);
    std::uniform_real_distribution<float> nz(-1, 1);
    for (int i = 0; i * step * tempo < seconds; ++i) {
        const double at = i * step * tempo + (jitterMs > 0 ? jn(jit) * jitterMs / 1000 : 0);
        const double r = u(rng);
        if (i % 4 == 0 && r < 0.8) add(at, [](double t) { return 0.6 * std::sin(2 * 3.14159265 * 60 * t) * std::exp(-t * 18); }, 0.2);
        if (i % 8 == 4 && u(rng) < 0.9) add(at, [&](double t) { return 0.35 * nz(noise) * std::exp(-t * 25); }, 0.18);
        if (u(rng) < 0.55) add(at, [&](double t) { return 0.12 * nz(noise) * std::exp(-t * 90) * std::sin(2 * 3.14159265 * 4500 * t); }, 0.05);
        if (u(rng) < 0.3) {
            const double f = (220 + 880 * u(rng)) * timbre;
            const double len = 0.15 + 0.35 * u(rng);
            add(at, [f](double t) { return 0.25 * std::sin(2 * 3.14159265 * f * t) * std::exp(-t * 6); }, len);
        }
    }
    return out;
}

std::vector<float> slice(const std::vector<float>& x, double from, double to) {
    const size_t a = std::min(x.size(), static_cast<size_t>(std::max(0.0, from) * kRate));
    const size_t b = std::min(x.size(), static_cast<size_t>(std::max(0.0, to) * kRate));
    return std::vector<float>(x.begin() + static_cast<ptrdiff_t>(a), x.begin() + static_cast<ptrdiff_t>(std::max(a, b)));
}

std::vector<float> join(std::initializer_list<std::vector<float>> parts) {
    std::vector<float> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Something that isn't the song: low noise and murmur (a film's sound), at a lower level.
std::vector<float> cinematic(uint32_t seed, double seconds) {
    auto x = makeSong(seed, seconds, 1.7, 0, 0.6);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> nz(-1, 1);
    for (auto& v : x) v = v * 0.5f + 0.03f * nz(rng);
    return x;
}

// The video read `speed` times faster than the song (a transfer at another speed): sample t of the result is the
// song at t * speed.
std::vector<float> resampled(const std::vector<float>& x, double speed) {
    std::vector<float> out(static_cast<size_t>(static_cast<double>(x.size()) / speed));
    for (size_t i = 0; i < out.size(); ++i) {
        const double pos = static_cast<double>(i) * speed;
        const size_t k = static_cast<size_t>(pos);
        const double f = pos - static_cast<double>(k);
        out[i] = k + 1 < x.size() ? static_cast<float>(x[k] * (1 - f) + x[k + 1] * f) : 0.f;
    }
    return out;
}

st::video::AlignResult alignPcm(const std::vector<float>& song, const std::vector<float>& video) {
    const auto a = st::video::analyzeAudio(song.data(), song.size(), kRate);
    const auto b = st::video::analyzeAudio(video.data(), video.size(), kRate);
    auto r = st::video::alignAudio(a, b);
    std::printf("  %s (%s), %zu part(s), coverage %.0f %%, r %.2f, margin %.2f", r.accepted ? "accepted" : "declined", r.verdict.c_str(),
                r.segments.size(), r.coverage * 100, r.score, r.margin);
    for (const auto& s : r.segments) std::printf(" | %.2f-%.2f s %+.3f s x%.5f", s.audioStart, s.audioEnd, s.offsetAt(s.audioStart), s.slope);
    std::printf("\n");
    if (std::getenv("VIDEO_TEST_WINDOWS"))
        for (const auto& w : r.windows)
            std::printf("    window %6.1f s: %s lag %+8.3f s r %.2f\n", w.center, w.silent ? "silent " : w.matched ? "matched" : "none   ", w.lag, w.r);
    return r;
}

void testAlignment() {
    std::printf("official video: alignment on synthetic songs\n");
    const auto song = makeSong(11, 100);
    {   // the video has a 12.34 s cinematic intro and an outro: one part, the offset to a few ms
        const auto video = join({cinematic(91, 12.34), song, cinematic(92, 6)});
        const auto r = alignPcm(song, video);
        CHECK(r.accepted && r.segments.size() == 1);
        if (!r.segments.empty()) {
            CHECK(std::fabs(r.segments[0].offsetAt(10) - 12.34) < 0.006);
            CHECK(std::fabs(r.segments[0].offsetAt(90) - 12.34) < 0.006);
            CHECK(std::fabs(r.segments[0].slope - 1) < 1e-9);
            CHECK(r.segments[0].audioStart < 1 && r.segments[0].audioEnd > 99);
        }
        CHECK(r.coverage > 0.95);
    }
    {   // identical timing, another master (quieter, a little noise): offset 0
        std::vector<float> video = song;
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> nz(-1, 1);
        for (auto& v : video) v = v * 0.4f + 0.004f * nz(rng);
        const auto r = alignPcm(song, video);
        CHECK(r.accepted && r.segments.size() == 1);
        if (!r.segments.empty()) CHECK(std::fabs(r.segments[0].offsetAt(50)) < 0.004);
    }
    {   // a transfer 0.3 % fast: followed as a slope, in sync at both ends
        const double speed = 1.003;
        const auto video = join({cinematic(93, 2.0), resampled(song, speed)});
        const auto r = alignPcm(song, video);
        CHECK(r.accepted && r.segments.size() == 1);
        if (!r.segments.empty()) {
            const auto& s = r.segments[0];
            CHECK(std::fabs(s.slope - 1 / speed) < 2e-4);
            CHECK(std::fabs(s.videoAt(10) - (2.0 + 10 / speed)) < 0.012);
            CHECK(std::fabs(s.videoAt(95) - (2.0 + 95 / speed)) < 0.012);
        }
    }
    {   // the video inserts 3 s (a spoken break) after 50 s of the song: two parts, the jump found to a second
        const auto video = join({cinematic(94, 5), slice(song, 0, 50), cinematic(95, 3), slice(song, 50, 100)});
        const auto r = alignPcm(song, video);
        CHECK(r.accepted && r.segments.size() == 2);
        if (r.segments.size() == 2) {
            CHECK(std::fabs(r.segments[0].offsetAt(20) - 5) < 0.006);
            CHECK(std::fabs(r.segments[1].offsetAt(80) - 8) < 0.006);
            CHECK(std::fabs(r.segments[0].audioEnd - 50) < 1.0);
            CHECK(std::fabs(r.segments[1].audioStart - 50) < 1.0);
            // The time map: the jump becomes a short hold on the next part's first frame, then plays from there.
            const st::video::TimeMap map(r.segments);
            const auto before = map.at(30), during = map.at(r.segments[1].audioStart + 0.3), after = map.at(70);
            CHECK(before.shown && std::fabs(before.videoSec - 35) < 0.006 && before.rate == 1);
            CHECK(!during.shown && during.rate == 0);
            CHECK(std::fabs(during.videoSec - (r.segments[1].audioStart + 1.2 + 8)) < 0.01);
            CHECK(after.shown && std::fabs(after.videoSec - 78) < 0.006);
        }
    }
    {   // 20 s of the song are cut from the video: the cut is held, the rest plays
        const auto video = join({slice(song, 0, 40), slice(song, 60, 100)});
        const auto r = alignPcm(song, video);
        CHECK(r.accepted && r.segments.size() == 2);
        if (r.segments.size() == 2) {
            CHECK(std::fabs(r.segments[0].offsetAt(20)) < 0.006);
            CHECK(std::fabs(r.segments[1].offsetAt(80) + 20) < 0.006);
            CHECK(r.segments[0].audioEnd < 41 && r.segments[1].audioStart > 59);
            const st::video::TimeMap map(r.segments);
            CHECK(!map.at(50).shown && map.at(50).rate == 0);
        }
    }
    {   // repeated identical choruses: every part on its own place, not on a copy
        const auto a = makeSong(21, 16), b = makeSong(22, 20), c = makeSong(23, 16), d = makeSong(24, 12);
        const auto rep = join({a, b, c, b, c, d, c});
        const auto video = join({cinematic(96, 7.5), rep});
        const auto r = alignPcm(rep, video);
        CHECK(r.accepted && r.segments.size() == 1);
        if (!r.segments.empty()) CHECK(std::fabs(r.segments[0].offsetAt(60) - 7.5) < 0.006);
    }
    {   // the song's 1.5 s of silence at the start: the part reaches over it
        const auto quiet = join({std::vector<float>(static_cast<size_t>(1.5 * kRate), 0.f), song});
        const auto video = join({cinematic(97, 4), quiet});
        const auto r = alignPcm(quiet, video);
        CHECK(r.accepted && !r.segments.empty() && r.segments[0].audioStart < 0.05);
    }
    {   // another song: declined
        const auto r = alignPcm(song, join({cinematic(98, 3), makeSong(12, 100)}));
        CHECK(!r.accepted);
    }
    {   // a live take: 2 % slower, every hit 25 ms off at random, other notes: declined
        const auto r = alignPcm(song, makeSong(11, 104, 1.02, 25, 1.06));
        CHECK(!r.accepted);
    }
    {   // a live take at the same tempo but loose (40 ms): declined (never "almost in sync")
        const auto r = alignPcm(song, makeSong(11, 100, 1.0, 40, 1.0));
        CHECK(!r.accepted);
    }
    {   // the second half before the first: declined
        const auto r = alignPcm(song, join({slice(song, 50, 100), slice(song, 0, 50)}));
        CHECK(!r.accepted && r.verdict == "reordered");
    }
    {   // too short to say
        const auto shortSong = makeSong(13, 10);
        const auto r = alignPcm(shortSong, shortSong);
        CHECK(!r.accepted);
    }
    // The time map by itself.
    {
        const st::video::TimeMap id;
        CHECK(id.identity() && id.at(42).videoSec == 42 && id.at(42).shown && id.at(42).rate == 1);
        st::video::AlignSegment a{5, 60, 17, 1, 0.9}, b{60, 120, 72.04, 1, 0.9};   // a 40 ms micro-edit at 60 s
        const st::video::TimeMap m({a, b});
        CHECK(!m.at(2).shown && m.at(2).videoSec == 17 && m.at(2).rate == 0);   // before the first part: its first frame
        CHECK(m.at(30).shown && std::fabs(m.at(30).videoSec - 42) < 1e-9);
        CHECK(m.at(60.5).shown && std::fabs(m.at(60.5).videoSec - 72.54) < 1e-9);   // no hold for a micro-edit
        CHECK(!m.at(130).shown && std::fabs(m.at(130).videoSec - 132.04) < 1e-9);   // after the last: its end
        st::video::AlignSegment c{0, 100, 3, 0.997, 0.9};
        const st::video::TimeMap slow({c});
        CHECK(std::fabs(slow.at(50).videoSec - (3 + 0.997 * 50)) < 1e-9 && std::fabs(slow.at(50).rate - 0.997) < 1e-9);
    }
}

// ---- official video: candidates and the decision cache -----------------------------------------------------------------

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void testOfficialCandidates() {
    std::printf("official video: YouTube Music search results and the ranking\n");
    using st::youtube::OfficialQuery;
    const fs::path dir = ST_VIDEO_FIXTURES;
    {
        const auto items = st::youtube::parseMusicVideoSearch(readText(dir / "ytm_videos_rick.json"));
        CHECK(items.size() == 12);
        if (!items.empty()) {
            CHECK(items[0].videoId == "dQw4w9WgXcQ" && items[0].omv && !items[0].ugc);
            CHECK(items[0].title == "Never Gonna Give You Up" && items[0].durationSec == 214);
            CHECK(items[0].artists.size() == 1 && items[0].artists[0] == "Rick Astley" && items[0].channel == "Rick Astley");
            CHECK(!items[0].thumbnailUrl.empty());
        }
        CHECK(std::count_if(items.begin(), items.end(), [](const auto& c) { return c.ugc; }) >= 8);
        const auto ranked = st::youtube::rankOfficialVideos(OfficialQuery{"Never Gonna Give You Up", {"Rick Astley"}, 213, "Whenever You Need Somebody"}, items);
        for (const auto& c : ranked) std::printf("    %s %.0f \"%s\" (%s)\n", c.videoId.c_str(), c.score, c.title.c_str(), c.channel.c_str());
        CHECK(!ranked.empty() && ranked[0].videoId == "dQw4w9WgXcQ");
        // Another song by him, a tutorial, a 35 s clip, a bass-boosted upload, a mashup, a translation, a live radio take:
        // none of them.
        for (const auto& c : ranked) {
            CHECK(c.title.find("Together Forever") == std::string::npos);
            CHECK(c.title.find("Tutorial") == std::string::npos && c.title.find("Chorus") == std::string::npos);
            CHECK(c.title.find("BASS") == std::string::npos && c.title.find("Live") == std::string::npos);
        }
        // Without a duration (an unknown length) the official video still comes first.
        const auto any = st::youtube::rankOfficialVideos(OfficialQuery{"Never Gonna Give You Up", {"Rick Astley"}, 0, ""}, items);
        CHECK(!any.empty() && any[0].videoId == "dQw4w9WgXcQ");
        // Another artist's song of that name: nothing.
        CHECK(st::youtube::rankOfficialVideos(OfficialQuery{"Never Gonna Give You Up", {"Someone Else"}, 213, ""}, items).empty());
    }
    {   // Only live videos are official: nothing
        const auto items = st::youtube::parseMusicVideoSearch(readText(dir / "ytm_videos_eagles.json"));
        CHECK(items.size() >= 8);
        CHECK(std::any_of(items.begin(), items.end(), [](const auto& c) { return c.omv && c.title.find("Live") != std::string::npos; }));
        const auto ranked = st::youtube::rankOfficialVideos(OfficialQuery{"Hotel California - 2013 Remaster", {"Eagles"}, 391, "Hotel California"}, items);
        for (const auto& c : ranked) std::printf("    %s %.0f \"%s\" (%s)\n", c.videoId.c_str(), c.score, c.title.c_str(), c.channel.c_str());
        CHECK(ranked.empty());
    }
    {   // The official video first; the official lyric video of a remix and the Super Bowl show are refused
        const auto items = st::youtube::parseMusicVideoSearch(readText(dir / "ytm_videos_weeknd.json"));
        const auto ranked = st::youtube::rankOfficialVideos(OfficialQuery{"Blinding Lights", {"The Weeknd"}, 200, "After Hours"}, items);
        for (const auto& c : ranked) std::printf("    %s %.0f \"%s\" (%s)\n", c.videoId.c_str(), c.score, c.title.c_str(), c.channel.c_str());
        CHECK(!ranked.empty() && ranked[0].videoId == "4NRXx6U8ABQ");
        for (const auto& c : ranked) CHECK(c.title.find("Lyric") == std::string::npos && c.title.find("live") == std::string::npos);
    }
    {   // Made-up YouTube results (no YouTube Music labels): official signals are needed
        std::vector<st::youtube::OfficialCandidate> v(4);
        v[0] = {"aaaaaaaaaaa", "Artist - Song (Official Music Video)", "ArtistVEVO", {}, 230};
        v[1] = {"bbbbbbbbbbb", "Artist - Song", "Some Fan", {}, 215};
        v[2] = {"ccccccccccc", "Artist - Song (Official Audio)", "Artist", {}, 214};
        v[3] = {"ddddddddddd", "Song", "Artist - Topic", {}, 214};
        const auto ranked = st::youtube::rankOfficialVideos(OfficialQuery{"Song", {"Artist"}, 214, ""}, v);
        CHECK(ranked.size() == 1 && ranked[0].videoId == "aaaaaaaaaaa");
        // Far too long (a 14 minute short film) or much too short: refused
        v[0].durationSec = 840;
        CHECK(st::youtube::rankOfficialVideos(OfficialQuery{"Song", {"Artist"}, 214, ""}, v).empty());
        v[0].durationSec = 150;
        CHECK(st::youtube::rankOfficialVideos(OfficialQuery{"Song", {"Artist"}, 214, ""}, v).empty());
    }
    CHECK(st::youtube::parseMusicVideoSearch("not json").empty());
    CHECK(st::youtube::parseMusicVideoSearch("{}").empty());
}

void testDecisionCache(const fs::path& work) {
    std::printf("official video: the decision cache\n");
    using st::app::OfficialDecision;
    const fs::path file = work / L"officialvideos.json";
    const int64_t now = 1'800'000'000;
    {
        st::app::OfficialVideoCache c(file);
        c.load();
        OfficialDecision d;
        d.audioVideoId = "ATVATVATV01";
        d.videoId = "OMVOMVOMV01";
        d.title = "Şarkı (Official Video)";
        d.segments = {{0, 100, 12.34, 1, 0.9}, {101.2, 190, 115.0, 0.99746, 0.8}};
        d.tried = {{"OMVOMVOMV01", "match"}, {"LIVELIVE001", "no match"}};
        d.rejected = {"REJECTED001"};
        d.decidedAt = now;
        c.put("track:1", d);
        OfficialDecision none;
        none.audioVideoId = "ATVATVATV02";
        none.verdict = "no candidates";
        none.decidedAt = now;
        c.put("track:2", none);
        OfficialDecision off = none;
        off.manual = true;
        off.audioVideoId = "ATVATVATV03";
        c.put("track:3", off);
        c.flush();
    }
    st::app::OfficialVideoCache c(file);
    c.load();
    CHECK(c.size() == 3);
    const auto d = c.find("track:1", "ATVATVATV01", now + 10);
    CHECK(d && d->videoId == "OMVOMVOMV01" && d->title == "Şarkı (Official Video)" && d->segments.size() == 2);
    if (d && d->segments.size() == 2) {
        CHECK(std::fabs(d->segments[1].slope - 0.99746) < 1e-9 && std::fabs(d->segments[0].videoStart - 12.34) < 1e-9);
        CHECK(d->verdictOf("LIVELIVE001") == "no match" && d->rejected.size() == 1);
    }
    CHECK(!c.find("track:1", "ANOTHERUPLD", now));                                   // another upload plays the song now
    CHECK(c.find("track:2", "ATVATVATV02", now + 3600));                              // "none" is kept a while...
    CHECK(!c.find("track:2", "ATVATVATV02", now + 8LL * 24 * 3600));               // ...then searched again (a week)
    CHECK(c.find("track:3", "ATVATVATV03", now + 400LL * 24 * 3600));               // the user's "no video" stays
    OfficialDecision old = *c.raw("track:1");
    old.version = 0;
    c.put("track:1", old);
    CHECK(!c.find("track:1", "ATVATVATV01", now));                                   // an older alignment: made again
    CHECK(c.raw("track:1") && c.raw("track:1")->rejected.size() == 1);               // ...the rejections still known
    // A broken file: an empty cache, no crash.
    {
        std::ofstream f(work / L"broken.json", std::ios::binary);
        f << "{\"x\": [1, 2";
    }
    st::app::OfficialVideoCache broken(work / L"broken.json");
    broken.load();
    CHECK(broken.size() == 0);
}

// ---- alignment against real soundtracks -------------------------------------------------------------------------------

std::vector<float> readRaw(const fs::path& p) {
    std::vector<float> v;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize(static_cast<size_t>(std::max(0L, n)) / sizeof(float));
    fread(v.data(), sizeof(float), v.size(), f);
    fclose(f);
    return v;
}

// The sample-accurate offset of the song's waveform at `t` inside the video's (2 s of song, searched +-80 ms around
// `predicted`): the reference the envelope alignment is measured against. r < 0.5: the waveforms differ there (another
// mix / master), no reference.
struct Truth {
    double offset = 0, r = 0;
};
Truth waveformOffset(const std::vector<float>& song, const std::vector<float>& video, double t, double predicted, int rate,
                     double slope = 1) {
    const int n = 2 * rate, range = rate * 8 / 100;
    const long s0 = std::lround(t * rate);
    Truth best{predicted, -1};
    if (s0 < 0 || s0 + n > static_cast<long>(song.size())) return best;
    double ea = 0;
    for (int i = 0; i < n; ++i) ea += static_cast<double>(song[static_cast<size_t>(s0 + i)]) * song[static_cast<size_t>(s0 + i)];
    const long c = std::lround((t + predicted) * rate);
    std::vector<double> rs(static_cast<size_t>(2 * range + 1), -1);
    for (int d = -range; d <= range; ++d) {
        const long v0 = c + d;
        const double last = v0 + slope * n + 1;
        if (v0 < 0 || last >= static_cast<double>(video.size())) continue;
        double num = 0, ev = 0;
        for (int i = 0; i < n; ++i) {
            // The video read at the segment's speed (linear interpolation).
            const double pos = static_cast<double>(v0) + slope * i;
            const size_t k = static_cast<size_t>(pos);
            const double f = pos - static_cast<double>(k);
            const double a = song[static_cast<size_t>(s0 + i)], b = video[k] * (1 - f) + video[k + 1] * f;
            num += a * b;
            ev += b * b;
        }
        rs[static_cast<size_t>(d + range)] = ea > 0 && ev > 0 ? num / std::sqrt(ea * ev) : 0;
    }
    const size_t bi = static_cast<size_t>(std::max_element(rs.begin(), rs.end()) - rs.begin());
    double frac = 0;
    if (bi > 0 && bi + 1 < rs.size()) {
        const double den = rs[bi - 1] - 2 * rs[bi] + rs[bi + 1];
        if (den < 0) frac = 0.5 * (rs[bi - 1] - rs[bi + 1]) / den;
    }
    best.r = rs[bi];
    best.offset = static_cast<double>(c + static_cast<long>(bi) - range - s0 + frac) / rate;
    return best;
}

st::video::AlignResult alignAndReport(const std::vector<float>& song, const std::vector<float>& video, int rate) {
    const double t0 = nowMs();
    const auto fs_ = st::video::analyzeAudio(song.data(), song.size(), rate);
    const auto fv = st::video::analyzeAudio(video.data(), video.size(), rate);
    const double t1 = nowMs();
    const auto r = st::video::alignAudio(fs_, fv);
    const double t2 = nowMs();
    std::printf("  song %.1f s, video %.1f s: %s (%s), coverage %.0f %%, score %.2f, margin %.2f; features %.0f ms, alignment %.0f ms\n",
                fs_.seconds(), fv.seconds(), r.accepted ? "ACCEPTED" : "declined", r.verdict.c_str(), r.coverage * 100, r.score, r.margin,
                t1 - t0, t2 - t1);
    if (std::getenv("VIDEO_TEST_WINDOWS"))
        for (const auto& w : r.windows)
            std::printf("    window %6.1f s: %s lag %+8.3f s r %.2f\n", w.center, w.silent ? "silent " : w.matched ? "matched" : "none   ", w.lag,
                        w.r);
    for (const auto& s : r.segments) {
        std::printf("    song %7.2f .. %7.2f s -> video %7.2f s, offset %+8.3f s, slope %.5f, r %.2f\n", s.audioStart, s.audioEnd,
                    s.videoStart, s.offsetAt(s.audioStart), s.slope, s.score);
        // Measured against the waveforms every 15 s inside the segment.
        double worst = 0, sum = 0;
        int n = 0, skipped = 0;
        for (double t = s.audioStart + 3; t + 3 < s.audioEnd; t += 15) {
            const auto tr = waveformOffset(song, video, t, s.offsetAt(t), rate, s.slope);
            if (tr.r < 0.5) {
                ++skipped;
                continue;
            }
            const double err = (s.offsetAt(t) - tr.offset) * 1000;
            if (std::getenv("VIDEO_TEST_WINDOWS")) std::printf("      at %6.1f s: waveform %+8.4f s (r %.2f), error %+.1f ms\n", t, tr.offset, tr.r, err);
            worst = std::max(worst, std::fabs(err));
            sum += std::fabs(err);
            ++n;
        }
        if (n) std::printf("      vs the waveform at %d points: mean |error| %.1f ms, worst %.1f ms (%d points without a waveform match)\n", n,
                           sum / n, worst, skipped);
        else std::printf("      no waveform reference (%d points: another mix or master)\n", skipped);
    }
    return r;
}

// ---- live ---------------------------------------------------------------------------------------------------------

void testLivePlay(st::youtube::MatchService& service, const std::string& playId);

void testLive(st::youtube::MatchService& service, std::vector<std::string> ids) {
    std::printf("YouTube picture streams\n");
    const auto dec = st::video::optionalDecoders();
    const st::youtube::VideoDecoders decoders{dec.vp9, dec.av1};
    std::printf("  optional decoders: VP9 %s, AV1 %s\n", dec.vp9 ? "yes" : "no", dec.av1 ? "yes" : "no");
    // Songs as the app matches them: what kind of upload comes back, and the still-picture verdict.
    struct Song {
        const char* artist;
        const char* title;
        int ms;
    };
    const Song songs[] = {{"Tarkan", "ÅÄ±marÄ±k", 236000},        {"Daft Punk", "Get Lucky", 248000},
                          {"Adele", "Hello", 295000},           {"Sezen Aksu", "Gidiyorum", 260000},
                          {"The Weeknd", "Blinding Lights", 200000}, {"Billie Eilish", "bad guy", 194000}};
    for (const auto& s : songs) {
        st::catalog::Track t;
        t.id = std::string("test:") + s.artist + ":" + s.title;
        t.name = s.title;
        t.artists = {{"", s.artist}};
        t.durationMs = s.ms;
        try {
            const auto r = service.resolve(t, false, false);
            const auto v = service.videoStream(r.match.videoId, 720, decoders, r.match.durationSec, false);
            std::printf("  %-14s %-18s -> %s \"%s\" (%s): itag %d %dx%d@%d %s, %s\n", s.artist, s.title, r.match.videoId.c_str(),
                        r.match.title.c_str(), r.match.channel.c_str(), v.itag, v.width, v.height, v.fps, v.codec.c_str(),
                        v.staticImage ? "STILL PICTURE" : "video");
            if (r.match.channel.ends_with(" - Topic")) CHECK(v.staticImage);
            if (std::find(ids.begin(), ids.end(), r.match.videoId) == ids.end() && !v.staticImage) ids.push_back(r.match.videoId);
        } catch (const std::exception& e) {
            std::printf("  %s - %s: %s\n", s.artist, s.title, e.what());
        }
    }
    std::string playId;
    for (const auto& id : ids) {
        try {
            const auto v = service.videoStream(id, 720, decoders, 0, false);
            std::printf("  %s: itag %d %s %dx%d@%d %d kbps, %lld bytes\n", id.c_str(), v.itag, v.codec.c_str(), v.width, v.height, v.fps,
                        v.bitrateKbps, static_cast<long long>(v.contentLength));
            CHECK(v.height <= 720 && !v.url.empty());
            if (playId.empty()) playId = id;
        } catch (const std::exception& e) {
            std::printf("  %s: %s\n", id.c_str(), e.what());
        }
    }
    if (!playId.empty()) testLivePlay(service, playId);
}

void testLivePlay(st::youtube::MatchService& service, const std::string& playId) {
    const auto dec = st::video::optionalDecoders();
    const st::youtube::VideoDecoders decoders{dec.vp9, dec.av1};
    const auto v = service.videoStream(playId, 720, decoders, 0, false);
    std::printf("playing %s from 30 s on the GPU (real-time clock)\n", playId.c_str());
    st::video::Source src;
    src.url = v.url;
    src.contentLength = v.contentLength;
    src.mimeType = v.mimeType;
    src.readAheadBytes = std::max<int64_t>(1 << 20, static_cast<int64_t>(v.bitrateKbps) * 1000 / 8 * 8);
    if (const char* w = std::getenv("VIDEO_TEST_WINDOW")) src.readAheadBytes = std::atoll(w);
    std::printf("  download window %lld KB\n", static_cast<long long>(src.readAheadBytes >> 10));
    const double t0 = nowMs();
    const int64_t startUs = std::getenv("VIDEO_TEST_START") ? std::atoll(std::getenv("VIDEO_TEST_START")) * 1000 : 30'000'000;
    st::video::VideoPlayer p(src, startUs);
    PlayRun r = play(p, startUs, 1.0, 8.0, false);
    std::printf("  %d paints (slowest %.1f ms), %llu shown, %llu dropped, %llu resyncs, %s, opened in %.0f ms, worst pick error %.1f ms\n",
                r.paints, r.slowestMs, static_cast<unsigned long long>(r.shown), static_cast<unsigned long long>(r.dropped),
                static_cast<unsigned long long>(r.resyncs), r.hardware ? "DXVA" : "software", nowMs() - t0 - 8000, r.worstUs / 1000.0);
    std::printf("  downloaded %.1f %% of the stream after 8 s\n", p.bufferedFraction() * 100);
    CHECK(r.shown > 100);
    CHECK(r.resyncs <= 1);
    std::printf("  seek to 90 s\n");
    r = play(p, 90'000'000, 1.0, 4.0, false);
    std::printf("  %llu shown in total, %llu resyncs\n", static_cast<unsigned long long>(r.shown), static_cast<unsigned long long>(r.resyncs));
    CHECK(r.resyncs >= 1 && r.resyncs <= 3);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path work = fs::path(exe).parent_path() / (L"video-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(work);
    if (!GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr, 0)) SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (work / L"profile").c_str());
    st::log::init();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const std::string mode = argc > 1 ? argv[1] : "";

    // "official": only the official video's offline checks; "alignraw": none (a dev mode on files).
    const bool offline = mode != "alignraw" && mode != "official" && mode != "officiallive";
    if (offline) {
        testSelection();
        testStatic();
        testFmp4Index();
        testScheduler();
    }
    if (offline || mode == "official") {
        testAlignment();
        testOfficialCandidates();
        testDecisionCache(work);
    }
    const fs::path file = work / L"test.mp4";
    if (!offline) {
    } else if (writeTestVideo(file)) {
        testSoftwareDecode(file);
        try {
            st::gfx::Device::get().init();
            testGpuDecode(file);
        } catch (const std::exception& e) {
            std::printf("  FAIL  graphics device: %s\n", e.what());
            ++g_failures;
        }
    } else {
        std::printf("  FAIL  Media Foundation could not write the test video\n");
        ++g_failures;
    }
    if (mode == "alignraw" && argc > 3) {   // dev: two 12 kHz mono f32 files (video_test soundtrack ...)
        alignAndReport(readRaw(argv[2]), readRaw(argv[3]), st::video::kSoundtrackRate);
    }
    if (mode == "live" || mode == "play" || mode == "fetch" || mode == "file" || mode == "mfurl" || mode == "soundtrack" ||
        mode == "officiallive") {
        yte::YoutubeClientOptions o;
        o.httpClient = st::http::client();
        st::youtube::MatchService service{yte::YoutubeClient(o)};
        service.setAltSource("off", "");
        std::vector<std::string> ids;   // UTF-8 (song titles), from the wide command line
        {
            int wargc = 0;
            LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
            for (int i = 2; i < wargc; ++i) ids.push_back(st::toUtf8(wargv[i]));
            LocalFree(wargv);
        }
        if (ids.empty()) ids = {"dQw4w9WgXcQ"};
        if (mode == "mfurl" && !ids.empty()) {   // dev: Media Foundation's own file source on a file, samples counted
            ComPtr<IMFSourceReader> reader;
            HRESULT hr = MFCreateSourceReaderFromURL(fs::path(ids[0]).c_str(), nullptr, &reader);
            int samples = 0;
            LONGLONG first = -1;
            if (SUCCEEDED(hr)) {
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
                for (int i = 0; i < 300; ++i) {
                    DWORD flags = 0;
                    LONGLONG ts = 0;
                    ComPtr<IMFSample> s;
                    hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &s);
                    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
                    if (s) {
                        if (first < 0) first = ts;
                        ++samples;
                    }
                }
            }
            std::printf("  MF file source: hr 0x%08X, %d samples (compressed), first at %.3f s\n", static_cast<unsigned>(hr), samples,
                        first / 1e7);
        } else if (mode == "file" && !ids.empty()) {   // dev: a local video file through the player from <start s>
            st::video::Source src;
            src.localPath = fs::path(ids[0]).wstring();
            src.mimeType = "video/mp4";
            const int64_t startUs = ids.size() > 1 ? std::atoll(ids[1].c_str()) * 1'000'000 : 0;
            auto& dev = st::gfx::Device::get();
            const auto before = dev.gpuMemory();
            st::video::VideoPlayer p(src, startUs);
            const PlayRun r = play(p, startUs, 1.0, 5.0, false);
            const auto during = dev.gpuMemory();
            std::printf("  GPU memory: %.1f MB before, %.1f MB while playing (+%.1f MB; slots + display %.1f MB)\n",
                        before.local / 1048576.0, during.local / 1048576.0, (during.local - before.local) / 1048576.0,
                        p.gpuBytes() / 1048576.0);
            std::printf("  %s from %lld s: %d paints, %llu shown, %llu dropped, %llu resyncs, %s, state %d, %s\n", ids[0].c_str(),
                        static_cast<long long>(startUs / 1'000'000), r.paints, static_cast<unsigned long long>(r.shown),
                        static_cast<unsigned long long>(r.dropped), static_cast<unsigned long long>(r.resyncs),
                        r.hardware ? "DXVA" : "software", static_cast<int>(p.state()), p.error().c_str());
        } else if (mode == "fetch" && ids.size() >= 2) {   // dev: the picture stream of a video into a file
            const auto v = service.videoStream(ids[0], 720, {}, 0, false);
            auto buf = std::make_shared<st::audio::ProgressiveBuffer>(v.url, L"", v.contentLength, nullptr);
            buf->start();
            const int64_t len = buf->waitForLength();
            std::vector<uint8_t> all(static_cast<size_t>(std::max<int64_t>(0, len)));
            const bool ok = len > 0 && buf->read(0, all.data(), all.size()) == st::audio::ProgressiveBuffer::ReadStatus::Ok;
            if (ok) {
                FILE* out = _wfopen(fs::path(ids[1]).c_str(), L"wb");
                if (out) {
                    fwrite(all.data(), 1, all.size(), out);
                    fclose(out);
                }
            }
            std::printf("  fetched %lld bytes: %s\n", static_cast<long long>(len), ok ? "ok" : "failed");
        } else if (mode == "soundtrack" && ids.size() >= 2) {   // dev: a video's smallest audio stream, 12 kHz mono f32
            try {
                // [codec]: "opus" (default), "mp4a.40.2", "mp4a.40.5"; "best" / "bestopus": the engine's own stream.
                const std::string codec = ids.size() > 2 ? ids[2] : "opus";
                const bool best = codec.starts_with("best");
                const auto s = best ? service.stream(ids[0], codec == "bestopus", false, false) : service.soundtrackStream(ids[0], codec, true);
                std::printf("  %s: %s, %lld bytes\n", ids[0].c_str(), s.mimeType.c_str(), static_cast<long long>(s.contentLength));
                const auto t = st::video::decodeSoundtrack({s.url, L"", s.contentLength, s.mimeType});
                FILE* out = _wfopen(fs::path(ids[1]).c_str(), L"wb");
                if (out) {
                    fwrite(t.mono.data(), sizeof(float), t.mono.size(), out);
                    fclose(out);
                }
                std::printf("  %s: itag %d %s %d kbps, %.2f s at %d Hz\n", ids[0].c_str(), s.itag, s.codec.c_str(), s.bitrateKbps,
                            t.seconds(), t.sampleRate);
            } catch (const std::exception& e) {
                std::printf("  FAIL  %s: %s\n", ids[0].c_str(), e.what());
                ++g_failures;
            }
        } else if (mode == "officiallive" && ids.size() >= 3) {
            // dev: the official video of a song as the app finds it: <artist> <title> <the song's upload> [codec]
            const std::string codec = ids.size() > 3 ? ids[3] : "opus";
            st::youtube::OfficialQuery q{ids[1], {ids[0]}, 0, ""};
            const char* region = std::getenv("VIDEO_TEST_REGION");
            const auto cands = st::youtube::findOfficialVideos(service.client(), q, {ids[2]}, region ? region : "TR");
            std::printf("  %zu candidate(s)\n", cands.size());
            for (const auto& c : cands)
                std::printf("    %s %.0f%s \"%s\" (%s) %d s\n", c.videoId.c_str(), c.score, c.omv ? " OMV" : "", c.title.c_str(),
                            c.channel.c_str(), c.durationSec);
            if (!cands.empty()) {
                try {
                    const auto a = service.soundtrackStream(ids[2], codec, true);
                    const auto sa = st::video::decodeSoundtrack({a.url, L"", a.contentLength, a.mimeType});
                    for (size_t i = 0; i < cands.size() && i < 3; ++i) {
                        const auto b = service.soundtrackStream(cands[i].videoId, codec, true);
                        const auto sb = st::video::decodeSoundtrack({b.url, L"", b.contentLength, b.mimeType});
                        std::printf("  %s against %s (itag %d / %d):\n", cands[i].videoId.c_str(), ids[2].c_str(), a.itag, b.itag);
                        if (alignAndReport(sa.mono, sb.mono, sa.sampleRate).accepted) break;
                    }
                } catch (const std::exception& e) {
                    std::printf("  FAIL  %s\n", e.what());
                    ++g_failures;
                }
            }
        } else if (mode == "play") testLivePlay(service, ids.front());
        else testLive(service, ids);
    }
    st::gfx::Device::get().shutdown();
    MFShutdown();
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    st::log::shutdown();
    if (!g_failures) {
        std::error_code ec;
        fs::remove_all(work, ec);
    }
    return g_failures ? 1 : 0;
}
