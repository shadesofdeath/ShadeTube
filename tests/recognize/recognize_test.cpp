// recognize_test: song recognition ("ÅarkÄ±yÄ± tanÄ±", src/recognize).
//
//   offline (default) : base64 / CRC-32 vectors; the signature of a synthetic signal with known peaks (seven tone pings:
//                       exactly one peak each, in the right band, at the right frequency and time; silence has none);
//                       encode / decode round trips (the 0xff pass escape, padding), the header fields, a damaged byte
//                       refused; the synthetic signal's signature against its stored fixture; recorded service answers
//                       (a match, no match, odd shapes); the session on test audio with a fake service (silence, the
//                       checkpoint schedule, a match at the second request, 429, a network failure, stop() with a
//                       request out)
//   live <file> [start s] [seconds] : a local audio file decoded by the app's own decoder, 16 kHz, its signature sent
//   yt "<Artist - Title>" [start s] [seconds] [--wav <out.wav>] : the song's YouTube audio (MatchService), decoded,
//                       sent; --wav also writes that excerpt (native rate, 16-bit) for a playback test
//   noise             : white noise, a quiet hum and silence sent to the service: no match expected
//   dump <file|"Artist - Title"> <out.json> [start s] : the raw answer, for fixtures
//   capture <system|mic> [seconds] : a real session on the device (WASAPI) against the service; prints its events
//   level <system|mic> [seconds] : the capture alone, its level every 250 ms (nothing is sent)
//   songs [--short] [--snr <dB>] "<Artist - Title>"... : each song's 11 s excerpt from the middle (--short: also 4 s and
//                       7 s; --snr: white noise mixed in at that signal-to-noise ratio)
#include "app/Recognition.h"
#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/Resampler.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "recognize/Capture.h"
#include "recognize/Session.h"
#include "recognize/Shazam.h"
#include "recognize/Signature.h"
#include "youtube/MatchService.h"

#include <YoutubeExplode/YoutubeClient.hpp>
#include <nlohmann/json.hpp>
#include <mfapi.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::recognize;
namespace fs = std::filesystem;
namespace yte = YoutubeExplode;

static int g_failures = 0;
static bool g_short = false;   // live: also 4 s and 7 s excerpts
static double g_snr = 1e9;     // live: white noise mixed in at this SNR (dB; --snr)
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static fs::path fixtures() { return fs::path(ST_RECOGNIZE_FIXTURES); }

// ---- synthetic audio ------------------------------------------------------------------------------------------------

struct Ping {
    double at;   // center, seconds
    double hz;
};
const Ping kPings[] = {{0.6, 400}, {1.4, 1000}, {2.2, 2000}, {3.0, 4000}, {3.8, 700}, {4.6, 3000}, {5.4, 1500}};

// Tone pings with a 0.3 s Hann envelope (a single loudest moment each), 0.8 s apart, on silence.
static std::vector<float> pings(double seconds = 6.2) {
    std::vector<float> a(static_cast<size_t>(seconds * kSampleRate), 0.f);
    for (const auto& p : kPings) {
        const double len = 0.3;
        const size_t from = static_cast<size_t>((p.at - len / 2) * kSampleRate), n = static_cast<size_t>(len * kSampleRate);
        for (size_t i = 0; i < n && from + i < a.size(); ++i) {
            const double env = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n);
            a[from + i] += static_cast<float>(0.3 * env * std::sin(2 * std::numbers::pi * p.hz * (from + i) / kSampleRate));
        }
    }
    return a;
}

static std::vector<float> noise(double seconds, float amp, uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-amp, amp);
    std::vector<float> a(static_cast<size_t>(seconds * kSampleRate));
    for (auto& s : a) s = d(rng);
    return a;
}

// ---- offline ----------------------------------------------------------------------------------------------------------

static void testHelpers() {
    std::printf("helpers\n");
    auto b64 = [](const char* s) { return base64(std::span(reinterpret_cast<const uint8_t*>(s), std::strlen(s))); };
    CHECK(b64("").empty());
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("foobar") == "Zm9vYmFy");
    const char* nine = "123456789";
    CHECK(crc32(std::span(reinterpret_cast<const uint8_t*>(nine), 9)) == 0xCBF43926u);
    CHECK(bandOf(249.9) == -1 && bandOf(250) == 0 && bandOf(519) == 0 && bandOf(520) == 1 && bandOf(1449) == 1 &&
          bandOf(1450) == 2 && bandOf(3499) == 2 && bandOf(3500) == 3 && bandOf(5500) == 3 && bandOf(5501) == -1);
    const std::string u = randomUuid();
    CHECK(u.size() == 36 && u[8] == '-' && u[13] == '-' && u[14] == '4' && u[18] == '-' && u[23] == '-');
    CHECK(std::none_of(u.begin(), u.end(), [](char c) { return c >= 'a' && c <= 'z'; }));
    CHECK(randomUuid() != u);
}

