#include "audio/Downloader.h"

#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace st::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int64_t kHnsPerSecond = 10'000'000;
constexpr int64_t kFadeMs = 8;              // raised-cosine fade-out before / fade-in after every splice
constexpr int64_t kResyncToleranceMs = 50;  // decoder timestamp vs. frame counter drift that means a real gap
constexpr int64_t kMinKeptMs = 10'000;      // cuts that would leave less than this are bogus data: ignored

// RAII COM + Media Foundation for one download worker call.
struct MfScope {
    bool ok = false;
    HRESULT co = E_FAIL;
    MfScope() {
        co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE on an STA thread: not ours
        ok = SUCCEEDED(MFStartup(MF_VERSION));
    }
    ~MfScope() {
        if (ok) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();   // balance only our own init
    }
};

// A sibling temp path that KEEPS the .mp3 extension — MFCreateSinkWriterFromURL picks the output container
// from the extension, so ".part" would leave it with no MP3 sink.
std::wstring tempSibling(const std::wstring& path) {
    std::filesystem::path p(path);
    return (p.parent_path() / (p.stem().wstring() + L".part" + p.extension().wstring())).wstring();
}

std::shared_ptr<ProgressiveBuffer> openSource(const DownloadRequest& req) {
    // A local file ignores the URL; ProgressiveBuffer reads it into memory with the same machinery.
    auto buffer = std::make_shared<ProgressiveBuffer>(req.localPath.empty() ? req.url : std::string{}, req.localPath,
                                                      req.contentLength, nullptr);
    buffer->start();
    return buffer;
}

void closeSource(const std::shared_ptr<ProgressiveBuffer>& buffer) {
    buffer->cancel();
    buffer->release();
}

