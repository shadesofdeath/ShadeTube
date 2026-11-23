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
#include "audio/AudioEngine.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Http.h"
#include "core/Log.h"
#include "gfx/Device.h"
#include "video/Fmp4Index.h"
#include "video/SyncScheduler.h"
#include "video/VideoDecoder.h"
#include "video/VideoPlayer.h"
#include "youtube/MatchService.h"
#include "youtube/VideoStreams.h"

#include <YoutubeExplode/YoutubeClient.hpp>

#include <windows.h>

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

    testSelection();
    testStatic();
    testFmp4Index();
    testScheduler();
    const fs::path file = work / L"test.mp4";
    if (writeTestVideo(file)) {
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
    if (mode == "live" || mode == "play" || mode == "fetch" || mode == "file" || mode == "mfurl") {
        yte::YoutubeClientOptions o;
        o.httpClient = st::http::client();
        st::youtube::MatchService service{yte::YoutubeClient(o)};
        service.setAltSource("off", "");
        std::vector<std::string> ids;
        for (int i = 2; i < argc; ++i) ids.emplace_back(argv[i]);
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