static void testSynthetic() {
    std::printf("synthetic signal\n");
    const auto a = pings();
    const double t0 = nowMs();
    const Signature s = makeSignature(a.data(), a.size());
    std::printf("  %zu peaks from %.1f s in %.1f ms\n", s.peakCount(), s.seconds(), nowMs() - t0);
    CHECK(s.samples == a.size());
    CHECK(s.peakCount() == std::size(kPings));
    for (const auto& p : kPings) {
        const int band = bandOf(p.hz);
        bool found = false;
        for (const auto& k : s.bands[static_cast<size_t>(band)]) {
            // A pass is the window ending at (pass + 1) * 128 samples: its center is 1024 samples (64 ms) earlier.
            const double center = ((k.pass + 1) * 128.0 - 1024) / kSampleRate;
            if (std::abs(k.hz() - p.hz) < 4 && std::abs(center - p.at) < 0.02) found = true;
        }
        if (!found) {
            std::printf("  FAIL no peak for %.0f Hz at %.2f s; band %d has:", p.hz, p.at, band);
            for (const auto& k : s.bands[static_cast<size_t>(band)])
                std::printf(" (%.1f Hz, %.3f s, mag %u)", k.hz(), ((k.pass + 1) * 128.0 - 1024) / kSampleRate, k.magnitude);
            std::printf("\n");
            ++g_failures;
        }
    }
    // Louder = a larger magnitude; the same signal 6 dB down keeps its peaks where they are.
    std::vector<float> quiet = a;
    for (auto& v : quiet) v *= 0.5f;
    const Signature q = makeSignature(quiet.data(), quiet.size());
    CHECK(q.peakCount() == s.peakCount());
    if (q.peakCount() == s.peakCount())
        for (int b = 0; b < kBands; ++b)
            for (size_t i = 0; i < s.bands[b].size(); ++i) {
                CHECK(q.bands[b][i].pass == s.bands[b][i].pass && q.bands[b][i].bin == s.bands[b][i].bin);
                // ln(0.25) * 1477.3 = -2048: power is the square of the amplitude.
                CHECK(std::abs((s.bands[b][i].magnitude - q.bands[b][i].magnitude) - 2048) <= 2);
            }
    // Fed in odd pieces = fed at once.
    SignatureGenerator g;
    for (size_t i = 0; i < a.size();) {
        const size_t n = std::min<size_t>(a.size() - i, 1 + (i * 7919) % 333);
        g.feed(a.data() + i, n);
        i += n;
    }
    const Signature pieces = g.signature();
    CHECK(pieces.bands == s.bands && pieces.samples == s.samples);
    // Nothing in silence; a reset starts over.
    std::vector<float> silence(static_cast<size_t>(5 * kSampleRate), 0.f);
    CHECK(makeSignature(silence.data(), silence.size()).peakCount() == 0);
    g.reset();
    g.feed(silence);
    CHECK(g.signature().peakCount() == 0 && g.samplesFed() == silence.size());
    // Noise has peaks everywhere, in every band.
    const auto n = noise(5, 0.3f);
    const Signature ns = makeSignature(n.data(), n.size());
    std::printf("  white noise: %zu / %zu / %zu / %zu peaks\n", ns.bands[0].size(), ns.bands[1].size(), ns.bands[2].size(),
                ns.bands[3].size());
    for (int b = 0; b < kBands; ++b) CHECK(!ns.bands[b].empty());
    for (int b = 0; b < kBands; ++b)
        for (size_t i = 1; i < ns.bands[b].size(); ++i) CHECK(ns.bands[b][i].pass >= ns.bands[b][i - 1].pass);

    // The stored fixture: the same signal, the same bytes (an algorithm change shows here first).
    const std::string uri = s.dataUri();
    const fs::path fx = fixtures() / "synthetic.uri";
    if (fs::exists(fx)) {
        std::string want = readFile(fx);
        while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
        CHECK(uri == want);
        if (uri != want) std::printf("  synthetic signature differs from the fixture (%zu vs %zu chars)\n", uri.size(), want.size());
    } else {
        std::ofstream(fx, std::ios::binary) << uri << "\n";
        std::printf("  wrote %s\n", fx.string().c_str());
    }
}

static void testEncoding() {
    std::printf("encoding\n");
    const auto a = noise(4, 0.2f, 11);
    const Signature s = makeSignature(a.data(), a.size());
    const auto bytes = s.encode();
    CHECK(bytes.size() % 4 == 0);
    auto u32 = [&](size_t at) {
        return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
    };
    CHECK(u32(0) == 0xcafe2580u);
    CHECK(u32(4) == crc32(std::span(bytes).subspan(8)));
    CHECK(u32(8) == bytes.size() - 48);
    CHECK(u32(12) == 0x94119c00u);
    CHECK(u32(16) == 0 && u32(20) == 0 && u32(24) == 0);
    CHECK(u32(28) == (3u << 27));
    CHECK(u32(40) == s.samples + 3840);
    CHECK(u32(44) == 0x7c0000u);
    CHECK(u32(48) == 0x40000000u && u32(52) == bytes.size() - 48);
    CHECK(u32(56) == 0x60030040u);   // the first band with peaks: 250-520 Hz
    const auto back = Signature::decode(bytes);
    CHECK(back && back->bands == s.bands && back->samples == s.samples && back->sampleRate == 16000);
    auto bad = bytes;
    bad[bad.size() / 2] ^= 0x10;
    CHECK(!Signature::decode(bad));
    CHECK(!Signature::decode(std::span(bytes).first(40)));
    CHECK(s.dataUri().rfind("data:audio/vnd.shazam.sig;base64,", 0) == 0);

    // A gap of 255 passes or more: the 0xff escape with the absolute pass.
    Signature g;
    g.samples = 160000;
    g.bands[1] = {{3, 7000, 7000}, {3, 7100, 7200}, {300, 6500, 9000}, {301, 6600, 9100}, {900, 6000, 11000}};
    g.bands[3] = {{1000, 5000, 30000}};
    const auto gb = g.encode();
    const auto gd = Signature::decode(gb);
    CHECK(gd && gd->bands == g.bands);
    // band 1: 5 peaks * 5 bytes + 2 escapes * 5 bytes = 35 bytes, padded to 36.
    CHECK(u32(0) == 0xcafe2580u);
    const size_t band1 = 56;
    CHECK((uint32_t(gb[band1]) | uint32_t(gb[band1 + 1]) << 8) == 0x0041);
    CHECK((uint32_t(gb[band1 + 4]) | uint32_t(gb[band1 + 5]) << 8) == 35);
    CHECK(gb[band1 + 8] == 3);                         // first delta
    CHECK(gb[band1 + 8 + 10] == 0xff);                 // 300 - 3 >= 255: escape
    CHECK(gb.size() == 48 + 8 + 8 + 36 + 8 + 12);   // band 3: an escape + one peak, padded
    Signature empty;
    const auto eb = empty.encode();
    CHECK(eb.size() == 56 && Signature::decode(eb) && Signature::decode(eb)->peakCount() == 0);
}