// ---- ID3v2.3 tag builder -------------------------------------------------------------------------------
void putBE32(std::vector<uint8_t>& v, uint32_t n) {
    v.push_back(static_cast<uint8_t>(n >> 24));
    v.push_back(static_cast<uint8_t>(n >> 16));
    v.push_back(static_cast<uint8_t>(n >> 8));
    v.push_back(static_cast<uint8_t>(n));
}
void putSynchsafe(std::vector<uint8_t>& v, uint32_t n) {
    v.push_back((n >> 21) & 0x7F);
    v.push_back((n >> 14) & 0x7F);
    v.push_back((n >> 7) & 0x7F);
    v.push_back(n & 0x7F);
}
// ID3v2.3 knows only ISO-8859-1 (0) and UTF-16 with BOM (1); UTF-8 (3) is v2.4 and strict readers (the Windows
// property system -> Explorer, SMTC) ignore such frames. So text is UTF-16LE with a BOM.
void textFrame(std::vector<uint8_t>& frames, const char* id, const std::string& utf8) {
    if (utf8.empty()) return;
    std::vector<uint8_t> data;
    data.push_back(0x01); // UTF-16 with BOM
    data.push_back(0xFF);
    data.push_back(0xFE);
    for (wchar_t c : toWide(utf8)) {
        data.push_back(static_cast<uint8_t>(c & 0xFF));
        data.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    }
    frames.insert(frames.end(), id, id + 4);
    putBE32(frames, static_cast<uint32_t>(data.size()));
    frames.push_back(0);
    frames.push_back(0); // frame flags
    frames.insert(frames.end(), data.begin(), data.end());
}
void apicFrame(std::vector<uint8_t>& frames, const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty()) return;
    std::vector<uint8_t> data;
    data.push_back(0x00); // description encoding: ISO-8859-1 (the description is empty)
    const char* mime = "image/jpeg";
    data.insert(data.end(), mime, mime + 10);
    data.push_back(0);    // mime terminator
    data.push_back(0x03); // picture type: front cover
    data.push_back(0);    // empty description terminator
    data.insert(data.end(), jpeg.begin(), jpeg.end());
    const char* id = "APIC";
    frames.insert(frames.end(), id, id + 4);
    putBE32(frames, static_cast<uint32_t>(data.size()));
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), data.begin(), data.end());
}
uint32_t readBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The MF MP3 sink writes its own small ID3v2.3 tag (TFLT "MPG/3" + a COMM "iTunSMPB" frame carrying the
// encoder delay/padding for gapless players). Stacking our tag in front of it would leave two tags in the
// file, which many taggers don't expect. Returns the byte size of such a leading tag (0 = none / not a plain
// v2.3 tag) and appends its frames that we don't write ourselves to `keep`.
size_t takeSinkTag(std::ifstream& in, std::vector<uint8_t>& keep) {
    uint8_t h[10]{};
    in.read(reinterpret_cast<char*>(h), sizeof h);
    const bool plainV23 = in.gcount() == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3' && h[3] == 3 && h[5] == 0 &&
                          !((h[6] | h[7] | h[8] | h[9]) & 0x80);
    in.clear();
    in.seekg(0);
    if (!plainV23) return 0;
    const size_t size = (size_t(h[6]) << 21) | (size_t(h[7]) << 14) | (size_t(h[8]) << 7) | size_t(h[9]);
    if (size > (1u << 20)) return 0;   // not the tiny sink tag: leave the file alone
    std::vector<uint8_t> body(size);
    in.seekg(10);
    in.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(size));
    const bool ok = static_cast<size_t>(in.gcount()) == size;
    in.clear();
    in.seekg(0);
    if (!ok) return 0;
    static constexpr const char* kOurs[] = {"TIT2", "TPE1", "TALB", "TYER", "TSSE", "APIC"};
    size_t pos = 0;
    while (pos + 10 <= size && body[pos] != 0) {   // a zero byte starts the padding
        const uint32_t len = readBE32(&body[pos + 4]);
        if (len > size - pos - 10) break;          // malformed: keep what we have
        bool ours = false;
        for (const char* id : kOurs) ours = ours || std::memcmp(&body[pos], id, 4) == 0;
        if (!ours) keep.insert(keep.end(), body.begin() + static_cast<std::ptrdiff_t>(pos),
                               body.begin() + static_cast<std::ptrdiff_t>(pos + 10 + len));
        pos += 10 + len;
    }
    return 10 + size;
}

