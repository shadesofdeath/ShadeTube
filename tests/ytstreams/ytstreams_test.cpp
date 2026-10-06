// Developer test for audio streams end to end: MatchService (YoutubeExplode manifest, backup servers, SoundCloud) ->
// the engine's own downloader (audio::ProgressiveBuffer) -> the engine's decoder (Media Foundation), at the start and
// after a seek to the middle, plus the stream's tail (demuxers read it first).
//   ytstreams_test                 offline: SoundCloud parsing / matching rules
//   ytstreams_test live [<id>...]  + YouTube streams of the given (or default) videos, a refused URL recovered by a new
//                                    session, YouTube refusing everything (backup servers / SoundCloud take over) and
//                                    SoundCloud on its own for a few well-known tracks
//   ytstreams_test soak <id> <s>   resolve (and check) one video every 2 s for <s> seconds: how often YouTube refuses
// Prints the source of every stream (YouTube: the innertube client from googlevideo's `c=`). Runs in its own profile:
// SHADETUBE_DATA_DIR = <exe dir>\ytstreams-profile-<pid> (removed at the end).
#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Http.h"
#include "core/Log.h"
#include "youtube/MatchService.h"
#include "youtube/SoundCloud.h"

#include <YoutubeExplode/YoutubeClient.hpp>

#include <windows.h>

#include <mfapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace yte = YoutubeExplode;
namespace sc = st::youtube::soundcloud;

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

std::string clientOf(const std::string& url) {
    const auto p = url.find("&c=");
    if (p == std::string::npos) return "-";
    return url.substr(p + 3, url.find('&', p + 3) - p - 3);
}

st::catalog::Track track(std::string artist, std::string title, int durationMs) {
    st::catalog::Track t;
    t.id = "test:" + artist + ":" + title;
    t.name = std::move(title);
    t.artists = {{"", std::move(artist)}};
    t.durationMs = durationMs;
    return t;
}

// ~0.5 s decoded at `fraction` of the stream through the engine's decoder; -1 = could not open / seek.
double decodeAt(const std::shared_ptr<st::audio::ProgressiveBuffer>& buffer, const std::string& mime, double fraction) {
    st::audio::Decoder dec;
    bool unsupported = false;
    if (FAILED(dec.open(buffer, mime, unsupported)) || !dec.channels()) return -1;
    const int64_t at = dec.durationHns() > 0 ? static_cast<int64_t>(fraction * static_cast<double>(dec.durationHns())) : 600000000;   // 60 s
    if (fraction > 0 && FAILED(dec.seek(at))) return -1;
    std::vector<float> pcm;
    const size_t want = static_cast<size_t>(dec.sampleRate()) * dec.channels() / 2;
    for (int i = 0; i < 400 && pcm.size() < want; ++i) {
        int64_t ts = 0;
        bool changed = false;
        HRESULT hr = S_OK;
        if (dec.read(pcm, ts, changed, hr) != st::audio::Decoder::Status::Ok) break;
    }
    const double s = double(pcm.size()) / dec.channels() / dec.sampleRate();
    dec.close();
    return s;
}

struct Played {
    bool tail = false;
    double head = -1, middle = -1;
    std::string error;
    bool ok() const { return tail && head > 0.2 && middle > 0.2; }
};

Played play(const st::youtube::StreamInfo& s) {
    Played p;
    auto buffer = std::make_shared<st::audio::ProgressiveBuffer>(s.url, std::wstring{}, s.contentLength, nullptr);
    buffer->start();
    const int64_t length = buffer->waitForLength();
    if (length <= 0) {
        p.error = buffer->errorMessage();
        return p;
    }
    std::vector<char> tail(4096);
    p.tail = buffer->read(length - 4096, tail.data(), tail.size()) == st::audio::ProgressiveBuffer::ReadStatus::Ok;
    p.head = decodeAt(buffer, s.mimeType, 0);
    p.middle = decodeAt(buffer, s.mimeType, 0.5);   // a seek: the downloader restarts there
    if (!p.ok()) p.error = buffer->errorMessage();
    buffer->cancel();
    buffer->release();
    return p;
}

