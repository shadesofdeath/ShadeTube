// Developer test for audio/TagWriter (the tag editor's core), offline. Every file is generated in a per-process work
// folder (%TEMP%\ShadeTube-tags-test-<pid>, deleted at the end unless --keep):
//   * MP3: audio::downloadTrack (Media Foundation MP3 encoder: ID3v2.3 with APIC, USLT, SYLT, TXXX ReplayGain and the
//     sink's COMM / TFLT frames) plus hand-built tags around the same audio: v2.4 (UTF-8, multi-value, TDRC, POPM, PRIV,
//     an unsynchronised frame) + ID3v1, v2.2, no tag, ID3v1 only, big padding, a tag-wide unsynchronisation, junk;
//   * FLAC: Media Foundation's FLAC encoder when it has a sink, else ffmpeg (on PATH), else a hand-built FLAC (verbatim
//     subframes) - and always the hand-built one - with test metadata (Vorbis comments incl. ReplayGain, APPLICATION,
//     PICTURE, padding / no padding);
//   * M4A: Media Foundation AAC (moov after mdat, and before it via MF_MPEG4SINK_MOOV_BEFORE_MDAT), ALAC, a fragmented
//     MP4 (MF FMPEG4), and ffmpeg's faststart / fragmented AAC when ffmpeg is on PATH; freeform ReplayGain / ©lyr items.
// Checks: the tags read back (Turkish, Japanese), every other frame / block / item byte for byte, the audio payload's
// SHA-256, covers (small / ~2 MB / PNG), padding use, chunk offsets (the result decodes through Media Foundation to the
// very same PCM as the original), the Windows property system's view (local::readFile, what the library index reads),
// corrupt input, read-only, in-use and "playing" (memory-mapped) files, a tampered temporary file, a file changed during
// the write - and that a failure never leaves the original changed or a temporary file behind.
//   tags_test [--keep]
#include "app/LocalLibrary.h"
#include "audio/Downloader.h"
#include "audio/ReplayGain.h"
#include "audio/TagWriter.h"
#include "core/Utf.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <crtdbg.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace tags = st::audio::tags;
namespace local = st::app::local;
using Bytes = std::vector<uint8_t>;
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

fs::path g_dir;

// ---- files ---------------------------------------------------------------------------------------------------------
Bytes readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void writeAll(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
Bytes str(std::string_view s) { return Bytes(s.begin(), s.end()); }
void put16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put24(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 16));
    put16(b, v & 0xFFFF);
}
void put32(Bytes& b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}
void putLE32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) { return uint32_t(p[0]) << 21 | uint32_t(p[1]) << 14 | uint32_t(p[2]) << 7 | p[3]; }

// No "~*.shadetube.tmp" / ".bak" left in the work folder.
bool noLeftovers() {
    for (const auto& e : fs::directory_iterator(g_dir)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.find(L".shadetube.") != std::wstring::npos) {
            std::printf("    leftover: %s\n", toUtf8(n).c_str());
            return false;
        }
    }
    return true;
}

// ---- audio ---------------------------------------------------------------------------------------------------------
constexpr uint32_t kRate = 44100;

std::vector<int16_t> sinePcm(double seconds, double freq) {
    const size_t frames = static_cast<size_t>(seconds * kRate);
    std::vector<int16_t> pcm(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        pcm[i * 2] = static_cast<int16_t>(std::sin(2 * 3.14159265358979 * freq * t) * 9000);
        pcm[i * 2 + 1] = static_cast<int16_t>(std::sin(2 * 3.14159265358979 * freq * 1.5 * t) * 7000);
    }
    return pcm;
}

void writeWav(const fs::path& p, const std::vector<int16_t>& pcm) {
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    Bytes b = str("RIFF");
    putLE32(b, 36 + dataBytes);
    auto add = [&](std::string_view s) { b.insert(b.end(), s.begin(), s.end()); };
    add("WAVEfmt ");
    putLE32(b, 16);
    b.insert(b.end(), {1, 0, 2, 0});
    putLE32(b, kRate);
    putLE32(b, kRate * 4);
    b.insert(b.end(), {4, 0, 16, 0});
    add("data");
    putLE32(b, dataBytes);
    const auto* raw = reinterpret_cast<const uint8_t*>(pcm.data());
    b.insert(b.end(), raw, raw + dataBytes);
    writeAll(p, b);
}

// Decodes the first audio stream to float PCM through Media Foundation.
bool decodeAll(const fs::path& path, std::vector<float>& out) {
    out.clear();
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(audio, nullptr, type.Get()))) return false;
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
    return !out.empty();
}

// Both files decode to exactly the same samples (and something at all).
bool sameAudio(const fs::path& a, const fs::path& b) {
    std::vector<float> x, y;
    if (!decodeAll(a, x) || !decodeAll(b, y)) {
        std::printf("    decode failed\n");
        return false;
    }
    if (x != y) std::printf("    decoded %zu vs %zu samples, differ\n", x.size(), y.size());
    return x == y;
}

// Media Foundation sink writer: 16-bit stereo PCM -> `subtype` in the container the extension names.
// (MF_MPEG4SINK_MOOV_BEFORE_MDAT writes a broken file through a sink writer here: moov-first files are rearranged by
// moveMoovFirst() instead.)
bool mfEncode(const fs::path& out, const std::vector<int16_t>& pcm, const GUID& subtype, UINT32 avgBytes, bool fragmented = false) {
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    if (fragmented) attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
    ComPtr<IMFSinkWriter> w;
    HRESULT hr = MFCreateSinkWriterFromURL(out.c_str(), nullptr, attrs.Get(), &w);
    if (FAILED(hr)) {
        std::printf("    (sink writer for %s: 0x%08lx)\n", toUtf8(out.filename().wstring()).c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<IMFMediaType> outType, inType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, subtype);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (avgBytes) outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
    DWORD stream = 0;
    if (FAILED(hr = w->AddStream(outType.Get(), &stream)) || FAILED(hr = w->SetInputMediaType(stream, inType.Get(), nullptr)) ||
        FAILED(hr = w->BeginWriting())) {
        std::printf("    (encoder setup for %s: 0x%08lx)\n", toUtf8(out.filename().wstring()).c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    const size_t frames = pcm.size() / 2;
    for (size_t f = 0; f < frames; f += 4096) {
        const size_t n = std::min<size_t>(4096, frames - f);
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> s;
        BYTE* dst = nullptr;
        MFCreateMemoryBuffer(static_cast<DWORD>(n * 4), &buf);
        buf->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, pcm.data() + f * 2, n * 4);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(n * 4));
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(static_cast<LONGLONG>(f) * 10'000'000 / kRate);
        s->SetSampleDuration(static_cast<LONGLONG>(n) * 10'000'000 / kRate);
        if (FAILED(w->WriteSample(stream, s.Get()))) return false;
    }
    hr = w->Finalize();
    if (FAILED(hr)) std::printf("    (finalize %s: 0x%08lx)\n", toUtf8(out.filename().wstring()).c_str(), static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool ffmpegAvailable() {
    static const bool ok = _wsystem(L"ffmpeg -hide_banner -loglevel quiet -version >nul 2>nul") == 0;
    return ok;
}
bool ffmpeg(const std::wstring& args) {
    const std::wstring cmd = L"ffmpeg -hide_banner -loglevel error -y " + args + L" >nul 2>nul";
    return _wsystem(cmd.c_str()) == 0;
}

// ---- images (WIC) --------------------------------------------------------------------------------------------------
Bytes encodeImage(int w, int h, bool noise, const GUID& container) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return {};
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    std::mt19937 rng(42);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 3];
            if (noise) {
                p[0] = static_cast<uint8_t>(rng());
                p[1] = static_cast<uint8_t>(rng());
                p[2] = static_cast<uint8_t>(rng());
            } else {
                p[0] = static_cast<uint8_t>(x * 255 / w);
                p[1] = static_cast<uint8_t>(y * 255 / h);
                p[2] = 160;
            }
        }
    ComPtr<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat24bppBGR, w * 3, static_cast<UINT>(px.size()), px.data(), &bmp)))
        return {};
    ComPtr<IStream> stream;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(container, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return {};
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    enc->CreateNewFrame(&frame, &props);
    frame->Initialize(props.Get());
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&fmt);
    frame->WriteSource(bmp.Get(), nullptr);
    frame->Commit();
    enc->Commit();
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    Bytes out(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    stream->Read(out.data(), static_cast<ULONG>(out.size()), &got);
    out.resize(got);
    return out;
}

