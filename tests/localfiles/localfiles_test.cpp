// Developer test for app/LocalLibrary ("Yerel dosyalar"), offline:
//   1. builds a music folder of COPIES in a work folder (never touches the originals): up to two MP3s from the user's
//      Music\ShadeTube downloads (real ID3 tags + APIC cover), generated WAV sines named "Artist - Title" /
//      "03 - Title" (file-name fallback), and - when ffmpeg is on PATH - tagged FLAC / M4A (AAC, ALAC) / ADTS AAC /
//      WMA / MP3 / Ogg / Opus files with an embedded cover;
//   2. scan: tags (property system), embedded covers extracted once, "local:<hash>" ids, sorting;
//   3. index round trip, incremental rescan (unchanged files are not re-read), a changed file is re-read, removed
//      files disappear, nested folders don't duplicate, the file cap, cancellation (before and while reading), cover
//      pruning, an unreachable folder keeps its tracks while a deleted one loses them, walk rules (junctions, hidden
//      and "." folders, unstorable names), file-name parsing, long paths, disc order, folder add / remove rules;
//   3b. files dropped from Explorer (app/DroppedFiles): collecting files + folders, the cap, order, the remembered
//      list, and the drop rules for sidebar rows and the queue;
//   4. formats: each file decoded through the engine's own path (ProgressiveBuffer -> MF byte stream -> Source
//      Reader) with the per-extension mime Player::localMimeType passes and with the old "audio/mp4" fallback (MF
//      sniffs the content); then ~1 s of real playback
//      of a local MP3 (+ FLAC / M4A / WAV / WMA) through the AudioEngine at volume 0.
//   localfiles_test [parent-folder] [--keep]   works in <parent>\ShadeTube-localfiles-test (default: %TEMP%), which it
//                                              deletes and recreates; any other folder is never touched
#include "app/DroppedFiles.h"
#include "app/LocalLibrary.h"
#include "audio/AudioEngine.h"
#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Settings.h"
#include "core/Utf.h"

#include <windows.h>

#include <mfapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace local = st::app::local;
using st::toUtf8;
using st::toWide;

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

void writeWav(const fs::path& p, double seconds, double freq) {
    constexpr uint32_t rate = 44100, ch = 2;
    const uint32_t frames = static_cast<uint32_t>(seconds * rate);
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * ch);
    for (uint32_t i = 0; i < frames; ++i) {
        const auto v = static_cast<int16_t>(std::sin(2 * 3.14159265358979 * freq * i / rate) * 9000);
        pcm[i * 2] = pcm[i * 2 + 1] = v;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(ch); u32(rate); u32(rate * ch * 2); u16(ch * 2); u16(16);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
}

std::wstring findOnPath(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr) ? std::wstring(buf) : std::wstring{};
}

bool run(const std::wstring& cmdLine) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Deletes the work folder: the junction first (never followed), the long-path tree through "\\?\".
void wipe(const fs::path& work) {
    std::error_code ec;
    RemoveDirectoryW((work / L"walk" / L"bağlantı").c_str());
    fs::remove_all(fs::path(L"\\\\?\\" + (work / L"uzun").wstring()), ec);
    fs::remove_all(work, ec);
}

const local::Entry* byName(const std::vector<local::Entry>& v, const std::wstring& fileName) {
    for (const auto& e : v)
        if (fs::path(e.path).filename().wstring() == fileName) return &e;
    return nullptr;
}

// The engine's decode path for a local file: ProgressiveBuffer (reads the file) -> MF byte stream -> Source Reader.
// Returns decoded seconds (up to ~1 s), -1 when it can't be opened.
double decodeSeconds(const std::wstring& path, const std::string& mime) {
    auto buffer = std::make_shared<st::audio::ProgressiveBuffer>(std::string{}, path, 0, nullptr);
    buffer->start();
    if (buffer->waitForLength() < 0) return -1;
    st::audio::Decoder dec;
    bool unsupported = false;
    if (FAILED(dec.open(buffer, mime, unsupported))) return -1;
    std::vector<float> pcm;
    for (int i = 0; i < 200 && dec.channels() && pcm.size() < static_cast<size_t>(dec.sampleRate()) * dec.channels(); ++i) {
        int64_t ts = 0;
        bool changed = false;
        HRESULT hr = S_OK;
        if (dec.read(pcm, ts, changed, hr) != st::audio::Decoder::Status::Ok) break;
    }
    const double s = dec.channels() && dec.sampleRate() ? double(pcm.size()) / dec.channels() / dec.sampleRate() : 0;
    dec.close();
    buffer->cancel();
    buffer->release();
    return s;
}

// The Player's former local-file mime (".mp3" -> audio/mpeg, everything else audio/mp4): still decodes, MF sniffs.
std::string fallbackMime(const std::wstring& path) {
    return path.size() > 4 && path.compare(path.size() - 4, 4, L".mp3") == 0 ? "audio/mpeg" : "audio/mp4";
}

// Same values as Player::localMimeType for the scanned extensions.
std::string extMime(const std::wstring& path) {
    static const std::map<std::wstring, std::string> m = {
        {L".mp3", "audio/mpeg"}, {L".m4a", "audio/mp4"},       {L".aac", "audio/aac"},  {L".flac", "audio/flac"},
        {L".wav", "audio/wav"},  {L".wma", "audio/x-ms-wma"},  {L".ogg", "audio/ogg"},  {L".opus", "audio/ogg"}};
    const auto it = m.find(fs::path(path).extension().wstring());
    return it == m.end() ? std::string{} : it->second;
}