static void testAnswers() {
    std::printf("answers\n");
    const fs::path fx = fixtures();
    {
        const auto a = parseAnswer(readFile(fx / "answer_match.json"));
        CHECK(a.song.has_value());
        if (a.song) {
            const auto& s = *a.song;
            std::printf("  match: \"%s\" by \"%s\" (%s, %s, %s) isrc %s offset %.2f\n", s.title.c_str(), s.artist.c_str(),
                        s.album.c_str(), s.year.c_str(), s.genre.c_str(), s.isrc.c_str(), s.offsetSec);
            CHECK(!s.key.empty());
            CHECK(s.title == "Bohemian Rhapsody");
            CHECK(s.artist == "Queen");
            CHECK(!s.album.empty());
            CHECK(!s.isrc.empty());
            CHECK(s.coverUrl.rfind("https://", 0) == 0);
            CHECK(s.shazamUrl.rfind("https://www.shazam.com/", 0) == 0);
            CHECK(s.offsetSec > 0);
        }
        CHECK(!a.tagId.empty());
    }
    {
        const auto a = parseAnswer(readFile(fx / "answer_nomatch.json"));
        CHECK(!a.song);
        CHECK(!a.tagId.empty());
    }
    // Shapes that must not throw or invent a song.
    CHECK(!parseAnswer(R"({"matches":[],"track":{"title":"x"}})").song);
    CHECK(!parseAnswer(R"({"matches":[{"id":"1"}]})").song);
    CHECK(!parseAnswer(R"({"matches":[{"id":"1"}],"track":{"subtitle":"y"}})").song);
    CHECK(!parseAnswer(R"({"matches":"no","track":[]})").song);
    {
        const auto a = parseAnswer(
            R"({"matches":[{"id":"9","offset":"x","timeskew":1e-4}],"track":{"key":42,"title":"T","subtitle":"A","images":{"coverart":"https://c/1.jpg"},"sections":[{"type":"SONG","metadata":[{"title":"Album","text":"Al"},{"title":"Released","text":"1999"},{"title":7}]},{"type":"LYRICS"}]}, "retryms": 3000})");
        CHECK(a.song && a.song->key == "42" && a.song->title == "T" && a.song->artist == "A" && a.song->album == "Al" &&
              a.song->year == "1999" && a.song->coverUrl == "https://c/1.jpg" && a.song->offsetSec == 0 &&
              a.song->shazamUrl == "https://www.shazam.com/track/42");
        CHECK(a.retryMs == 3000);
    }
    bool threw = false;
    try {
        parseAnswer("<html>");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    // The request.
    const auto aud = pings();
    const Signature s = makeSignature(aud.data(), aud.size());
    const auto body = nlohmann::json::parse(buildBody(s, 1700000000123));
    CHECK(body["signature"]["uri"] == s.dataUri());
    CHECK(body["signature"]["samplems"] == static_cast<int64_t>(s.seconds() * 1000));
    CHECK(body["timestamp"] == 1700000000123);
    CHECK(body["geolocation"].is_object() && body["geolocation"].empty());
    const std::string url = buildUrl("A", "B");
    CHECK(url.rfind("https://amp.shazam.com/discovery/v5/en-US/US/iphone/-/tag/A/B?", 0) == 0);
}

// The session on test audio with a fake service.
struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Event> events;
    void operator()(const Event& e) {
        std::lock_guard l(m);
        events.push_back(e);
        cv.notify_all();
    }
    bool waitFinal(int ms) {
        std::unique_lock l(m);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return !events.empty() && events.back().final(); });
    }
    Event last() {
        std::lock_guard l(m);
        return events.empty() ? Event{} : events.back();
    }
};