// ---- ID3 -----------------------------------------------------------------------------------------------------------
Bytes id3Frame(int major, std::string_view id, const Bytes& body, uint8_t f0 = 0, uint8_t f1 = 0) {
    Bytes b = str(id);
    const auto n = static_cast<uint32_t>(body.size());
    if (major == 2) put24(b, n);
    else if (major == 4) b.insert(b.end(), {static_cast<uint8_t>((n >> 21) & 0x7F), static_cast<uint8_t>((n >> 14) & 0x7F),
                                            static_cast<uint8_t>((n >> 7) & 0x7F), static_cast<uint8_t>(n & 0x7F)});
    else put32(b, n);
    if (major != 2) b.insert(b.end(), {f0, f1});
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
Bytes id3Tag(int major, const Bytes& frames, size_t padding, uint8_t flags = 0) {
    const auto n = static_cast<uint32_t>(frames.size() + padding);
    Bytes b{'I', 'D', '3', static_cast<uint8_t>(major), 0, flags, static_cast<uint8_t>((n >> 21) & 0x7F),
            static_cast<uint8_t>((n >> 14) & 0x7F), static_cast<uint8_t>((n >> 7) & 0x7F), static_cast<uint8_t>(n & 0x7F)};
    b.insert(b.end(), frames.begin(), frames.end());
    b.resize(b.size() + padding, 0);
    return b;
}
Bytes latin(std::string_view s) { return cat({{0}, str(s)}); }
Bytes utf8Text(std::string_view s) { return cat({{3}, str(s)}); }
Bytes v1Tag(std::string_view title, std::string_view artist, std::string_view album, std::string_view year, int track, int genre) {
    Bytes b(128, 0);
    b[0] = 'T', b[1] = 'A', b[2] = 'G';
    const auto put = [&](size_t at, size_t len, std::string_view s) { std::memcpy(&b[at], s.data(), std::min(len, s.size())); };
    put(3, 30, title);
    put(33, 30, artist);
    put(63, 30, album);
    put(93, 4, year);
    b[126] = static_cast<uint8_t>(track);
    b[127] = static_cast<uint8_t>(genre);
    return b;
}
size_t id3Size(const Bytes& b) {
    if (b.size() < 10 || std::memcmp(b.data(), "ID3", 3) != 0) return 0;
    return 10 + syncsafe(&b[6]);
}
// Frames of a (not unsynchronised) v2.3 / v2.4 tag: id -> raw bodies.
std::multimap<std::string, Bytes> id3Frames(const Bytes& file) {
    std::multimap<std::string, Bytes> out;
    const size_t end = id3Size(file);
    if (!end) return out;
    const int major = file[3];
    for (size_t pos = 10; pos + 10 <= end && file[pos] != 0;) {
        const uint32_t n = major == 4 ? syncsafe(&file[pos + 4]) : be32(&file[pos + 4]);
        if (pos + 10 + n > end) break;
        out.emplace(std::string(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + 4)),
                    Bytes(file.begin() + static_cast<std::ptrdiff_t>(pos + 10), file.begin() + static_cast<std::ptrdiff_t>(pos + 10 + n)));
        pos += 10 + n;
    }
    return out;
}
std::vector<Bytes> bodies(const std::multimap<std::string, Bytes>& m, const std::string& id) {
    std::vector<Bytes> out;
    for (auto [a, b] = m.equal_range(id); a != b; ++a) out.push_back(a->second);
    return out;
}

// ---- FLAC ----------------------------------------------------------------------------------------------------------
struct FBlock {
    uint8_t type;
    Bytes data;
};
bool flacBlocks(const Bytes& f, std::vector<FBlock>& blocks, size_t& audioStart) {
    blocks.clear();
    if (f.size() < 8 || std::memcmp(f.data(), "fLaC", 4) != 0) return false;
    size_t pos = 4;
    while (pos + 4 <= f.size()) {
        const uint8_t h = f[pos];
        const size_t n = (size_t(f[pos + 1]) << 16) | (size_t(f[pos + 2]) << 8) | f[pos + 3];
        if (pos + 4 + n > f.size()) return false;
        blocks.push_back({static_cast<uint8_t>(h & 0x7F), Bytes(f.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                                                                 f.begin() + static_cast<std::ptrdiff_t>(pos + 4 + n))});
        pos += 4 + n;
        if (h & 0x80) break;
    }
    audioStart = pos;
    return true;
}
Bytes flacWith(const Bytes& f, const std::vector<FBlock>& blocks) {
    std::vector<FBlock> old;
    size_t audio = 0;
    flacBlocks(f, old, audio);
    Bytes out = str("fLaC");
    for (size_t i = 0; i < blocks.size(); ++i) {
        out.push_back(static_cast<uint8_t>(blocks[i].type | (i + 1 == blocks.size() ? 0x80 : 0)));
        put24(out, static_cast<uint32_t>(blocks[i].data.size()));
        out.insert(out.end(), blocks[i].data.begin(), blocks[i].data.end());
    }
    out.insert(out.end(), f.begin() + static_cast<std::ptrdiff_t>(audio), f.end());
    return out;
}
FBlock vorbisBlock(std::string_view vendor, const std::vector<std::string>& comments) {
    Bytes d;
    putLE32(d, static_cast<uint32_t>(vendor.size()));
    d.insert(d.end(), vendor.begin(), vendor.end());
    putLE32(d, static_cast<uint32_t>(comments.size()));
    for (const auto& c : comments) {
        putLE32(d, static_cast<uint32_t>(c.size()));
        d.insert(d.end(), c.begin(), c.end());
    }
    return {4, d};
}
std::vector<std::string> vorbisComments(const std::vector<FBlock>& blocks, std::string* vendor = nullptr) {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
        if (b.type != 4) continue;
        size_t i = 0;
        const uint32_t vn = le32(&b.data[i]);
        if (vendor) vendor->assign(reinterpret_cast<const char*>(&b.data[4]), vn);
        i = 4 + vn;
        const uint32_t count = le32(&b.data[i]);
        i += 4;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t n = le32(&b.data[i]);
            out.emplace_back(reinterpret_cast<const char*>(&b.data[i + 4]), n);
            i += 4 + n;
        }
    }
    return out;
}
FBlock pictureBlock(const Bytes& jpeg, uint32_t type) {
    Bytes d;
    put32(d, type);
    put32(d, 10);
    for (char c : std::string_view("image/jpeg")) d.push_back(static_cast<uint8_t>(c));
    put32(d, 0);
    put32(d, 64);
    put32(d, 64);
    put32(d, 24);
    put32(d, 0);
    put32(d, static_cast<uint32_t>(jpeg.size()));
    d.insert(d.end(), jpeg.begin(), jpeg.end());
    return {6, d};
}
int countType(const std::vector<FBlock>& blocks, uint8_t type) {
    return static_cast<int>(std::count_if(blocks.begin(), blocks.end(), [&](const FBlock& b) { return b.type == type; }));
}

