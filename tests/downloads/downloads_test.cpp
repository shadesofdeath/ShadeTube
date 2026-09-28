// Developer test for audio/Downloader's SponsorBlock trimming (offline, no network):
//   1. writes a 20 s 44.1 kHz stereo WAV: a quiet 440 Hz sine (amplitude 0.3) everywhere EXCEPT inside the two
//      ranges that will be cut, which hold a loud 500 Hz square wave (amplitude 0.9);
//   2. runs audio::downloadTrack on it (localPath source) with and without the cuts -> MP3 320;
//   3. verifies with Media Foundation: duration (MF_PD_DURATION) = original - cut (within 50 ms), ONE ID3 tag,
//      no loud (cut) audio left in the output (10 ms RMS windows), no click at the splices (the 1st splice joins
//      a zero crossing to a sine peak: a hard cut would jump by ~0.3 in one sample), DownloadStats;
//   4. edge cases: a range reaching past the end / starting before 0, and the "would leave < 10 s" guard.
//   downloads_test            run everything
//   downloads_test --keep     keep the generated files (prints the folder)
#include "audio/Downloader.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace audio = st::audio;

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

constexpr uint32_t kRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr int64_t kSourceMs = 20000;
constexpr double kPi = 3.14159265358979323846;
constexpr float kSineAmp = 0.3f;     // RMS 0.212
constexpr float kSquareAmp = 0.9f;   // RMS 0.9

// The two cut ranges. Chosen so the first splice joins sin(0) (3000 ms: whole cycles) to a sine PEAK
// (5521 ms -> 440 * 5.521 = 2429.24 cycles): without a fade that is a ~0.3 jump between two samples.
const std::vector<std::pair<int64_t, int64_t>> kCuts = {{3000, 5521}, {12250, 15000}};
constexpr int64_t kCutTotalMs = 2521 + 2750;

bool insideCut(int64_t frame) {
    for (const auto& [s, e] : kCuts)
        if (frame >= s * kRate / 1000 && frame < e * kRate / 1000) return true;   // same ms->frame rule as the splicer
    return false;
}

void putLE(std::ofstream& f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) f.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool writeWav(const fs::path& path) {
    const int64_t frames = kSourceMs * kRate / 1000;
    const uint32_t dataBytes = static_cast<uint32_t>(frames * kChannels * 2);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("RIFF", 4);
    putLE(f, 36 + dataBytes, 4);
    f.write("WAVEfmt ", 8);
    putLE(f, 16, 4);
    putLE(f, 1, 2);   // PCM
    putLE(f, kChannels, 2);
    putLE(f, kRate, 4);
    putLE(f, kRate * kChannels * 2, 4);
    putLE(f, kChannels * 2, 2);
    putLE(f, 16, 2);
    f.write("data", 4);
    putLE(f, dataBytes, 4);
    std::vector<int16_t> pcm(static_cast<size_t>(frames * kChannels));
    for (int64_t n = 0; n < frames; ++n) {
        float v;
        if (insideCut(n)) v = ((n * 1000 / kRate) % 2 == 0 ? kSquareAmp : -kSquareAmp);   // 500 Hz-ish square
        else v = kSineAmp * static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / kRate));
        const auto s = static_cast<int16_t>(std::lround(v * 32767.f));
        for (uint32_t c = 0; c < kChannels; ++c) pcm[static_cast<size_t>(n * kChannels + c)] = s;
    }
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(int16_t)));
    return static_cast<bool>(f);
}

// MF_PD_DURATION of a media file in ms (-1 on failure).
int64_t mfDurationMs(const fs::path& path) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return -1;
    PROPVARIANT var;
    PropVariantInit(&var);
    int64_t ms = -1;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8)
        ms = static_cast<int64_t>(var.uhVal.QuadPart / 10'000);
    PropVariantClear(&var);
    return ms;
}

