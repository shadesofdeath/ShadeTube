#include "audio/Track.h"

#include "audio/Decoder.h"
#include "audio/LiveDecoder.h"
#include "audio/LiveStream.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/ReplayGain.h"
#include "core/Log.h"

#include <windows.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace st::audio {

namespace {
constexpr double kRingSeconds = 1.0;
constexpr int kMaxRecoveries = 3;  // consecutive decoder reopen attempts without progress
constexpr int kMaxBadLiveFrames = 200;  // consecutive live frames the decoder rejects before the track fails
constexpr size_t kMaxLiveTitles = 32;

int64_t hnsToFrames(int64_t hns, uint32_t rate) {
    return static_cast<int64_t>((static_cast<long double>(hns) * rate) / 10'000'000.0L + 0.5L);
}
} // namespace

Track::Track(const StreamSource& source, int64_t startMs, std::atomic<int64_t>* memCounter, void* wakeEvent)
    : source_(source), liveStartMs_(source.live ? std::max<int64_t>(0, startMs) : 0), wakeEvent_(wakeEvent) {
    if (source.live) {   // endless: no duration, no seeking; startMs only starts the clock
        live_ = std::make_shared<LiveStream>(source.url, source.mimeType, memCounter, source.allowLocalNetwork);
        live_->start();
    } else {
        buffer_ = std::make_shared<ProgressiveBuffer>(source.url, source.localPath, source.contentLength, memCounter);
        durationMs_.store(std::max<int64_t>(0, source.durationMsHint));
        if (startMs > 0) {
            seekPending_ = true;
            seekTarget_ = startMs;
        }
        buffer_->start();
    }
    thread_ = std::thread([this] { run(); });
}

Track::~Track() {
    cancel();
    if (thread_.joinable()) thread_.join();
    if (buffer_) buffer_->release();
    if (live_) live_->join();   // the engine may still hold the stream for liveInfo(): only the thread goes now
}

void Track::cancel() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (buffer_) buffer_->cancel();
    if (live_) live_->cancel();
}

bool Track::finished() const {
    return decodeDone_.load(std::memory_order_acquire) && (buffer_ ? buffer_->finished() : live_->finished());
}

int64_t Track::durationMs() const { return durationMs_.load(std::memory_order_relaxed); }

float Track::gainDb() const {
    if (!source_.replayGain) return source_.gainDb;
    const float rg = replayGainDb_.load(std::memory_order_acquire);
    return std::isnan(rg) ? 0.f : rg + source_.gainDb;
}

ErrorKind Track::errorKind() const {
    std::lock_guard lock(mutex_);
    return errorKind_;
}

std::string Track::errorMessage() const {
    std::lock_guard lock(mutex_);
    return errorMessage_;
}

void Track::wake() { SetEvent(static_cast<HANDLE>(wakeEvent_)); }

void Track::setError(ErrorKind kind, std::string message) {
    ST_LOG_WARN("audio", "track {} failed: {}", source_.tag, message);
    {
        std::lock_guard lock(mutex_);
        errorKind_ = kind;
        errorMessage_ = std::move(message);
    }
    failed_.store(true, std::memory_order_release);
    wake();
}

// ---- engine side ------------------------------------------------------------------------------

size_t Track::read(float* dst, size_t frames, int64_t& firstFrame, uint32_t channels, uint32_t rate) {
    std::unique_lock lock(mutex_);
    firstFrame = readPos_;
    if (channels != channels_ || rate != sampleRate_) return 0;   // a live format change the engine hasn't seen yet
    const size_t n = std::min(frames, count_);
    if (n == 0) return 0;
    const size_t ch = channels_;
    const size_t first = std::min(n, capFrames_ - readIdx_);
    std::memcpy(dst, ring_.data() + readIdx_ * ch, first * ch * sizeof(float));
    if (n > first) std::memcpy(dst + first * ch, ring_.data(), (n - first) * ch * sizeof(float));
    readIdx_ = (readIdx_ + n) % capFrames_;
    count_ -= n;
    readPos_ += static_cast<int64_t>(n);
    const bool notify = producerWaiting_;
    lock.unlock();
    if (notify) cv_.notify_one();
    return n;
}