static void testSession() {
    std::printf("session\n");
    auto tone = std::make_shared<std::vector<float>>(pings(13));
    auto silence = std::make_shared<std::vector<float>>(static_cast<size_t>(13 * kSampleRate), 0.f);
    auto run = [](std::shared_ptr<const std::vector<float>> audio, auto service, double speed, int waitMs, Options o = {}) {
        o.testAudio = std::move(audio);
        o.testSpeed = speed;
        o.service = service;
        auto rec = std::make_shared<Recorder>();
        auto s = std::make_unique<Session>(o, [rec](const Event& e) { (*rec)(e); });
        const double t0 = nowMs();
        const bool done = rec->waitFinal(waitMs);
        const double took = nowMs() - t0;
        s.reset();
        return std::tuple{done, rec->last(), took, rec};
    };
    std::atomic<int> calls{0};
    std::vector<double> seen;
    std::mutex seenM;
    auto none = [&](const Signature& sig, const yte::CancellationToken&) {
        ++calls;
        std::lock_guard l(seenM);
        seen.push_back(sig.seconds());
        return Answer{};
    };
    {   // Silence: Silent after the window, no request.
        Options o;
        o.silenceWindowSec = 1;
        calls = 0;
        auto [done, e, took, rec] = run(silence, none, 4, 5000, o);
        CHECK(done && e.phase == Phase::Silent && e.requests == 0 && calls == 0);
        CHECK(took < 2000);
        std::printf("  silence: %s after %.0f ms\n", phaseName(e.phase), took);
    }
    {   // No song: requests at 4, 7 and 11 s of audio, then NotFound.
        calls = 0;
        seen.clear();
        auto [done, e, took, rec] = run(tone, none, 8, 8000);
        CHECK(done && e.phase == Phase::NotFound && e.requests == 3 && calls == 3);
        std::lock_guard l(seenM);
        CHECK(seen.size() == 3);
        if (seen.size() == 3) {
            std::printf("  not found: signatures of %.2f / %.2f / %.2f s, %.0f ms\n", seen[0], seen[1], seen[2], took);
            CHECK(seen[0] >= 4 && seen[0] < 6.5);
            CHECK(seen[1] >= 7 && seen[1] < 9.5);
            CHECK(seen[2] >= 11 && seen[2] <= 12);
        }
        int identifying = 0;
        for (const auto& ev : rec->events) identifying += ev.phase == Phase::Identifying;
        CHECK(identifying == 3 && rec->events.front().phase == Phase::Listening);
    }
    {   // A song at the second request.
        calls = 0;
        auto second = [&](const Signature&, const yte::CancellationToken&) {
            Answer a;
            if (++calls == 2) {
                Song s;
                s.title = "T";
                s.artist = "A";
                a.song = s;
            }
            return a;
        };
        auto [done, e, took, rec] = run(tone, second, 8, 8000);
        CHECK(done && e.phase == Phase::Matched && e.requests == 2 && e.song && e.song->title == "T");
    }
    {   // An unsure first answer (skew 0.0012): the longer signature confirms; a different later answer wins.
        auto song = [](const char* title, double skew) {
            Answer a;
            Song s;
            s.title = title;
            s.artist = "A";
            s.timeSkew = skew;
            a.song = s;
            return a;
        };
        calls = 0;
        auto later = [&](const Signature&, const yte::CancellationToken&) {
            return ++calls == 1 ? song("Copy", 0.0012) : song("Original", 0.0009);
        };
        auto [d1, e1, t1, r1] = run(tone, later, 8, 8000);
        CHECK(d1 && e1.phase == Phase::Matched && e1.requests == 2 && e1.song && e1.song->title == "Original");
        bool confirming = false;
        for (const auto& ev : r1->events) confirming |= ev.confirming && ev.phase == Phase::Listening;
        CHECK(confirming);
        // Unsure, then nothing: the first answer stands after the last checkpoint.
        calls = 0;
        auto lone = [&](const Signature&, const yte::CancellationToken&) { return ++calls == 1 ? song("Only", -0.002) : Answer{}; };
        auto [d2, e2, t2, r2] = run(tone, lone, 8, 8000);
        CHECK(d2 && e2.phase == Phase::Matched && e2.requests == 3 && e2.song && e2.song->title == "Only");
        // A sure first answer (|skew| <= 0.0005) is taken at once.
        calls = 0;
        auto sure = [&](const Signature&, const yte::CancellationToken&) {
            ++calls;
            return song("Sure", -0.0004);
        };
        auto [d3, e3, t3, r3] = run(tone, sure, 8, 8000);
        CHECK(d3 && e3.phase == Phase::Matched && e3.requests == 1 && calls == 1);
    }
    {   // 429 and no network.
        auto busy = [](const Signature&, const yte::CancellationToken&) -> Answer { throw ServiceError(429, "HTTP 429"); };
        auto [d1, e1, t1, r1] = run(tone, busy, 8, 8000);
        CHECK(d1 && e1.phase == Phase::Failed && e1.failure == Failure::RateLimited && e1.status == 429);
        auto offline = [](const Signature&, const yte::CancellationToken&) -> Answer { throw ServiceError(0, "no network"); };
        auto [d2, e2, t2, r2] = run(tone, offline, 8, 8000);
        CHECK(d2 && e2.phase == Phase::Failed && e2.failure == Failure::Network);
    }
    {   // stop() with a request out: returns at once, no event after it.
        std::atomic<bool> entered{false};
        auto slow = [&](const Signature&, const yte::CancellationToken& ct) {
            entered = true;
            for (int i = 0; i < 300 && !ct.isCancellationRequested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return Answer{};
        };
        Options o;
        o.testAudio = tone;
        o.testSpeed = 8;
        o.service = slow;
        auto rec = std::make_shared<Recorder>();
        auto s = std::make_unique<Session>(o, [rec](const Event& e) { (*rec)(e); });
        for (int i = 0; i < 200 && !entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(entered.load());
        size_t before = 0;
        {
            std::lock_guard l(rec->m);
            before = rec->events.size();
        }
        const double t0 = nowMs();
        s->stop();
        s.reset();
        const double took = nowMs() - t0;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard l(rec->m);
        CHECK(rec->events.size() == before);
        CHECK(took < 200);
        std::printf("  stop with a request out: %.0f ms\n", took);
    }
}

// ---- app/Recognition: the song -> a catalog track, the history ----------------------------------------------------------

static catalog::Track track(const char* id, const char* name, std::vector<const char*> artists) {
    catalog::Track t;
    t.id = id;
    t.name = name;
    for (const char* a : artists) t.artists.push_back({"", a});
    return t;
}

static void testRecognition(const fs::path& work) {
    namespace rc = app::recognition;
    std::printf("catalog matching\n");
    CHECK(rc::foldTitle("Şımarık") == "simarik");
    CHECK(rc::foldTitle("Bohemian Rhapsody - Remastered 2011") == "bohemianrhapsody");
    CHECK(rc::foldTitle("Levitating (feat. DaBaby)") == "levitating");
    CHECK(rc::foldTitle("Gülümse") == rc::foldTitle("GULUMSE"));
    CHECK(rc::foldArtist("Beyoncé") == "beyonce");
    CHECK((rc::splitArtists("Dua Lipa feat. DaBaby") == std::vector<std::string>{"Dua Lipa", "DaBaby"}));
    CHECK((rc::splitArtists("Simon & Garfunkel") == std::vector<std::string>{"Simon", "Garfunkel"}));
    CHECK((rc::splitArtists("A, B & C") == std::vector<std::string>{"A", "B", "C"}));
    CHECK((rc::splitArtists("Tarkan") == std::vector<std::string>{"Tarkan"}));
    CHECK((rc::splitArtists("Calvin Harris FEAT. Rihanna") == std::vector<std::string>{"Calvin Harris", "Rihanna"}));

    Song s;
    s.title = "Şımarık";
    s.artist = "Tarkan";
    const std::vector<catalog::Track> hits = {
        track("1", "Şımarık (Remix)", {"Some DJ"}),            // another artist
        track("2", "Kuzu Kuzu", {"Tarkan"}),                    // another song
        track("3", "Simarik - Live", {"TARKAN"}),               // the song, live: a fallback
        track("4", "Şımarık", {"Tarkan"}),                      // the song
    };
    CHECK(rc::bestMatch(hits, s) == 3);
    CHECK(rc::bestMatch({hits[0], hits[1], hits[2]}, s) == 2);
    CHECK(rc::bestMatch({hits[0], hits[1]}, s) == -1);
    catalog::Track unplayable = hits[3];
    unplayable.playable = false;
    CHECK(rc::bestMatch({unplayable}, s) == -1);
    // Several credited artists: any of them is enough; the whole credit as one name too.
    Song duo;
    duo.title = "Under Pressure";
    duo.artist = "Queen & David Bowie";
    CHECK(rc::bestMatch({track("a", "Under Pressure", {"David Bowie"})}, duo) == 0);
    CHECK(rc::bestMatch({track("a", "Under Pressure", {"Queen & David Bowie"})}, duo) == 0);
    CHECK(rc::bestMatch({track("a", "Under Pressure", {"Vanilla Ice"})}, duo) == -1);
    Song band;
    band.title = "September";
    band.artist = "Earth, Wind & Fire";
    CHECK(rc::bestMatch({track("e", "September", {"Earth, Wind & Fire"})}, band) == 0);
    // The query and the stand-in track.
    Song feat;
    feat.title = "Levitating (feat. DaBaby)";
    feat.artist = "Dua Lipa feat. DaBaby";
    feat.album = "Future Nostalgia";
    feat.coverUrl = "https://example.invalid/c.jpg";
    CHECK(rc::searchQuery(feat) == "Dua Lipa Levitating");
    const catalog::Track fb = rc::fallbackTrack(feat);
    CHECK(fb.id.rfind("import:", 0) == 0 && fb.name == feat.title && fb.artists.size() == 2 && fb.artists[0].name == "Dua Lipa");
    CHECK(fb.album.name == "Future Nostalgia" && fb.album.images.size() == 1 && fb.album.images[0].url == feat.coverUrl);
    CHECK(rc::fallbackTrack(feat).id == fb.id);   // stable

    std::printf("history store\n");
    const fs::path file = work / L"recognized.json";
    std::error_code ec;
    fs::remove(file, ec);
    {
        rc::History h;
        h.load(file);   // missing: empty
        CHECK(h.entries().empty() && h.source() == Source::System);
        for (int i = 0; i < 25; ++i) {
            rc::Entry e;
            e.at = 1000 + i;
            e.song.title = "Song " + std::to_string(i);
            e.song.artist = "Artist";
            e.song.offsetSec = 12;
            h.add(e);
        }
        CHECK(h.entries().size() == rc::kHistoryMax && h.entries().front().song.title == "Song 24");
        CHECK(h.entries().front().song.offsetSec == 0);   // not kept
        rc::Entry again;
        again.at = 5000;
        again.song.title = "SONG 20";   // the same song, another spelling: moves to the top, once
        again.song.artist = "artist";
        h.add(again);
        CHECK(h.entries().size() == rc::kHistoryMax && h.entries().front().at == 5000);
        CHECK(std::count_if(h.entries().begin(), h.entries().end(), [](const rc::Entry& e) {
                  return rc::foldTitle(e.song.title) == "song20";
              }) == 1);
        h.setTrack(again.song, track("spotify:track:x", "Song 20", {"Artist"}));
        h.setSource(Source::Microphone);
        CHECK(h.save());
    }
    {
        rc::History h;
        h.load(file);
        CHECK(h.entries().size() == rc::kHistoryMax);
        CHECK(h.source() == Source::Microphone);
        CHECK(!h.entries().empty() && h.entries().front().track.id == "spotify:track:x" && h.entries().front().at == 5000);
        CHECK(h.entries().size() > 1 && h.entries()[1].track.id.empty());
        h.clear();
        CHECK(h.save());
    }
    {
        rc::History h;
        h.load(file);
        CHECK(h.entries().empty() && h.source() == Source::Microphone);
    }
    std::ofstream(file, std::ios::binary) << "{ not json";
    rc::History broken;
    broken.load(file);
    CHECK(broken.entries().empty());
    std::ofstream(file, std::ios::binary) << R"({"songs":[{"song":{"title":""}},{"song":7},{"song":{"title":"T","artist":"A"},"at":3}]})";
    rc::History odd;
    odd.load(file);
    CHECK(odd.entries().size() == 1 && odd.entries()[0].song.title == "T" && odd.entries()[0].at == 3);
}

// ---- live --------------------------------------------------------------------------------------------------------------

struct Decoded {
    uint32_t rate = 0, channels = 0;
    std::vector<float> interleaved;   // the whole stream
    double seconds() const { return rate && channels ? static_cast<double>(interleaved.size()) / channels / rate : 0; }
};

// The app's own decoder on a URL or a local file, at the stream's own rate.
static Decoded decode(const std::string& url, const std::wstring& path, int64_t length, const std::string& mime) {
    auto buffer = std::make_shared<audio::ProgressiveBuffer>(url, path, length, nullptr);
    buffer->start();
    Decoded d;
    if (buffer->waitForLength() <= 0) {
        std::printf("  FAIL download: %s\n", buffer->errorMessage().c_str());
        ++g_failures;
        return d;
    }
    audio::Decoder dec;
    bool unsupported = false;
    HRESULT hr = dec.open(buffer, mime, unsupported, audio::RatePolicy{});
    if (FAILED(hr)) {
        std::printf("  FAIL decoder: 0x%08X\n", static_cast<unsigned>(hr));
        ++g_failures;
        return d;
    }
    for (;;) {
        int64_t ts = -1;
        bool changed = false;
        const auto st = dec.read(d.interleaved, ts, changed, hr);
        if (st != audio::Decoder::Status::Ok) break;
    }
    d.rate = dec.sampleRate();
    d.channels = dec.channels();
    buffer->cancel();
    buffer->release();
    return d;
}

// [start, start + seconds) of a decoded stream as 16 kHz mono (the Capture path's resampler).
static std::vector<float> excerpt16k(const Decoded& d, double start, double seconds) {
    const size_t ch = std::max<uint32_t>(1, d.channels);
    const size_t frames = d.interleaved.size() / ch;
    const size_t from = std::min(frames, static_cast<size_t>(start * d.rate));
    const size_t to = std::min(frames, from + static_cast<size_t>(seconds * d.rate));
    std::vector<float> mono(to - from);
    for (size_t i = from; i < to; ++i) {
        float s = 0;
        for (size_t c = 0; c < ch; ++c) s += d.interleaved[i * ch + c];
        mono[i - from] = s / static_cast<float>(ch);
    }
    audio::Resampler r;
    r.configure(d.rate, kSampleRate, 1);
    std::vector<float> out;
    r.process(mono.data(), mono.size(), out);
    r.flush(out);
    return out;
}

static void writeWav(const fs::path& file, const Decoded& d, double start, double seconds) {
    const size_t ch = std::max<uint32_t>(1, d.channels);
    const size_t frames = d.interleaved.size() / ch;
    const size_t from = std::min(frames, static_cast<size_t>(start * d.rate));
    const size_t to = std::min(frames, from + static_cast<size_t>(seconds * d.rate));
    std::vector<int16_t> pcm((to - from) * ch);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(std::clamp(d.interleaved[from * ch + i] * 32767.f, -32768.f, 32767.f));
    std::ofstream f(file, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4);
    w32(36 + bytes);
    f.write("WAVEfmt ", 8);
    w32(16);
    w16(1);
    w16(static_cast<uint16_t>(ch));
    w32(d.rate);
    w32(d.rate * static_cast<uint32_t>(ch) * 2);
    w16(static_cast<uint16_t>(ch * 2));
    w16(16);
    f.write("data", 4);
    w32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    std::printf("  wrote %s (%.1f s)\n", file.string().c_str(), (to - from) / static_cast<double>(d.rate));
}

// The service refuses bursts (HTTP 429 after a few requests within seconds, for a minute or so): live requests keep
// this many seconds apart (RECOGNIZE_GAP, default 20).
static void spaceRequests() {
    static double last = 0;
    char v[16] = {};
    const double gap = GetEnvironmentVariableA("RECOGNIZE_GAP", v, sizeof v) ? std::atof(v) : 20.0;
    const double wait = last > 0 ? last + gap * 1000 - nowMs() : 0;
    if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(wait)));
    last = nowMs();
}