// Decodes a file to interleaved float PCM (first audio stream).
bool decodeAll(const fs::path& path, std::vector<float>& out, uint32_t& rate, uint32_t& channels) {
    out.clear();
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(audio, nullptr, type.Get()))) return false;
    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(audio, &current))) return false;
    rate = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    channels = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    if (!rate || !channels) return false;
    while (true) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(audio, 0, nullptr, &flags, &ts, &sample))) return false;
        if (sample) {
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
            BYTE* data = nullptr;
            DWORD len = 0;
            buf->Lock(&data, nullptr, &len);
            const size_t n = len / sizeof(float);
            const size_t old = out.size();
            out.resize(old + n);
            std::memcpy(out.data() + old, data, n * sizeof(float));
            buf->Unlock();
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
    }
    return true;
}

// Largest RMS over 10 ms windows of channel 0.
double maxWindowRms(const std::vector<float>& pcm, uint32_t rate, uint32_t channels) {
    const size_t win = rate / 100;
    const size_t frames = pcm.size() / channels;
    double best = 0;
    for (size_t start = 0; start + win <= frames; start += win / 2) {
        double sum = 0;
        for (size_t i = 0; i < win; ++i) {
            const double v = pcm[(start + i) * channels];
            sum += v * v;
        }
        best = std::max(best, std::sqrt(sum / static_cast<double>(win)));
    }
    return best;
}

// Largest sample-to-sample step of channel 0 (a click shows up as a big step).
double maxStep(const std::vector<float>& pcm, uint32_t channels) {
    double best = 0;
    for (size_t i = channels; i < pcm.size(); i += channels) best = std::max(best, std::fabs(static_cast<double>(pcm[i]) - pcm[i - channels]));
    return best;
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<uint8_t>& hay, const std::string& needle, size_t limit) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    const size_t end = std::min(limit, hay.size() - needle.size() + 1);
    for (size_t i = 0; i < end; ++i)
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) return true;
    return false;
}