size_t Track::available() const {
    std::lock_guard lock(mutex_);
    return count_;
}

size_t Track::capacity() const {
    std::lock_guard lock(mutex_);
    return capFrames_;
}

bool Track::ended() const {
    std::lock_guard lock(mutex_);
    return decEof_ && count_ == 0 && !seekPending_;
}

bool Track::decoderEnded() const {
    std::lock_guard lock(mutex_);
    return decEof_ && !seekPending_;
}

void Track::seek(int64_t ms) {
    if (live_) return;
    {
        std::lock_guard lock(mutex_);
        seekPending_ = true;
        seekTarget_ = std::max<int64_t>(0, ms);
        count_ = 0;
        readIdx_ = 0;
        posValid_ = false;
        decEof_ = false;
        if (sampleRate_) readPos_ = seekTarget_ * sampleRate_ / 1000;
    }
    cv_.notify_all();
    // If the decode thread is stuck waiting for bytes of the OLD position (e.g. a slow download
    // frontier), break that wait: the decoder is reopened at the new position instead.
    if (formatReady() && inDecoder_.load() && buffer_->hasWaiters()) buffer_->interruptWaiters();
}

// ---- decode thread ----------------------------------------------------------------------------

void Track::run() {
    SetThreadDescription(GetCurrentThread(), L"st-audio-decode");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        if (live_) {
            liveLoop();
        } else {
            Decoder decoder;
            decodeLoop(decoder);
        }
    } catch (const std::exception& e) {
        setError(ErrorKind::Other, std::string("decoder: ") + e.what());
    }
    if (SUCCEEDED(co)) CoUninitialize();
    decodeDone_.store(true, std::memory_order_release);
    wake();
}

bool Track::openDecoder(Decoder& decoder) {
    for (int attempt = 0;; ++attempt) {
        bool unsupported = false;
        inDecoder_.store(true);
        const HRESULT hr = decoder.open(buffer_, source_.mimeType, unsupported);
        inDecoder_.store(false);
        if (SUCCEEDED(hr)) return true;
        {
            std::lock_guard lock(mutex_);
            if (quit_) return false;
        }
        if (buffer_->failed()) {
            setError(ErrorKind::Network, buffer_->errorMessage());
            return false;
        }
        if (hr == E_ABORT && attempt < 8) continue;  // interrupted by a seek: try again
        setError(unsupported ? ErrorKind::UnsupportedFormat : ErrorKind::Other,
                 std::format("cannot open {} stream (hr=0x{:08X})", source_.mimeType.empty() ? "media" : source_.mimeType,
                             static_cast<uint32_t>(hr)));
        return false;
    }
}