std::vector<uint8_t> buildId3(const DownloadRequest& req, const std::vector<uint8_t>& extraFrames) {
    std::vector<uint8_t> frames;
    textFrame(frames, "TIT2", req.title);
    textFrame(frames, "TPE1", req.artist);
    textFrame(frames, "TALB", req.album);
    textFrame(frames, "TYER", req.year.size() >= 4 ? req.year.substr(0, 4) : req.year);
    textFrame(frames, "TSSE", "ShadeTube");
    apicFrame(frames, req.coverJpeg);
    frames.insert(frames.end(), extraFrames.begin(), extraFrames.end());
    if (frames.empty()) return {};
    std::vector<uint8_t> tag;
    tag.push_back('I');
    tag.push_back('D');
    tag.push_back('3');
    tag.push_back(0x03); // v2.3
    tag.push_back(0x00);
    tag.push_back(0x00); // flags
    putSynchsafe(tag, static_cast<uint32_t>(frames.size()));
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

// ---- download the raw stream into memory (throttle-safe, cancellable) ----------------------------------
DownloadStatus fetchAll(const DownloadRequest& req, const std::function<void(float)>& progress,
                        const std::atomic<bool>& cancel, std::vector<uint8_t>& out, float progressShare) {
    auto buffer = openSource(req);
    const int64_t length = buffer->waitForLength();
    if (length <= 0 || buffer->failed()) {
        closeSource(buffer);
        return DownloadStatus::NetworkError;
    }
    out.resize(static_cast<size_t>(length));
    int64_t offset = 0;
    const int64_t chunk = 256 * 1024;
    DownloadStatus status = DownloadStatus::Ok;
    while (offset < length) {
        if (cancel.load()) {
            status = DownloadStatus::Cancelled;
            break;
        }
        const int64_t n = std::min(chunk, length - offset);
        const auto rs = buffer->read(offset, out.data() + offset, static_cast<size_t>(n));
        if (rs != ProgressiveBuffer::ReadStatus::Ok) {
            status = buffer->failed() ? DownloadStatus::NetworkError : DownloadStatus::Cancelled;
            break;
        }
        offset += n;
        if (progress) progress(progressShare * static_cast<float>(offset) / static_cast<float>(length));
    }
    closeSource(buffer);
    return status;
}

// ---- PCM splicer: drops the frames of the cut ranges, fades around every splice --------------------------
// Works on SOURCE frame positions (decoder timeline), so a cut boundary can fall anywhere inside a decoded
// buffer: every frame is kept or dropped individually. The gain of a kept frame depends only on its distance
// to the neighbouring cuts, so no look-ahead is needed across buffers.
class Splicer {
public:
    Splicer(const std::vector<std::pair<int64_t, int64_t>>& cutsMs, uint32_t rate) {
        for (const auto& [s, e] : cutsMs) {
            Range r;
            r.start = s * rate / 1000;
            r.end = e * rate / 1000;
            if (r.end > r.start) ranges_.push_back(r);
        }
        fade_ = std::max<int64_t>(1, kFadeMs * rate / 1000);
    }

    bool empty() const { return ranges_.empty(); }
    void clear() { ranges_.clear(); }

    // Frames the ranges would remove from a source of `frames` frames.
    int64_t coveredFrames(int64_t frames) const {
        int64_t n = 0;
        for (const auto& r : ranges_) n += std::max<int64_t>(0, std::min(r.end, frames) - std::max<int64_t>(0, r.start));
        return n;
    }

    // Appends the kept frames of in[0, frames) — source frames [pos, pos + frames) — to `out` as 16-bit PCM.
    // Returns the number of frames kept.
    int64_t process(const float* in, int64_t frames, uint32_t ch, int64_t pos, std::vector<int16_t>& out) {
        if (ranges_.empty()) {
            appendScaled(in, frames * ch, 1.f, out);
            return frames;
        }
        const size_t n = ranges_.size();
        // First range that ends after `pos` (ranges are sorted and disjoint).
        size_t ci = static_cast<size_t>(
            std::partition_point(ranges_.begin(), ranges_.end(), [pos](const Range& r) { return r.end <= pos; }) -
            ranges_.begin());
        int64_t kept = 0;
        int64_t i = 0;
        while (i < frames) {
            const int64_t p = pos + i;
            while (ci < n && ranges_[ci].end <= p) ++ci;
            if (ci < n && p >= ranges_[ci].start) {
                // Inside a cut: skip to its end (or the end of this buffer).
                const int64_t stop = std::min(ranges_[ci].end, pos + frames);
                dropped_ += stop - p;
                ranges_[ci].hit = true;
                i = stop - pos;
                continue;
            }
            // Kept frame: the run up to the next cut (or the buffer end) is copied in one go; only the frames
            // within `fade_` of a splice get a per-frame gain.
            const int64_t runEnd = ci < n ? std::min(ranges_[ci].start, pos + frames) : pos + frames;
            const int64_t fadeOutFrom = ci < n ? ranges_[ci].start - fade_ : INT64_MAX;
            const int64_t fadeInUntil = ci > 0 ? ranges_[ci - 1].end + fade_ : INT64_MIN;
            for (int64_t q = p; q < runEnd;) {
                if (q >= fadeInUntil && q < fadeOutFrom) {
                    // Unfaded stretch: bulk copy up to the fade-out start.
                    const int64_t to = std::min(runEnd, fadeOutFrom);
                    appendScaled(in + (q - pos) * ch, (to - q) * ch, 1.f, out);
                    q = to;
                    continue;
                }
                float g = 1.f;
                if (ci < n && q >= fadeOutFrom) g *= curve(static_cast<float>(ranges_[ci].start - q) / fade_);
                if (ci > 0 && q < fadeInUntil) g *= curve(static_cast<float>(q - ranges_[ci - 1].end + 1) / fade_);
                appendScaled(in + (q - pos) * ch, ch, g, out);
                ++q;
            }
            kept += runEnd - p;
            i = runEnd - pos;
        }
        return kept;
    }

    int64_t droppedFrames() const { return dropped_; }
    int segmentsHit() const {
        return static_cast<int>(std::count_if(ranges_.begin(), ranges_.end(), [](const Range& r) { return r.hit; }));
    }

private:
    struct Range {
        int64_t start = 0, end = 0;   // source frames [start, end)
        bool hit = false;             // removed at least one frame
    };

    // Raised cosine, x in (0, 1]: ~0 next to the splice, 1 at the far end of the fade.
    static float curve(float x) {
        x = std::clamp(x, 0.f, 1.f);
        return 0.5f - 0.5f * std::cos(3.14159265f * x);
    }

    static void appendScaled(const float* src, int64_t count, float gain, std::vector<int16_t>& out) {
        const size_t old = out.size();
        out.resize(old + static_cast<size_t>(count));
        int16_t* dst = out.data() + old;
        for (int64_t k = 0; k < count; ++k) {
            const float f = std::clamp(src[k] * gain, -1.f, 1.f);
            dst[k] = static_cast<int16_t>(f * 32767.f);
        }
    }

    std::vector<Range> ranges_;
    int64_t fade_ = 1;
    int64_t dropped_ = 0;
};

// ---- MP3 encode: ProgressiveBuffer -> Decoder (PCM) -> Splicer -> Sink Writer (MP3) ----------------------
DownloadStatus encodeMp3(const DownloadRequest& req, const std::function<void(float)>& progress,
                         const std::atomic<bool>& cancel, const std::wstring& tempPath, DownloadStats& stats) {
    auto buffer = openSource(req);
    if (buffer->waitForLength() < 0) {
        closeSource(buffer);
        return DownloadStatus::NetworkError;
    }

    Decoder decoder;
    bool unsupported = false;
    if (FAILED(decoder.open(buffer, req.mimeType, unsupported))) {
        closeSource(buffer);
        return DownloadStatus::DecodeError;
    }
    const uint32_t rate = decoder.sampleRate() ? decoder.sampleRate() : 44100;
    const uint32_t channels = decoder.channels() ? decoder.channels() : 2;
    const int64_t durationHns = decoder.durationHns();

    Splicer splicer(normalizeCuts(req.cutMs), rate);
    if (!splicer.empty() && durationHns > 0) {
        const int64_t srcFrames = durationHns * rate / kHnsPerSecond;
        if (srcFrames - splicer.coveredFrames(srcFrames) < kMinKeptMs * rate / 1000) {
            ST_LOG_WARN("download", "cut ranges would leave < {} s of {} ms: not cutting", kMinKeptMs / 1000,
                        durationHns / 10'000);
            splicer.clear();
        }
    }

    // Output MP3 type.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, req.mp3Kbps * 1000 / 8);
    // Input PCM (16-bit) type.
    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);

    ComPtr<IMFSinkWriter> writer;
    if (FAILED(MFCreateSinkWriterFromURL(tempPath.c_str(), nullptr, nullptr, &writer))) {
        closeSource(buffer);
        return DownloadStatus::IoError;
    }
    DWORD streamIndex = 0;
    if (FAILED(writer->AddStream(outType.Get(), &streamIndex)) ||
        FAILED(writer->SetInputMediaType(streamIndex, inType.Get(), nullptr)) || FAILED(writer->BeginWriting())) {
        closeSource(buffer);
        return DownloadStatus::EncodeError;
    }

    DownloadStatus status = DownloadStatus::Ok;
    std::vector<float> scratch;
    std::vector<int16_t> pcm;
    int64_t srcPos = -1;     // source frame position of the next decoded frame (decoder timeline)
    int64_t srcFrames = 0;   // frames decoded
    int64_t outFrames = 0;   // frames written: the output timeline (continuous, no gaps at the cuts)
    const int64_t tolerance = kResyncToleranceMs * rate / 1000;
    while (true) {
        if (cancel.load()) {
            status = DownloadStatus::Cancelled;
            break;
        }
        scratch.clear();
        int64_t ts = -1;
        bool formatChanged = false;
        HRESULT hr = S_OK;
        const Decoder::Status ds = decoder.read(scratch, ts, formatChanged, hr);
        if (ds == Decoder::Status::Error) {
            status = DownloadStatus::DecodeError;
            break;
        }
        // A mid-stream sample-rate/channel change would desync the encoder; treat it as done (rare).
        if (formatChanged && (decoder.sampleRate() != rate || decoder.channels() != channels)) break;
        const int64_t frames = static_cast<int64_t>(scratch.size() / channels);
        if (frames > 0) {
            // Frame positions come from a running counter (sample-exact); the decoder timestamp only re-syncs
            // it when they disagree by more than a jitter tolerance (a real gap/discontinuity in the stream).
            if (ts >= 0) {
                const int64_t tsFrames = (ts * rate + kHnsPerSecond / 2) / kHnsPerSecond;
                if (srcPos < 0 || std::llabs(tsFrames - srcPos) > tolerance) srcPos = tsFrames;
            } else if (srcPos < 0) {
                srcPos = 0;
            }

            pcm.clear();
            const int64_t kept = splicer.process(scratch.data(), frames, channels, srcPos, pcm);
            srcPos += frames;
            srcFrames += frames;

            if (kept > 0) {
                const DWORD bytes = static_cast<DWORD>(pcm.size() * sizeof(int16_t));
                ComPtr<IMFMediaBuffer> mbuf;
                ComPtr<IMFSample> sample;
                BYTE* dst = nullptr;
                if (FAILED(MFCreateMemoryBuffer(bytes, &mbuf)) || FAILED(mbuf->Lock(&dst, nullptr, nullptr))) {
                    status = DownloadStatus::EncodeError;
                    break;
                }
                std::memcpy(dst, pcm.data(), bytes);
                mbuf->Unlock();
                mbuf->SetCurrentLength(bytes);
                if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(mbuf.Get()))) {
                    status = DownloadStatus::EncodeError;
                    break;
                }
                const int64_t t0 = outFrames * kHnsPerSecond / rate;
                outFrames += kept;
                sample->SetSampleTime(t0);
                sample->SetSampleDuration(outFrames * kHnsPerSecond / rate - t0);
                if (FAILED(writer->WriteSample(streamIndex, sample.Get()))) {
                    status = DownloadStatus::EncodeError;
                    break;
                }
            }
            if (progress && durationHns > 0)
                progress(std::min(0.98f, static_cast<float>(static_cast<double>(srcPos) * kHnsPerSecond / rate / durationHns)));
        }
        if (ds == Decoder::Status::EndOfStream) break;
    }

    if (status == DownloadStatus::Ok && outFrames == 0) status = DownloadStatus::DecodeError;   // nothing decoded
    if (status == DownloadStatus::Ok) {
        if (FAILED(writer->Finalize())) status = DownloadStatus::EncodeError;
    }
    writer.Reset();
    closeSource(buffer);

    const auto toMs = [rate](int64_t frames) { return (frames * 1000 + rate / 2) / rate; };
    stats.segmentsCut = splicer.segmentsHit();
    stats.cutMs = toMs(splicer.droppedFrames());
    stats.sourceMs = toMs(srcFrames);
    stats.outputMs = toMs(outFrames);
    if (status == DownloadStatus::Ok && stats.segmentsCut > 0)
        ST_LOG_INFO("download", "cut {} segment(s): {} ms removed, {} ms -> {} ms", stats.segmentsCut, stats.cutMs,
                    stats.sourceMs, stats.outputMs);
    return status;
}