void report(const char* what, const st::youtube::StreamInfo& s, const Played& p) {
    std::printf("  %-26s %-24s %-9s %-10s itag %-3d  head %.1fs  middle %.1fs  tail %s  %s\n", what, s.source.substr(0, 24).c_str(),
                clientOf(s.url).c_str(), s.mimeType.c_str(), s.itag, p.head, p.middle, p.tail ? "ok" : "-", p.error.c_str());
    std::fflush(stdout);
}

// ---- offline ----------------------------------------------------------------------------------------------------

void testSoundCloudParsing(st::youtube::MatchService& service) {
    std::printf("[SoundCloud parsing]\n");
    const std::string html = R"(<script crossorigin src="https://a-v2.sndcdn.com/assets/0-abc.js"></script>
<script crossorigin src="https://a-v2.sndcdn.com/assets/49-def.js"></script><script src="https://evil.example/x.js"></script>)";
    const auto scripts = sc::scriptUrls(html);
    CHECK(scripts.size() == 2 && scripts.back() == "https://a-v2.sndcdn.com/assets/49-def.js");
    CHECK(sc::parseClientId(R"(x={env:"prod",client_id:"F4NBFDqtGCLXXsV1EvDyDg27XXRYDdKh",y:1})") == "F4NBFDqtGCLXXsV1EvDyDg27XXRYDdKh");
    CHECK(sc::parseClientId(R"(a.client_id = "short")") == std::nullopt);
    CHECK(sc::parseClientId(R"(my_client_id:"F4NBFDqtGCLXXsV1EvDyDg27XXRYDdKh")") == std::nullopt);

    const std::string search = R"js({"collection":[
      {"kind":"track","id":1,"title":"Never Gonna Give You Up","duration":30000,"policy":"SNIP","user":{"username":"Rick Astley"},
       "media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/1/a/stream/progressive","snipped":true,
                                  "format":{"protocol":"progressive","mime_type":"audio/mpeg"}}]}},
      {"kind":"track","id":2,"title":"Rick Astley - Never Gonna Give You Up (slowed + reverb)","duration":213000,"policy":"ALLOW",
       "user":{"username":"lofi"},"media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/2/b/stream/progressive",
       "snipped":false,"format":{"protocol":"progressive","mime_type":"audio/mpeg"}}]}},
      {"kind":"track","id":3,"title":"Never Gonna Give You Up","duration":212500,"policy":"ALLOW","track_authorization":"tok",
       "permalink_url":"https://soundcloud.com/x/y","user":{"username":"Rick Astley Fans"},
       "media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/3/c/stream/hls","snipped":false,
                                  "format":{"protocol":"hls","mime_type":"audio/mpeg"}},
                                 {"url":"https://api-v2.soundcloud.com/media/3/c/stream/progressive","snipped":false,
                                  "format":{"protocol":"progressive","mime_type":"audio/mpeg"}}]}},
      {"kind":"track","id":4,"title":"Never Gonna Give You Up","duration":190000,"policy":"ALLOW","user":{"username":"Rick Astley"},
       "media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/4/d/stream/progressive","snipped":false,
                                  "format":{"protocol":"progressive","mime_type":"audio/mpeg"}}]}},
      {"kind":"playlist","id":5,"title":"x"}]})js";
    const auto items = sc::parseSearch(search);
    CHECK(items.size() == 4);
    CHECK(items.size() == 4 && items[0].progressiveUrl.empty());   // snipped preview
    CHECK(items.size() == 4 && items[2].progressiveUrl.ends_with("/progressive") && items[2].trackAuthorization == "tok");
    const yte::Music::TrackMatcher ranker(service.client());
    const auto rick = track("Rick Astley", "Never Gonna Give You Up", 213573);
    const auto* best = sc::pick(items, rick, ranker);
    CHECK(best && best->id == 3);   // not the preview, not "slowed + reverb", not 23 s short
    auto noDuration = rick;
    noDuration.durationMs = 0;
    CHECK(sc::pick(items, noDuration, ranker) == nullptr);
    CHECK(sc::pick(items, track("Daft Punk", "Around the World", 213573), ranker) == nullptr);
    // A medley that contains the song (found for "Shape of You" on 2026-10-06) is not the song; a year is no extra word.
    auto medley = items;
    medley[2].title = "Ed Sheeran & Sia - Shape of You / The Greatest / Cheap Thrills";
    medley[2].durationMs = 233712;
    CHECK(sc::pick(medley, track("Ed Sheeran", "Shape of You", 233712), ranker) == nullptr);
    medley[2].title = "Ed Sheeran - Shape of You (Official Audio 2017)";
    CHECK(sc::pick(medley, track("Ed Sheeran", "Shape of You", 233712), ranker) == &medley[2]);
    bool threw = false;
    try {
        sc::parseSearch("<html>");
    } catch (const st::youtube::alt::AltSourceError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- live ---------------------------------------------------------------------------------------------------------

void testYoutube(st::youtube::MatchService& service, const std::vector<std::string>& ids) {
    std::printf("\n[YouTube streams: engine download + decode at 0 %% and 50 %%]\n");
    for (const bool webm : {true, false}) {
        for (const auto& id : ids) {
            st::youtube::StreamInfo s;
            Played p;
            try {
                s = service.stream(id, webm, false, true);
                p = play(s);
            } catch (const std::exception& e) {
                p.error = e.what();
            }
            report((id + (webm ? " webm" : " mp4")).c_str(), s, p);
            CHECK(s.source == "youtube" && p.ok());
        }
    }
}

// The first URL check answers 403 (a refused session): MatchService must start a new session and resolve again.
void testRefusedOnce(st::youtube::MatchService& service, const std::string& id) {
    std::printf("\n[a refused URL: new YouTube session, resolved again]\n");
    // A session renewed in the last 20 s is not renewed again (see MatchService): give an earlier real one time.
    Sleep(21000);
    std::atomic<int> checks{0};
    service.setUrlCheck([&](const std::string&, int64_t) { return checks++ == 0 ? 403 : 206; });
    service.forgetStream(id);   // a first play (a re-resolve right after a play counts as an engine failure)
    st::youtube::StreamInfo s;
    Played p;
    try {
        s = service.stream(id, true, false, false);
        p = play(s);
    } catch (const std::exception& e) {
        p.error = e.what();
    }
    service.setUrlCheck({});
    report(id.c_str(), s, p);
    CHECK(checks == 2 && s.source == "youtube" && p.ok());
}

// YouTube refuses every URL: with the backup source on, the backup servers or SoundCloud must take over.
void testYoutubeRefused(st::youtube::MatchService& service) {
    std::printf("\n[YouTube refuses everything: backup servers, then SoundCloud]\n");
    service.setUrlCheck([](const std::string&, int64_t) { return 403; });
    service.setAltSource("invidious", "");
    service.setSoundCloud(true);
    int served = 0;
    const std::vector<st::catalog::Track> tracks = {
        track("Rick Astley", "Never Gonna Give You Up", 213573),
        track("Luis Fonsi", "Despacito", 229360),
        track("The Weeknd", "Blinding Lights", 200040),
    };
    for (const auto& t : tracks) {
        st::youtube::Resolved r;
        Played p;
        const auto t0 = std::chrono::steady_clock::now();
        auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        try {
            r = service.resolveForPlayback(t, true, false, false);
            std::printf("      resolved in %.1f s\n", seconds());
            p = play(r.stream);
        } catch (const std::exception& e) {
            p.error = e.what();
            std::printf("      gave up after %.1f s\n", seconds());
        }
        report(t.name.c_str(), r.stream, p);
        CHECK(r.stream.source != "youtube");
        if (p.ok()) ++served;
    }
    service.setUrlCheck({});
    service.setAltSource("off", "");
    std::printf("  %d of %zu played from a fallback\n", served, tracks.size());
    CHECK(served > 0);
}

void testSoundCloud(st::youtube::MatchService& service) {
    std::printf("\n[SoundCloud on its own]\n");
    sc::SoundCloud source;
    const yte::Music::TrackMatcher ranker(service.client());
    const std::vector<st::catalog::Track> tracks = {
        track("Rick Astley", "Never Gonna Give You Up", 213573),
        track("Luis Fonsi", "Despacito", 229360),
        track("The Weeknd", "Blinding Lights", 200040),
        track("Ed Sheeran", "Shape of You", 233712),
        track("Tarkan", "Şımarık", 234466),
        track("Queen", "Bohemian Rhapsody - Remastered 2011", 354320),
    };
    int played = 0;
    for (const auto& t : tracks) {
        st::youtube::StreamInfo s;
        Played p;
        std::string match;
        try {
            const auto served = source.find(t, ranker);
            s.url = served.stream.url;
            s.contentLength = served.stream.contentLength;
            s.mimeType = served.stream.mimeType;
            s.source = "soundcloud";
            match = served.track.title + " | " + served.track.user;
            p = play(s);
        } catch (const std::exception& e) {
            p.error = e.what();
        }
        report(t.name.c_str(), s, p);
        if (!match.empty()) std::printf("      matched: %s\n", match.c_str());
        if (!match.empty()) CHECK(p.ok());   // whatever SoundCloud matched must play and seek
        if (p.ok()) ++played;
    }
    std::printf("  %d of %zu tracks found and played on SoundCloud\n", played, tracks.size());
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path profile;
    if (!GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr, 0)) {
        profile = fs::path(exe).parent_path() / (L"ytstreams-profile-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(profile);
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", profile.c_str());
    }
    st::log::init();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    {
        yte::YoutubeClientOptions o;
        o.httpClient = st::http::client();
        o.language = "tr";
        o.region = "TR";
        st::youtube::MatchService service{yte::YoutubeClient(o)};
        service.setAltSource("off", "");

        testSoundCloudParsing(service);
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "soak" && argc > 3) {
            // Diagnostics: resolve (and check) the same video every 2 s and count what YouTube refused.
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(argv[3]));
            int ok = 0, failed = 0;
            for (int i = 0; std::chrono::steady_clock::now() < end; ++i) {
                service.forgetStream(argv[2]);
                try {
                    const auto s = service.stream(argv[2], i % 2 == 0, false, false);
                    ++ok;
                    if (i % 5 == 0) {
                        const Played p = play(s);
                        report("soak play", s, p);
                    }
                } catch (const std::exception& e) {
                    ++failed;
                    std::printf("  %d: %s\n", i, e.what());
                }
                Sleep(2000);
            }
            std::printf("  soak: %d resolved, %d failed\n", ok, failed);
        }
        if (mode == "live") {
            std::vector<std::string> ids;
            for (int i = 2; i < argc; ++i) ids.emplace_back(argv[i]);
            if (ids.empty()) ids = {"NxSSSK_WdQE", "nxaDRwSa3H8", "X6gf7-oS5xU", "vlZXJp0wkE8", "dQw4w9WgXcQ"};
            testYoutube(service, ids);
            testRefusedOnce(service, ids.front());
            {
                // No cached streams: every URL goes through the (refusing) check.
                st::youtube::MatchService fresh{yte::YoutubeClient(o)};
                testYoutubeRefused(fresh);
            }
            testSoundCloud(service);
        }
    }

    MFShutdown();
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    st::log::shutdown();
    if (!profile.empty() && !g_failures) {   // kept after a failure: its log tells what YouTube answered
        std::error_code ec;
        fs::remove_all(profile, ec);
    }
    return g_failures ? 1 : 0;
}