uint8_t crc8(const Bytes& b) {
    uint8_t crc = 0;
    for (uint8_t v : b) {
        crc ^= v;
        for (int i = 0; i < 8; ++i) crc = static_cast<uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}
uint16_t crc16(const Bytes& b) {
    uint16_t crc = 0;
    for (uint8_t v : b) {
        crc ^= static_cast<uint16_t>(v << 8);
        for (int i = 0; i < 8; ++i) crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}
// A minimal FLAC stream: STREAMINFO + frames of VERBATIM subframes (16-bit stereo, 4096-sample blocks).
Bytes handFlac(const std::vector<int16_t>& pcm) {
    constexpr uint32_t bs = 4096;
    const uint64_t frames = pcm.size() / 2;
    Bytes out = str("fLaC");
    Bytes si;
    put16(si, bs);
    put16(si, bs);
    put24(si, 0);
    put24(si, 0);
    const uint64_t v = (uint64_t(kRate) << 44) | (uint64_t(1) << 41) | (uint64_t(15) << 36) | frames;
    put32(si, static_cast<uint32_t>(v >> 32));
    put32(si, static_cast<uint32_t>(v));
    si.resize(si.size() + 16, 0);   // MD5 unknown
    out.push_back(0x80);
    put24(out, static_cast<uint32_t>(si.size()));
    out.insert(out.end(), si.begin(), si.end());
    uint32_t n = 0;
    for (uint64_t f0 = 0; f0 < frames; f0 += bs, ++n) {
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(bs, frames - f0));
        const uint8_t code = count == bs ? 0xC : 0x7;
        Bytes fr{0xFF, 0xF8, static_cast<uint8_t>(code << 4 | 0x9), 0x18};
        if (n < 0x80) fr.push_back(static_cast<uint8_t>(n));
        else if (n < 0x800) fr.insert(fr.end(), {static_cast<uint8_t>(0xC0 | n >> 6), static_cast<uint8_t>(0x80 | (n & 0x3F))});
        else fr.insert(fr.end(), {static_cast<uint8_t>(0xE0 | n >> 12), static_cast<uint8_t>(0x80 | ((n >> 6) & 0x3F)),
                                  static_cast<uint8_t>(0x80 | (n & 0x3F))});
        if (code == 0x7) put16(fr, count - 1);
        fr.push_back(crc8(fr));
        for (int c = 0; c < 2; ++c) {
            fr.push_back(0x02);   // VERBATIM
            for (uint32_t i = 0; i < count; ++i) put16(fr, static_cast<uint16_t>(pcm[(f0 + i) * 2 + c]));
        }
        put16(fr, crc16(fr));
        out.insert(out.end(), fr.begin(), fr.end());
    }
    return out;
}

// ---- MP4 -----------------------------------------------------------------------------------------------------------
struct B {
    std::string type;
    size_t start, content, end;
};
std::vector<B> kids(const Bytes& b, size_t from, size_t to) {
    std::vector<B> out;
    for (size_t pos = from; pos + 8 <= to;) {
        uint64_t n = be32(&b[pos]);
        size_t head = 8;
        if (n == 1) {
            n = (uint64_t(be32(&b[pos + 8])) << 32) | be32(&b[pos + 12]);
            head = 16;
        } else if (n == 0) {
            n = to - pos;
        }
        if (n < head || pos + n > to) break;
        out.push_back({std::string(reinterpret_cast<const char*>(&b[pos + 4]), 4), pos, pos + head, static_cast<size_t>(pos + n)});
        pos += static_cast<size_t>(n);
    }
    return out;
}
std::string topOrder(const Bytes& f) {
    std::string s;
    for (const auto& b : kids(f, 0, f.size())) s += (s.empty() ? "" : " ") + b.type;
    return s;
}
const B* findBox(const std::vector<B>& v, const std::string& t) {
    for (const auto& b : v)
        if (b.type == t) return &b;
    return nullptr;
}
Bytes boxBytes(const char* type, const Bytes& content) {
    Bytes b;
    put32(b, static_cast<uint32_t>(content.size() + 8));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), content.begin(), content.end());
    return b;
}
Bytes dataAtom(uint32_t type, const Bytes& payload) {
    Bytes d{0};
    put24(d, type);
    put32(d, 0);
    d.insert(d.end(), payload.begin(), payload.end());
    return boxBytes("data", d);
}
Bytes freeform(std::string_view name, std::string_view value) {
    Bytes mean{0, 0, 0, 0};
    for (char c : std::string_view("com.apple.iTunes")) mean.push_back(static_cast<uint8_t>(c));
    Bytes nm{0, 0, 0, 0};
    for (char c : name) nm.push_back(static_cast<uint8_t>(c));
    return boxBytes("----", cat({boxBytes("mean", mean), boxBytes("name", nm), dataAtom(1, str(value))}));
}
// Appends items to the moov/udta/meta/ilst of a file whose moov is the last top-level box (so nothing that moves holds
// a chunk offset). Written independently of the tag writer. False when the layout is different.
bool appendIlstItems(const fs::path& p, const Bytes& items) {
    Bytes f = readAll(p);
    const auto top = kids(f, 0, f.size());
    if (top.empty() || top.back().type != "moov") return false;
    const B moov = top.back();
    const auto mk = kids(f, moov.content, moov.end);
    const B* udta = findBox(mk, "udta");
    if (!udta) return false;
    const B u = *udta;
    const auto uk = kids(f, u.content, u.end);
    const B* meta = findBox(uk, "meta");
    if (!meta) return false;
    const B m = *meta;
    const auto metaKids = kids(f, m.content + 4, m.end);
    const B* ilst = findBox(metaKids, "ilst");
    if (!ilst) return false;
    const B l = *ilst;
    for (size_t at : {moov.start, u.start, m.start, l.start}) {
        const uint32_t n = be32(&f[at]) + static_cast<uint32_t>(items.size());
        f[at] = static_cast<uint8_t>(n >> 24);
        f[at + 1] = static_cast<uint8_t>(n >> 16);
        f[at + 2] = static_cast<uint8_t>(n >> 8);
        f[at + 3] = static_cast<uint8_t>(n);
    }
    f.insert(f.begin() + static_cast<std::ptrdiff_t>(l.end), items.begin(), items.end());
    writeAll(p, f);
    return true;
}
// "ftyp ... mdat moov" -> "ftyp ... moov mdat" with no free box: every stco / co64 entry moves by the moov's size.
// Written independently of the tag writer (the result must decode like the source before the writer touches it).
void shiftChunkOffsets(Bytes& f, size_t from, size_t to, uint64_t delta) {
    for (const auto& b : kids(f, from, to)) {
        if (b.type == "trak" || b.type == "mdia" || b.type == "minf" || b.type == "stbl") shiftChunkOffsets(f, b.content, b.end, delta);
        if (b.type != "stco" && b.type != "co64") continue;
        const uint32_t n = be32(&f[b.content + 4]);
        const size_t w = b.type == "co64" ? 8 : 4;
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* p = &f[b.content + 8 + i * w];
            uint64_t v = 0;
            for (size_t k = 0; k < w; ++k) v = v << 8 | p[k];
            v += delta;
            for (size_t k = w; k-- > 0; v >>= 8) p[k] = static_cast<uint8_t>(v);
        }
    }
}
bool moveMoovFirst(const fs::path& src, const fs::path& dst) {
    const Bytes f = readAll(src);
    const auto top = kids(f, 0, f.size());
    const B* moov = findBox(top, "moov");
    const B* mdat = findBox(top, "mdat");
    if (!moov || !mdat || moov->start < mdat->start || moov->end != f.size()) return false;
    Bytes m(f.begin() + static_cast<std::ptrdiff_t>(moov->start), f.end());
    shiftChunkOffsets(m, 8, m.size(), m.size());
    Bytes out(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(mdat->start));
    out.insert(out.end(), m.begin(), m.end());
    out.insert(out.end(), f.begin() + static_cast<std::ptrdiff_t>(mdat->start), f.begin() + static_cast<std::ptrdiff_t>(moov->start));
    writeAll(dst, out);
    return true;
}