void Track::decodeLoop(Decoder& decoder) {
    if (buffer_->waitForLength() < 0) {
        if (buffer_->failed()) setError(ErrorKind::Network, buffer_->errorMessage());
        return;
    }
    if (source_.replayGain && !source_.localPath.empty()) {
        if (const auto rg = replaygain::readTrackGainDb(source_.localPath)) {
            replayGainDb_.store(*rg, std::memory_order_release);
            ST_LOG_DEBUG("audio", "track {} ReplayGain {:+.2f} dB", source_.tag, *rg);
        }
    }
    if (!openDecoder(decoder)) return;
    if (decoder.durationHns() > 0) durationMs_.store(decoder.durationHns() / 10'000);

    std::vector<float> pending;   // decoded frames not yet in the ring
    size_t pendingOff = 0;        // floats already moved out of `pending`
    int64_t pendingPos = 0;       // track frame of pending[pendingOff]
    int64_t nextPos = 0;          // track frame following the last decoded frame
    int64_t trimUntil = 0;        // drop frames before this (sample-accurate seek)
    bool needTimestamp = true;    // take the position from the next sample's timestamp
    bool eos = false;
    bool broken = false;          // the MF reader failed (interrupted); reopen before further use
    int recoveries = 0;
    std::vector<float> scratch;

    for (;;) {
        int64_t seekMs = -1;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (quit_) return;
                if (seekPending_) {
                    seekMs = seekTarget_;
                    seekPending_ = false;
                    break;
                }
                const bool published = formatReady_.load(std::memory_order_relaxed);
                const size_t ch = channels_;
                const size_t pendingFrames = published ? (pending.size() - pendingOff) / ch : 0;
                if (pendingFrames > 0) {
                    const size_t space = capFrames_ - count_;
                    if (space == 0) {
                        producerWaiting_ = true;
                        cv_.wait(lock);
                        producerWaiting_ = false;
                        continue;
                    }
                    const size_t n = std::min(space, pendingFrames);
                    if (!posValid_) {
                        readPos_ = pendingPos;
                        posValid_ = true;
                    }
                    const size_t writeIdx = (readIdx_ + count_) % capFrames_;
                    const size_t first = std::min(n, capFrames_ - writeIdx);
                    const float* src = pending.data() + pendingOff;
                    std::memcpy(ring_.data() + writeIdx * ch, src, first * ch * sizeof(float));
                    if (n > first) std::memcpy(ring_.data(), src + first * ch, (n - first) * ch * sizeof(float));
                    count_ += n;
                    pendingOff += n * ch;
                    pendingPos += static_cast<int64_t>(n);
                    if (pendingOff == pending.size()) {
                        pending.clear();
                        pendingOff = 0;
                    }
                    if (wakeRequested_.exchange(false)) wake();
                    continue;
                }
                if (eos && published) {
                    if (!decEof_) {
                        decEof_ = true;
                        wake();
                    }
                    cv_.wait(lock);  // until seek or quit
                    continue;
                }
                break;  // decode more
            }
        }

        if (seekMs >= 0) {
            pending.clear();
            pendingOff = 0;
            eos = false;
            HRESULT hr = E_FAIL;
            if (!broken) {
                inDecoder_.store(true);
                hr = decoder.seek(seekMs * 10'000);
                inDecoder_.store(false);
            }
            if (FAILED(hr)) {
                if (!openDecoder(decoder)) return;
                inDecoder_.store(true);
                hr = decoder.seek(seekMs * 10'000);
                inDecoder_.store(false);
            }
            broken = FAILED(hr);
            if (broken) {  // interrupted again (a newer seek is usually pending) or not seekable
                if (buffer_->failed()) {
                    setError(ErrorKind::Network, buffer_->errorMessage());
                    return;
                }
                if (++recoveries > kMaxRecoveries * 3) {
                    setError(ErrorKind::Other, std::format("seek failed (hr=0x{:08X})", static_cast<uint32_t>(hr)));
                    return;
                }
                std::lock_guard lock(mutex_);
                if (!seekPending_ && !quit_) {  // nothing newer to do: retry this one
                    seekPending_ = true;
                    seekTarget_ = seekMs;
                }
                continue;
            }
            // Before the format is known the target is kept negative (in ms) and converted to
            // frames once the sample rate is published below.
            trimUntil = formatReady() ? seekMs * static_cast<int64_t>(sampleRate_) / 1000 : -seekMs;
            needTimestamp = true;
            continue;
        }

        scratch.clear();
        int64_t ts = -1;
        bool formatChanged = false;
        HRESULT hr = S_OK;
        inDecoder_.store(true);
        const Decoder::Status status = decoder.read(scratch, ts, formatChanged, hr);
        inDecoder_.store(false);

        if (status == Decoder::Status::Error) {
            {
                std::lock_guard lock(mutex_);
                if (quit_) return;
                if (seekPending_) {  // interrupted for a seek
                    broken = true;
                    continue;
                }
            }
            if (buffer_->failed()) {
                setError(ErrorKind::Network, buffer_->errorMessage());
                return;
            }
            // Unexpected reader failure: reopen and continue at the current position.
            if (++recoveries > kMaxRecoveries) {
                setError(ErrorKind::Other, std::format("decode failed (hr=0x{:08X})", static_cast<uint32_t>(hr)));
                return;
            }
            ST_LOG_WARN("audio", "decoder error 0x{:08X}, reopening at frame {}", static_cast<uint32_t>(hr), nextPos);
            std::lock_guard lock(mutex_);
            seekPending_ = true;
            seekTarget_ = sampleRate_ ? nextPos * 1000 / sampleRate_ : 0;
            broken = true;
            continue;
        }

        if (!formatReady()) {
            // Publish the format after the first decoded sample: AAC (HE-AAC/SBR) may change the
            // output type on the first sample.
            const uint32_t rate = decoder.sampleRate(), ch = decoder.channels();
            {
                std::lock_guard lock(mutex_);
                sampleRate_ = rate;
                channels_ = ch;
                capFrames_ = static_cast<size_t>(rate * kRingSeconds);
                ring_.assign(capFrames_ * ch, 0.f);
                readIdx_ = count_ = 0;
            }
            formatReady_.store(true, std::memory_order_release);
            if (trimUntil < 0) trimUntil = -trimUntil * static_cast<int64_t>(rate) / 1000;
            wake();
        } else if (formatChanged && (decoder.sampleRate() != sampleRate_ || decoder.channels() != channels_)) {
            setError(ErrorKind::UnsupportedFormat, "output format changed mid-stream");
            return;
        }

        const size_t ch = channels_;
        const size_t frames = scratch.size() / ch;
        if (frames > 0) {
            recoveries = 0;
            int64_t pos = (needTimestamp && ts >= 0) ? hnsToFrames(ts, sampleRate_) : nextPos;
            needTimestamp = false;
            nextPos = pos + static_cast<int64_t>(frames);
            size_t skip = 0;
            if (pos < trimUntil) skip = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(frames), trimUntil - pos));
            if (skip < frames) {
                if (pending.size() == pendingOff) {
                    pending.clear();
                    pendingOff = 0;
                    pendingPos = pos + static_cast<int64_t>(skip);
                }
                pending.insert(pending.end(), scratch.begin() + static_cast<ptrdiff_t>(skip * ch),
                               scratch.begin() + static_cast<ptrdiff_t>(frames * ch));
            }
        }
        if (status == Decoder::Status::EndOfStream) {
            eos = true;
            if (sampleRate_ && durationMs_.load() <= 0) durationMs_.store(nextPos * 1000 / sampleRate_);
        }
    }
}

