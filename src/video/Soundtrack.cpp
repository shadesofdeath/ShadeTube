#include "video/Soundtrack.h"

#include "audio/Decoder.h"
#include "audio/MfByteStream.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Log.h"
#include "video/Fmp4Index.h"

#include <mfapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>

namespace st::video {

namespace {

// COM + Media Foundation for this call (both reference counted, so nesting with the engine's own is fine).
struct MfScope {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ~MfScope() {
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }
};

// A plain decimation to kSoundtrackRate when the Source Reader kept the native rate: each output sample averages the
// input samples around its position (a box low-pass, enough for features up to ~5 kHz).
std::vector<float> resample(const std::vector<float>& in, int inRate) {
    if (inRate == kSoundtrackRate || in.empty() || inRate <= 0) return in;
    const double ratio = static_cast<double>(inRate) / kSoundtrackRate;
    const size_t n = static_cast<size_t>(static_cast<double>(in.size()) / ratio);
    std::vector<float> out(n);
    const double half = std::max(0.5, ratio * 0.5);
    for (size_t k = 0; k < n; ++k) {
        const double c = static_cast<double>(k) * ratio;
        const ptrdiff_t lo = std::max<ptrdiff_t>(0, static_cast<ptrdiff_t>(std::ceil(c - half)));
        const ptrdiff_t hi = std::min<ptrdiff_t>(static_cast<ptrdiff_t>(in.size()) - 1, static_cast<ptrdiff_t>(std::floor(c + half)));
        double sum = 0;
        for (ptrdiff_t i = lo; i <= hi; ++i) sum += in[static_cast<size_t>(i)];
        out[k] = hi >= lo ? static_cast<float>(sum / static_cast<double>(hi - lo + 1)) : 0.f;
    }
    return out;
}

struct Decoded {
    std::vector<float> mono;
    uint32_t rate = 0, channels = 0;
    int reads = 0;
    int64_t firstHns = 0;
};

// One pass of the Source Reader over the buffer (or a view of it), mono at the reader's rate. `editHns` comes off the
// time stamps (the edit list the view freed).
Decoded decodeOnce(const std::shared_ptr<audio::ProgressiveBuffer>& buffer, const std::string& mime,
                   std::shared_ptr<const audio::ByteStreamView> view, int64_t editHns, const std::function<bool()>& cancelled) {
    audio::Decoder decoder;
    bool unsupported = false;
    HRESULT hr = decoder.open(buffer, mime, unsupported, kSoundtrackRate, std::move(view));
    if (FAILED(hr)) throw std::runtime_error(std::format("soundtrack decoder failed: 0x{:08X}", static_cast<uint32_t>(hr)));
    Decoded d;
    std::vector<float> scratch;
    bool first = true;
    for (;; ++d.reads) {
        if (cancelled && cancelled()) throw std::runtime_error("cancelled");
        scratch.clear();
        int64_t ts = -1;
        bool formatChanged = false;
        const auto status = decoder.read(scratch, ts, formatChanged, hr);
        if (status == audio::Decoder::Status::Error) {
            if (buffer->failed()) throw std::runtime_error("soundtrack download failed: " + buffer->errorMessage());
            throw std::runtime_error(std::format("soundtrack decode failed: 0x{:08X}", static_cast<uint32_t>(hr)));
        }
        const uint32_t ch = std::max<uint32_t>(1, decoder.channels());
        const size_t frames = scratch.size() / ch;
        if (frames > 0) {
            size_t skip = 0;
            if (first) {
                // The engine's timeline: the first sample sits at its time stamp, the rest are counted from it.
                first = false;
                d.firstHns = (ts >= 0 ? ts : 0) - editHns;
                const int64_t lead = d.firstHns * static_cast<int64_t>(decoder.sampleRate()) / 10'000'000;
                if (lead > 0 && lead < static_cast<int64_t>(decoder.sampleRate()) * 5) d.mono.assign(static_cast<size_t>(lead), 0.f);
                if (lead < 0) skip = static_cast<size_t>(std::min<int64_t>(-lead, static_cast<int64_t>(frames)));
            }
            const size_t old = d.mono.size();
            d.mono.resize(old + frames - skip);
            for (size_t f = skip; f < frames; ++f) {
                float s = 0;
                for (uint32_t c = 0; c < ch; ++c) s += scratch[f * ch + c];
                d.mono[old + f - skip] = s / static_cast<float>(ch);
            }
        }
        if (status == audio::Decoder::Status::EndOfStream) break;
    }
    d.rate = decoder.sampleRate();
    d.channels = decoder.channels();
    return d;
}

} // namespace

Soundtrack decodeSoundtrack(const SoundtrackSource& source, const std::function<bool()>& cancelled) {
    MfScope mf;
    if (FAILED(mf.mf)) throw std::runtime_error("Media Foundation unavailable");
    auto buffer = std::make_shared<audio::ProgressiveBuffer>(source.url, source.localPath, source.contentLength, nullptr);
    buffer->start();
    struct Stop {
        std::shared_ptr<audio::ProgressiveBuffer> b;
        ~Stop() {
            b->cancel();
            b->release();
        }
    } stop{buffer};
    if (buffer->waitForLength() <= 0) throw std::runtime_error("soundtrack download failed: " + buffer->errorMessage());

    // First exactly as the engine opens a stream (its clock counts that timeline, decoder delays included). Some of
    // YouTube's fragmented MP4s carry an edit list (Art Tracks' AAC), of which Media Foundation presents no sample at
    // all (as with the picture streams): those are read again with the init boxes' edts freed (video/Fmp4Index) and
    // the edit's media time taken off the time stamps.
    Decoded d = decodeOnce(buffer, source.mimeType, nullptr, 0, cancelled);
    int64_t editHns = 0;
    if (d.mono.empty() && source.mimeType.starts_with("audio/mp4")) {
        std::vector<uint8_t> head(static_cast<size_t>(std::min<int64_t>(buffer->length(), 64 * 1024)));
        if (buffer->read(0, head.data(), head.size()) == audio::ProgressiveBuffer::ReadStatus::Ok) {
            const auto index = parseFmp4Index(head, buffer->length());
            if (index.valid()) {
                auto view = std::make_shared<audio::ByteStreamView>();
                view->head = index.init;
                view->bodyOffset = index.segments.front().offset;
                editHns = index.editUs * 10;
                d = decodeOnce(buffer, source.mimeType, std::move(view), editHns, cancelled);
            }
        }
    }
    Soundtrack out;
    out.sampleRate = kSoundtrackRate;
    out.mono = resample(d.mono, static_cast<int>(d.rate));
    ST_LOG_INFO("video", "soundtrack: {} reads, {} Hz x {} -> {:.2f} s at {} Hz, first sample at {:.3f} s{} ({} KB)", d.reads, d.rate,
                d.channels, out.seconds(), out.sampleRate, d.firstHns / 1e7,
                editHns ? std::format(", edit list {:.3f} s", editHns / 1e7) : std::string(), buffer->length() >> 10);
    if (out.mono.empty()) throw std::runtime_error("soundtrack decoded to nothing");
    return out;
}

} // namespace st::video
