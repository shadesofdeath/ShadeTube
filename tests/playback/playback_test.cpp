// Developer test for the playback features (offline, no network):
//   1. Kara liste (app/Blacklist.cpp, compiled in): store round trip through a temporary blacklist.json, the track
//      rules (id; another release: title + first artist with durations within 3 s, or an unknown duration across
//      catalogs; a different "Intro" of the same artist stays unblocked), the artist rules (id; the same name across
//      catalogs or for an id-less credit; two Spotify artists sharing a name stay apart), unblocking through another
//      id, remove(entry), revision + change listeners, wrong-typed fields and a corrupt file (kept as .bad).
//   2. player::Player::localMimeType for every local-file extension.
//   3. Media Foundation decode (st_audio Decoder, the engine's MFSTARTUP_LITE) of local files with the mapped type: a
//      generated WAV, plus every audio file in <dir> when given (e.g. a folder of FLAC / M4A / AAC / MP3 / OGG samples).
//   4. A real player::Player (AudioEngine + WASAPI at volume 0) on generated 1.2 s WAVs served through localFileFor (no
//      network): auto-advance / next / previous / header play pass over skipped (blocked) items while an explicit pick
//      still plays one, onQueueLow fires as the queue runs low / out, extend() continues a queue that had ended,
//      replaceUpcoming() keeps the current track, firstPlayable(), a missing local file steps on after 1.5 s and a
//      queue whose only playable track fails stops instead of looping, crossfade (Settings::crossfadeSec) hands over
//      that long before the end while an album playing in order stays gapless, the session saves a window around a
//      current track past the 500th.
//   playback_test [<audio dir>]
// Runs in a temporary profile (SHADETUBE_DATA_DIR) and needs an audio output device for part 4.
#include "app/Blacklist.h"
#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Dispatcher.h"
#include "player/Player.h"
#include "youtube/MatchService.h"

#include <windows.h>

#include <mfapi.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace bl = st::app::blacklist;
using st::catalog::ArtistRef;
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

Track track(std::string id, std::string name, std::vector<ArtistRef> artists, int durationMs = 0) {
    Track t;
    t.id = std::move(id);
    t.name = std::move(name);
    t.artists = std::move(artists);
    t.durationMs = durationMs;
    return t;
}

const ArtistRef kFonsi{"spotify:artist:4V8Sr092TqfHkfAA5fXXqG", "Luis Fonsi"};
const ArtistRef kDaddy{"spotify:artist:4VMYDCV2IEDYJArk749S6m", "Daddy Yankee"};
const ArtistRef kFonsiMb{"d68ab5a8-1c34-4c3d-9f55-1fa3e19a1b6b", "Luis Fonsi"};
const ArtistRef kNirvanaUs{"spotify:artist:6olE6TJLqED3rqDCT0FyPh", "Nirvana"};
const ArtistRef kNirvanaUk{"spotify:artist:0g4Kq4qzYh6LzlNQ6FNBF2", "Nirvana"};
const ArtistRef kRapper{"spotify:artist:1111111111111111111111", "MC Test"};

void writeText(const fs::path& file, const std::string& text) {
    std::ofstream f(file, std::ios::trunc | std::ios::binary);
    f << text;
}