// ---- live tracks ------------------------------------------------------------------------------

int64_t Track::bufferedAheadMs() const {
    const uint32_t rate = sampleRate();
    int64_t ms = 0;
    if (rate) {
        std::lock_guard lock(mutex_);
        ms = static_cast<int64_t>(count_) * 1000 / rate;
    }
    return ms + (live_ ? live_->bufferedMs() : 0);
}

bool Track::setLivePaused(bool paused) {
    if (!live_) return false;
    live_->setPaused(paused);
    if (paused || !live_->takeStale()) return false;
    {
        std::lock_guard lock(mutex_);
        count_ = 0;
        readIdx_ = 0;
        liveFlush_ = true;
        for (auto& t : liveTitles_) t.first = std::min(t.first, readPos_);   // titles of dropped audio: show now
    }
    cv_.notify_all();
    return true;
}

std::optional<std::string> Track::takeLiveTitle(int64_t audibleMs) {
    const uint32_t rate = sampleRate();
    std::lock_guard lock(mutex_);
    if (liveTitles_.empty() || !rate) return std::nullopt;
    const int64_t frame = audibleMs * rate / 1000;
    std::optional<std::string> title;
    while (!liveTitles_.empty() && liveTitles_.front().first <= frame) {
        title = std::move(liveTitles_.front().second);
        liveTitles_.pop_front();
    }
    return title;
}