DownloadStatus finalizeMp3WithTag(const DownloadRequest& req, const std::wstring& tempPath) {
    std::ifstream in(tempPath, std::ios::binary);
    if (!in) return DownloadStatus::IoError;
    std::vector<uint8_t> sinkFrames;
    const size_t sinkTag = takeSinkTag(in, sinkFrames);
    const std::vector<uint8_t> tag = buildId3(req, sinkFrames);
    if (!tag.empty() && sinkTag > 0) in.seekg(static_cast<std::streamoff>(sinkTag));   // merged into ours: skip it
    std::ofstream outFile(req.outPath, std::ios::binary | std::ios::trunc);
    if (!outFile) return DownloadStatus::IoError;
    if (!tag.empty()) outFile.write(reinterpret_cast<const char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
    auto buf = std::make_unique<char[]>(64 * 1024);
    while (in) {
        in.read(buf.get(), 64 * 1024);
        outFile.write(buf.get(), in.gcount());
    }
    in.close();
    outFile.close();
    DeleteFileW(tempPath.c_str());
    return outFile ? DownloadStatus::Ok : DownloadStatus::IoError;
}

} // namespace

const char* toString(DownloadStatus s) {
    switch (s) {
    case DownloadStatus::Ok: return "ok";
    case DownloadStatus::Cancelled: return "cancelled";
    case DownloadStatus::NetworkError: return "network";
    case DownloadStatus::DecodeError: return "decode";
    case DownloadStatus::EncodeError: return "encode";
    case DownloadStatus::IoError: return "io";
    }
    return "?";
}