void testBlacklist(const fs::path& dir) {
    std::printf("kara liste\n");
    const fs::path file = dir / L"blacklist.json";
    const fs::path bad = fs::path(file).concat(L".bad");
    bl::setStoreFile(file);
    CHECK(bl::empty());
    int notified = 0;
    bl::onChanged([&] { ++notified; });
    const uint64_t rev0 = bl::revision();

    // Tracks: by id; another release of the same recording by title + first artist (same duration within 3 s, or a
    // duration unknown and another catalog).
    const Track despacito = track("spotify:track:6habFhsOp2NvshLv26DqMb", "Despacito", {kFonsi, kDaddy}, 229360);
    bl::setTrackBlocked(despacito, true);
    CHECK(notified == 1);
    CHECK(bl::revision() > rev0);
    CHECK(bl::isBlocked(despacito));
    CHECK(bl::isTrackBlocked(despacito.id));
    CHECK(bl::blockedTracks().size() == 1);
    CHECK(bl::blockedTracks()[0].artists == "Luis Fonsi, Daddy Yankee");
    CHECK(bl::blockedTracks()[0].durationMs == 229360);
    CHECK(bl::isBlocked(track("spotify:track:0000000000000000000001", "DESPACİTO ", {{"", "luis fonsi"}}, 230100)));   // single
    CHECK(!bl::isBlocked(track("spotify:track:0000000000000000000007", "Despacito", {kFonsi}, 281000)));   // another version
    CHECK(!bl::isBlocked(track("spotify:track:0000000000000000000008", "Despacito", {kFonsi}, 0)));        // same catalog, ?
    CHECK(bl::isBlocked(track("5f1a9d42-0000-0000-0000-000000000000", "Despacito", {kFonsiMb}, 0)));       // MusicBrainz
    CHECK(bl::isBlocked(track("local:abc", "Despacito", {{"", "Luis Fonsi"}}, 0)));                        // local file
    CHECK(bl::isBlocked(track("local:abd", "Despacito", {{"", "Luis Fonsi"}}, 228000)));
    CHECK(!bl::isBlocked(track("local:abe", "Despacito", {{"", "Luis Fonsi"}}, 200000)));
    CHECK(!bl::isBlocked(track("spotify:track:0000000000000000000002", "Despacito - Remix", {kFonsi}, 229360)));
    CHECK(!bl::isBlocked(track("spotify:track:0000000000000000000003", "Despacito", {{"", "Pedro Capó"}}, 229360)));
    CHECK(!bl::isTrackBlocked("spotify:track:0000000000000000000001"));   // id-only query
    CHECK(!bl::isBlocked(track("", "", {})));
    bl::setTrackBlocked(despacito, true);   // already blocked: no duplicate, no change
    CHECK(bl::blockedTracks().size() == 1);
    CHECK(notified == 1);

    // A generic title shared by different songs of one artist: only that "Intro" (and its other releases) is blocked.
    bl::setTrackBlocked(track("spotify:track:A000000000000000000000", "Intro", {kRapper}, 60000), true);
    CHECK(!bl::isBlocked(track("spotify:track:B000000000000000000000", "Intro", {kRapper}, 95000)));
    CHECK(bl::isBlocked(track("spotify:track:C000000000000000000000", "intro", {kRapper}, 61000)));
    CHECK(bl::isBlocked(track("local:z", "Intro", {{"", "MC Test"}}, 0)));
    bl::setTrackBlocked(track("spotify:track:B000000000000000000000", "Intro", {kRapper}, 95000), true);   // a 2nd one
    CHECK(bl::blockedTracks().size() == 3);
    bl::setTrackBlocked(track("spotify:track:C000000000000000000000", "Intro", {kRapper}, 61000), false);  // unblock #1
    CHECK(bl::blockedTracks().size() == 2);
    CHECK(bl::isBlocked(track("spotify:track:B000000000000000000000", "Intro", {kRapper}, 95000)));        // #2 stays
    bl::remove(bl::blockedTracks()[0]);   // exactly that entry (the newest: the 2nd "Intro")
    CHECK(bl::blockedTracks().size() == 1 && bl::blockedTracks()[0].name == "Despacito");

    // Artists: by id; by name across catalogs / for id-less credits; same-name Spotify artists stay apart.
    bl::setArtistBlocked(kNirvanaUs.id, kNirvanaUs.name, true);
    CHECK(bl::isArtistBlocked(kNirvanaUs.id));
    CHECK(bl::artistMatches(kNirvanaUs));
    CHECK(!bl::artistMatches(kNirvanaUk));
    CHECK(bl::artistMatches({"5b11f4ce-a62d-471e-81fc-a69a8278c7da", "Nirvana"}));   // MusicBrainz credit
    CHECK(bl::artistMatches({"", "NİRVANA"}));                                        // plain-text credit
    CHECK(bl::isBlocked(track("spotify:track:0000000000000000000004", "Lithium", {kNirvanaUs})));
    CHECK(bl::isBlocked(track("spotify:track:0000000000000000000005", "Feat. song", {{"x", "Someone"}, kNirvanaUs})));
    CHECK(!bl::isBlocked(track("spotify:track:0000000000000000000006", "Lithium", {kNirvanaUk})));
    CHECK(!bl::isArtistBlocked(kNirvanaUk.id));

    // Round trip through the file.
    {
        std::ifstream f(file);
        const auto j = nlohmann::json::parse(f, nullptr, false);
        CHECK(j.is_object() && j["tracks"].size() == 1 && j["artists"].size() == 1);
        CHECK(j["tracks"][0].value("id", "") == despacito.id && j["tracks"][0].value("a0", "") == "Luis Fonsi");
        CHECK(j["tracks"][0].value("t", int64_t{0}) > 0 && j["tracks"][0].value("d", 0) == 229360);
        CHECK(j["artists"][0].value("n", "") == "Nirvana");
    }
    bl::setStoreFile(file);
    CHECK(bl::blockedTracks().size() == 1 && bl::blockedArtists().size() == 1);
    CHECK(bl::isBlocked(despacito));
    CHECK(bl::blockedTracks()[0].name == "Despacito" && bl::blockedTracks()[0].artist0 == "Luis Fonsi");
    CHECK(bl::isArtistBlocked(kNirvanaUs.id));
    CHECK(!fs::exists(fs::path(file).concat(L".tmp")));
    CHECK(!fs::exists(bad));

    // Unblock through another release (title + artist) and through the MusicBrainz id of the artist.
    bl::setTrackBlocked(track("local:xyz", "despacito", {{"", "Luis Fonsi"}}), false);
    CHECK(!bl::isBlocked(despacito));
    CHECK(bl::blockedTracks().empty());
    bl::setArtistBlocked("5b11f4ce-a62d-471e-81fc-a69a8278c7da", "Nirvana", false);
    CHECK(!bl::isArtistBlocked(kNirvanaUs.id));
    CHECK(bl::empty());
    const int before = notified;
    bl::setArtistBlocked(kNirvanaUk.id, kNirvanaUk.name, false);   // nothing to remove: no change
    CHECK(notified == before);

    // Newest first.
    bl::setArtistBlocked(kFonsi.id, kFonsi.name, true);
    bl::setArtistBlocked(kDaddy.id, kDaddy.name, true);
    CHECK(bl::blockedArtists().size() == 2 && bl::blockedArtists()[0].name == "Daddy Yankee");
    CHECK(bl::isBlocked(despacito));   // through its artists now

    // Wrong-typed fields never throw: they read as empty / 0; entries that still have an id or a name survive, and the
    // damaged file is kept as .bad.
    writeText(file, R"({"version":1,"tracks":[{"id":123,"n":"Keep me","a0":"X","t":"2026-09-28","d":"long"},{"id":null},7],)"
                    R"("artists":[{"id":"spotify:artist:2222222222222222222222","n":["x"],"t":1790000000}]})");
    bool threw = false;
    try {
        bl::setStoreFile(file);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(bl::blockedTracks().size() == 1 && bl::blockedTracks()[0].name == "Keep me" && bl::blockedTracks()[0].addedAt == 0);
    CHECK(bl::blockedArtists().size() == 1 && bl::blockedArtists()[0].addedAt == 1790000000);
    CHECK(fs::exists(bad));
    fs::remove(bad);
    writeText(file, R"({"tracks":"nope","artists":{}})");
    bl::setStoreFile(file);
    CHECK(bl::empty() && fs::exists(bad));
    fs::remove(bad);

    // A corrupt file starts empty and is kept aside before the next save could overwrite it.
    writeText(file, "{not json");
    bl::setStoreFile(file);
    CHECK(bl::empty());
    CHECK(fs::exists(bad) && fs::file_size(bad) == 9);
    bl::setArtistBlocked(kFonsi.id, kFonsi.name, true);   // the next save replaces blacklist.json, not the .bad copy
    CHECK(fs::file_size(bad) == 9);
    bl::setArtistBlocked(kFonsi.id, kFonsi.name, false);
}

void testMime() {
    std::printf("local mime types\n");
    using st::player::Player;
    CHECK(Player::localMimeType(L"C:\\Music\\a.mp3") == "audio/mpeg");
    CHECK(Player::localMimeType(L"C:\\Music\\a.MP3") == "audio/mpeg");
    CHECK(Player::localMimeType(L"C:\\Music\\a.m4a") == "audio/mp4");
    CHECK(Player::localMimeType(L"C:\\Music\\a.mp4") == "audio/mp4");
    CHECK(Player::localMimeType(L"C:\\Music\\a.aac") == "audio/aac");
    CHECK(Player::localMimeType(L"C:\\Music\\a.flac") == "audio/flac");
    CHECK(Player::localMimeType(L"C:\\Music\\a.Wav") == "audio/wav");
    CHECK(Player::localMimeType(L"C:\\Music\\a.wma") == "audio/x-ms-wma");
    CHECK(Player::localMimeType(L"C:\\Music\\a.ogg") == "audio/ogg");
    CHECK(Player::localMimeType(L"C:\\Music\\a.opus") == "audio/ogg");
    CHECK(Player::localMimeType(L"C:\\Music\\a.webm") == "audio/webm");
    CHECK(Player::localMimeType(L"C:\\Music\\a.mka") == "audio/x-matroska");
    CHECK(Player::localMimeType(L"C:\\Music\\a.xyz").empty());
    CHECK(Player::localMimeType(L"C:\\Mu.sic\\noext").empty());
    CHECK(Player::localMimeType(L"").empty());
}

void writeWav(const fs::path& path, uint32_t ms = 500) {
    constexpr uint32_t rate = 44100, channels = 2;
    const uint32_t frames = rate * ms / 1000;
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(8000 * std::sin(2 * 3.14159265358979 * 440 * i / rate));
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

// Opens `file` the way the engine does for a local source and decodes a few blocks.
bool decodes(const fs::path& file, std::string& info) {
    const std::string mime = st::player::Player::localMimeType(file.wstring());
    auto buffer = std::make_shared<st::audio::ProgressiveBuffer>(std::string{}, file.wstring(), 0, nullptr);
    buffer->start();
    char msg[160];
    if (buffer->waitForLength() <= 0) {   // the engine's Track waits the same way before opening the decoder
        info = "empty / unreadable file";
        buffer->cancel();
        return false;
    }
    st::audio::Decoder decoder;
    bool unsupported = false;
    const HRESULT hr = decoder.open(buffer, mime, unsupported);
    if (FAILED(hr)) {
        std::snprintf(msg, sizeof msg, "%s: open hr=0x%08X%s", mime.c_str(), static_cast<unsigned>(hr),
                      unsupported ? " (unsupported)" : "");
        info = msg;
        buffer->cancel();
        return false;
    }
    std::vector<float> pcm;
    for (int i = 0; i < 50 && pcm.size() < 8192; ++i) {
        int64_t ts = 0;
        bool changed = false;
        HRESULT rhr = S_OK;
        if (decoder.read(pcm, ts, changed, rhr) != st::audio::Decoder::Status::Ok) break;
    }
    std::snprintf(msg, sizeof msg, "%s: %u Hz, %u ch, %zu samples", mime.empty() ? "(sniffed)" : mime.c_str(),
                  decoder.sampleRate(), decoder.channels(), pcm.size());
    info = msg;
    decoder.close();
    buffer->cancel();
    return !pcm.empty();
}

void testDecode(const fs::path& tmp, const fs::path& samples) {
    std::printf("local decode\n");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const fs::path wav = tmp / L"sine.wav";
    writeWav(wav);
    std::string info;
    const bool ok = decodes(wav, info);
    std::printf("        %s\n", info.c_str());
    CHECK(ok);
    if (!samples.empty()) {
        // Per file (a sample folder may hold deliberately broken files), then per type: at least one file of every
        // type decodes. Ogg has no Media Foundation handler in a stock Windows: reported, not required.
        std::map<std::string, std::pair<int, int>> byType;   // mime -> (decoded, files)
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(samples, ec)) {
            if (!e.is_regular_file()) continue;
            const std::string mime = st::player::Player::localMimeType(e.path().wstring());
            if (mime.empty()) continue;
            const bool decoded = decodes(e.path(), info);
            auto& [okCount, total] = byType[mime];
            okCount += decoded ? 1 : 0;
            ++total;
            std::printf("        %s %-28ls %s\n", decoded ? "+" : "-", e.path().filename().c_str(), info.c_str());
        }
        for (const auto& [mime, counts] : byType) {
            std::printf("  %s  %-18s %d/%d decoded\n", counts.first > 0 ? "ok   " : mime == "audio/ogg" ? "skip " : "FAIL ",
                        mime.c_str(), counts.first, counts.second);
            if (counts.first == 0 && mime != "audio/ogg") ++g_failures;
        }
    }
    MFShutdown();
}

// Pumps the UI thread (Dispatcher posts, the player's WM_TIMER prefetch tick) until `done()` or the timeout.
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

void testPlayer(const fs::path& tmp) {
    std::printf("player: skips + endless hooks (local WAVs, volume 0)\n");
    using st::player::Status;
    st::Dispatcher::init();
    std::map<std::string, fs::path> files;
    for (const char* id : {"a", "b", "c", "d", "e"}) {
        files[id] = tmp / (std::string(id) + ".wav");
        writeWav(files[id], 1200);
    }
    files["x"] = tmp / L"missing.wav";   // a downloaded / local track whose file is gone
    auto T = [](const std::string& id) {
        Track t;
        t.id = id;
        t.name = "Track " + id;
        t.artists = {{"", "Tester"}};
        t.durationMs = 1200;
        return t;
    };
    st::youtube::MatchService matcher{YoutubeExplode::YoutubeClient()};
    {
        st::player::Player player(matcher);
        player.setVolume(0.f);
        player.localFileFor = [&](const std::string& id) {
            if (id.rfind("q", 0) == 0) return files["a"].wstring();   // the big session queue
            const auto it = files.find(id);
            return it == files.end() ? std::wstring{} : it->second.wstring();
        };
        const std::unordered_set<std::string> blocked{"b"};
        player.shouldSkip = [&](const Track& t) { return blocked.contains(t.id); };
        int low = 0;
        player.onQueueLow = [&] { ++low; };
        std::vector<std::string> started;
        player.onTrackChanged = [&](const Track& t) { started.push_back(t.id); };
        std::wstring error;
        player.onError = [&](const std::wstring& m) { error = m; };

        // Auto-advance passes over "b"; the queue runs out -> onQueueLow; extend() continues with the new tracks.
        player.playContext({T("a"), T("b"), T("c")}, 0, {"test", L"Test"});
        const uint64_t gen = player.queueGeneration();
        CHECK(pumpUntil([&] { return started.size() >= 2 && player.status() == Status::Idle; }, 20000));
        CHECK((started == std::vector<std::string>{"a", "c"}));
        CHECK(low >= 2);   // "a" started with one playable item left, then "c" (none left) / the end
        CHECK(player.remainingPlayable() == 0);
        CHECK(player.extend({T("b"), T("d"), T("e")}) == 3);
        CHECK(pumpUntil([&] { return started.size() >= 3; }, 5000));
        CHECK(started.size() >= 3 && started[2] == "d");   // the blocked "b" at the front of the new tracks is passed over
        CHECK(pumpUntil([&] { return started.size() >= 4 && player.status() == Status::Idle; }, 20000));
        CHECK(started.size() == 4 && started[3] == "e");
        CHECK(player.queueGeneration() == gen);   // extend() keeps the queue

        // Header play (no start index) starts on a track the player may pick; an explicit pick plays a blocked one.
        started.clear();
        player.playContext({T("b"), T("c")}, -1, {"test2", L"Test"});
        CHECK((started == std::vector<std::string>{"c"}));
        CHECK(player.queueGeneration() > gen);
        player.playContext({T("a"), T("b"), T("c")}, 1, {"test3", L"Test"});
        CHECK(started.back() == "b");
        player.next();
        CHECK(started.back() == "c");
        player.previous();   // just started: goes back, over "b"
        CHECK(started.back() == "a");
        CHECK(player.remainingPlayable() == 1);
        const size_t n = started.size();
        player.playContext({T("b")}, -1, {"test4", L"Test"});   // nothing the player may pick by itself
        CHECK(started.size() == n && !error.empty());

        // replaceUpcoming: the current track keeps playing, the rest of the queue is replaced.
        player.replaceUpcoming({T("d"), T("e")}, {"radio", L"Radyo"});
        CHECK(player.current() && player.current()->id == "a");
        CHECK(player.items().size() == 3 && player.context().uri == "radio");
        CHECK(started.size() == n);   // no track change

        // firstPlayable: the first track the player may pick by itself (header / card play that ignores shuffle).
        CHECK(player.firstPlayable({T("b"), T("c")}) == 1);
        CHECK(player.firstPlayable({T("b")}) == -1);

        // A missing file: an error, then (1.5 s later) the next track.
        started.clear();
        error.clear();
        player.playContext({T("x"), T("c")}, 0, {"test5", L"Test"});
        CHECK(pumpUntil([&] { return started.size() >= 2; }, 8000));
        CHECK((started == std::vector<std::string>{"x", "c"}));
        CHECK(error.rfind(L"Dosya okunamadı", 0) == 0);
        pumpUntil([&] { return player.status() == Status::Idle; }, 5000);
        // Its only playable track fails with repeat on: one stop and a message, never a loop back onto it.
        player.setRepeat(st::RepeatMode::All);
        started.clear();
        error.clear();
        player.playContext({T("x"), T("b")}, 0, {"test6", L"Test"});
        CHECK(pumpUntil([&] { return player.status() == Status::Idle; }, 8000));
        pumpUntil([] { return false; }, 2500);
        CHECK(started.size() == 1);
        CHECK(error == L"Çalınabilir şarkı bulunamadı");
        player.setRepeat(st::RepeatMode::Off);

        // Crossfade: the next track takes over 2 s before the end (5 s WAVs, preloaded from disk); two consecutive
        // tracks of one album stay gapless.
        {
            for (const char* id : {"f", "g"}) {
                files[id] = tmp / (std::string(id) + ".wav");
                writeWav(files[id], 5000);
            }
            auto T5 = [&](const std::string& id) {
                Track t = T(id);
                t.durationMs = 5000;
                return t;
            };
            std::vector<std::pair<std::string, DWORD>> changes;
            player.onTrackChanged = [&](const Track& t) { changes.push_back({t.id, GetTickCount()}); };
            auto handoffMs = [&](std::vector<Track> tracks) -> long {
                changes.clear();
                player.playContext(std::move(tracks), 0, {"xfade", L"Test"});
                if (!pumpUntil([&] { return changes.size() >= 2; }, 15000)) return -1;
                return static_cast<long>(changes[1].second - changes[0].second);
            };
            st::Settings::get().crossfadeSec = 2;
            const long fade = handoffMs({T5("f"), T5("g")});
            std::printf("        crossfade: g audible %ld ms after f started\n", fade);
            CHECK(fade > 2000 && fade < 4500);
            Track a = T5("f"), b = T5("g");
            a.album.id = b.album.id = "album-1";
            a.trackNumber = 1;
            b.trackNumber = 2;
            const long album = handoffMs({a, b});
            std::printf("        same album: g audible %ld ms after f started\n", album);
            CHECK(album > 4700);
            st::Settings::get().crossfadeSec = 0;
            player.onTrackChanged = [&](const Track& t) { started.push_back(t.id); };
            player.pause();
        }

        // Session: a window of the play order around a current track past the 500th.
        std::vector<Track> big;
        for (int i = 0; i < 700; ++i) big.push_back(T("q" + std::to_string(i)));
        player.playContext(big, 650, {"big", L"Big"});
        player.pause();
        player.saveSession();
        pumpUntil([] { return false; }, 300);
    }
    {
        st::player::Player restored(matcher);
        restored.restoreSession();
        CHECK(restored.current() && restored.current()->id == "q650");
        CHECK(restored.items().size() == 500 && restored.currentOrderIndex() == 450);   // slots 200..699
    }
    st::Dispatcher::shutdown();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path tmp = fs::temp_directory_path() / L"shadetube_playback_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (tmp / L"profile").c_str());   // never the user's profile
    testBlacklist(tmp);
    testMime();
    testDecode(tmp, argc > 1 ? fs::path(argv[1]) : fs::path{});
    testPlayer(tmp);
    fs::remove_all(tmp, ec);
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