// ID3v2.3 text frames are UTF-16LE with a BOM (encoding 1): the bytes of a UTF-8 string as they appear in the tag.
std::string utf16le(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    std::string out;
    for (wchar_t c : w) {
        out.push_back(static_cast<char>(c & 0xFF));
        out.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    return out;
}

audio::DownloadStatus run(const fs::path& wav, const fs::path& out, std::vector<std::pair<int64_t, int64_t>> cuts,
                          audio::DownloadStats& stats, float& lastProgress) {
    audio::DownloadRequest req;
    req.localPath = wav.wstring();
    req.mimeType = "audio/wav";
    req.outPath = out.wstring();
    req.mp3Kbps = 320;
    req.cutMs = std::move(cuts);
    req.title = "Deneme Şarkısı";
    req.artist = "ShadeTube Test";
    req.album = "Kesim Albümü";
    std::atomic<bool> cancel{false};
    lastProgress = 0;
    stats = {};
    return audio::downloadTrack(req, [&](float f) { lastProgress = f; }, cancel, &stats);
}

void testNormalize() {
    std::printf("[normalizeCuts]\n");
    const auto n = audio::normalizeCuts({{5000, 6000}, {-300, 1000}, {900, 1500}, {7000, 7000}, {5500, 5800}, {9000, 8000}});
    CHECK(n.size() == 2);
    CHECK(n.size() == 2 && n[0].first == 0 && n[0].second == 1500);
    CHECK(n.size() == 2 && n[1].first == 5000 && n[1].second == 6000);
    CHECK(audio::normalizeCuts({}).empty());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool keep = argc > 1 && std::wstring(argv[1]) == L"--keep";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("MFStartup failed\n");
        return 2;
    }

    testNormalize();

    const fs::path dir = fs::temp_directory_path() / L"shadetube_downloads_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path wav = dir / L"source.wav";
    const fs::path full = dir / L"full.mp3";
    const fs::path cut = dir / L"cut.mp3";
    const fs::path tail = dir / L"tail.mp3";
    const fs::path guard = dir / L"guard.mp3";

    std::printf("[source]\n");
    CHECK(writeWav(wav));
    const int64_t srcMs = mfDurationMs(wav);
    std::printf("    source.wav duration %lld ms\n", static_cast<long long>(srcMs));
    CHECK(std::llabs(srcMs - kSourceMs) <= 1);

    audio::DownloadStats st;
    float prog = 0;

    std::printf("[no cuts]\n");
    CHECK(run(wav, full, {}, st, prog) == audio::DownloadStatus::Ok);
    const int64_t fullMs = mfDurationMs(full);
    std::printf("    full.mp3 %lld ms, stats cut %d/%lld ms, out %lld ms\n", static_cast<long long>(fullMs), st.segmentsCut,
                static_cast<long long>(st.cutMs), static_cast<long long>(st.outputMs));
    CHECK(st.segmentsCut == 0 && st.cutMs == 0);
    CHECK(std::llabs(fullMs - kSourceMs) <= 50);
    CHECK(prog == 1.0f);

    std::printf("[two cuts: [3000,5521) + [12250,15000) = %lld ms]\n", static_cast<long long>(kCutTotalMs));
    CHECK(run(wav, cut, kCuts, st, prog) == audio::DownloadStatus::Ok);
    const int64_t cutMs = mfDurationMs(cut);
    std::printf("    cut.mp3 %lld ms (want %lld +-50), stats: %d segment(s), %lld ms cut, source %lld ms, out %lld ms\n",
                static_cast<long long>(cutMs), static_cast<long long>(kSourceMs - kCutTotalMs), st.segmentsCut,
                static_cast<long long>(st.cutMs), static_cast<long long>(st.sourceMs), static_cast<long long>(st.outputMs));
    CHECK(std::llabs(cutMs - (srcMs - kCutTotalMs)) <= 50);
    CHECK(std::llabs((fullMs - cutMs) - kCutTotalMs) <= 30);   // same encoder overhead on both sides
    CHECK(st.segmentsCut == 2);
    CHECK(std::llabs(st.cutMs - kCutTotalMs) <= 1);
    CHECK(std::llabs(st.sourceMs - kSourceMs) <= 1);
    CHECK(std::llabs(st.outputMs - (kSourceMs - kCutTotalMs)) <= 1);

    std::printf("[ID3 tag]\n");
    const auto bytes = readFile(cut);
    const bool id3 = bytes.size() > 10 && std::memcmp(bytes.data(), "ID3", 3) == 0 && bytes[3] == 3;
    CHECK(id3);
    size_t tagSize = 0;
    if (id3) tagSize = 10 + ((bytes[6] & 0x7F) << 21 | (bytes[7] & 0x7F) << 14 | (bytes[8] & 0x7F) << 7 | (bytes[9] & 0x7F));
    CHECK(contains(bytes, "TIT2", tagSize));
    CHECK(contains(bytes, "\x01\xFF\xFE" + utf16le("Deneme Şarkısı"), tagSize));   // encoding 1 + BOM + text
    CHECK(contains(bytes, "TALB", tagSize) && contains(bytes, utf16le("Kesim Albümü"), tagSize));
    CHECK(contains(bytes, "iTunSMPB", tagSize));   // the MF sink's gapless info, merged into our tag
    CHECK(tagSize + 4 < bytes.size() && bytes[tagSize] == 0xFF && (bytes[tagSize + 1] & 0xE0) == 0xE0);   // one tag, then audio

    std::printf("[content: nothing of the cut ranges left, no clicks]\n");
    std::vector<float> pcmFull, pcmCut;
    uint32_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    CHECK(decodeAll(full, pcmFull, r1, c1));
    CHECK(decodeAll(cut, pcmCut, r2, c2));
    if (r1 && c1 && r2 && c2) {
        const double rmsFull = maxWindowRms(pcmFull, r1, c1);
        const double rmsCut = maxWindowRms(pcmCut, r2, c2);
        const double stepCut = maxStep(pcmCut, c2);
        std::printf("    max 10 ms RMS: full %.3f (square present), cut %.3f (sine only = 0.212)\n", rmsFull, rmsCut);
        std::printf("    max sample step in cut.mp3: %.4f (clean 440 Hz sine ~0.019; a hard splice here ~0.3)\n", stepCut);
        CHECK(rmsFull > 0.6);    // the detector sees the loud part when it is there
        CHECK(rmsCut < 0.26);    // ...and nothing of it survived the cut (even ~1 ms would push a window above)
        CHECK(stepCut < 0.08);   // raised-cosine fades: no discontinuity at the splices
        const double decodedMs = 1000.0 * static_cast<double>(pcmCut.size() / c2) / r2;
        std::printf("    decoded cut.mp3: %.1f ms of audio\n", decodedMs);
        CHECK(std::fabs(decodedMs - static_cast<double>(kSourceMs - kCutTotalMs)) <= 80);
    }

    std::printf("[edge: range before 0 and past the end]\n");
    CHECK(run(wav, tail, {{-500, 1000}, {18000, 30000}}, st, prog) == audio::DownloadStatus::Ok);
    const int64_t tailMs = mfDurationMs(tail);
    std::printf("    tail.mp3 %lld ms (want 17000 +-50), stats: %d segment(s), %lld ms cut\n", static_cast<long long>(tailMs),
                st.segmentsCut, static_cast<long long>(st.cutMs));
    CHECK(st.segmentsCut == 2 && std::llabs(st.cutMs - 3000) <= 1);
    CHECK(std::llabs(tailMs - 17000) <= 50);

    std::printf("[guard: cuts leaving < 10 s are ignored]\n");
    CHECK(run(wav, guard, {{0, 15000}}, st, prog) == audio::DownloadStatus::Ok);
    const int64_t guardMs = mfDurationMs(guard);
    std::printf("    guard.mp3 %lld ms, stats: %d segment(s)\n", static_cast<long long>(guardMs), st.segmentsCut);
    CHECK(st.segmentsCut == 0 && st.cutMs == 0);
    CHECK(std::llabs(guardMs - kSourceMs) <= 50);

    std::printf("[cancel: Cancelled, no output and no .part file left]\n");
    {
        audio::DownloadRequest req;
        req.localPath = wav.wstring();
        req.mimeType = "audio/wav";
        req.outPath = (dir / L"cancelled.mp3").wstring();
        req.mp3Kbps = 320;
        req.cutMs = kCuts;
        std::atomic<bool> cancel{true};
        CHECK(audio::downloadTrack(req, {}, cancel, nullptr) == audio::DownloadStatus::Cancelled);
        CHECK(!fs::exists(dir / L"cancelled.mp3", ec));
        CHECK(!fs::exists(dir / L"cancelled.part.mp3", ec));
    }

    std::printf("[passthrough ignores cuts]\n");
    {
        audio::DownloadRequest req;
        req.localPath = wav.wstring();
        req.mimeType = "audio/wav";
        req.outPath = (dir / L"copy.wav").wstring();
        req.mp3Kbps = 0;
        req.cutMs = kCuts;
        std::atomic<bool> cancel{false};
        audio::DownloadStats ps;
        CHECK(audio::downloadTrack(req, {}, cancel, &ps) == audio::DownloadStatus::Ok);
        CHECK(fs::file_size(dir / L"copy.wav", ec) == fs::file_size(wav, ec));
        CHECK(ps.segmentsCut == 0);
    }

    if (keep) {
        std::printf("files kept in %ls\n", dir.c_str());
    } else {
        fs::remove_all(dir, ec);
    }
    MFShutdown();
    CoUninitialize();
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