static bool printAnswer(const char* what, const Signature& sig, const std::string& dumpTo = {}) {
    spaceRequests();
    const double t0 = nowMs();
    try {
        Answer a;
        if (!dumpTo.empty()) {
            http::HttpRequest r;
            r.method = "POST";
            r.url = buildUrl(randomUuid(), randomUuid());
            r.body = buildBody(sig, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::system_clock::now().time_since_epoch())
                                                            .count()));
            r.headers = {{"Content-Type", "application/json"}, {"Accept", "*/*"}, {"Accept-Language", "en-US"},
                         {"X-Shazam-Platform", "IPHONE"}, {"X-Shazam-AppVersion", "14.1.0"},
                         {"User-Agent", "Dalvik/2.1.0 (Linux; U; Android 6.0.1; SM-G920F Build/MMB29K)"}};
            const auto res = http::send(r);
            std::ofstream(dumpTo, std::ios::binary) << res.body;
            std::printf("  HTTP %d, %zu bytes -> %s\n", res.statusCode, res.body.size(), dumpTo.c_str());
            a = parseAnswer(res.body);
        } else {
            a = identify(sig);
        }
        const double took = nowMs() - t0;
        if (a.song) {
            const auto& s = *a.song;
            std::printf("  %s: MATCH \"%s\" - \"%s\" | album \"%s\" %s | %s | isrc %s | offset %.1f s, skew %.5f/%.5f | %zu peaks, %.1f s | %.0f ms\n",
                        what, s.artist.c_str(), s.title.c_str(), s.album.c_str(), s.year.c_str(), s.genre.c_str(), s.isrc.c_str(),
                        s.offsetSec, s.timeSkew, s.frequencySkew, sig.peakCount(), sig.seconds(), took);
            return true;
        }
        std::printf("  %s: no match (retryms %d) | %zu peaks, %.1f s | %.0f ms\n", what, a.retryMs, sig.peakCount(), sig.seconds(), took);
    } catch (const ServiceError& e) {
        std::printf("  %s: FAIL service: %s (status %d)\n", what, e.what(), e.status);
        ++g_failures;
    } catch (const std::exception& e) {
        std::printf("  %s: FAIL %s\n", what, e.what());
        ++g_failures;
    }
    return false;
}