// XORs one byte of a file in place (offset < 0: from the end).
void flipByte(const fs::path& p, int64_t offset) {
    FILE* f = _wfopen(p.c_str(), L"r+b");
    if (!f) return;
    _fseeki64(f, offset, offset < 0 ? SEEK_END : SEEK_SET);
    const int c = fgetc(f);
    _fseeki64(f, offset, offset < 0 ? SEEK_END : SEEK_SET);
    fputc(c ^ 0x55, f);
    fclose(f);
}

bool containsBytes(const Bytes& hay, const Bytes& needle) {
    return !needle.empty() && std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// ---- helpers -------------------------------------------------------------------------------------------------------
fs::path copyAs(const fs::path& src, const wchar_t* name) {
    const fs::path dst = g_dir / name;
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return dst;
}

void dump(const char* label, const tags::Tags& t) {
    std::printf("    %s: title '%s' artist '%s' album '%s' albumArtist '%s' genre '%s' track %d/%d disc %d/%d year %d cover %zu\n",
                label, t.title.c_str(), t.artist.c_str(), t.album.c_str(), t.albumArtist.c_str(), t.genre.c_str(), t.track,
                t.trackTotal, t.disc, t.discTotal, t.year, t.cover ? t.cover->data.size() : size_t{0});
}

// The Windows property system's view (what the local library's scanner stores).
local::Entry propertyView(const fs::path& p) {
    local::Entry e;
    e.path = p.wstring();
    local::readFile(e, g_dir / L"covers");
    return e;
}

const std::string kTitleTr = "Ağlama Değmez Hayat — İçin Şarkı";
const std::string kArtistsIn = "Barış Manço;坂本龍一 ";
const std::string kArtistsOut = "Barış Manço; 坂本龍一";
const std::string kAlbumJa = "日本語のアルバム";
const std::string kAlbumArtistTr = "Çeşitli Sanatçılar";

tags::Edit fullEdit() {
    tags::Edit e;
    e.title = kTitleTr;
    e.artist = kArtistsIn;
    e.album = kAlbumJa;
    e.albumArtist = kAlbumArtistTr;
    e.track = 3;
    e.disc = 1;
    e.year = 1985;
    e.genre = "Anadolu Rock";
    return e;
}

bool fullEditApplied(const tags::Tags& t) {
    return t.title == kTitleTr && t.artist == kArtistsOut && t.album == kAlbumJa && t.albumArtist == kAlbumArtistTr &&
           t.track == 3 && t.disc == 1 && t.year == 1985 && t.genre == "Anadolu Rock";
}

tags::Edit coverEdit(const Bytes& data, const char* mime) {
    tags::Edit e;
    e.cover = tags::Edit::Cover::Set;
    e.picture.data = data;
    e.picture.mime = mime;
    return e;
}

// ===================================================================================================================

void testHelpers() {
    std::printf("[helpers]\n");
    CHECK(tags::normalizeList(" A ;B;; C ") == "A; B; C");
    CHECK(tags::id3v1Genre(17) == "Rock");
    CHECK(tags::id3v1GenreIndex("rock") == 17);
    CHECK(tags::id3v1GenreIndex("Anadolu Rock") == -1);
    CHECK(tags::writableExtension(L"a.MP3") && tags::writableExtension(L"a.flac") && tags::writableExtension(L"a.m4a"));
    CHECK(!tags::writableExtension(L"a.wav") && !tags::writableExtension(L"a.wma") && !tags::writableExtension(L"a.ogg"));
    tags::Tags t;
    t.track = 2;
    t.trackTotal = 9;
    tags::Edit e;
    e.track = 0;
    CHECK(tags::applied(t, e).track == 0 && tags::applied(t, e).trackTotal == 0);
    CHECK(tags::Edit{}.empty());
}

struct Images {
    Bytes jpeg, big, png;   // ("small" is a Windows macro)
};

void testMp3(const fs::path& wav, const Images& img) {
    std::printf("[mp3: a Media Foundation download]\n");
    const fs::path mp3 = g_dir / L"download.mp3";
    st::audio::DownloadRequest req;
    req.localPath = wav.wstring();
    req.mimeType = "audio/wav";
    req.outPath = mp3.wstring();
    req.mp3Kbps = 128;
    req.title = "Eski Başlık";
    req.artist = "Eski Sanatçı";
    req.album = "Eski Albüm";
    req.year = "2001";
    req.coverJpeg = img.jpeg;
    req.lyricsText = "Birinci satır\nİkinci satır";
    req.syncedLyrics = {{0, "Bir"}, {1500, "İki"}};
    req.replayGainDb = -4.5f;
    std::atomic<bool> cancel{false};
    CHECK(st::audio::downloadTrack(req, {}, cancel) == st::audio::DownloadStatus::Ok);
    const fs::path orig = copyAs(mp3, L"download.orig.mp3");
    const auto r0 = tags::read(mp3);
    dump("before", r0.tags);
    CHECK(r0.ok && r0.format == tags::Format::Mp3);
    CHECK(r0.tags.title == "Eski Başlık" && r0.tags.year == 2001 && r0.tags.cover && r0.tags.cover->data == img.jpeg);
    const std::string pay0 = tags::payloadHash(mp3);
    CHECK(!pay0.empty());
    const auto frames0 = id3Frames(readAll(mp3));

    auto w = tags::write(mp3, fullEdit());
    CHECK(w.status == tags::Status::Ok);
    const auto r1 = tags::read(mp3);
    dump("after", r1.tags);
    CHECK(fullEditApplied(r1.tags));
    CHECK(r1.tags.cover && r1.tags.cover->data == img.jpeg);
    CHECK(tags::payloadHash(mp3) == pay0);
    const Bytes f1 = readAll(mp3);
    CHECK(f1[3] == 3);   // still ID3v2.3
    const auto frames1 = id3Frames(f1);
    for (const char* id : {"USLT", "SYLT", "TXXX", "COMM", "TSSE", "APIC"})
        CHECK(bodies(frames0, id) == bodies(frames1, id) && !bodies(frames1, id).empty());
    CHECK(frames1.count("TIT2") == 1 && frames1.count("TYER") == 1 && frames1.count("TPE2") == 1);
    const auto gain = st::audio::replaygain::readTrackGainDb(mp3);
    CHECK(gain && std::fabs(*gain + 4.5f) < 0.01f);
    CHECK(static_cast<uint64_t>(w.size) == fs::file_size(mp3));
    CHECK(sameAudio(orig, mp3));
    CHECK(!(GetFileAttributesW(mp3.c_str()) & FILE_ATTRIBUTE_HIDDEN));
    {
        const auto e = propertyView(mp3);
        std::printf("    property system: '%s' by '%s', album '%s', track %d, year %d, cover %d\n", e.title.c_str(),
                    e.artists.empty() ? "" : e.artists[0].c_str(), e.album.c_str(), e.trackNumber, e.year, e.embeddedCover);
        CHECK(e.title == kTitleTr && e.album == kAlbumJa && e.albumArtist == kAlbumArtistTr);
        CHECK(e.artists.size() == 2 && e.artists[0] == "Barış Manço" && e.artists[1] == "坂本龍一");
        CHECK(e.trackNumber == 3 && e.year == 1985 && e.embeddedCover);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(mp3.c_str(), GetFileExInfoStandard, &fa);
        CHECK(w.size == (static_cast<int64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow) &&
              w.mtime == (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32 | fa.ftLastWriteTime.dwLowDateTime));
    }

    std::printf("[mp3: a ~2 MB cover, then a PNG, then none]\n");
    const size_t tag1 = id3Size(f1);
    w = tags::write(mp3, coverEdit(img.big, "image/jpeg"));
    CHECK(w.status == tags::Status::Ok);
    auto r2 = tags::read(mp3);
    CHECK(r2.tags.cover && r2.tags.cover->data == img.big && r2.tags.title == kTitleTr);
    CHECK(id3Size(readAll(mp3)) >= tag1 + img.big.size() - img.jpeg.size());
    CHECK(tags::payloadHash(mp3) == pay0);
    w = tags::write(mp3, coverEdit(img.png, "image/png"));
    r2 = tags::read(mp3);
    CHECK(w.status == tags::Status::Ok && r2.tags.cover && r2.tags.cover->data == img.png && r2.tags.cover->mime == "image/png");
    tags::Edit rm;
    rm.cover = tags::Edit::Cover::Remove;
    w = tags::write(mp3, rm);
    r2 = tags::read(mp3);
    CHECK(w.status == tags::Status::Ok && !r2.tags.cover && r2.tags.title == kTitleTr);
    CHECK(bodies(id3Frames(readAll(mp3)), "APIC").empty() && !bodies(id3Frames(readAll(mp3)), "SYLT").empty());
    CHECK(tags::payloadHash(mp3) == pay0);
    CHECK(sameAudio(orig, mp3));

    // The audio of the download, for hand-built tags.
    const Bytes all = readAll(orig);
    const Bytes audio(all.begin() + static_cast<std::ptrdiff_t>(id3Size(all)), all.end());

    std::printf("[mp3: ID3v2.4 (UTF-8, two artists, TDRC, POPM, PRIV, an unsynchronised frame) + ID3v1]\n");
    {
        const Bytes popm = cat({str("user@example.com"), {0, 196, 0, 0, 0, 7}});
        const Bytes priv = cat({str("WM/Provider"), {0}, str("ShadeTube")});
        const Bytes unsynced = cat({str("Test"), {0, 0xFF, 0x00, 0xE0, 0x41}});   // PRIV data FF E0 41, unsynchronised
        const Bytes frames = cat({id3Frame(4, "TIT2", utf8Text("Başlık 2.4")), id3Frame(4, "TPE1", cat({utf8Text("Bir"), {0}, str("İki")})),
                                  id3Frame(4, "TDRC", latin("2019-05-03")), id3Frame(4, "POPM", popm), id3Frame(4, "PRIV", priv),
                                  id3Frame(4, "PRIV", unsynced, 0, 0x02), id3Frame(4, "TCON", latin("17"))});
        const fs::path p = g_dir / L"v24.mp3";
        writeAll(p, cat({id3Tag(4, frames, 512), audio, v1Tag("Baslik", "Bir", "Album", "2019", 4, 17)}));
        const auto before = tags::read(p);
        dump("before", before.tags);
        CHECK(before.ok && before.tags.title == "Başlık 2.4" && before.tags.artist == "Bir; İki" && before.tags.year == 2019 &&
              before.tags.genre == "Rock" && before.tags.track == 4);
        const std::string pay = tags::payloadHash(p);
        tags::Edit e;
        e.year = 2021;
        e.title = "Yeni Başlık ş";
        w = tags::write(p, e);
        CHECK(w.status == tags::Status::Ok);
        const Bytes f = readAll(p);
        const auto fr = id3Frames(f);
        dump("after", tags::read(p).tags);
        CHECK(f[3] == 4);
        CHECK(bodies(fr, "TDRC").size() == 1 && bodies(fr, "TDRC")[0] == latin("2021-05-03"));
        CHECK(bodies(fr, "POPM") == std::vector<Bytes>{popm});
        CHECK(bodies(fr, "PRIV").size() == 2 && bodies(fr, "PRIV")[1] == unsynced);
        CHECK(bodies(fr, "TPE1") == bodies(id3Frames(cat({id3Tag(4, frames, 0)})), "TPE1"));
        const Bytes v1(f.end() - 128, f.end());
        CHECK(std::memcmp(v1.data(), "TAG", 3) == 0 && std::string(v1.begin() + 3, v1.begin() + 16) == "Yeni Baslik s");
        CHECK(std::string(v1.begin() + 93, v1.begin() + 97) == "2021" && v1[126] == 4 && v1[127] == 17);
        CHECK(tags::payloadHash(p) == pay);
        const auto after = tags::read(p).tags;
        CHECK(after.year == 2021 && after.title == "Yeni Başlık ş" && after.artist == "Bir; İki" && after.track == 4);
    }

    std::printf("[mp3: ID3v2.2 becomes v2.3]\n");
    {
        const Bytes com = cat({{0}, str("eng"), {0}, str("a comment")});
        const Bytes pic = cat({{0}, str("JPG"), {3}, {0}, img.jpeg});
        const Bytes frames = cat({id3Frame(2, "TT2", latin("Old v22")), id3Frame(2, "TP1", latin("Artist")),
                                  id3Frame(2, "COM", com), id3Frame(2, "PIC", pic)});
        const fs::path p = g_dir / L"v22.mp3";
        writeAll(p, cat({id3Tag(2, frames, 64), audio}));
        const auto before = tags::read(p);
        CHECK(before.ok && before.tags.title == "Old v22" && before.tags.cover && before.tags.cover->data == img.jpeg);
        tags::Edit e;
        e.album = "Yeni";
        CHECK(tags::write(p, e).status == tags::Status::Ok);
        const Bytes f = readAll(p);
        const auto fr = id3Frames(f);
        CHECK(f[3] == 3 && bodies(fr, "TIT2").size() == 1 && bodies(fr, "COMM") == std::vector<Bytes>{com});
        const auto after = tags::read(p).tags;
        CHECK(after.title == "Old v22" && after.album == "Yeni" && after.cover && after.cover->data == img.jpeg);
        CHECK(sameAudio(orig, p));
    }

    std::printf("[mp3: no tag at all / only ID3v1]\n");
    {
        const fs::path p = g_dir / L"bare.mp3";
        writeAll(p, audio);
        tags::Edit e;
        e.title = "Çıplak";
        CHECK(tags::write(p, e).status == tags::Status::Ok);
        const Bytes f = readAll(p);
        CHECK(f[0] == 'I' && f[3] == 3 && tags::read(p).tags.title == "Çıplak");
        CHECK(Bytes(f.begin() + static_cast<std::ptrdiff_t>(id3Size(f)), f.end()) == audio);
        const fs::path q = g_dir / L"v1only.mp3";
        writeAll(q, cat({audio, v1Tag("V1 Title", "V1 Artist", "", "1999", 7, 13)}));
        const auto before = tags::read(q).tags;
        CHECK(before.title == "V1 Title" && before.artist == "V1 Artist" && before.year == 1999 && before.track == 7 && before.genre == "Pop");
        tags::Edit a;
        a.album = "Eklendi";
        CHECK(tags::write(q, a).status == tags::Status::Ok);
        const Bytes g = readAll(q);
        const auto fr = id3Frames(g);
        CHECK(bodies(fr, "TIT2").size() == 1 && bodies(fr, "TALB").size() == 1);
        const auto after = tags::read(q).tags;
        CHECK(after.title == "V1 Title" && after.album == "Eklendi" && after.track == 7 && after.year == 1999);
        CHECK(std::string(g.end() - 128 + 63, g.end() - 128 + 70) == "Eklendi");
        CHECK(sameAudio(orig, q));
    }

    std::printf("[mp3: padding absorbs a small edit (the audio stays put)]\n");
    {
        const fs::path p = g_dir / L"padded.mp3";
        const Bytes tag = id3Tag(3, cat({id3Frame(3, "TIT2", latin("Kisa")), id3Frame(3, "TPE1", latin("A"))}), 4096);
        writeAll(p, cat({tag, audio}));
        tags::Edit e;
        e.title = "Biraz daha uzun bir başlık";
        e.genre = "Caz";
        CHECK(tags::write(p, e).status == tags::Status::Ok);
        const Bytes f = readAll(p);
        CHECK(f.size() == tag.size() + audio.size() && id3Size(f) == tag.size());
        CHECK(tags::read(p).tags.title == "Biraz daha uzun bir başlık");
    }

    std::printf("[mp3: a tag-wide unsynchronisation is undone]\n");
    {
        const Bytes priv = cat({str("x"), {0, 0xFF, 0xE0, 0xFF, 0x00, 0x12}});
        Bytes frames = cat({id3Frame(3, "TIT2", latin("Unsync")), id3Frame(3, "PRIV", priv)});
        Bytes unsynced;
        for (size_t i = 0; i < frames.size(); ++i) {
            unsynced.push_back(frames[i]);
            if (frames[i] == 0xFF && (i + 1 == frames.size() || frames[i + 1] >= 0xE0 || frames[i + 1] == 0)) unsynced.push_back(0);
        }
        const fs::path p = g_dir / L"unsync.mp3";
        writeAll(p, cat({id3Tag(3, unsynced, 0, 0x80), audio}));
        CHECK(tags::read(p).tags.title == "Unsync");
        tags::Edit e;
        e.artist = "Sanatçı";
        CHECK(tags::write(p, e).status == tags::Status::Ok);
        const Bytes f = readAll(p);
        CHECK(f[5] == 0 && bodies(id3Frames(f), "PRIV") == std::vector<Bytes>{priv});
        CHECK(tags::read(p).tags.title == "Unsync" && tags::read(p).tags.artist == "Sanatçı");
    }

    std::printf("[mp3: corrupt tags are refused, the file untouched]\n");
    {
        const fs::path p = g_dir / L"junk.mp3";
        Bytes body = id3Frame(3, "TIT2", latin("Junk"));
        body.insert(body.end(), {'x', 'y', 'z', '!', 1, 2, 3});
        const Bytes original = cat({id3Tag(3, body, 32), audio});
        writeAll(p, original);
        tags::Edit e;
        e.title = "Değişmemeli";
        CHECK(tags::write(p, e).status == tags::Status::Corrupt);
        CHECK(readAll(p) == original);
        const fs::path q = g_dir / L"truncated.mp3";
        Bytes trunc = id3Tag(3, id3Frame(3, "TIT2", latin("T")), 0);
        trunc[6] = 0x7F;   // claims a tag far longer than the file
        writeAll(q, trunc);
        CHECK(tags::write(q, e).status == tags::Status::Corrupt);
        CHECK(readAll(q) == trunc);
        CHECK(!tags::read(q).ok);
        CHECK(noLeftovers());
    }
}

void testSafety(const fs::path& sourceMp3, const fs::path& wav) {
    std::printf("[safety: read-only]\n");
    {
        const fs::path p = copyAs(sourceMp3, L"readonly.mp3");
        const Bytes before = readAll(p);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
        tags::Edit e;
        e.title = "X";
        CHECK(tags::write(p, e).status == tags::Status::ReadOnly);
        CHECK(readAll(p) == before && (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_READONLY));
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        CHECK(noLeftovers());
    }
    std::printf("[safety: open in another program without delete sharing]\n");
    {
        const fs::path p = copyAs(sourceMp3, L"inuse.mp3");
        const Bytes before = readAll(p);
        const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        tags::Edit e;
        e.title = "X";
        CHECK(tags::write(p, e).status == tags::Status::InUse);
        CloseHandle(h);
        CHECK(readAll(p) == before);
        CHECK(noLeftovers());
    }
    std::printf("[safety: the playing file (FILE_SHARE_DELETE + memory-mapped, like the audio engine)]\n");
    {
        const fs::path p = copyAs(sourceMp3, L"playing.mp3");
        const Bytes before = readAll(p);
        const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        const auto* view = static_cast<const uint8_t*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
        tags::Edit e;
        e.title = "Çalarken değişti";
        CHECK(tags::write(p, e).status == tags::Status::Ok);
        CHECK(view && std::memcmp(view, before.data(), before.size()) == 0);   // the old data keeps playing
        UnmapViewOfFile(view);
        CloseHandle(m);
        CloseHandle(h);
        CHECK(tags::read(p).tags.title == "Çalarken değişti");
        CHECK(noLeftovers());
    }
    std::printf("[safety: a damaged temporary file is caught by the check]\n");
    {
        const fs::path p = copyAs(sourceMp3, L"tamper.mp3");
        const Bytes before = readAll(p);
        tags::Edit e;
        e.title = "X";
        tags::WriteOptions o;
        // (the temporary is hidden: opened as an existing file, "r+b", not re-created)
        o.tamper = [](const fs::path& temp) { flipByte(temp, -500); };   // a byte of the audio
        const auto w = tags::write(p, e, o);
        std::printf("    %s: %s\n", tags::toString(w.status), w.detail.c_str());
        CHECK(w.status == tags::Status::VerifyFailed);
        o.tamper = [](const fs::path& temp) { flipByte(temp, 15); };   // inside the new tag
        CHECK(tags::write(p, e, o).status == tags::Status::VerifyFailed);
        CHECK(readAll(p) == before);
        CHECK(noLeftovers());
    }
    std::printf("[safety: the file changed while it was written]\n");
    {
        static fs::path changed;
        changed = copyAs(sourceMp3, L"changed.mp3");
        tags::Edit e;
        e.title = "X";
        tags::WriteOptions o;
        o.tamper = [](const fs::path&) {
            std::ofstream f(changed, std::ios::binary | std::ios::app);
            f.put('!');
        };
        CHECK(tags::write(changed, e, o).status == tags::Status::Changed);
        const Bytes after = readAll(changed);
        CHECK(!after.empty() && after.back() == '!');
        CHECK(noLeftovers());
    }
    std::printf("[safety: nothing to change / missing / unsupported]\n");
    {
        const fs::path p = copyAs(sourceMp3, L"same.mp3");
        const auto t = tags::read(p).tags;
        const auto mtime = fs::last_write_time(p);
        tags::Edit e;
        e.title = t.title;
        CHECK(tags::write(p, e).status == tags::Status::Ok && fs::last_write_time(p) == mtime);
        CHECK(tags::write(g_dir / L"missing.mp3", e).status == tags::Status::NotFound);
        CHECK(tags::write(wav, e).status == tags::Status::Unsupported);
        const fs::path fake = g_dir / L"fake.m4a";
        writeAll(fake, str("this is not an mp4 file at all"));
        CHECK(tags::write(fake, e).status == tags::Status::Unsupported);
        CHECK(noLeftovers());
    }
}

void flacChecks(const char* label, const Bytes& base, const Images& img) {
    std::printf("[flac (%s): padding absorbs the edit]\n", label);
    std::vector<FBlock> blocks;
    size_t audioStart = 0;
    CHECK(flacBlocks(base, blocks, audioStart));
    if (blocks.empty()) return;
    const Bytes app = cat({str("TEST"), str("application data")});
    const std::vector<std::string> comments = {"TITLE=Eski", "ARTIST=A", "ARTIST=B", "REPLAYGAIN_TRACK_GAIN=-6.50 dB",
                                               "LYRICS=Söz satırı", "CUSTOM=değer", "TRACKNUMBER=2", "TRACKTOTAL=9",
                                               "DATE=2019-05-03"};
    std::vector<FBlock> mine{blocks[0]};
    for (size_t i = 1; i < blocks.size(); ++i)
        if (blocks[i].type == 3) mine.push_back(blocks[i]);   // keep a SEEKTABLE
    mine.push_back({2, app});
    mine.push_back(vorbisBlock("test vendor 1.0", comments));
    mine.push_back(pictureBlock(img.jpeg, 3));
    mine.push_back({1, Bytes(8192, 0)});
    const Bytes withMeta = flacWith(base, mine);
    const fs::path p = g_dir / (toWide(label) + L".flac");
    writeAll(p, withMeta);
    const fs::path orig = g_dir / (toWide(label) + L".orig.flac");
    writeAll(orig, withMeta);
    const auto r0 = tags::read(p);
    dump("before", r0.tags);
    CHECK(r0.ok && r0.format == tags::Format::Flac && r0.tags.title == "Eski" && r0.tags.artist == "A; B" &&
          r0.tags.track == 2 && r0.tags.trackTotal == 9 && r0.tags.year == 2019 && r0.tags.cover);
    const std::string pay = tags::payloadHash(p);
    auto w = tags::write(p, fullEdit());
    CHECK(w.status == tags::Status::Ok);
    Bytes f = readAll(p);
    std::vector<FBlock> nb;
    size_t na = 0;
    flacBlocks(f, nb, na);
    CHECK(f.size() == withMeta.size());   // the padding took the change
    size_t oa = 0;
    std::vector<FBlock> ob;
    flacBlocks(withMeta, ob, oa);
    CHECK(na == oa);
    std::string vendor;
    const auto c = vorbisComments(nb, &vendor);
    const auto has = [&](const std::string& s) { return std::find(c.begin(), c.end(), s) != c.end(); };
    CHECK(vendor == "test vendor 1.0");
    CHECK(has("REPLAYGAIN_TRACK_GAIN=-6.50 dB") && has("LYRICS=Söz satırı") && has("CUSTOM=değer"));
    CHECK(has("TITLE=" + kTitleTr) && has("ARTIST=Barış Manço") && has("ARTIST=坂本龍一") && has("ALBUM=" + kAlbumJa));
    CHECK(has("DATE=1985-05-03") && has("TRACKNUMBER=3") && has("TRACKTOTAL=9") && has("ALBUMARTIST=" + kAlbumArtistTr));
    CHECK(countType(nb, 2) == 1 && nb[static_cast<size_t>(std::find_if(nb.begin(), nb.end(), [](const FBlock& b) { return b.type == 2; }) - nb.begin())].data == app);
    CHECK(nb[0].data == blocks[0].data);
    const auto r1 = tags::read(p);
    dump("after", r1.tags);
    CHECK(fullEditApplied(r1.tags) && r1.tags.trackTotal == 9 && r1.tags.cover && r1.tags.cover->data == img.jpeg);
    CHECK(tags::payloadHash(p) == pay);
    const auto gain = st::audio::replaygain::readTrackGainDb(p);
    CHECK(gain && std::fabs(*gain + 6.5f) < 0.01f);
    CHECK(sameAudio(orig, p));
    {
        const auto e = propertyView(p);
        std::printf("    property system: '%s', album '%s', track %d, year %d\n", e.title.c_str(), e.album.c_str(), e.trackNumber, e.year);
        CHECK(e.title == kTitleTr && e.album == kAlbumJa && e.trackNumber == 3 && e.year == 1985);
    }

    std::printf("[flac (%s): a ~2 MB cover doesn't fit: rewritten with new padding]\n", label);
    w = tags::write(p, coverEdit(img.big, "image/jpeg"));
    CHECK(w.status == tags::Status::Ok);
    f = readAll(p);
    flacBlocks(f, nb, na);
    CHECK(f.size() > withMeta.size() && countType(nb, 6) == 1 && nb.back().type == 1 && nb.back().data.size() == 4096);
    auto r2 = tags::read(p);
    CHECK(r2.tags.cover && r2.tags.cover->data == img.big && r2.tags.title == kTitleTr);
    CHECK(tags::payloadHash(p) == pay);
    tags::Edit rm;
    rm.cover = tags::Edit::Cover::Remove;
    rm.track = 0;
    CHECK(tags::write(p, rm).status == tags::Status::Ok);
    f = readAll(p);
    flacBlocks(f, nb, na);
    r2 = tags::read(p);
    CHECK(countType(nb, 6) == 0 && !r2.tags.cover && r2.tags.track == 0 && r2.tags.trackTotal == 0);
    const auto c2 = vorbisComments(nb);
    CHECK(std::none_of(c2.begin(), c2.end(), [](const std::string& s) { return s.starts_with("TRACK"); }));
    CHECK(tags::payloadHash(p) == pay);
    CHECK(sameAudio(orig, p));

    std::printf("[flac (%s): no padding at all]\n", label);
    const fs::path q = g_dir / (toWide(label) + L".nopad.flac");
    std::vector<FBlock> bare{blocks[0], vorbisBlock("v", {"TITLE=Dar"})};
    writeAll(q, flacWith(base, bare));
    tags::Edit e;
    e.title = "Artık daha uzun bir başlık";
    CHECK(tags::write(q, e).status == tags::Status::Ok);
    f = readAll(q);
    flacBlocks(f, nb, na);
    CHECK(nb.back().type == 1 && nb.back().data.size() == 4096 && tags::read(q).tags.title == "Artık daha uzun bir başlık");
    CHECK(sameAudio(orig, q));
    std::printf("[flac (%s): no comment block yet]\n", label);
    const fs::path r = g_dir / (toWide(label) + L".novorbis.flac");
    writeAll(r, flacWith(base, {blocks[0]}));
    CHECK(tags::write(r, fullEdit()).status == tags::Status::Ok);
    CHECK(fullEditApplied(tags::read(r).tags));
    CHECK(sameAudio(orig, r));
    CHECK(noLeftovers());
}

void testFlac(const std::vector<int16_t>& pcm, const fs::path& wav, const Images& img) {
    const fs::path mf = g_dir / L"mf.flac";
    if (mfEncode(mf, pcm, MFAudioFormat_FLAC, 0) && fs::exists(mf) && readAll(mf).size() > 4 && readAll(mf)[0] == 'f') {
        std::printf("[flac: Media Foundation's encoder]\n");
        flacChecks("mf", readAll(mf), img);
    } else if (ffmpegAvailable() && ffmpeg(L"-i \"" + wav.wstring() + L"\" \"" + (g_dir / L"ff.flac").wstring() + L"\"")) {
        std::printf("[flac: no Media Foundation FLAC sink; ffmpeg's encoder]\n");
        flacChecks("ffmpeg", readAll(g_dir / L"ff.flac"), img);
    } else {
        std::printf("[flac: neither a Media Foundation FLAC sink nor ffmpeg]\n");
    }
    flacChecks("hand", handFlac(pcm), img);

    std::printf("[flac: corrupt]\n");
    const fs::path p = g_dir / L"corrupt.flac";
    Bytes bad = handFlac(pcm);
    bad[5] = 0x7F;   // a STREAMINFO length past the end of the file
    bad[6] = 0xFF;
    writeAll(p, bad);
    tags::Edit e;
    e.title = "X";
    CHECK(tags::write(p, e).status == tags::Status::Corrupt);
    CHECK(readAll(p) == bad);
    CHECK(noLeftovers());
}

void mp4Checks(const char* label, const fs::path& source, const Images& img, bool injectItems) {
    std::printf("[m4a (%s): %s]\n", label, topOrder(readAll(source)).c_str());
    const fs::path p = g_dir / (toWide(label) + L".m4a");
    std::error_code ec;
    fs::copy_file(source, p, fs::copy_options::overwrite_existing, ec);
    const std::string pay = tags::payloadHash(p);
    CHECK(!pay.empty());
    auto w = tags::write(p, fullEdit());
    std::printf("    %s %s\n", tags::toString(w.status), w.detail.c_str());
    CHECK(w.status == tags::Status::Ok);
    auto r = tags::read(p);
    dump("after", r.tags);
    CHECK(r.ok && r.format == tags::Format::Mp4 && fullEditApplied(r.tags));
    CHECK(tags::payloadHash(p) == pay);
    CHECK(sameAudio(source, p));
    {
        const auto e = propertyView(p);
        std::printf("    property system: '%s', album '%s', album artist '%s', track %d, year %d\n", e.title.c_str(), e.album.c_str(),
                    e.albumArtist.c_str(), e.trackNumber, e.year);
        CHECK(e.title == kTitleTr && e.album == kAlbumJa && e.trackNumber == 3 && e.year == 1985);
    }
    if (injectItems) {
        const Bytes rg = freeform("replaygain_track_gain", "-7.25 dB");
        const Bytes lyr = boxBytes("\xA9lyr", dataAtom(1, str("Söz\nsatırları")));
        const bool injected = appendIlstItems(p, cat({rg, lyr}));
        if (!injected) std::printf("    (ilst layout: %s)\n", topOrder(readAll(p)).c_str());
        if (injected) {
            std::printf("    + freeform ReplayGain and \xC2\xA9lyr items\n");
            CHECK(st::audio::replaygain::readTrackGainDb(p).has_value());
            w = tags::write(p, coverEdit(img.jpeg, "image/jpeg"));
            CHECK(w.status == tags::Status::Ok);
            tags::Edit t;
            t.title = "İkinci düzenleme";
            CHECK(tags::write(p, t).status == tags::Status::Ok);
            const Bytes f = readAll(p);
            CHECK(containsBytes(f, rg) && containsBytes(f, lyr));
            const auto gain = st::audio::replaygain::readTrackGainDb(p);
            CHECK(gain && std::fabs(*gain + 7.25f) < 0.01f);
            r = tags::read(p);
            CHECK(r.tags.title == "İkinci düzenleme" && r.tags.album == kAlbumJa && r.tags.cover && r.tags.cover->data == img.jpeg);
            CHECK(tags::payloadHash(p) == pay);
            CHECK(sameAudio(source, p));
        }
    }
    std::printf("[m4a (%s): a ~2 MB cover, then less]\n", label);
    const size_t size0 = readAll(p).size();
    w = tags::write(p, coverEdit(img.big, "image/jpeg"));
    CHECK(w.status == tags::Status::Ok);
    r = tags::read(p);
    CHECK(r.tags.cover && r.tags.cover->data == img.big);
    CHECK(tags::payloadHash(p) == pay);
    CHECK(sameAudio(source, p));
    w = tags::write(p, coverEdit(img.png, "image/png"));
    r = tags::read(p);
    CHECK(w.status == tags::Status::Ok && r.tags.cover && r.tags.cover->data == img.png && r.tags.cover->mime == "image/png");
    const Bytes afterShrink = readAll(p);
    std::printf("    %s\n", topOrder(afterShrink).c_str());
    CHECK(sameAudio(source, p));
    // A little more text: a free box right behind moov (left by the shrink when moov comes first) absorbs it.
    tags::Edit t;
    t.genre = "Biraz daha uzun bir tür adı";
    CHECK(tags::write(p, t).status == tags::Status::Ok);
    const Bytes afterGrow = readAll(p);
    const auto top = kids(afterGrow, 0, afterGrow.size());
    const B* moovBox = findBox(top, "moov");
    const B* mdatBox = findBox(top, "mdat") ? findBox(top, "mdat") : findBox(top, "moof");
    const bool moovFirst = moovBox && mdatBox && moovBox->start < mdatBox->start;
    if (moovFirst) CHECK(afterGrow.size() == afterShrink.size());
    CHECK(tags::read(p).tags.genre == "Biraz daha uzun bir tür adı" && tags::payloadHash(p) == pay);
    CHECK(sameAudio(source, p));
    tags::Edit rm;
    rm.cover = tags::Edit::Cover::Remove;
    rm.title = "";
    CHECK(tags::write(p, rm).status == tags::Status::Ok);
    r = tags::read(p);
    CHECK(!r.tags.cover && r.tags.title.empty() && r.tags.album == kAlbumJa);
    CHECK(sameAudio(source, p));
    (void)size0;
    CHECK(noLeftovers());
}

void testMp4(const std::vector<int16_t>& pcm, const fs::path& wav, const Images& img) {
    const fs::path after = g_dir / L"aac-mf.m4a";
    if (mfEncode(after, pcm, MFAudioFormat_AAC, 16000)) mp4Checks("aac", after, img, true);
    else CHECK(!"Media Foundation AAC encode failed");
    const fs::path before = g_dir / L"aac-mf-moovfirst.m4a";
    if (moveMoovFirst(after, before)) {
        std::printf("[m4a: moov moved in front of mdat, no free box]\n");
        CHECK(sameAudio(after, before));
        mp4Checks("aac-moovfirst", before, img, false);
    }
    const fs::path alac = g_dir / L"alac-mf.m4a";
    if (mfEncode(alac, pcm, MFAudioFormat_ALAC, 0)) mp4Checks("alac", alac, img, false);
    else std::printf("[m4a: no ALAC encoder]\n");
    const fs::path frag = g_dir / L"aac-mf-fragmented.mp4";
    if (mfEncode(frag, pcm, MFAudioFormat_AAC, 16000, true)) mp4Checks("aac-fragmented", frag, img, false);
    else std::printf("[m4a: no fragmented MP4 sink]\n");
    if (ffmpegAvailable()) {
        const fs::path fast = g_dir / L"aac-ff-faststart.m4a", ffrag = g_dir / L"aac-ff-frag.m4a";
        if (ffmpeg(L"-i \"" + wav.wstring() + L"\" -c:a aac -b:a 128k -movflags +faststart \"" + fast.wstring() + L"\""))
            mp4Checks("ffmpeg-faststart", fast, img, false);
        if (ffmpeg(L"-i \"" + wav.wstring() + L"\" -c:a aac -b:a 128k -movflags frag_keyframe+empty_moov -frag_duration 500000 \"" +
                   ffrag.wstring() + L"\""))
            mp4Checks("ffmpeg-fragmented", ffrag, img, false);
    } else {
        std::printf("[m4a: ffmpeg not on PATH, its variants skipped]\n");
    }
    std::printf("[m4a: corrupt]\n");
    const Bytes good = readAll(after);
    const fs::path p = g_dir / L"truncated.m4a";
    const Bytes bad(good.begin(), good.end() - 100);
    writeAll(p, bad);
    tags::Edit e;
    e.title = "X";
    CHECK(tags::write(p, e).status == tags::Status::Corrupt);
    CHECK(readAll(p) == bad);
    CHECK(noLeftovers());
}


} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // nothing lost if a check crashes
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        std::printf("\nCRASH: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    const bool keep = argc > 1 && std::wstring(argv[1]) == L"--keep";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("MFStartup failed\n");
        return 2;
    }
    g_dir = fs::temp_directory_path() / (L"ShadeTube-tags-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    std::printf("work folder: %s\n", toUtf8(g_dir.wstring()).c_str());

    testHelpers();
    const auto pcm = sinePcm(3.0, 440);
    const fs::path wav = g_dir / L"source.wav";
    writeWav(wav, pcm);
    Images img;
    img.jpeg = encodeImage(300, 300, false, GUID_ContainerFormatJpeg);
    img.big = encodeImage(1400, 1400, true, GUID_ContainerFormatJpeg);
    img.png = encodeImage(200, 120, false, GUID_ContainerFormatPng);
    std::printf("[images] jpeg %zu bytes, large jpeg %zu bytes, png %zu bytes\n", img.jpeg.size(), img.big.size(), img.png.size());
    CHECK(img.jpeg.size() > 1000 && img.big.size() > 1'500'000 && img.png.size() > 100);
    CHECK(tags::imageSize(img.jpeg) == std::make_pair(300, 300) && tags::imageSize(img.png) == std::make_pair(200, 120));

    const auto t0 = std::chrono::steady_clock::now();
    testMp3(wav, img);
    testSafety(g_dir / L"download.orig.mp3", wav);
    testFlac(pcm, wav, img);
    testMp4(pcm, wav, img);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("(%lld ms)\n", static_cast<long long>(ms));

    MFShutdown();
    if (!keep) fs::remove_all(g_dir, ec);
    else std::printf("kept %s\n", toUtf8(g_dir.wstring()).c_str());
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
