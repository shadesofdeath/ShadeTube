#include "video/Soundtrack.h"

#include "audio/Decoder.h"
#include "audio/ProgressiveBuffer.h"
#include "core/Log.h"

#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <thread>

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

// A plain decimation to kSoundtrackRate when a decoder kept another rate: each output sample averages the input
// samples around its position (a box low-pass, enough for features up to ~5 kHz).
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
    // A read blocked on a stalled network only returns when the buffer is cancelled: watched from the side.
    std::jthread watch;
    if (cancelled)
        watch = std::jthread([buffer, cancelled](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancelled()) {
                    buffer->cancel();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    if (buffer->waitForLength() <= 0) throw std::runtime_error("soundtrack download failed: " + buffer->errorMessage());

    // The engine's own decoder (Opus / Vorbis built in, the rest through Media Foundation, a fragmented MP4's edit list
    // applied), so the timeline is the one the engine's clock counts.
    audio::Decoder decoder;
    bool unsupported = false;
    // At the stream's own rate, decimated here: the built-in path's windowed-sinc resampler would cost far more than
    // the decoding itself (8 s against 1 s for a 3.5 minute song in a Debug build), and the features only need 6 kHz.
    HRESULT hr = decoder.open(buffer, source.mimeType, unsupported, audio::RatePolicy{});
    if (FAILED(hr)) throw std::runtime_error(std::format("soundtrack decoder failed: 0x{:08X}", static_cast<uint32_t>(hr)));

    std::vector<float> mono, scratch;
    int64_t firstHns = 0;
    bool first = true;
    int reads = 0;
    for (;; ++reads) {
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
            if (first) {
                // As the engine does: the first sample sits at its time stamp, the rest are counted from it.
                first = false;
                firstHns = ts >= 0 ? ts : 0;
                const int64_t lead = firstHns * static_cast<int64_t>(decoder.sampleRate()) / 10'000'000;
                if (lead > 0 && lead < static_cast<int64_t>(decoder.sampleRate()) * 5) mono.assign(static_cast<size_t>(lead), 0.f);
            }
            const size_t old = mono.size();
            mono.resize(old + frames);
            for (size_t f = 0; f < frames; ++f) {
                float s = 0;
                for (uint32_t c = 0; c < ch; ++c) s += scratch[f * ch + c];
                mono[old + f] = s / static_cast<float>(ch);
            }
        }
        if (status == audio::Decoder::Status::EndOfStream) break;
    }
    Soundtrack out;
    out.sampleRate = kSoundtrackRate;
    out.mono = resample(mono, static_cast<int>(decoder.sampleRate()));
    ST_LOG_INFO("video", "soundtrack: {} reads, {} Hz x {} ({}) -> {:.2f} s at {} Hz, first sample at {:.3f} s ({} KB)", reads,
                decoder.sampleRate(), decoder.channels(), decoder.builtIn() ? "built-in" : "Media Foundation", out.seconds(),
                out.sampleRate, firstHns / 1e7, buffer->length() >> 10);
    if (out.mono.empty()) throw std::runtime_error("soundtrack decoded to nothing");
    return out;
}

} // namespace st::video