// ~1 s of real playback (WASAPI, volume 0) of a local file through the AudioEngine with the Player's mime.
bool plays(const std::wstring& path, const std::string& mime) {
    std::atomic<int> state{-1};
    std::atomic<bool> error{false};
    st::audio::EngineEvents ev;
    ev.onState = [&](st::audio::State s, uint64_t) { state = static_cast<int>(s); };
    ev.onError = [&](st::audio::ErrorKind, const std::string& m, uint64_t) {
        std::printf("        engine error: %s\n", m.c_str());
        error = true;
    };
    st::audio::AudioEngine engine(std::move(ev));
    engine.setVolume(0.f);
    st::audio::StreamSource s;
    s.localPath = path;
    s.mimeType = mime;
    s.tag = 1;
    engine.open(s, 0, true);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until && !error) {
        if (engine.positionMs() >= 800) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int64_t pos = engine.positionMs(), dur = engine.durationMs();
    engine.stop();
    std::printf("        %-28s %-11s pos %lld ms, duration %lld ms\n", toUtf8(fs::path(path).filename().wstring()).c_str(),
                mime.c_str(), static_cast<long long>(pos), static_cast<long long>(dur));
    return !error && pos >= 800;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // The test only ever works in (and wipes) its OWN folder "ShadeTube-localfiles-test" inside the given parent,
    // and only when that folder carries the marker it wrote itself.
    fs::path parent = fs::temp_directory_path();
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--keep") keep = true;
        else parent = argv[i];
    }
    const fs::path work = parent / L"ShadeTube-localfiles-test";
    const fs::path marker = work / L".shadetube-localfiles-test";
    std::error_code ec;
    if (fs::exists(work, ec)) {
        if (!fs::exists(marker, ec)) {
            std::printf("%s exists but was not made by this test: not touching it\n", toUtf8(work.wstring()).c_str());
            return 2;
        }
        wipe(work);
    }
    fs::create_directories(work);
    std::ofstream(marker) << "localfiles_test work folder (deleted by the test)";
    const fs::path music = work / L"Müzik klasörü", sub = music / L"Alt klasör", covers = work / L"covers";
    fs::create_directories(sub);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (work / L"profile").c_str());   // logs etc. never reach the real profile
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    std::printf("work folder: %s\n", toUtf8(work.wstring()).c_str());
    std::printf("scanned extensions:");
    for (const auto& e : local::audioExtensions()) std::printf(" %s", toUtf8(e).c_str());
    std::printf("\n");

    // ---- 1. the test library (copies only) ------------------------------------------------------------
    std::printf("\n[setup]\n");
    int realMp3 = 0;
    {
        wchar_t* musicDir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &musicDir))) {
            const fs::path downloads = fs::path(musicDir) / L"ShadeTube";
            for (const auto& de : fs::recursive_directory_iterator(downloads, fs::directory_options::skip_permission_denied, ec)) {
                if (realMp3 >= 2) break;
                if (!de.is_regular_file(ec) || de.path().extension() != L".mp3") continue;
                const fs::path dst = (realMp3 == 0 ? music : sub) / de.path().filename();
                if (CopyFileW(de.path().c_str(), dst.c_str(), TRUE)) {   // read-only access to the original
                    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
                    ++realMp3;
                    std::printf("  copied %s\n", toUtf8(de.path().filename().wstring()).c_str());
                }
            }
        }
        CoTaskMemFree(musicDir);
    }
    writeWav(music / L"Test Sanatçı - Dalga.wav", 2.0, 440);
    writeWav(sub / L"03 - Sadece Başlık.wav", 1.5, 660);
    writeWav(music / L"._gizli.wav", 0.2, 440);   // macOS resource fork name: skipped
    std::ofstream(music / L"notlar.txt") << "not audio";
    std::ofstream(music / L"bos.mp3");            // empty: skipped
    const std::wstring ffmpeg = findOnPath(L"ffmpeg.exe");
    std::vector<std::wstring> generated;
    auto generatedHas = [&](const wchar_t* n) { return std::find(generated.begin(), generated.end(), n) != generated.end(); };
    if (!ffmpeg.empty()) {
        const std::wstring q = L"\"";
        const std::wstring base = q + ffmpeg + q + L" -hide_banner -v error -y ";
        const std::wstring cover = (work / L"cover.jpg").wstring();
        run(base + L"-f lavfi -i color=c=0x3366cc:s=320x320 -frames:v 1 " + q + cover + q);
        const std::wstring meta = L" -metadata title=\"Dalga Testi\" -metadata artist=\"Test Sanatçı\" -metadata album=\"Sentez Albüm\""
                                  L" -metadata album_artist=\"Test Sanatçı\" -metadata track=3 -metadata date=2024 ";
        const std::wstring sine = L"-f lavfi -i sine=frequency=440:duration=4 ";
        const std::wstring withCover = L"-i " + q + cover + q + L" -map 0:a -map 1:v -c:v copy -disposition:v attached_pic ";
        struct Gen {
            const wchar_t* name;
            std::wstring args;
        };
        const Gen gens[] = {
            {L"t_flac.flac", sine + withCover + L"-c:a flac"},
            {L"t_aac.m4a", sine + withCover + L"-c:a aac -b:a 128k"},
            {L"t_alac.m4a", sine + L"-c:a alac"},
            {L"t_adts.aac", sine + L"-c:a aac -b:a 128k -f adts"},
            {L"t_wma.wma", sine + L"-c:a wmav2 -b:a 128k"},
            {L"t_mp3.mp3", sine + withCover + L"-c:a libmp3lame -b:a 128k -id3v2_version 3"},
            {L"t_ogg.ogg", sine + L"-c:a libvorbis"},
            {L"t_opus.opus", sine + L"-c:a libopus"},
        };
        for (const auto& g : gens) {
            const fs::path out = sub / g.name;
            if (run(base + g.args + meta + q + out.wstring() + q)) generated.push_back(g.name);
            else std::printf("  ffmpeg could not make %s\n", toUtf8(g.name).c_str());
        }
        // The ShadeTube downloader's tag shape: ID3v2.3 whose APIC uses UTF-8 (encoding 3, a v2.4 value), which
        // Windows' property handler ignores -> the scanner's own ID3 reader must find the picture.
        if (generatedHas(L"t_mp3.mp3")) {
            std::ifstream in(sub / L"t_mp3.mp3", std::ios::binary);
            std::string mp3((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::ifstream ci(cover, std::ios::binary);
            const std::string jpg((std::istreambuf_iterator<char>(ci)), std::istreambuf_iterator<char>());
            const size_t oldTag = 10 + ((mp3[6] & 0x7F) << 21 | (mp3[7] & 0x7F) << 14 | (mp3[8] & 0x7F) << 7 | (mp3[9] & 0x7F));
            auto frame = [](const char* id, const std::string& body) {
                const auto n = static_cast<uint32_t>(body.size());
                return std::string(id, 4) + char(n >> 24) + char(n >> 16 & 0xFF) + char(n >> 8 & 0xFF) + char(n & 0xFF) +
                       std::string(2, '\0') + body;
            };
            const std::string frames = frame("TIT2", "\x03" + toUtf8(L"UTF-8 Başlık")) +
                                       frame("APIC", std::string("\x03image/jpeg\0\x03\0", 14) + jpg);
            const auto n = static_cast<uint32_t>(frames.size());
            const std::string head = std::string("ID3\x03\0\0", 6) + char(n >> 21 & 0x7F) + char(n >> 14 & 0x7F) +
                                     char(n >> 7 & 0x7F) + char(n & 0x7F);
            std::ofstream(sub / L"t_id3utf8.mp3", std::ios::binary) << head << frames << mp3.substr(oldTag);
            generated.push_back(L"t_id3utf8.mp3");
        }
        std::printf("  ffmpeg: %zu tagged files generated\n", generated.size());
    } else {
        std::printf("  ffmpeg not on PATH: FLAC / M4A / WMA / Ogg checks skipped\n");
    }
    int expected = realMp3 + 2;
    for (const auto& g : generated)
        if (local::isAudioFile(g)) ++expected;

    // ---- 2. first scan ------------------------------------------------------------------------------------
    std::printf("\n[scan]\n");
    local::ScanOptions o;
    o.folders = {music.wstring()};
    o.coverDir = covers;
    std::atomic<int> progressCalls{0}, lastDone{0}, lastTotal{0};
    o.progress = [&](int d, int t) {
        ++progressCalls;
        lastDone = d;
        lastTotal = t;
    };
    local::ScanStats st;
    auto first = local::scan(o, {}, &st);
    for (const auto& e : first) {
        std::string artists;
        for (const auto& a : e.artists) artists += (artists.empty() ? "" : ", ") + a;
        std::printf("  %-26s | %-22s | %-14s | %-14s | #%d %d | %6d ms | %s%s\n", toUtf8(fs::path(e.path).filename().wstring()).c_str(),
                    e.title.c_str(), artists.c_str(), e.album.c_str(), e.trackNumber, e.year, e.durationMs,
                    e.tagged ? "tags" : "name", e.cover.empty() ? "" : (e.embeddedCover ? " +cover" : " +folder art"));
    }
    CHECK(st.files == expected);
    CHECK(static_cast<int>(first.size()) == expected);
    CHECK(st.read == expected && st.reused == 0 && st.failed == 0 && !st.capped && !st.cancelled);
    CHECK(progressCalls > 0 && lastDone == lastTotal && lastTotal == expected);
    if (const auto* e = byName(first, L"Test Sanatçı - Dalga.wav")) {
        CHECK(e->title == "Dalga");
        CHECK(e->artists.size() == 1 && e->artists[0] == "Test Sanatçı");
        CHECK(!e->tagged);
        CHECK(e->durationMs >= 1900 && e->durationMs <= 2100);
    } else CHECK(!"Test Sanatçı - Dalga.wav listed");
    if (const auto* e = byName(first, L"03 - Sadece Başlık.wav")) {
        CHECK(e->title == "Sadece Başlık");
        CHECK(e->trackNumber == 3);
        CHECK(e->artists.empty());
    } else CHECK(!"03 - Sadece Başlık.wav listed");
    CHECK(!byName(first, L"._gizli.wav") && !byName(first, L"bos.mp3") && !byName(first, L"notlar.txt"));
    int realTagged = 0, realCovers = 0, realApic = 0;
    for (const auto& e : first) {
        if (fs::path(e.path).extension() != L".mp3" || fs::path(e.path).filename().wstring().starts_with(L"t_")) continue;
        realTagged += e.tagged && !e.title.empty() && e.durationMs > 10'000;   // (a download may have no artist tag)
        realCovers += e.embeddedCover && fs::exists(e.cover);
        std::ifstream in(e.path, std::ios::binary);   // does the file carry a picture at all?
        std::string head(256 * 1024, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        realApic += head.find("APIC") != std::string::npos;
    }
    std::printf("  downloaded MP3 copies: %d, tagged %d, with a picture %d, cover extracted %d\n", realMp3, realTagged, realApic,
                realCovers);
    CHECK(realTagged == realMp3);
    CHECK(realCovers == realApic);
    for (const wchar_t* n : {L"t_flac.flac", L"t_aac.m4a", L"t_mp3.mp3", L"t_wma.wma", L"t_alac.m4a"}) {
        if (!generatedHas(n)) continue;
        const auto* e = byName(first, n);
        std::printf("  %s\n", toUtf8(n).c_str());
        CHECK(e != nullptr);
        if (!e) continue;
        CHECK(e->tagged && e->title == "Dalga Testi");
        CHECK(e->artists.size() == 1 && e->artists[0] == "Test Sanatçı");
        CHECK(e->album == "Sentez Albüm");
        CHECK(e->trackNumber == 3);
        CHECK(e->durationMs >= 3900 && e->durationMs <= 4200);
        if (std::wstring(n) != L"t_wma.wma" && std::wstring(n) != L"t_alac.m4a") CHECK(e->embeddedCover && fs::exists(e->cover));
        std::printf("        year %d\n", e->year);
    }
    if (generatedHas(L"t_id3utf8.mp3")) {
        const auto* e = byName(first, L"t_id3utf8.mp3");
        std::printf("  t_id3utf8.mp3 (ID3v2.3 + UTF-8 APIC)\n");
        CHECK(e && e->title == "UTF-8 Başlık");
        CHECK(e && e->embeddedCover && fs::exists(e->cover));
    }
    // One cover file per distinct picture (the generated files share one).
    int coverFiles = 0;
    for (const auto& de : fs::directory_iterator(covers, ec)) coverFiles += de.is_regular_file(ec);
    std::printf("  cover files: %d\n", coverFiles);
    CHECK(coverFiles >= 1 && coverFiles <= realMp3 + 1);

    // ids + tracks
    std::printf("\n[ids]\n");
    for (const auto& e : first) {
        const auto t = local::toTrack(e);
        if (t.id != local::trackIdFor(e.path) || t.id.size() != 6 + 16 || t.id.rfind("local:", 0) != 0) {
            CHECK(!"track id format");
            break;
        }
        if (!t.album.id.empty() || (!t.artists.empty() && !t.artists[0].id.empty())) {
            CHECK(!"no album / artist ids");
            break;
        }
        if (!e.cover.empty() && (t.album.images.empty() || t.album.images[0].url.rfind("file:///", 0) != 0)) {
            CHECK(!"cover as file:/// image");
            break;
        }
    }
    CHECK(local::trackIdFor(L"C:\\Müzik\\A.mp3") == local::trackIdFor(L"c:\\müzik\\a.MP3"));
    CHECK(local::trackIdFor(L"C:\\Müzik\\A.mp3") != local::trackIdFor(L"C:\\Müzik\\B.mp3"));
    CHECK(local::fileUrl(L"C:\\Kapak klasörü\\a b.jpg") == "file:///C:/Kapak%20klas%C3%B6r%C3%BC/a%20b.jpg");

    // ---- 3. index + incremental rescans ----------------------------------------------------------------------
    std::printf("\n[index]\n");
    const fs::path indexFile = work / L"local-library.json";
    const local::IndexMeta meta{1234, {music.wstring()}, false};
    CHECK(local::saveIndex(indexFile, first, meta));
    local::IndexMeta readMeta;
    const auto loaded = local::loadIndex(indexFile, &readMeta);
    CHECK(readMeta.scannedAt == 1234 && readMeta.folders == meta.folders && !readMeta.capped);
    CHECK(loaded.size() == first.size());
    bool same = loaded.size() == first.size();
    for (size_t i = 0; same && i < loaded.size(); ++i) {
        const auto &a = loaded[i], &b = first[i];
        same = a.path == b.path && a.size == b.size && a.mtime == b.mtime && a.title == b.title && a.artists == b.artists &&
               a.album == b.album && a.albumArtist == b.albumArtist && a.discNumber == b.discNumber &&
               a.trackNumber == b.trackNumber && a.year == b.year &&
               a.durationMs == b.durationMs && a.tagged == b.tagged && a.cover == b.cover && a.embeddedCover == b.embeddedCover;
    }
    CHECK(same);

    std::printf("\n[rescan: nothing changed]\n");
    progressCalls = 0;
    auto second = local::scan(o, loaded, &st);
    CHECK(st.files == expected && st.reused == expected && st.read == 0 && st.removed == 0);
    CHECK(progressCalls == 0);
    bool idsStable = second.size() == first.size();
    for (size_t i = 0; idsStable && i < second.size(); ++i) idsStable = local::toTrack(second[i]).id == local::toTrack(first[i]).id;
    CHECK(idsStable);

    std::printf("\n[rescan: one file changed]\n");
    const fs::path changed = music / L"Test Sanatçı - Dalga.wav";
    writeWav(changed, 3.0, 440);   // new size + mtime
    auto third = local::scan(o, second, &st);
    CHECK(st.read == 1 && st.reused == expected - 1 && st.files == expected);
    if (const auto* e = byName(third, changed.filename().wstring())) CHECK(e->durationMs >= 2900 && e->durationMs <= 3100);

    std::printf("\n[rescan: one file removed]\n");
    const std::string goneId = local::trackIdFor((sub / L"03 - Sadece Başlık.wav").wstring());
    fs::remove(sub / L"03 - Sadece Başlık.wav");
    auto fourth = local::scan(o, third, &st);
    CHECK(st.removed == 1 && st.files == expected - 1 && st.read == 0);
    bool gone = true;
    for (const auto& e : fourth) gone = gone && local::trackIdFor(e.path) != goneId;
    CHECK(gone);

    std::printf("\n[nested folders, cap, cancel]\n");
    local::ScanOptions nested = o;
    nested.folders = {music.wstring(), sub.wstring(), music.wstring() + L"\\"};
    auto fifth = local::scan(nested, fourth, &st);
    CHECK(st.files == expected - 1);
    local::ScanOptions capped = o;
    capped.maxFiles = 2;
    local::scan(capped, {}, &st);
    CHECK(st.capped && st.files == 2);
    local::ScanOptions cancelled = o;
    cancelled.cancel = std::make_shared<local::ScanCancel>();
    cancelled.cancel->request();
    CHECK(local::scan(cancelled, {}, &st).empty() && st.cancelled);
    // Requested while the readers run (from a reader thread's progress callback): the scan stops and says so.
    local::ScanOptions midway = o;
    auto stopper = std::make_shared<local::ScanCancel>();
    midway.cancel = stopper;
    midway.progress = [stopper](int done, int) {
        if (done >= 1) stopper->request();
    };
    CHECK(local::scan(midway, {}, &st).empty() && st.cancelled);

    std::printf("\n[cover pruning]\n");
    std::vector<local::Entry> noCovers;
    for (const auto& e : fifth)
        if (!e.embeddedCover) noCovers.push_back(e);
    const int withCover = static_cast<int>(fifth.size() - noCovers.size());
    const int pruned = local::pruneCovers(covers, noCovers);
    int left = 0;
    for (const auto& de : fs::directory_iterator(covers, ec)) left += de.is_regular_file(ec);
    CHECK(pruned == coverFiles && left == 0);
    local::scan(o, fifth, &st);   // embedded covers whose file vanished are re-read
    CHECK(st.read == withCover);

    std::printf("\n[folder helpers]\n");
    CHECK(local::normalizeFolder(L"C:\\Müzik\\") == L"C:\\Müzik");
    CHECK(local::normalizeFolder(L"C:\\Müzik\\Rock\\..\\Pop") == L"C:\\Müzik\\Pop");
    CHECK(local::normalizeFolder(L"D:\\") == L"D:\\");
    CHECK(local::isUnder(L"C:\\Müzik\\Rock\\a.mp3", L"C:\\müzik"));
    CHECK(local::isUnder(L"C:\\Müzik", L"C:\\Müzik"));
    CHECK(!local::isUnder(L"C:\\Müzik2\\a.mp3", L"C:\\Müzik"));
    CHECK(local::isUnder(L"D:\\a.mp3", L"D:\\"));
    CHECK(local::normalizeFolder(L"C:\\K\xD800t").empty());   // an unpaired surrogate can't be stored

    std::printf("\n[unreachable folder: its tracks are kept]\n");
    {
        // A drive letter that doesn't exist stands in for an unplugged USB drive / an offline share.
        wchar_t letter = 0;
        const DWORD drives = GetLogicalDrives();
        for (wchar_t d = L'Z'; d >= L'G' && !letter; --d)
            if (!(drives & (1u << (d - L'A')))) letter = d;
        const std::wstring offlineRoot = std::wstring(1, letter) + L":\\M\u00fczik";
        std::vector<local::Entry> indexed = fourth;   // as if they had been indexed on that drive
        for (auto& e : indexed) e.path = offlineRoot + e.path.substr(music.wstring().size());
        local::ScanOptions offline = o;
        offline.folders = {offlineRoot};
        const auto kept = local::scan(offline, indexed, &st);
        CHECK(st.incomplete && st.missingRoots.size() == 1 && st.removed == 0 && st.read == 0);
        CHECK(st.carried == static_cast<int>(indexed.size()) && kept.size() == indexed.size());
        bool allKept = kept.size() == indexed.size();
        for (const auto& e : indexed)
            allKept = allKept && std::any_of(kept.begin(), kept.end(), [&](const local::Entry& k) {
                          return k.path == e.path && k.title == e.title && k.cover == e.cover;
                      });
        CHECK(allKept);
    }

    std::printf("\n[deleted folder: its tracks go]\n");
    {
        const fs::path doomed = work / L"silinecek";
        fs::create_directories(doomed);
        writeWav(doomed / L"A - B.wav", 0.5, 440);
        local::ScanOptions d = o;
        d.folders = {doomed.wstring()};
        const auto before = local::scan(d, {}, &st);
        CHECK(before.size() == 1);
        fs::remove_all(doomed);
        const auto after = local::scan(d, before, &st);
        CHECK(after.empty() && st.removed == 1 && !st.incomplete && st.missingRoots.empty());
    }

    std::printf("\n[walk rules and file names]\n");
    const fs::path walk = work / L"walk", target = work / L"walk-target";
    fs::create_directories(walk / L"alt");
    fs::create_directories(walk / L".gizli");
    fs::create_directories(walk / L"sakl\u0131");
    SetFileAttributesW((walk / L"sakl\u0131").c_str(), FILE_ATTRIBUTE_HIDDEN);
    fs::create_directories(target);
    writeWav(target / L"Hedef - Par\u00e7a.wav", 0.3, 440);
    writeWav(walk / L".gizli" / L"Gizli - Par\u00e7a.wav", 0.3, 440);
    writeWav(walk / L"sakl\u0131" / L"Sakl\u0131 - Par\u00e7a.wav", 0.3, 440);
    const bool junction = run(L"cmd.exe /c mklink /J \"" + (walk / L"ba\u011flant\u0131").wstring() + L"\" \"" + target.wstring() + L"\"");
    std::printf("  junction %s\n", junction ? "created" : "could not be created (junction checks skipped)");
    struct NameCase {
        const wchar_t* file;
        const char* title;
        const char* artist;
        int track;
    };
    const NameCase names[] = {
        {L"Queen \u2013 Bohemian Rhapsody - Remastered 2011.wav", "Bohemian Rhapsody - Remastered 2011", "Queen", 0},
        {L"01 \u2013 Ba\u015fl\u0131k Bir.wav", "Ba\u015fl\u0131k Bir", "", 1},
        {L"2 \u2013 Ba\u015fl\u0131k \u0130ki.wav", "Ba\u015fl\u0131k \u0130ki", "", 2},
        {L"07_Yedi.wav", "Yedi", "", 7},
        {L"1. Intro.wav", "Intro", "", 1},
        {L"50 Cent - In Da Club.wav", "In Da Club", "50 Cent", 0},
        {L"\u00b3 - Outro.wav", "Outro", "\u00b3", 0},   // superscript 3: not a track number, and not dropped
    };
    for (const auto& n : names) writeWav(walk / L"alt" / n.file, 0.2, 330);
    writeWav(fs::path((walk / L"alt").wstring() + L"\\K\xD800t.wav"), 0.2, 330);   // unpaired surrogate: skipped
    local::ScanOptions wo = o;
    wo.folders = {walk.wstring()};
    const auto walked = local::scan(wo, {}, &st);
    CHECK(walked.size() == std::size(names));   // only alt\: no junction target, no .gizli, no hidden folder, no bad name
    for (const auto& n : names) {
        const auto* e = byName(walked, n.file);
        const bool ok = e && e->title == n.title && (n.artist[0] ? e->artists.size() == 1 && e->artists[0] == n.artist : e->artists.empty()) &&
                        e->trackNumber == n.track;
        std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", toUtf8(n.file).c_str());
        if (!ok) ++g_failures;
    }
    CHECK(local::walkedFrom(walk.wstring(), (walk / L"alt").wstring()));
    CHECK(!local::walkedFrom(walk.wstring(), (walk / L".gizli").wstring()));
    CHECK(!local::walkedFrom(walk.wstring(), (walk / L"sakl\u0131").wstring()));
    if (junction) CHECK(!local::walkedFrom(walk.wstring(), (walk / L"ba\u011flant\u0131").wstring()));

    std::printf("\n[folders: add / remove rules]\n");
    {
        auto& lib = st::app::LocalLibrary::get();
        auto& folders = st::Settings::get().localFolders;
        folders.clear();
        CHECK(lib.addFolder(walk.wstring()));
        CHECK(!lib.addFolder(walk.wstring() + L"\\"));            // the same folder
        CHECK(!lib.addFolder((walk / L"alt").wstring()));         // its scan already covers it
        CHECK(lib.addFolder((walk / L".gizli").wstring()));       // the walk skips it: it may be listed on its own
        if (junction) CHECK(lib.addFolder((walk / L"ba\u011flant\u0131").wstring()));
        lib.removeFolder(walk.wstring());
        CHECK(folders.size() == (junction ? 2u : 1u));
        CHECK(lib.addFolder(walk.wstring()));                     // the parent again: the skipped ones stay listed
        CHECK(folders.size() == (junction ? 3u : 2u));
        CHECK(!lib.addFolder(L"C:\\K\xD800t"));
        folders.clear();
    }

    std::printf("\n[long path]\n");
    {
        std::wstring deep = (work / L"uzun").wstring();
        std::vector<std::wstring> levels{deep};
        while (deep.size() < 300) levels.push_back(deep += L"\\\u00c7ok uzun bir klas\u00f6r ad\u0131 0123456789");
        for (const auto& l : levels) CreateDirectoryW((L"\\\\?\\" + l).c_str(), nullptr);
        writeWav(fs::path(L"\\\\?\\" + deep + L"\\Uzun Sanat\u00e7\u0131 - Uzun Ba\u015fl\u0131k.wav"), 0.5, 440);
        local::ScanOptions lo = o;
        lo.folders = {levels.front()};
        const auto longs = local::scan(lo, {}, &st);
        CHECK(longs.size() == 1 && st.failed == 0);
        if (!longs.empty()) {
            std::printf("  path length %zu\n", longs[0].path.size());
            CHECK(longs[0].path.size() > MAX_PATH && longs[0].title == "Uzun Ba\u015fl\u0131k");
            CHECK(local::openablePath(longs[0].path).starts_with(L"\\\\?\\"));
            CHECK(decodeSeconds(local::openablePath(longs[0].path), "audio/wav") > 0.4);   // what pathFor() hands the player
        }
    }

    std::printf("\n[disc order]\n");
    {
        const fs::path discs = work / L"diskler";
        fs::create_directories(discs / L"CD2");
        writeWav(discs / L"CD2" / L"05 - Klas\u00f6rden.wav", 0.2, 440);   // no tags: the disc comes from "CD2"
        local::ScanOptions dopt = o;
        dopt.folders = {discs.wstring()};
        std::vector<std::wstring> made;
        if (!ffmpeg.empty()) {
            const std::wstring q = L"\"";
            const std::wstring base = q + ffmpeg + q + L" -hide_banner -v error -y -f lavfi -i sine=frequency=440:duration=1 ";
            const std::wstring common = L" -metadata artist=\"Disk Test\" -metadata album_artist=\"Disk Test\" -metadata album=\"\u0130ki Disk\" ";
            struct D {
                const wchar_t* file;
                const wchar_t* args;
            };
            const D ds[] = {
                {L"a.mp3", L"-c:a libmp3lame -id3v2_version 3 -metadata title=\"Disk2 Par\u00e7a1\" -metadata disc=2/2 -metadata track=1"},
                {L"b.flac", L"-c:a flac -metadata title=\"Disk1 Par\u00e7a2\" -metadata disc=1/2 -metadata track=2"},
                {L"c.m4a", L"-c:a aac -metadata title=\"Disk1 Par\u00e7a1\" -metadata disc=1/2 -metadata track=1"},
            };
            for (const auto& d : ds)
                if (run(base + d.args + common + q + (discs / d.file).wstring() + q)) made.push_back(d.file);
        }
        const auto sorted = local::scan(dopt, {}, &st);
        for (const auto& e : sorted)
            std::printf("  disc %d track %d  %s\n", e.discNumber, e.trackNumber, toUtf8(fs::path(e.path).filename().wstring()).c_str());
        if (const auto* e = byName(sorted, L"05 - Klas\u00f6rden.wav")) CHECK(e->discNumber == 2);
        if (made.size() == 3) {
            std::vector<std::wstring> order;
            for (const auto& e : sorted)
                if (e.album == "\u0130ki Disk") order.push_back(fs::path(e.path).filename().wstring());
            CHECK((order == std::vector<std::wstring>{L"c.m4a", L"b.flac", L"a.mp3"}));
        }
    }

    // ---- 3b. dropped files -----------------------------------------------------------------------------------
    std::printf("\n[dropped from Explorer]\n");
    {
        namespace dropped = st::app::dropped;
        const fs::path drop = work / L"dropped";
        fs::create_directories(drop / L"Albüm" / L"CD1", ec);
        writeWav(drop / L"Albüm" / L"CD1" / L"02 - İkinci.wav", 0.3, 330);
        writeWav(drop / L"Albüm" / L"CD1" / L"01 - Birinci.wav", 0.3, 330);
        writeWav(drop / L"Tek - Şarkı.wav", 0.3, 440);
        std::ofstream(drop / L"notlar.txt") << "not audio";
        CHECK(dropped::droppable((drop / L"Tek - Şarkı.wav").wstring()));
        CHECK(dropped::droppable((drop / L"Albüm").wstring()));
        CHECK(!dropped::droppable((drop / L"notlar.txt").wstring()));
        CHECK(dropped::droppable((drop / L"yok.wav").wstring()));   // by its extension: the disk isn't asked
        const fs::path dropCovers = work / L"dropped-covers";
        // A loose file, a folder, the same file again inside the folder list, something that isn't audio, a ghost.
        const auto r = dropped::collect({(drop / L"notlar.txt").wstring(), (drop / L"Albüm").wstring(),
                                         (drop / L"Tek - Şarkı.wav").wstring(), (drop / L"Tek - Şarkı.wav").wstring(),
                                         (drop / L"gone.wav").wstring()},
                                        dropCovers);
        for (const auto& e : r.entries)
            std::printf("  %s | %s | disc %d track %d\n", e.title.c_str(), toUtf8(fs::path(e.path).filename().wstring()).c_str(),
                        e.discNumber, e.trackNumber);
        CHECK(r.entries.size() == 3);
        CHECK(r.folders.size() == 1);
        CHECK(r.skipped == 2);   // notlar.txt + gone.wav
        CHECK(!r.capped);
        if (r.entries.size() == 3) {
            // Untagged: "Tek - Şarkı" has an artist (sorts first); the album files by track number from their names.
            CHECK(r.entries[0].title == "Şarkı" && !r.entries[0].artists.empty() && r.entries[0].artists[0] == "Tek");
            CHECK(fs::path(r.entries[1].path).filename() == L"01 - Birinci.wav");
            CHECK(fs::path(r.entries[2].path).filename() == L"02 - İkinci.wav");
            CHECK(local::toTrack(r.entries[0]).id == local::trackIdFor(r.entries[0].path));
        }
        const auto few = dropped::collect({(drop / L"Albüm").wstring(), (drop / L"Tek - Şarkı.wav").wstring()}, {}, 2);
        CHECK(few.entries.size() == 2 && few.capped);
        CHECK(dropped::collect({(drop / L"notlar.txt").wstring()}, {}).entries.empty());

        // Remembered list: a file dropped again moves to the end; the newest `cap` are kept.
        auto entry = [](const wchar_t* path) {
            local::Entry e;
            e.path = path;
            return e;
        };
        std::vector<local::Entry> list;
        dropped::mergeRemembered(list, {entry(L"C:\\a.mp3"), entry(L"C:\\b.mp3"), entry(L"C:\\c.mp3")}, 3);
        dropped::mergeRemembered(list, {entry(L"C:\\A.MP3"), entry(L"C:\\d.mp3"), entry(L"C:\\d.mp3")}, 3);
        std::wstring order;
        for (const auto& e : list) order += fs::path(e.path).stem().wstring();
        std::printf("  remembered: %s\n", toUtf8(order).c_str());
        CHECK(order == L"cAd");   // b fell off the end, a moved behind c (with its new spelling), d once

        // Drop rules.
        auto tr = [](const char* id) {
            st::catalog::Track t;
            t.id = id;
            t.name = id;
            return t;
        };
        const std::vector<st::catalog::Track> mixed{tr("spotify:track:1"), tr("local:ab"), tr("radio:x"), tr(""),
                                                    tr("11111111-1111-1111-1111-111111111111")};
        const auto onlySpotify = [](const std::string& id) { return id.rfind("spotify:track:", 0) == 0; };
        using DR = dropped::DropRow;
        CHECK(dropped::droppableOn(DR::SpotifyPlaylist, mixed, onlySpotify).size() == 1);
        CHECK(dropped::droppableOn(DR::Liked, mixed, onlySpotify).size() == 1);
        CHECK(dropped::droppableOn(DR::Liked, mixed, [](const std::string&) { return true; }).size() == 3);
        CHECK(dropped::droppableOn(DR::LocalPlaylist, mixed, onlySpotify).size() == 3);   // never stations / no id
        CHECK(dropped::droppableOn(DR::Liked, mixed, nullptr).empty());
        // Queue: current slot 2 of 6; rows of 52 below the list top.
        CHECK(dropped::queueDropIndex(-40, 52, 2, 6) == 3);    // above the list: right after the current track
        CHECK(dropped::queueDropIndex(20, 52, 2, 6) == 3);
        CHECK(dropped::queueDropIndex(30, 52, 2, 6) == 4);     // past the first row's middle
        CHECK(dropped::queueDropIndex(1000, 52, 2, 6) == 6);   // below everything: the end
        CHECK(dropped::queueDropIndex(10, 52, -1, 0) == 0);    // empty queue
        CHECK(dropped::queueDropIndex(10, 0, 2, 6) == 6);
    }

    // ---- 4. formats ---------------------------------------------------------------------------------------
    std::printf("\n[formats: decode through the engine's byte-stream path]\n");
    std::printf("  %-26s %-24s %s\n", "file", "old fallback mime", "Player::localMimeType");
    std::vector<fs::path> all;
    for (const auto& de : fs::recursive_directory_iterator(music, ec))
        if (de.is_regular_file(ec) && !de.path().filename().wstring().starts_with(L"._") && de.file_size(ec) > 0 &&
            de.path().extension() != L".txt")
            all.push_back(de.path());
    for (const auto& p : all) {
        const double a = decodeSeconds(p.wstring(), fallbackMime(p.wstring()));
        const double b = decodeSeconds(p.wstring(), extMime(p.wstring()));
        std::printf("  %-26s %-11s %-12s %-14s %s%s\n", toUtf8(p.filename().wstring()).c_str(), fallbackMime(p.wstring()).c_str(),
                    a > 0.5 ? "decodes" : "FAILS", extMime(p.wstring()).c_str(), b > 0.5 ? "decodes" : "FAILS",
                    local::isAudioFile(p.wstring()) ? "" : "   (not scanned: no MF handler)");
        if (local::isAudioFile(p.wstring())) CHECK(b > 0.5);   // every scanned format decodes with the Player's mime
    }

    std::printf("\n[playback: AudioEngine, volume 0, per-extension mime]\n");
    std::map<std::wstring, bool> tried;   // one file per extension
    for (const auto& p : all) {
        const std::wstring ext = p.extension().wstring();
        if (tried[ext] || !local::isAudioFile(p.wstring())) continue;
        tried[ext] = true;
        CHECK(plays(p.wstring(), extMime(p.wstring())));
    }

    MFShutdown();
    CoUninitialize();
    if (!keep) wipe(work);
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