static void liveDecoded(const std::string& label, const Decoded& d, double start, double seconds, const std::string& dumpTo,
                        const fs::path& wav) {
    if (d.interleaved.empty()) return;
    if (start < 0) start = std::max(0.0, d.seconds() / 2 - seconds / 2);
    std::printf("  %s: %.1f s at %u Hz x %u; excerpt %.1f..%.1f s\n", label.c_str(), d.seconds(), d.rate, d.channels, start,
                start + seconds);
    auto pcm = excerpt16k(d, start, seconds);
    if (g_snr < 1e8) {   // a noisy room: white noise at the given signal-to-noise ratio
        double power = 0;
        for (float v : pcm) power += double(v) * v;
        power /= std::max<size_t>(1, pcm.size());
        const float amp = static_cast<float>(std::sqrt(3 * power / std::pow(10.0, g_snr / 10)));   // uniform: power amp^2 / 3
        const auto n = noise(pcm.size() / double(kSampleRate) + 1, amp, 99);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] += n[i];
        std::printf("  noise mixed in at %.0f dB SNR\n", g_snr);
    }
    const Signature sig = makeSignature(pcm.data(), pcm.size());
    printAnswer(label.c_str(), sig, dumpTo);
    // Shorter excerpts (--short): how soon a match comes.
    if (dumpTo.empty() && g_short)
        for (double s : {4.0, 7.0}) {
            if (s >= seconds) continue;
            const auto part = excerpt16k(d, start, s);
            const Signature ps = makeSignature(part.data(), part.size());
            printAnswer((label + " (" + std::to_string(static_cast<int>(s)) + " s)").c_str(), ps);
        }
    if (!wav.empty()) writeWav(wav, d, start, seconds);
}