std::vector<std::pair<int64_t, int64_t>> normalizeCuts(std::vector<std::pair<int64_t, int64_t>> cuts) {
    for (auto& c : cuts) c.first = std::max<int64_t>(0, c.first);
    std::erase_if(cuts, [](const auto& c) { return c.second <= c.first; });
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::pair<int64_t, int64_t>> out;
    for (const auto& c : cuts) {
        if (!out.empty() && c.first <= out.back().second) out.back().second = std::max(out.back().second, c.second);
        else out.push_back(c);
    }
    return out;
}

DownloadStatus downloadTrack(const DownloadRequest& req, const std::function<void(float)>& progress,
                             const std::atomic<bool>& cancel, DownloadStats* stats) {
    if ((req.url.empty() && req.localPath.empty()) || req.outPath.empty()) return DownloadStatus::IoError;
    MfScope mf;
    if (!mf.ok) return DownloadStatus::EncodeError;

    if (req.mp3Kbps <= 0) {
        // Passthrough: write the original stream bytes. Cut ranges are ignored (would need a re-encode).
        std::vector<uint8_t> data;
        const auto st = fetchAll(req, progress, cancel, data, 1.0f);
        if (st != DownloadStatus::Ok) return st;
        std::ofstream f(req.outPath, std::ios::binary | std::ios::trunc);
        if (!f) return DownloadStatus::IoError;
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return f ? DownloadStatus::Ok : DownloadStatus::IoError;
    }

    // MP3: decode (+ cut) + encode to a temp file, then prepend the ID3 tag into the final path.
    const std::wstring temp = tempSibling(req.outPath);
    DeleteFileW(temp.c_str());
    DownloadStats local;
    const auto st = encodeMp3(req, progress, cancel, temp, local);
    if (st != DownloadStatus::Ok) {
        DeleteFileW(temp.c_str());
        return st;
    }
    const auto fin = finalizeMp3WithTag(req, temp);
    DeleteFileW(temp.c_str());   // finalize's early IoError returns leave it behind (no-op when already gone)
    if (fin == DownloadStatus::Ok) {
        if (stats) *stats = local;
        if (progress) progress(1.0f);
    }
    return fin;
}

} // namespace st::audio