// Live decode thread: compressed frames from the stream -> decoder -> ring. The position runs on continuously
// (reconnects, pauses and format changes included): it is the listening time since the item started.
void Track::liveLoop() {
    LiveDecoder decoder;
    live::Frame frame;
    std::vector<float> pcm;
    std::optional<std::string> title;   // waits for the first frame that produces audio
    int bad = 0;
    int64_t nextPos = 0;
    for (;;) {
        const LiveStream::Pop got = live_->pop(frame);
        if (got == LiveStream::Pop::Cancelled) return;
        if (got == LiveStream::Pop::Failed) {
            setError(live_->errorKind(), live_->errorMessage());
            return;
        }
        if (frame.title) title = std::move(frame.title);
        if (!decoder.isOpen() || decoder.configKey() != live::frameConfigKey(frame)) {
            bool unsupported = false;
            const HRESULT hr = decoder.open(frame, unsupported);
            if (FAILED(hr)) {
                setError(unsupported ? ErrorKind::UnsupportedFormat : ErrorKind::Other,
                         std::format("cannot decode {} {} Hz / {} ch (hr=0x{:08X})", live::codecName(frame), frame.sampleRate,
                                     frame.channels, static_cast<uint32_t>(hr)));
                return;
            }
        } else if (frame.discontinuity) {
            decoder.flush();
        }
        pcm.clear();
        const HRESULT hr = decoder.decode(frame, pcm);
        if (FAILED(hr)) {   // a corrupt frame (e.g. right after a resync): skip it
            if (++bad > kMaxBadLiveFrames) {
                setError(ErrorKind::Other, std::format("live decode failed (hr=0x{:08X})", static_cast<uint32_t>(hr)));
                return;
            }
            decoder.flush();
            continue;
        }
        bad = 0;
        const uint32_t rate = decoder.sampleRate(), ch = decoder.channels();
        if (pcm.empty() || !rate || !ch) continue;   // decoder priming
        if (!formatReady()) {
            {
                std::lock_guard lock(mutex_);
                sampleRate_ = rate;
                channels_ = ch;
                capFrames_ = static_cast<size_t>(rate * kRingSeconds);
                ring_.assign(capFrames_ * ch, 0.f);
                readIdx_ = count_ = 0;
            }
            nextPos = liveStartMs_ * rate / 1000;
            formatReady_.store(true, std::memory_order_release);
            live_->setDecodedFormat(rate, ch);
            wake();
        } else if (rate != sampleRate() || ch != channels()) {
            // New output format: let the engine play out the old one, then switch (it reopens the device).
            std::unique_lock lock(mutex_);
            producerWaiting_ = true;
            cv_.wait(lock, [&] { return quit_ || count_ == 0; });
            producerWaiting_ = false;
            if (quit_) return;
            const uint32_t oldRate = sampleRate_;
            nextPos = nextPos * rate / oldRate;
            for (auto& t : liveTitles_) t.first = t.first * rate / oldRate;
            sampleRate_ = rate;
            channels_ = ch;
            capFrames_ = static_cast<size_t>(rate * kRingSeconds);
            ring_.assign(capFrames_ * ch, 0.f);
            readIdx_ = 0;
            posValid_ = false;
            lock.unlock();
            live_->setDecodedFormat(rate, ch);
            ST_LOG_INFO("audio", "live track {}: output format now {} Hz / {} ch", source_.tag, rate, ch);
        }
        if (title) {
            std::lock_guard lock(mutex_);
            liveTitles_.emplace_back(nextPos, std::move(*title));
            if (liveTitles_.size() > kMaxLiveTitles) liveTitles_.pop_front();
            title.reset();
        }
        if (!pushLive(pcm, nextPos)) return;
    }
}

bool Track::pushLive(const std::vector<float>& pcm, int64_t& nextPos) {
    std::unique_lock lock(mutex_);
    const size_t ch = channels_;
    const size_t frames = pcm.size() / ch;
    size_t off = 0;
    while (off < frames) {
        if (quit_) return false;
        if (liveFlush_) {   // resumed after a pause that dropped audio: this PCM is as stale as the ring was
            liveFlush_ = false;
            nextPos = readPos_;
            for (auto& t : liveTitles_) t.first = std::min(t.first, readPos_);
            return true;
        }
        const size_t space = capFrames_ - count_;
        if (space == 0) {   // full (paused, or far enough ahead): 0% CPU until read() makes room
            producerWaiting_ = true;
            cv_.wait(lock);
            producerWaiting_ = false;
            continue;
        }
        const size_t n = std::min(space, frames - off);
        if (!posValid_) {
            readPos_ = nextPos;
            posValid_ = true;
        }
        const size_t writeIdx = (readIdx_ + count_) % capFrames_;
        const size_t first = std::min(n, capFrames_ - writeIdx);
        const float* src = pcm.data() + off * ch;
        std::memcpy(ring_.data() + writeIdx * ch, src, first * ch * sizeof(float));
        if (n > first) std::memcpy(ring_.data(), src + first * ch, (n - first) * ch * sizeof(float));
        count_ += n;
        off += n;
        nextPos += static_cast<int64_t>(n);
        if (wakeRequested_.exchange(false)) wake();
    }
    return true;
}

} // namespace st::audio