static Decoded fetchSong(const std::string& query) {
    yte::YoutubeClientOptions o;
    o.httpClient = http::client();
    youtube::MatchService service{yte::YoutubeClient(o)};
    service.setAltSource("off", "");
    catalog::Track t;
    const auto dash = query.find(" - ");
    t.artists.push_back({"", dash == std::string::npos ? "" : query.substr(0, dash)});
    t.name = dash == std::string::npos ? query : query.substr(dash + 3);
    t.id = "recognize-test:" + query;
    try {
        const auto r = service.resolve(t, true, false);
        std::printf("  %s -> video %s \"%s\" (%s, %d s)\n", query.c_str(), r.match.videoId.c_str(), r.match.title.c_str(),
                    r.stream.mimeType.c_str(), r.match.durationSec);
        return decode(r.stream.url, L"", r.stream.contentLength, r.stream.mimeType);
    } catch (const std::exception& e) {
        std::printf("  FAIL resolve %s: %s\n", query.c_str(), e.what());
        ++g_failures;
        return {};
    }
}

static void liveNoise() {
    std::printf("no-match checks\n");
    const auto n = noise(10, 0.3f);
    printAnswer("white noise", makeSignature(n.data(), n.size()));
    std::vector<float> hum(static_cast<size_t>(10 * kSampleRate));
    for (size_t i = 0; i < hum.size(); ++i)
        hum[i] = static_cast<float>(0.05 * std::sin(2 * std::numbers::pi * 300 * i / kSampleRate) +
                                    0.02 * std::sin(2 * std::numbers::pi * 900 * i / kSampleRate));
    printAnswer("hum", makeSignature(hum.data(), hum.size()));
    std::vector<float> silence(static_cast<size_t>(10 * kSampleRate), 0.f);
    printAnswer("silence", makeSignature(silence.data(), silence.size()));
    const auto p = pings(10);
    printAnswer("pings", makeSignature(p.data(), p.size()));
}

static void liveCapture(Source source, double seconds) {
    std::printf("capture (%s, up to %.0f s)\n", source == Source::System ? "system sound" : "microphone", seconds);
    Options o;
    o.source = source;
    auto rec = std::make_shared<Recorder>();
    const double t0 = nowMs();
    auto s = std::make_unique<Session>(o, [rec, t0](const Event& e) {
        std::printf("  %6.0f ms  %s  heard %.1f s, requests %d%s%s\n", nowMs() - t0, phaseName(e.phase), e.heardSec, e.requests,
                    e.ownSoundIncluded ? ", own sound included" : "", e.song ? (" -> " + e.song->artist + " - " + e.song->title + " (skew " + std::to_string(e.song->timeSkew) + " / " + std::to_string(e.song->frequencySkew) + ", offset " + std::to_string(e.song->offsetSec) + ")").c_str() : "");
        (*rec)(e);
    });
    const bool done = rec->waitFinal(static_cast<int>(seconds * 1000));
    std::printf("  level at the end %.3f\n", s->level());
    s.reset();
    if (!done) std::printf("  (no final event within %.0f s)\n", seconds);
    const auto e = rec->last();
    if (e.phase == Phase::Failed) std::printf("  failure %d status %d\n", static_cast<int>(e.failure), e.status);
}

// The capture alone (nothing is sent): opens the device, prints the level every 250 ms and what arrived.
static void liveLevel(Source source, double seconds) {
    std::printf("level (%s, %.0f s, nothing sent)\n", source == Source::System ? "system sound" : "microphone", seconds);
    Capture c;
    const double t0 = nowMs();
    if (!c.start(source)) {
        std::printf("  not opened: %s (%.0f ms)\n", captureErrorName(c.error()), nowMs() - t0);
        return;
    }
    std::printf("  opened in %.0f ms%s\n", nowMs() - t0, c.ownSoundIncluded() ? " (ShadeTube's own sound included)" : "");
    std::vector<float> got;
    float peak = 0;
    while (nowMs() - t0 < seconds * 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const size_t before = got.size();
        c.take(got);
        float p = 0;
        for (size_t i = before; i < got.size(); ++i) p = std::max(p, std::abs(got[i]));
        peak = std::max(peak, p);
        std::printf("  %5.0f ms  +%5zu samples  peak %.4f  level %.3f  error %s%s\n", nowMs() - t0, got.size() - before, p, c.level(),
                    captureErrorName(c.error()), Capture::outputHeldExclusively() ? "  (output held exclusively)" : "");
    }
    c.stop();
    std::printf("  %.2f s of 16 kHz audio in %.2f s, peak %.4f, reopened %d\n", got.size() / 16000.0, (nowMs() - t0) / 1000, peak,
                c.reopened());
}

int wmain(int, wchar_t**) {
    SetConsoleOutputCP(CP_UTF8);
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(toUtf8(wargv[i]));
    LocalFree(wargv);
    const std::string mode = args.empty() ? "offline" : args[0];
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    if (mode == "offline") {
        testHelpers();
        testSynthetic();
        testEncoding();
        testAnswers();
        testSession();
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        const fs::path work = fs::path(tmp) / (L"recognize_test_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(work);
        testRecognition(work);
        std::error_code ec;
        fs::remove_all(work, ec);
    } else if (mode == "live" && args.size() >= 2) {
        const double start = args.size() > 2 ? std::atof(args[2].c_str()) : -1;
        const double secs = args.size() > 3 ? std::atof(args[3].c_str()) : 11;
        const fs::path p = fs::path(toWide(args[1]));
        const auto ext = p.extension().wstring();
        const std::string mime = ext == L".mp3" ? "audio/mpeg" : ext == L".webm" ? "audio/webm" : ext == L".ogg" || ext == L".opus" ? "audio/ogg"
                                 : ext == L".flac" ? "audio/flac" : ext == L".wav" ? "audio/wav" : "audio/mp4";
        liveDecoded(p.filename().string(), decode("", p.wstring(), 0, mime), start, secs, {}, {});
    } else if (mode == "yt" && args.size() >= 2) {
        double start = -1, secs = 11;
        fs::path wav;
        for (size_t i = 2, k = 0; i < args.size(); ++i) {
            if (args[i] == "--wav" && i + 1 < args.size()) wav = fs::path(toWide(args[++i]));
            else if (k++ == 0) start = std::atof(args[i].c_str());
            else secs = std::atof(args[i].c_str());
        }
        liveDecoded(args[1], fetchSong(args[1]), start, secs, {}, wav);
    } else if (mode == "songs") {   // several queries, an 11 s excerpt from the middle of each
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--short") {
                g_short = true;
                continue;
            }
            if (args[i] == "--snr" && i + 1 < args.size()) {
                g_snr = std::atof(args[++i].c_str());
                continue;
            }
            liveDecoded(args[i], fetchSong(args[i]), -1, 11, {}, {});
        }
    } else if (mode == "noise") {
        liveNoise();
    } else if (mode == "dump" && args.size() >= 3) {
        const double start = args.size() > 3 ? std::atof(args[3].c_str()) : -1;
        if (fs::exists(fs::path(toWide(args[1])))) {
            const fs::path p = fs::path(toWide(args[1]));
            liveDecoded(p.filename().string(), decode("", p.wstring(), 0, p.extension() == ".mp3" ? "audio/mpeg" : "audio/mp4"), start,
                        11, args[2], {});
        } else if (args[1] == "noise") {
            const auto n = noise(10, 0.3f);
            printAnswer("noise", makeSignature(n.data(), n.size()), args[2]);
        } else {
            liveDecoded(args[1], fetchSong(args[1]), start, 11, args[2], {});
        }
    } else if (mode == "level") {
        const Source src = args.size() > 1 && args[1] == "mic" ? Source::Microphone : Source::System;
        liveLevel(src, args.size() > 2 ? std::atof(args[2].c_str()) : 3);
    } else if (mode == "capture") {
        const Source src = args.size() > 1 && args[1] == "mic" ? Source::Microphone : Source::System;
        liveCapture(src, args.size() > 2 ? std::atof(args[2].c_str()) : 20);
    } else {
        std::printf("usage: recognize_test [offline | live <file> [start] [seconds] | yt \"Artist - Title\" [start] [seconds] "
                    "[--wav out.wav] | songs [--short] \"Artist - Title\"... | noise | dump <file|query|noise> <out.json> [start] | "
                    "capture <system|mic> [seconds] | level <system|mic> [seconds]]\n");
        return 2;
    }
    MFShutdown();
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
